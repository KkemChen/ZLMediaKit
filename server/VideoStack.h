#pragma once
#if defined(ENABLE_VIDEOSTACK) && defined(ENABLE_X264) && defined(ENABLE_FFMPEG)
#include "Codec/Transcode.h"
#include "Common/Device.h"
#include "Player/MediaPlayer.h"
#include "json/json.h"
#include <mutex>
#include "AudioMixer.h"

template<typename T> class RefWrapper {
public:
    using Ptr = std::shared_ptr<RefWrapper<T>>;

    template<typename... Args>
    explicit RefWrapper(Args&&... args) : _rc(0), _entity(std::forward<Args>(args)...) {}

    T acquire() {
        ++_rc;
        return _entity;
    }

    bool dispose() { return --_rc <= 0; }

    T weak() { return _entity;}

private:
    std::atomic<int> _rc;
    T _entity;
};

class VideoChannel;

struct Param {
    using Ptr = std::shared_ptr<Param>;

    int posX = 0;
    int posY = 0;
    int width = 0;
    int height = 0;
    AVPixelFormat pixfmt = AV_PIX_FMT_YUV420P;
    std::string id{};

    // runtime
    std::weak_ptr<VideoChannel> weak_chn;
    std::weak_ptr<mediakit::FFmpegFrame> weak_buf;

    ~Param();
};

using Params = std::shared_ptr<std::vector<Param::Ptr>>;

class VideoChannel : public std::enable_shared_from_this<VideoChannel> {
public:
    using Ptr = std::shared_ptr<VideoChannel>;

    VideoChannel(const std::string& id, int width, int height, AVPixelFormat pixfmt);

    void addParam(const std::weak_ptr<Param>& p);

    void onFrame(const mediakit::FFmpegFrame::Ptr& frame);

    void fillBuffer(const Param::Ptr& p);

protected:
    void forEachParam(const std::function<void(const Param::Ptr&)>& func);

    void copyData(const mediakit::FFmpegFrame::Ptr& buf, const Param::Ptr& p);

    void resizeFrame(const mediakit::FFmpegFrame::Ptr &frame);

    void resizeFrameImplWithAspectRatio(const mediakit::FFmpegFrame::Ptr &frame);

    void resizeFrameImplWithoutAspectRatio(const mediakit::FFmpegFrame::Ptr &frame);

private:
    std::string _id;
    int _width;
    int _height;
    AVPixelFormat _pixfmt;

    int _lastWidht;
    int _lastHeight;
    bool _keepAspectRatio;
    int _offsetX;
    int _offsetY;

    mediakit::FFmpegFrame::Ptr _tmp;

    std::recursive_mutex _mx;
    std::vector<std::weak_ptr<Param>> _params;

    mediakit::FFmpegSws::Ptr _sws;
    toolkit::EventPoller::Ptr _poller;
};

class StackPlayer : public std::enable_shared_from_this<StackPlayer> {
public:
    using Ptr = std::shared_ptr<StackPlayer>;
    
    StackPlayer(const std::string& url) : _url(url) {}

    void addVideoChannel(const std::weak_ptr<VideoChannel>& chn);

    void addAudioChannel(const std::weak_ptr<AudioChannel> &chn);

    void play();

    void onVideoFrame(const mediakit::FFmpegFrame::Ptr& frame);

     void onAudioFrame(const mediakit::FFmpegFrame::Ptr &frame);

    void onDisconnect();

protected:
    void rePlay(const std::string& url);

private:
    std::string _url;
    mediakit::MediaPlayer::Ptr _player;

    // 用于断线重连  [AUTO-TRANSLATED:18fd242a]
    // Used for disconnection and reconnection
    toolkit::Timer::Ptr _timer;
    int _failedCount = 0;

    std::recursive_mutex _mx_video_chn;
    std::vector<std::weak_ptr<VideoChannel>> _video_channels;

    std::recursive_mutex _mx_audio_chn;
    std::vector<std::weak_ptr<AudioChannel>> _audio_channels;
};

class VideoStack {
public:
    using Ptr = std::shared_ptr<VideoStack>;

    VideoStack(const std::string& url, int width = 1920, int height = 1080,
               AVPixelFormat pixfmt = AV_PIX_FMT_YUV420P, float fps = 25.0,
               int bitRate = 2 * 1024 * 1024);

    ~VideoStack();

    void setParam(const Params& params);
    void setMixer(const std::map<std::string, std::weak_ptr<StackPlayer>> &players) {
        std::vector<std::string> ids;
        for (auto& it : players) {
            ids.push_back(it.first);
        }
        auto tmp = _mixer->getAudioChannel(ids);
        for (auto &it : tmp) {
            auto player_it = players.find(it.first);
            if (player_it != players.end()) {
                if (auto player = player_it->second.lock()) {
                    player->addAudioChannel(it.second);
                }
            }
        }
    }
     
    void run();

protected:
    void start();

    void initBgColor();

public:
    Params _params;

    mediakit::FFmpegFrame::Ptr _buffer;

private:
    std::string _id;
    int _width;
    int _height;
    AVPixelFormat _pixfmt;
    float _fps;
    int _bitRate;

    AudioMixer::Ptr _mixer;
    mediakit::DevChannel::Ptr _dev;

    bool _isExit;

    std::thread _thread;
};

class VideoStackManager {
public:
    // 创建拼接流  [AUTO-TRANSLATED:ebb3a8ec]
    // Create a concatenated stream
    int startVideoStack(const Json::Value& json);

    // 停止拼接流  [AUTO-TRANSLATED:a46f341f]
    // Stop the concatenated stream
    int stopVideoStack(const std::string& id);

    // 可以在不断流的情况下，修改拼接流的配置(实现切换拼接屏内容)  [AUTO-TRANSLATED:f9b59b6b]
    // You can modify the configuration of the concatenated stream (to switch the content of the concatenated screen) without stopping the stream
    int resetVideoStack(const Json::Value& json);

public:
    static VideoStackManager& Instance();

    VideoChannel::Ptr getChannel(const std::string& id, int width, int height, AVPixelFormat pixfmt);

    void unrefChannel(const std::string& id, int width, int height, AVPixelFormat pixfmt);

    bool loadBgImg(const std::string& path);

    void clear();

    mediakit::FFmpegFrame::Ptr getBgImg();

protected:
    Params parseParams(const Json::Value& json, std::string& id, int& width, int& height);

protected:
    VideoChannel::Ptr createChannel(const std::string& id, int width, int height, AVPixelFormat pixfmt);

    StackPlayer::Ptr createPlayer(const std::string& id);

private:
    mediakit::FFmpegFrame::Ptr _bgImg;

private:
    std::recursive_mutex _mx;

    std::unordered_map<std::string, VideoStack::Ptr> _stackMap;

    std::unordered_map<std::string, RefWrapper<VideoChannel::Ptr>::Ptr> _channelMap;
    
    std::unordered_map<std::string, RefWrapper<StackPlayer::Ptr>::Ptr> _playerMap;
};
#endif