#pragma once

#include "Codec/Transcode.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <array>
extern "C" {
#include <libavutil/audio_fifo.h>
}

class AudioMixer;

class AudioChannel {
public:
    using Ptr = std::shared_ptr<AudioChannel>;

    constexpr static int SAMPLE_RATE = 48000;
    constexpr static int SAMPLE_SIZE = 1024;
    constexpr static int CHANNELS = 1;
    constexpr static AVSampleFormat SAMPLE_FORMAT = AV_SAMPLE_FMT_FLTP;

    AudioChannel() {
        fifo.reset(av_audio_fifo_alloc(SAMPLE_FORMAT, CHANNELS, SAMPLE_SIZE * 120), [](AVAudioFifo *ptr) {
            if (ptr) av_audio_fifo_free(ptr);
        });
    }

    void inputFrame(const mediakit::FFmpegFrame::Ptr &frame){
        std::lock_guard<std::mutex> lock(mx_fifo);
        av_audio_fifo_write(fifo.get(), (void **)frame->get()->extended_data, frame->get()->nb_samples);
    }

   mediakit::FFmpegFrame::Ptr readSamples(int samples = SAMPLE_SIZE) {
       std::lock_guard<std::mutex> lock(mx_fifo);
       int available = av_audio_fifo_size(fifo.get());
       int toRead = MIN(samples, available);

       std::shared_ptr<AVFrame> frame(av_frame_alloc(), [](AVFrame *f) {
           if (f) av_frame_free(&f);
       });
       frame->nb_samples = samples;
       frame->format = SAMPLE_FORMAT;
       frame->sample_rate = SAMPLE_RATE;
       AVChannelLayout mono_layout;
       av_channel_layout_from_string(&mono_layout, "mono");
       frame->ch_layout = mono_layout;
       av_channel_layout_uninit(&mono_layout);

       // 分配音频buffer
       int ret = av_frame_get_buffer(frame.get(), 0);
       if (ret < 0) {
           return nullptr;
       }

       int read = 0;
       if (toRead > 0) {
           // 读取实际存在的数据
           read = av_audio_fifo_read(fifo.get(), (void **)frame->extended_data, toRead);
       }
       if (read < 0) {
           // 读取失败
           return nullptr;
       }

       // 如果实际读取到的样本数少于请求的samples，则需要补静音
       if (read < samples) {
           std::cout << "add silence samples: " << samples - read << std::endl;
           int planes = av_sample_fmt_is_planar((AVSampleFormat)frame->format) ? frame->ch_layout.nb_channels : 1;
           int bytes_per_sample = av_get_bytes_per_sample((AVSampleFormat)frame->format);
           for (int ch = 0; ch < planes; ++ch) {
               uint8_t *dst = frame->extended_data[ch] + read * bytes_per_sample;
               int fill_samples = samples - read;
               memset(dst, 0, fill_samples * bytes_per_sample);
           }
       }

       // 一定要设置nb_samples为samples，表示总长度是补齐后的
       frame->nb_samples = samples;

       return std::make_shared<mediakit::FFmpegFrame>(frame);
   }


private:
    std::mutex mx_fifo;
    std::shared_ptr<AVAudioFifo> fifo;
};

class AudioMixer {
public:
    using Ptr = std::shared_ptr<AudioMixer>;

   AudioMixer() = default;

