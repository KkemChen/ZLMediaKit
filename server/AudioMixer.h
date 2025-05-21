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

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
}

// Optimized Dynamic FFmpeg Audio Mixer with proper synchronization to prevent segfaults.
// Now contains an internal AVAudioFifo so the callback always receives frames
// with exactly 1024 samples (nb_samples == 1024).
class AudioMixer {
public:
    using Ptr = std::shared_ptr<AudioMixer>;
    constexpr static int SAMPLE_RATE = 48000;
    constexpr static int SAMPLE_SIZE = 1024;
    constexpr static int CHANNELS = 1;
    constexpr static int BIT_PER_SAMPLE = 16;
    constexpr static AVSampleFormat SAMPLE_FORMAT = AV_SAMPLE_FMT_S16;

    struct AudioInput {
        AVFilterContext *bufferSrc = nullptr;
        std::string name;
        AVRational timeBase;
        std::queue<mediakit::FFmpegFrame::Ptr> frameQueue;
        std::mutex queueMutex;
        bool active = true;
    };

    AudioMixer() {
        // avfilter_register_all();
        av_log_set_level(AV_LOG_DEBUG);
        running_ = true;
        outputThread_ = std::thread(&AudioMixer::outputThreadFunc, this);
    }

    ~AudioMixer() {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            running_ = false;
        }
        outputCv_.notify_one();
        if (outputThread_.joinable())
            outputThread_.join();
        {
            std::lock_guard<std::mutex> fLock(filterMutex_);
            cleanupFilterGraphUnlocked();
        }
    }

    bool addAudioInput(const std::string &id, AVRational timeBase) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (inputs_.count(id))
            return false;
        auto input = std::make_unique<AudioInput>();
        input->name = "in" + id;
        input->timeBase = timeBase;
        inputs_.emplace(id, std::move(input));
        needRebuild_ = true;
        return true;
    }

    bool removeAudioInput(const std::string &id) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = inputs_.find(id);
        if (it == inputs_.end())
            return false;
        it->second->active = false;
        {
            std::lock_guard<std::mutex> ql(it->second->queueMutex);
            std::queue<mediakit::FFmpegFrame::Ptr> empty;
            std::swap(it->second->frameQueue, empty);
        }
        inputs_.erase(it);
        needRebuild_ = true;
        return true;
    }

    void inputFrame(const std::string &id, mediakit::FFmpegFrame::Ptr frame) {
        AudioInput *input = nullptr;
        bool rebuild = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            auto it = inputs_.find(id);
            if (it == inputs_.end() || !it->second->active)
                return;
            input = it->second.get();
            rebuild = needRebuild_;
        }

        if (rebuild) {
            {
                std::lock_guard<std::mutex> ql(input->queueMutex);
                input->frameQueue.push(frame);
            }
            rebuildFilterGraph();
            processQueuedFrames();
        } else {
            lockAndInput(id, frame);
        }
        outputCv_.notify_one();
    }

    void setOutputCallback(std::function<void(mediakit::FFmpegFrame::Ptr)> cb) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        outputCallback_ = std::move(cb);
    }

    void flush() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!initialized_)
            return;
        std::lock_guard<std::mutex> fLock(filterMutex_);
        for (auto &kv : inputs_) {
            if (kv.second->bufferSrc) {
                av_buffersrc_add_frame_flags(kv.second->bufferSrc, nullptr, 0);
            }
        }
        outputCv_.notify_one();
    }

