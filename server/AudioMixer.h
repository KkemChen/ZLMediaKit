#pragma once

#include "Codec/Transcode.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <array>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
}

class AudioMixer {
public:
    using Ptr                                     = std::shared_ptr<AudioMixer>;
    constexpr static int SAMPLE_RATE              = 48000;
    constexpr static int SAMPLE_SIZE              = 1024;
    constexpr static int CHANNELS                 = 1;
    constexpr static int BIT_PER_SAMPLE           = 32;
    constexpr static AVSampleFormat SAMPLE_FORMAT = AV_SAMPLE_FMT_FLTP;

    struct AudioInput {
        AVFilterContext *bufferSrc = nullptr;
        std::string name;
        AVRational timeBase;
        std::queue<mediakit::FFmpegFrame::Ptr> frameQueue;
        std::mutex queueMutex;
        bool active = true;
    };

    AudioMixer() {
#if LIBAVFILTER_VERSION_MAJOR < 7
        avfilter_register_all();
#endif
        _running      = true;
        _outputThread = std::thread(&AudioMixer::outputThreadFunc, this);
    }

    ~AudioMixer() {
        {
            std::lock_guard<std::mutex> lock(_graphMutex);
            _running = false;
        }
        _outputCv.notify_one();
        if (_outputThread.joinable())
            _outputThread.join();
        {
            std::lock_guard<std::mutex> lock(_graphMutex);
            cleanupFilterGraph();
        }
    }

    bool addAudioInput(const std::string &id, AVRational timeBase) {
        std::lock_guard<std::mutex> lock(_graphMutex);
        
        if (_inputs.count(id))
            return false;
            
        auto input      = std::make_unique<AudioInput>();
        input->name     = "in" + id;
        input->timeBase = timeBase;
        _inputs.emplace(id, std::move(input));
        
        rebuildFilterGraph();
        return true;
    }

    bool removeAudioInput(const std::string &id) {
        std::lock_guard<std::mutex> lock(_graphMutex);
        
        auto it = _inputs.find(id);
        if (it == _inputs.end())
            return false;
            
        it->second->active = false;
        {
            std::lock_guard<std::mutex> ql(it->second->queueMutex);
            std::queue<mediakit::FFmpegFrame::Ptr> empty;
            std::swap(it->second->frameQueue, empty);
        }
        _inputs.erase(it);
        
        rebuildFilterGraph();
        return true;
    }

    void inputFrame(const std::string &id, const mediakit::FFmpegFrame::Ptr& frame) {
        std::lock_guard<std::mutex> lock(_graphMutex);
        
        auto it = _inputs.find(id);
        if (it == _inputs.end() || !it->second->active || !_initialized)
            return;
            
        AVFilterContext *ctx = it->second->bufferSrc;
        if (!ctx) return;
        
        AVFrame *avf = frame->get();
        if (!avf) return;
        
        av_buffersrc_add_frame_flags(ctx, avf, AV_BUFFERSRC_FLAG_KEEP_REF);
        _outputCv.notify_one();
    }

    void setOutputCallback(const std::function<void(const std::shared_ptr<AVPacket> &, const std::array<uint8_t, 7> &)> &cb) {
        _cb = cb;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(_graphMutex);
        if (!_initialized)
            return;
            
        for (auto &kv : _inputs) {
            if (kv.second->bufferSrc) {
                av_buffersrc_add_frame_flags(kv.second->bufferSrc, nullptr, 0);
            }
        }
        _outputCv.notify_one();
    }

protected:
    void init_codec_context() {
        auto codec = const_cast<AVCodec *>(avcodec_find_encoder(AV_CODEC_ID_AAC));
        if (!codec) {
            WarnL << "FFmpeg AAC encoder not found";
            return;
        }
        _encCtx.reset(
            avcodec_alloc_context3(codec), [](AVCodecContext *ptr) {
                if (ptr) {
                    avcodec_free_context(&ptr);
                }
            });

        if (!_encCtx) {
            WarnL << "Could not alloc codec context";
            return;
        }

        _encCtx->sample_fmt  = AudioMixer::SAMPLE_FORMAT;
        _encCtx->sample_rate = AudioMixer::SAMPLE_RATE;
        AVChannelLayout mono_layout;
        av_channel_layout_from_string(&mono_layout, "mono");
        _encCtx->ch_layout = mono_layout;
        av_channel_layout_uninit(&mono_layout);
        _encCtx->ch_layout.nb_channels = AudioMixer::CHANNELS;
        _encCtx->bit_rate              = 32000;
        _encCtx->profile               = FF_PROFILE_AAC_LOW;
        _encCtx->time_base             = { 1, 1000 };

        if (avcodec_open2(_encCtx.get(), codec, nullptr) < 0) {
            WarnL << "Could not open encoder";
            return;
        }
    }