    ~AudioMixer() {
        _isExit = true;
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    void setAudioChannels(const std::vector<std::weak_ptr<AudioChannel>> &channels) {
        std::lock_guard<std::mutex> lock(_mx);
        _channels = channels;
    }

    void setOnOutputFrame(const std::function<void(const std::shared_ptr<AVPacket> &, const std::array<uint8_t, 7>& )> &cb) { _cb = cb; }

    void stop() {
        _isExit = true;
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    void start() {
        _isExit = false;
        _thread = std::thread([&]() {
            using namespace std::chrono;
            auto frame_interval = microseconds(1000000 * AudioChannel::SAMPLE_SIZE / AudioChannel::SAMPLE_RATE); // 如 21,333us
            auto next_time = steady_clock::now() + frame_interval * 3;

            while (!_isExit) {
                next_time += frame_interval;
                std::this_thread::sleep_until(next_time);
                if (_isExit) {
                    break;
                }

                std::vector<mediakit::FFmpegFrame::Ptr> frames;
                {
                    std::lock_guard<std::mutex> lock(_mx);
                   for (auto &weak_chn : _channels) {
                       if (auto chn = weak_chn.lock()) {
                           if (auto frame = chn->readSamples(AudioChannel::SAMPLE_SIZE)) {
                               frames.push_back(frame);
                           } 
                       }
                   }
                }

                std::shared_ptr<AVFrame> mix_frame(av_frame_alloc(), [](AVFrame *f) {
                    if (f) av_frame_free(&f);
                });

                mix_frame->nb_samples = AudioChannel::SAMPLE_SIZE;
                mix_frame->format = AudioChannel::SAMPLE_FORMAT;
                mix_frame->sample_rate = AudioChannel::SAMPLE_RATE;
                AVChannelLayout mono_layout;
                av_channel_layout_from_string(&mono_layout, "mono");
                mix_frame->ch_layout = mono_layout;
                mix_frame->ch_layout.nb_channels = AudioChannel::CHANNELS;
                av_channel_layout_uninit(&mono_layout);

                if (av_frame_get_buffer(mix_frame.get(), 0) < 0) {
                    // buffer分配失败
                    continue;
                }

                // 混音叠加
                float* mix_data = reinterpret_cast<float*>(mix_frame->extended_data[0]);
                auto bytes_per_sample = av_get_bytes_per_sample(AudioChannel::SAMPLE_FORMAT);
                memset(mix_data, 0, bytes_per_sample * AudioChannel::SAMPLE_SIZE * AudioChannel::CHANNELS);

                
                for (const auto &frame : frames) {
                    AVFrame *f = frame->get();
                    float *data = reinterpret_cast<float *>(f->extended_data[0]);

                    // 归一化音量
                    normalize_volume_float(data, f->nb_samples, 0.5f);

                    for (int i = 0; i < AudioChannel::SAMPLE_SIZE; ++i) {
                        float sample = mix_data[i] + data[i];
                        // 饱和限制到 [-1.0, 1.0]
                        if (sample > 1.0f)
                            sample = 1.0f;
                        if (sample < -1.0f)
                            sample = -1.0f;
                        mix_data[i] = sample;
                    }
                }

                // auto out_frame = std::make_shared<mediakit::FFmpegFrame>(mix_frame);

                int64_t pts_ms = _total_samples * 1000LL / mix_frame->sample_rate; //{1, 1000} timebase
                _total_samples += mix_frame->nb_samples;
                mix_frame->pts = pts_ms;

                if (_cb) {
                    auto pkt = encode_aac(mix_frame);
                    if (pkt) {
                        auto adts_header = createADTSHeader(AudioChannel::SAMPLE_RATE, AudioChannel::CHANNELS, pkt->size);
                        _cb(pkt, adts_header);
                    }
                }
            }
        });
    }


protected:
    void init_codec_context() {
        auto codec = const_cast<AVCodec *>(avcodec_find_encoder(AV_CODEC_ID_AAC));
        if (!codec) {
            WarnL << "FFmpeg AAC encoder not found";
            return;
        }
        _enc_ctx.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ptr) {
            if (ptr) {
                avcodec_free_context(&ptr);
            }
        });

        if (!_enc_ctx) {
            WarnL << "Could not alloc codec context";
            return;
        }

        _enc_ctx->sample_fmt = AudioChannel::SAMPLE_FORMAT;
        _enc_ctx->sample_rate = AudioChannel::SAMPLE_RATE;
        AVChannelLayout mono_layout;
        av_channel_layout_from_string(&mono_layout, "mono");
        _enc_ctx->ch_layout = mono_layout;
        av_channel_layout_uninit(&mono_layout);
        _enc_ctx->ch_layout.nb_channels = AudioChannel::CHANNELS;
        _enc_ctx->bit_rate = 32000; // 可以自行设置
        _enc_ctx->profile = FF_PROFILE_AAC_LOW;
        _enc_ctx->time_base = { 1, 1000 };

        if (avcodec_open2(_enc_ctx.get(), codec, nullptr) < 0) {
            WarnL << "Could not open encoder";
            return;
        }
    }