private:
    // FFmpeg filter graph contexts
    AVFilterGraph *graph_ = nullptr;
    AVFilterContext *amixCtx_ = nullptr;
    AVFilterContext *sinkCtx_ = nullptr;

    // Internal FIFO (48000 Hz, S16P, mono); created when graph is configured
    AVAudioFifo *fifo_ = nullptr;

    // Synchronization
    std::mutex stateMutex_; // protects inputs_, initialized_, needRebuild_, running_
    std::mutex filterMutex_; // protects graph_, amixCtx_, sinkCtx_, fifo_

    // Input state
    std::map<std::string, std::unique_ptr<AudioInput>> inputs_;
    bool initialized_ = false;
    bool needRebuild_ = false;

    // Output thread
    std::thread outputThread_;
    std::atomic<bool> running_ { false };
    std::condition_variable outputCv_;
    std::mutex outputMutex_;

    // Callback
    std::function<void(mediakit::FFmpegFrame::Ptr)> outputCallback_;
    std::mutex callbackMutex_;

    ////////////////////////////// INTERNAL HELPERS ////////////////////////////
    void cleanupFilterGraphUnlocked() {
        if (graph_) {
            avfilter_graph_free(&graph_);
            graph_ = nullptr;
            amixCtx_ = nullptr;
            sinkCtx_ = nullptr;
            initialized_ = false;
        }
        if (fifo_) {
            av_audio_fifo_free(fifo_);
            fifo_ = nullptr;
        }
    }

    void rebuildFilterGraph() {
        // Lock graph
        std::lock_guard<std::mutex> fLock(filterMutex_);

        // Gather active inputs snapshot
        std::vector<AudioInput *> act;
        {
            std::lock_guard<std::mutex> sLock(stateMutex_);
            for (auto &kv : inputs_) {
                if (kv.second->active)
                    act.push_back(kv.second.get());
            }
            if (act.empty()) {
                cleanupFilterGraphUnlocked();
                needRebuild_ = false;
                return;
            }
        }

        // Rebuild
        cleanupFilterGraphUnlocked();
        graph_ = avfilter_graph_alloc();
        if (!graph_)
            return;

        // Create sources
        for (auto *in : act) {
            const AVFilter *src = avfilter_get_by_name("abuffer");
            char args[256];
            snprintf(
                args, sizeof(args), "sample_rate=%d:sample_fmt=%s:channel_layout=mono:channels=%d", SAMPLE_RATE, av_get_sample_fmt_name(AV_SAMPLE_FMT_S16),
                CHANNELS);
            int ret = avfilter_graph_create_filter(&in->bufferSrc, src, in->name.c_str(), args, nullptr, graph_);
            printf("create buffer src %s ret %d\n", in->name.c_str(), ret);
        }

        // Create amix
        const AVFilter *mix = avfilter_get_by_name("amix");
        char mixArgs[64];
        snprintf(mixArgs, sizeof(mixArgs), "inputs=%zu:dropout_transition=0:normalize=0", act.size());
        avfilter_graph_create_filter(&amixCtx_, mix, "amix", mixArgs, nullptr, graph_);

        // Link sources -> amix
        for (size_t i = 0; i < act.size(); ++i) {
            avfilter_link(act[i]->bufferSrc, 0, amixCtx_, i);
        }

        // Create sink
        const AVFilter *sink = avfilter_get_by_name("abuffersink");
        avfilter_graph_create_filter(&sinkCtx_, sink, "out", nullptr, nullptr, graph_);

        // Configure sink – omit channel_layouts, set explicit fmt/rate/channels
        enum AVSampleFormat fmts[] = { SAMPLE_FORMAT, AV_SAMPLE_FMT_NONE };
        av_opt_set_int_list(sinkCtx_, "sample_fmts", fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        int rates[] = { SAMPLE_RATE, -1 };
        av_opt_set_int_list(sinkCtx_, "sample_rates", rates, -1, AV_OPT_SEARCH_CHILDREN);
        int channels[] = { CHANNELS, -1 };
        av_opt_set_int_list(sinkCtx_, "channels", channels, -1, AV_OPT_SEARCH_CHILDREN);

        // Final link & config
        int ret = avfilter_link(amixCtx_, 0, sinkCtx_, 0);
        printf("link amix -> out ret %d\n", ret);

        ret = avfilter_graph_config(graph_, nullptr);
        printf("config ret %d\n", ret);
        if (ret < 0) {
            cleanupFilterGraphUnlocked();
            return;
        }

        // Allocate FIFO (capacity for ~ 100 frames worth of samples)
        fifo_ = av_audio_fifo_alloc(SAMPLE_FORMAT, 1, SAMPLE_SIZE * 120);

        // Update state
        {
            std::lock_guard<std::mutex> sLock(stateMutex_);
            initialized_ = true;
            needRebuild_ = false;
        }
    }

    void processQueuedFrames() {
        // Collect queued frames
        std::vector<std::pair<std::string, mediakit::FFmpegFrame::Ptr>> toProcess;
        {
            std::lock_guard<std::mutex> sLock(stateMutex_);
            if (!initialized_)
                return;
            for (auto &kv : inputs_) {
                auto *in = kv.second.get();
                std::lock_guard<std::mutex> ql(in->queueMutex);
                while (!in->frameQueue.empty()) {
                    toProcess.emplace_back(kv.first, in->frameQueue.front());
                    in->frameQueue.pop();
                }
            }
        }
        for (auto &p : toProcess) {
            lockAndInput(p.first, p.second);
        }
    }

    void lockAndInput(const std::string &id, mediakit::FFmpegFrame::Ptr frame) {
        std::lock_guard<std::mutex> fLock(filterMutex_);
        if (!initialized_)
            return;
        // Find bufferSrc under state lock
        AVFilterContext *ctx = nullptr;
        {
            std::lock_guard<std::mutex> sLock(stateMutex_);
            auto it = inputs_.find(id);
            if (it == inputs_.end())
                return;
            ctx = it->second->bufferSrc;
        }
        if (!ctx)
            return;
        AVFrame *avf = frame->get();
        if (!avf)
            return;
        av_buffersrc_add_frame_flags(ctx, avf, AV_BUFFERSRC_FLAG_KEEP_REF);
    }

    void outputThreadFunc() {
        std::unique_lock<std::mutex> lock(outputMutex_);
        while (true) {
            outputCv_.wait(lock, [&] { return !running_ || (initialized_); });
            if (!running_)
                break;
            checkOutput();
        }
    }

    // Pulls frames from sinkCtx_, writes them into fifo_, and emits 1024-sample frames.
    void checkOutput() {
        std::lock_guard<std::mutex> fLock(filterMutex_);
        if (!initialized_ || !sinkCtx_ || !fifo_)
            return;

        while (true) {
            // 1. Read from sink
            AVFrame *in = av_frame_alloc();
            if (!in)
                break;
            int ret = av_buffersink_get_frame(sinkCtx_, in);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) {
                av_frame_free(&in);
                break;
            }

            // 2. Push samples into FIFO
            av_audio_fifo_write(fifo_, (void **)in->extended_data, in->nb_samples);
            int sampleRate = in->sample_rate;
            auto channelLayout = in->ch_layout;
            enum AVSampleFormat fmt = (enum AVSampleFormat)in->format;
            av_frame_free(&in);

            // 3. While enough samples, pop 1024 and send out
            while (av_audio_fifo_size(fifo_) >= SAMPLE_SIZE) {
                AVFrame *out = av_frame_alloc();
                out->nb_samples = SAMPLE_SIZE;
                out->format = fmt;
                out->ch_layout = channelLayout;
                // out->channel_layout = channelLayout;
                out->sample_rate = sampleRate;
                // For mono we can derive channels from layout or set explicitly
                out->ch_layout.nb_channels = CHANNELS;

                if (av_frame_get_buffer(out, 0) < 0) {
                    av_frame_free(&out);
                    break; // allocation failed – drop
                }

                av_audio_fifo_read(fifo_, (void **)out->extended_data, SAMPLE_SIZE);

                auto ptr = std::shared_ptr<AVFrame>(out, [](AVFrame *f) { av_frame_free(&f); });
                auto ffmFrame = std::make_shared<mediakit::FFmpegFrame>(ptr);
                std::lock_guard<std::mutex> cb(callbackMutex_);
                if (outputCallback_)
                    outputCallback_(ffmFrame);
            }
        }
    }
};