    std::shared_ptr<AVPacket> encode_aac(const std::shared_ptr<AVFrame> &frame) {
        if (!_encCtx) {
            init_codec_context();
        }
        if (_encCtx) {
            int ret = avcodec_send_frame(_encCtx.get(), frame.get());
            if (ret < 0) {
                return nullptr;
            }

            std::shared_ptr<AVPacket> pkt(
                av_packet_alloc(), [](AVPacket *p) {
                    if (p)
                        av_packet_free(&p);
                });
            ret = avcodec_receive_packet(_encCtx.get(), pkt.get());
            if (ret < 0) {
                return nullptr;
            }
            return pkt;
        }
        return nullptr;
    }

    std::array<uint8_t, 7> createADTSHeader(int sampleRate, int channels, int frameLength) {
        auto getSampleRateIndex = [](int sampleRate) -> int {
            switch (sampleRate) {
                case 96000: return 0;
                case 88200: return 1;
                case 64000: return 2;
                case 48000: return 3;
                case 44100: return 4;
                case 32000: return 5;
                case 24000: return 6;
                case 22050: return 7;
                case 16000: return 8;
                case 12000: return 9;
                case 11025: return 10;
                case 8000: return 11;
                case 7350: return 12;
                default: throw std::runtime_error("Unsupported sample rate.");
            }
        };

        std::array<uint8_t, 7> adtsHeader{};

        int profile = 1; // AAC LC
        int sampleRateIndex = getSampleRateIndex(sampleRate);
        int fullFrameLength = frameLength + 7;

        adtsHeader[0] = 0xFF;
        adtsHeader[1] = 0xF1;
        adtsHeader[2] = (uint8_t)(((profile - 1) << 6) + (sampleRateIndex << 2) + (channels >> 2));
        adtsHeader[3] = (uint8_t)(((channels & 3) << 6) + (fullFrameLength >> 11));
        adtsHeader[4] = (uint8_t)((fullFrameLength & 0x7FF) >> 3);
        adtsHeader[5] = (uint8_t)(((fullFrameLength & 7) << 5) + 0x1F);
        adtsHeader[6] = 0xFC;

        return adtsHeader;
    }

protected:
    void cleanupFilterGraph() {
        if (_graph) {
            avfilter_graph_free(&_graph);
            _graph       = nullptr;
            _amixCtx     = nullptr;
            _sinkCtx     = nullptr;
            _initialized = false;
        }
        if (_fifo) {
            av_audio_fifo_free(_fifo);
            _fifo = nullptr;
        }
    }