    void normalize_volume(int16_t *data, int nb_samples, int target_rms = 5000) {
        if (nb_samples <= 0)
            return;
        double rms = 0;
        for (int i = 0; i < nb_samples; ++i) {
            rms += data[i] * data[i];
        }
        rms = sqrt(rms / nb_samples);
        if (rms < 1e-6)
            return; // 避免除以0
        double gain = target_rms / rms;
        // 可根据实际场景限制最大增益，避免底噪被拉高
        if (gain > 10.0)
            gain = 10.0;
        for (int i = 0; i < nb_samples; ++i) {
            int sample = static_cast<int>(data[i] * gain);
            // 饱和保护
            if (sample > 32767)
                sample = 32767;
            if (sample < -32768)
                sample = -32768;
            data[i] = static_cast<int16_t>(sample);
        }
    }

    void normalize_volume_float(float *data, int nb_samples, float target_rms = 0.5f) {
        if (nb_samples <= 0)
            return;
        double rms = 0;
        for (int i = 0; i < nb_samples; ++i) {
            rms += data[i] * data[i];
        }
        rms = sqrt(rms / nb_samples);
        if (rms < 1e-8)
            return; // 避免除以0
        float gain = target_rms / rms;
        // 限制最大增益，防止底噪提升过大
        if (gain > 10.0f)
            gain = 10.0f;
        for (int i = 0; i < nb_samples; ++i) {
            float sample = data[i] * gain;
            // 饱和保护，float类型标准音频范围通常是 -1.0 ~ 1.0
            if (sample > 1.0f)
                sample = 1.0f;
            if (sample < -1.0f)
                sample = -1.0f;
            data[i] = sample;
        }
    }

    std::shared_ptr<AVPacket> encode_aac(const std::shared_ptr<AVFrame>& frame) {
        if (!_enc_ctx) {
            init_codec_context();
        }
        if (_enc_ctx) {
            int ret = avcodec_send_frame(_enc_ctx.get(), frame.get());
            if (ret < 0) {
                return nullptr;
            }

            std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *p) {
                if (p)
                    av_packet_free(&p);
            });
            ret = avcodec_receive_packet(_enc_ctx.get(), pkt.get());
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

        std::array<uint8_t, 7> adtsHeader {};

        // AAC LC profile (Low Complexity profile)
        int profile = 1; // AAC LC

        int sampleRateIndex = getSampleRateIndex(sampleRate);

        int fullFrameLength = frameLength + 7;

        adtsHeader[0] = 0xFF;
        adtsHeader[1] = 0xF1; // MPEG-4, NO CRC

        adtsHeader[2] = (uint8_t)(((profile - 1) << 6) + (sampleRateIndex << 2) + (channels >> 2));
        adtsHeader[3] = (uint8_t)(((channels & 3) << 6) + (fullFrameLength >> 11));
        adtsHeader[4] = (uint8_t)((fullFrameLength & 0x7FF) >> 3);
        adtsHeader[5] = (uint8_t)(((fullFrameLength & 7) << 5) + 0x1F);
        adtsHeader[6] = 0xFC;

        return adtsHeader;
    }

private:
    std::mutex _mx;
    std::vector<std::weak_ptr<AudioChannel>> _channels;

    std::function<void(const std::shared_ptr<AVPacket>& pkt,const std::array<uint8_t, 7>& adtsHeader)> _cb;

    int64_t _total_samples = 0;

    bool _isExit = false;
    std::thread _thread;

    std::shared_ptr<AVCodecContext> _enc_ctx;
};