    void rebuildFilterGraph() {
        cleanupFilterGraph();
        // Gather active inputs
        std::vector<AudioInput *> act;
        for (auto &kv : _inputs) {
            if (kv.second->active)
                act.push_back(kv.second.get());
        }
        
        if (act.empty()) {
            return;
        }

        _graph = avfilter_graph_alloc();
        if (!_graph)
            return;

        // Create sources
        for (auto *in : act) {
            const AVFilter *src = avfilter_get_by_name("abuffer");
            char args[256];
            snprintf(
                args, sizeof(args), "sample_rate=%d:sample_fmt=%s:channel_layout=mono:channels=%d", SAMPLE_RATE,
                av_get_sample_fmt_name(AudioMixer::SAMPLE_FORMAT),
                CHANNELS);
            int ret = avfilter_graph_create_filter(&in->bufferSrc, src, in->name.c_str(), args, nullptr, _graph);
            printf("create buffer src %s ret %d\n", in->name.c_str(), ret);
        }

        // Create amix
        const AVFilter *mix = avfilter_get_by_name("amix");
        char mixArgs[64];
        snprintf(mixArgs, sizeof(mixArgs), "inputs=%zu:dropout_transition=0:normalize=0", act.size());
        avfilter_graph_create_filter(&_amixCtx, mix, "amix", mixArgs, nullptr, _graph);

        // Link sources -> amix
        for (size_t i = 0; i < act.size(); ++i) {
            avfilter_link(act[i]->bufferSrc, 0, _amixCtx, i);
        }

        // Create sink
        const AVFilter *sink = avfilter_get_by_name("abuffersink");
        avfilter_graph_create_filter(&_sinkCtx, sink, "out", nullptr, nullptr, _graph);

        // Configure sink
        enum AVSampleFormat fmts[] = { SAMPLE_FORMAT, AV_SAMPLE_FMT_NONE };
        av_opt_set_int_list(_sinkCtx, "sample_fmts", fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        int rates[] = { SAMPLE_RATE, -1 };
        av_opt_set_int_list(_sinkCtx, "sample_rates", rates, -1, AV_OPT_SEARCH_CHILDREN);
        int channels[] = { CHANNELS, -1 };
        av_opt_set_int_list(_sinkCtx, "channels", channels, -1, AV_OPT_SEARCH_CHILDREN);

        // Final link & config
        int ret = avfilter_link(_amixCtx, 0, _sinkCtx, 0);
        ret = avfilter_graph_config(_graph, nullptr);
        if (ret < 0) {
            ErrorL << "Could not configure filter graph";
            cleanupFilterGraph();
            return;
        }

        // Allocate FIFO
        _fifo = av_audio_fifo_alloc(SAMPLE_FORMAT, 1, SAMPLE_SIZE * 120);

        _initialized = true;
    }

    void outputThreadFunc() {
        std::unique_lock<std::mutex> lock(_outputMutex);
        while (true) {
            _outputCv.wait(lock, [&] { return !_running || _initialized; });

            checkOutput();
        }
    }

    void checkOutput() {
        std::lock_guard<std::mutex> lock(_graphMutex);
        if (!_initialized || !_sinkCtx || !_fifo)
            return;

        while (true) {
            AVFrame *in = av_frame_alloc();
            if (!in)
                break;
            int ret = av_buffersink_get_frame(_sinkCtx, in);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
                av_frame_free(&in);
                break;
            }

            av_audio_fifo_write(_fifo, (void **)in->extended_data, in->nb_samples);
            int sampleRate          = in->sample_rate;
            auto channelLayout      = in->ch_layout;
            enum AVSampleFormat fmt = (enum AVSampleFormat)in->format;
            av_frame_free(&in);

            while (av_audio_fifo_size(_fifo) >= SAMPLE_SIZE) {
                AVFrame *out    = av_frame_alloc();
                out->nb_samples = SAMPLE_SIZE;
                out->format     = fmt;
                out->ch_layout  = channelLayout;
                out->sample_rate = sampleRate;
                out->ch_layout.nb_channels = CHANNELS;

                if (av_frame_get_buffer(out, 0) < 0) {
                    av_frame_free(&out);
                    break;
                }

                av_audio_fifo_read(_fifo, (void **)out->extended_data, SAMPLE_SIZE);

                int64_t pts_ms = _totalSamples * 1000LL / out->sample_rate;
                _totalSamples += out->nb_samples;
                out->pts = pts_ms;

                auto ptr = std::shared_ptr<AVFrame>(out, [](AVFrame *f) { av_frame_free(&f); });

                if (_cb) {
                    auto pkt = encode_aac(ptr);
                    if (pkt) {
                        auto adts_header = createADTSHeader(AudioMixer::SAMPLE_RATE, AudioMixer::CHANNELS, pkt->size);
                        _cb(pkt, adts_header);
                    }
                }
            }
        }
    }

private:
    // FFmpeg filter graph contexts
    AVFilterGraph *_graph     = nullptr;
    AVFilterContext *_amixCtx = nullptr;
    AVFilterContext *_sinkCtx = nullptr;
    AVAudioFifo *_fifo = nullptr;

    mutable std::mutex _graphMutex; 

    // Input state
    std::map<std::string, std::unique_ptr<AudioInput>> _inputs;
    bool _initialized = false;

    // Output thread
    std::thread _outputThread;
    std::atomic<bool> _running{ false };
    std::condition_variable _outputCv;
    std::mutex _outputMutex;  

    int64_t _totalSamples = 0;
    std::function<void(const std::shared_ptr<AVPacket> &pkt, const std::array<uint8_t, 7> &adtsHeader)> _cb;

    std::shared_ptr<AVCodecContext> _encCtx;
};