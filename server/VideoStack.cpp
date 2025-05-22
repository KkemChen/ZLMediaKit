#if defined(ENABLE_VIDEOSTACK) && defined(ENABLE_X264) && defined(ENABLE_FFMPEG)
#include "VideoStack.h"
#include "Codec/Transcode.h"
#include "Common/Device.h"
#include "Util/logger.h"
#include "Util/util.h"
#include "json/value.h"
#include <Thread/WorkThreadPool.h>
#include <fstream>
#include <libavutil/pixfmt.h>
#include <memory>
#include <mutex>

// ITU-R BT.601
// #define  RGB_TO_Y(R, G, B) ((( 66 * (R) + 129 * (G) +  25 * (B)+128) >> 8)+16)
// #define  RGB_TO_U(R, G, B) (((-38 * (R) -  74 * (G) + 112 * (B)+128) >> 8)+128)
// #define  RGB_TO_V(R, G, B) (((112 * (R) -  94 * (G) -  18 * (B)+128) >> 8)+128)

// ITU-R BT.709
#define RGB_TO_Y(R, G, B) (((47 * (R) + 157 * (G) + 16 * (B) + 128) >> 8) + 16)
#define RGB_TO_U(R, G, B) (((-26 * (R) - 87 * (G) + 112 * (B) + 128) >> 8) + 128)
#define RGB_TO_V(R, G, B) (((112 * (R) - 102 * (G) - 10 * (B) + 128) >> 8) + 128)

static void fill_yuv_func(const mediakit::FFmpegFrame::Ptr &frame, int y, int u, int v) {
    const auto& yuv = frame->get();
    memset(yuv->data[0], y, yuv->linesize[0] * yuv->height);
    memset(yuv->data[1], u, yuv->linesize[1] * ((yuv->height + 1) / 2));
    memset(yuv->data[2], v, yuv->linesize[2] * ((yuv->height + 1) / 2));
}


INSTANCE_IMP(VideoStackManager)

Param::~Param() {
    auto strongChn= weak_chn.lock();
    if (!strongChn) { return; }
    VideoStackManager::Instance().unrefChannel(id, width, height, pixfmt); 
}

VideoChannel::VideoChannel(const std::string& id, int width, int height, AVPixelFormat pixfmt)
    : _id(id), _width(width), _height(height), _pixfmt(pixfmt) {
#if defined(VIDEOSTACK_KEEP_ASPECT_RATIO)
    _keepAspectRatio = true;
#else
    _keepAspectRatio = false;
#endif
    _lastWidht = 0;
    _lastHeight = 0;
    _tmp = std::make_shared<mediakit::FFmpegFrame>();

    _tmp->get()->width = _width;
    _tmp->get()->height = _height;
    _tmp->get()->format = _pixfmt;

    av_frame_get_buffer(_tmp->get(), 32);

    auto frame = VideoStackManager::Instance().getBgImg();

    resizeFrame(frame);
}

void VideoChannel::addParam(const std::weak_ptr<Param>& p) {
    std::lock_guard<std::recursive_mutex> lock(_mx);
    _params.push_back(p);
}

void VideoChannel::onFrame(const mediakit::FFmpegFrame::Ptr& frame) {
    std::weak_ptr<VideoChannel> weakSelf = shared_from_this();
    _poller = _poller ? _poller : toolkit::WorkThreadPool::Instance().getPoller();
    _poller->async([weakSelf, frame]() {
        auto self = weakSelf.lock();
        if (!self) { return; }
        self->resizeFrame(frame);
        self->forEachParam([self](const Param::Ptr& p) { self->fillBuffer(p); });
    });
}

void VideoChannel::forEachParam(const std::function<void(const Param::Ptr&)>& func) {
    for (auto& wp : _params) {
        if (auto sp = wp.lock()) { func(sp); }
    }
}

void VideoChannel::fillBuffer(const Param::Ptr& p) {
    if (auto buf = p->weak_buf.lock()) { copyData(buf, p); }
}

void VideoChannel::copyData(const mediakit::FFmpegFrame::Ptr& buf, const Param::Ptr& p) {

    switch (p->pixfmt) {
        case AV_PIX_FMT_YUV420P: {
            for (int i = 0; i < p->height; i++) {
                memcpy(buf->get()->data[0] + buf->get()->linesize[0] * (i + p->posY) + p->posX,
                       _tmp->get()->data[0] + _tmp->get()->linesize[0] * i, _tmp->get()->width);
            }
            // 确保height为奇数时，也能正确的复制到最后一行uv数据  [AUTO-TRANSLATED:69895ea5]
            // Ensure that the uv data can be copied to the last line correctly when height is odd
            for (int i = 0; i < (p->height + 1) / 2; i++) {
                // U平面  [AUTO-TRANSLATED:8b73dc2d]
                // U plane
                memcpy(buf->get()->data[1] + buf->get()->linesize[1] * (i + p->posY / 2) +
                           p->posX / 2,
                       _tmp->get()->data[1] + _tmp->get()->linesize[1] * i, _tmp->get()->width / 2);

                // V平面  [AUTO-TRANSLATED:8fa72cc7]
                // V plane
                memcpy(buf->get()->data[2] + buf->get()->linesize[2] * (i + p->posY / 2) +
                           p->posX / 2,
                       _tmp->get()->data[2] + _tmp->get()->linesize[2] * i, _tmp->get()->width / 2);
            }
            break;
        }
        case AV_PIX_FMT_NV12: {
            // TODO: 待实现  [AUTO-TRANSLATED:247ec1df]
            // TODO: To be implemented
            break;
        }

        default: WarnL << "No support pixformat: " << av_get_pix_fmt_name(p->pixfmt); break;
    }
}

void VideoChannel::resizeFrame(const mediakit::FFmpegFrame::Ptr &frame) {
    if (_keepAspectRatio) {
        resizeFrameImplWithAspectRatio(frame);
    } else {
        resizeFrameImplWithoutAspectRatio(frame);
    }
}

void VideoChannel::resizeFrameImplWithAspectRatio(const mediakit::FFmpegFrame::Ptr &frame) {
    int srcWidth = frame->get()->width;
    int srcHeight = frame->get()->height;
    if (srcWidth <= 0 || srcHeight <= 0) {
        return;
    }

    // 当新frame宽高变化时，重新初始化sws
    if (srcWidth != _lastWidht || srcHeight != _lastHeight) {
        _lastWidht = srcWidth;
        _lastHeight = srcHeight;
        fill_yuv_func(_tmp, 16, 128, 128);

        int dstWidth = _width;
        int dstHeight = _height;

        float srcAspectRatio = static_cast<float>(srcWidth) / srcHeight;
        float dstAspectRatio = static_cast<float>(dstWidth) / dstHeight;

        int scaledWidth, scaledHeight;
        if (srcAspectRatio > dstAspectRatio) {
            scaledWidth = dstWidth;
            scaledHeight = static_cast<int>(dstWidth / srcAspectRatio);
        } else {
            scaledHeight = dstHeight;
            scaledWidth = static_cast<int>(dstHeight * srcAspectRatio);
        }

        _offsetX = (dstWidth - scaledWidth) / 2;
        _offsetY = (dstHeight - scaledHeight) / 2;
        _sws = std::make_shared<mediakit::FFmpegSws>(_pixfmt, scaledWidth, scaledHeight);
    }

    auto scaledFrame = _sws->inputFrame(frame);

    int copyWidth = ((_width) < (scaledFrame->get()->width) ? (_width) : (scaledFrame->get()->width));
    int copyHeight = ((_height) < (scaledFrame->get()->height) ? (_height) : (scaledFrame->get()->height));

    for (int i = 0; i < copyHeight; i++) {
        memcpy(
            _tmp->get()->data[0] + (i + _offsetY) * _tmp->get()->linesize[0] + _offsetX, scaledFrame->get()->data[0] + i * scaledFrame->get()->linesize[0],
            copyWidth);
    }

    for (int i = 0; i < (copyHeight + 1) / 2; i++) {
        memcpy(
            _tmp->get()->data[1] + (i + _offsetY / 2) * _tmp->get()->linesize[1] + _offsetX / 2,
            scaledFrame->get()->data[1] + i * scaledFrame->get()->linesize[1], copyWidth / 2);
        memcpy(
            _tmp->get()->data[2] + (i + _offsetY / 2) * _tmp->get()->linesize[2] + _offsetX / 2,
            scaledFrame->get()->data[2] + i * scaledFrame->get()->linesize[2], copyWidth / 2);
    }

}

void VideoChannel::resizeFrameImplWithoutAspectRatio(const mediakit::FFmpegFrame::Ptr &frame) {
    if (!_sws) {
        fill_yuv_func(_tmp, 16, 128, 128);
        _sws = std::make_shared<mediakit::FFmpegSws>(_pixfmt, _width, _height);
    }
    _tmp = _sws->inputFrame(frame);
}

void StackPlayer::addVideoChannel(const std::weak_ptr<VideoChannel>& chn) {
    std::lock_guard<std::recursive_mutex> lock(_mx_video_chn);
    _video_channels.push_back(chn);
}

void StackPlayer::addAudioChannel(const std::weak_ptr<AudioChannel> &chn) {
    std::lock_guard<std::recursive_mutex> lock(_mx_audio_chn);
    _audio_channels.push_back(chn);
}

void StackPlayer::play() {

    auto url = _url;
    // 创建拉流 解码对象  [AUTO-TRANSLATED:9267c5dc]
    // Create a pull stream decoding object
    _player = std::make_shared<mediakit::MediaPlayer>();
    std::weak_ptr<mediakit::MediaPlayer> weakPlayer = _player;

    std::weak_ptr<StackPlayer> weakSelf = shared_from_this();

    (*_player)[mediakit::Client::kWaitTrackReady] = false;
    (*_player)[mediakit::Client::kRtpType] = mediakit::Rtsp::RTP_TCP;

    _player->setOnPlayResult([weakPlayer, weakSelf, url](const toolkit::SockException& ex) mutable {
        TraceL << "StackPlayer: " << url << " OnPlayResult: " << ex.what();
        auto strongPlayer = weakPlayer.lock();
        if (!strongPlayer) { return; }
        auto self = weakSelf.lock();
        if (!self) { return; }

        if (!ex) {
            // 取消定时器  [AUTO-TRANSLATED:41ff7c9a]
            // Cancel the timer
            self->_timer.reset();
            self->_failedCount = 0;

        } else {
            self->onDisconnect();
            self->rePlay(url);
        }

        auto videoTrack = std::dynamic_pointer_cast<mediakit::VideoTrack>(
            strongPlayer->getTrack(mediakit::TrackVideo, false));
        auto audioTrack = std::dynamic_pointer_cast<mediakit::AudioTrack>(
            strongPlayer->getTrack(mediakit::TrackAudio, false));

        if (videoTrack) {
            // 如果每次不同 可以加个时间戳 time(NULL);
            // TODO:添加使用显卡还是cpu解码的判断逻辑  [AUTO-TRANSLATED:44bef37a]
            // TODO: Add logic to determine whether to use GPU or CPU decoding
            auto decoder = std::make_shared<mediakit::FFmpegDecoder>(videoTrack, 0, std::vector<std::string> { "h264", "hevc" });

            decoder->setOnDecode([weakSelf](const mediakit::FFmpegFrame::Ptr& frame) mutable {
                auto self = weakSelf.lock();
                if (!self) { return; }

                self->onVideoFrame(frame);
            });

            videoTrack->addDelegate([decoder](const mediakit::Frame::Ptr& frame) {
                return decoder->inputFrame(frame, false, true);
            });
        }

        if (audioTrack) {
            auto decoder = std::make_shared<mediakit::FFmpegDecoder>(audioTrack, 0);
            AVChannelLayout mono_layout;
            av_channel_layout_from_string(&mono_layout, "mono");
            mono_layout.nb_channels = 1;
            auto swr = std::make_shared<mediakit::FFmpegSwr>(AudioChannel::SAMPLE_FORMAT, &mono_layout, AudioChannel::SAMPLE_RATE);
            av_channel_layout_uninit(&mono_layout);
            auto dec_ctx = decoder->getContext();
            
            decoder->setOnDecode([weakSelf, swr, dec_ctx](const mediakit::FFmpegFrame::Ptr &frame) mutable {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }

                auto swr_frame = swr->inputFrame(frame);
                self->onAudioFrame(swr_frame);
            });

            audioTrack->addDelegate((std::function<bool(const mediakit::Frame::Ptr &)>)[decoder](const mediakit::Frame::Ptr &frame) {
                return decoder->inputFrame(frame, false, true);
            });
        }
    });

    _player->setOnShutdown([weakPlayer, url, weakSelf](const toolkit::SockException& ex) {
        TraceL << "StackPlayer: " << url << " OnShutdown: " << ex.what();
        auto strongPlayer = weakPlayer.lock();
        if (!strongPlayer) { return; }

        auto self = weakSelf.lock();
        if (!self) { return; }

        self->onDisconnect();
        self->rePlay(url);
    });

    _player->play(url);
}

void StackPlayer::onVideoFrame(const mediakit::FFmpegFrame::Ptr& frame) {
    std::lock_guard<std::recursive_mutex> lock(_mx_video_chn);
    for (auto& weak_chn : _video_channels) {
        if (auto chn = weak_chn.lock()) { chn->onFrame(frame); }
    }
}

void StackPlayer::onDisconnect() {
    std::lock_guard<std::recursive_mutex> lock(_mx_video_chn);
    for (auto& weak_chn : _video_channels) {
        if (auto chn = weak_chn.lock()) {
            auto frame = VideoStackManager::Instance().getBgImg();
            chn->onFrame(frame);
        }
    }
}

void StackPlayer::rePlay(const std::string& url) {
    _failedCount++;
    auto delay = MAX(2 * 1000, MIN(_failedCount * 3 * 1000, 60 * 1000));// 步进延迟 重试间隔
    std::weak_ptr<StackPlayer> weakSelf = shared_from_this();
    _timer = std::make_shared<toolkit::Timer>(delay / 1000.0f, [weakSelf, url]() {
        auto self = weakSelf.lock();
        if (!self) {}
        WarnL << "replay [" << self->_failedCount << "]:" << url;
        self->_player->play(url);
        return false;
    }, nullptr);
}

void StackPlayer::onAudioFrame(const mediakit::FFmpegFrame::Ptr &frame) {
    std::lock_guard<std::recursive_mutex> lock(_mx_audio_chn);
    for (auto &weak_chn : _audio_channels) {
        if (auto chn = weak_chn.lock()) {
            chn->inputFrame(frame);
        }
    }
}

VideoStack::VideoStack(const std::string& id, int width, int height, AVPixelFormat pixfmt,
                       float fps, int bitRate)
    : _id(id), _width(width), _height(height), _pixfmt(pixfmt), _fps(fps), _bitRate(bitRate) {

    _buffer = std::make_shared<mediakit::FFmpegFrame>();

    _buffer->get()->width = _width;
    _buffer->get()->height = _height;
    _buffer->get()->format = _pixfmt;

    av_frame_get_buffer(_buffer->get(), 32);

    _dev = std::make_shared<mediakit::DevChannel>(
        mediakit::MediaTuple{DEFAULT_VHOST, "live", _id, ""});

    mediakit::VideoInfo v_info;
    v_info.codecId = mediakit::CodecH264;
    v_info.iWidth = _width;
    v_info.iHeight = _height;
    v_info.iFrameRate = _fps;
    v_info.iBitRate = _bitRate;

    mediakit::AudioInfo a_info;
    a_info.codecId = mediakit::CodecAAC;
    a_info.iSampleRate = AudioChannel::SAMPLE_RATE;
    a_info.iChannel = AudioChannel::CHANNELS;
    a_info.iSampleBit = av_get_bytes_per_sample(AudioChannel::SAMPLE_FORMAT) * 8;

    _dev->initVideo(v_info);
    _dev->initAudio(a_info);

    _mixer = std::make_shared<AudioMixer>();
    _mixer->setOnOutputFrame([&](const std::shared_ptr<AVPacket> &pkt, const std::array<uint8_t, 7>& adtsHeader) {
        _dev->inputAAC((char*)pkt->data, pkt->size, pkt->pts, (char*)adtsHeader.data());
    });

    _dev->addTrackCompleted();
    _isExit = false;
}

VideoStack::~VideoStack() {
    _mixer->stop();
    _isExit = true;
    if (_thread.joinable()) { _thread.join(); }
}

void VideoStack::setParam(const Params& params) {
    if (_params) {
        for (auto& p : (*_params)) {
            if (!p) continue;
            p->weak_buf.reset();
        }
    }

    initBgColor();
    for (auto& p : (*params)) {
        if (!p) continue;
        p->weak_buf = _buffer;
        if (auto chn = p->weak_chn.lock()) {
            chn->addParam(p);
            chn->fillBuffer(p);
        }
    }
    _params = params;
}

void VideoStack::run() {
    _mixer->start();
    start();
}

void VideoStack::start() {
    _thread = std::thread([&]() {
        uint64_t pts = 0;
        int frameInterval = 1000 / _fps;
        auto lastEncTP = std::chrono::steady_clock::now();
        while (!_isExit) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEncTP);
            if (elapsed >= std::chrono::milliseconds(frameInterval)) {
                lastEncTP = std::chrono::steady_clock::now();
                _dev->inputYUV((char **)_buffer->get()->data, _buffer->get()->linesize, pts);
                pts += frameInterval;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(frameInterval) - elapsed);
            }
        }
    });
}


void VideoStack::initBgColor() {
    // 填充底色  [AUTO-TRANSLATED:ee9bbd46]
    // Fill the background color
    auto R = 20;
    auto G = 20;
    auto B = 20;

    double Y = RGB_TO_Y(R, G, B);
    double U = RGB_TO_U(R, G, B);
    double V = RGB_TO_V(R, G, B);

    fill_yuv_func(_buffer, Y, U, V);
}

VideoChannel::Ptr VideoStackManager::getChannel(const std::string& id, int width, int height,
                                           AVPixelFormat pixfmt) {

    std::lock_guard<std::recursive_mutex> lock(_mx);
    auto key = id + std::to_string(width) + std::to_string(height) + std::to_string(pixfmt);
    auto it = _channelMap.find(key);
    if (it != _channelMap.end()) { return it->second->acquire(); }

    return createChannel(id, width, height, pixfmt);
}

void VideoStackManager::unrefChannel(const std::string& id, int width, int height,
                                     AVPixelFormat pixfmt) {

    std::lock_guard<std::recursive_mutex> lock(_mx);
    auto key = id + std::to_string(width) + std::to_string(height) + std::to_string(pixfmt);
    auto chn_it = _channelMap.find(key);
    if (chn_it != _channelMap.end() && chn_it->second->dispose()) {
        _channelMap.erase(chn_it);

        auto player_it = _playerMap.find(id);
        if (player_it != _playerMap.end() && player_it->second->dispose()) {
            _playerMap.erase(player_it);
        }
    }
}

int VideoStackManager::startVideoStack(const Json::Value& json) {

    std::string id;
    int width, height;
    auto params = parseParams(json, id, width, height);

    if (!params) {
        ErrorL << "Videostack parse params failed!";
        return -1;
    }

    auto stack = std::make_shared<VideoStack>(id, width, height);
    std::map<std::string, std::weak_ptr<StackPlayer>> players;

    for (auto& p : (*params)) {
        if (!p) continue;
        p->weak_chn = getChannel(p->id, p->width, p->height, p->pixfmt);
        players[p->id] = _playerMap[p->id]->weak();
    }

    stack->setParam(params);
    stack->setMixer(players);
    stack->run();

    std::lock_guard<std::recursive_mutex> lock(_mx);
    _stackMap[id] = stack;
    return 0;
}

int VideoStackManager::resetVideoStack(const Json::Value& json) {
    std::string id;
    int width, height;
    auto params = parseParams(json, id, width, height);

    if (!params) {
        ErrorL << "Videostack parse params failed!";
        return -1;
    }

    VideoStack::Ptr stack;
    {
        std::lock_guard<std::recursive_mutex> lock(_mx);
        auto it = _stackMap.find(id);
        if (it == _stackMap.end()) { return -2; }
        stack = it->second;
    }
    std::map<std::string, std::weak_ptr<StackPlayer>> players;

    for (auto& p : (*params)) {
        if (!p) continue;
        p->weak_chn = getChannel(p->id, p->width, p->height, p->pixfmt);
        players[p->id] = _playerMap[p->id]->weak();
    }

    stack->setParam(params);
    stack->setMixer(players);
    return 0;
}

int VideoStackManager::stopVideoStack(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(_mx);
    auto it = _stackMap.find(id);
    if (it != _stackMap.end()) {
        _stackMap.erase(it);
        InfoL << "VideoStack stop: " << id;
        return 0;
    }
    return -1;
}

mediakit::FFmpegFrame::Ptr VideoStackManager::getBgImg() { return _bgImg; }

template<typename T> T getJsonValue(const Json::Value& json, const std::string& key) {
    if (!json.isMember(key)) {
        throw Json::LogicError("VideoStack parseParams missing required field: " + key);
    }
    return json[key].as<T>();
}

Params VideoStackManager::parseParams(const Json::Value& json, std::string& id, int& width,
                                      int& height) {

    id = getJsonValue<std::string>(json, "id");
    width = getJsonValue<int>(json, "width");
    height = getJsonValue<int>(json, "height");
    int rows = getJsonValue<int>(json, "row");// 行数
    int cols = getJsonValue<int>(json, "col");// 列数

    float gapv = json["gapv"].asFloat();// 垂直间距
    float gaph = json["gaph"].asFloat();// 水平间距

    // 单个间距  [AUTO-TRANSLATED:e1b9b5b6]
    // Single spacing
    int gaphPix = static_cast<int>(round(width * gaph));
    int gapvPix = static_cast<int>(round(height * gapv));

    // 根据间距计算格子宽高  [AUTO-TRANSLATED:b9972498]
    // Calculate the width and height of the grid according to the spacing
    int gridWidth = cols > 1 ? (width - gaphPix * (cols - 1)) / cols : width;
    int gridHeight = rows > 1 ? (height - gapvPix * (rows - 1)) / rows : height;

    auto params = std::make_shared<std::vector<Param::Ptr>>(rows * cols);

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            std::string url = json["url"][row][col].asString();

            auto param = std::make_shared<Param>();
            param->posX = gridWidth * col + col * gaphPix;
            param->posY = gridHeight * row + row * gapvPix;
            param->width = gridWidth;
            param->height = gridHeight;
            param->id = url;

            (*params)[row * cols + col] = param;
        }
    }

    // 判断是否需要合并格子 （焦点屏）  [AUTO-TRANSLATED:bfa14430]
    // Determine whether to merge grids (focus screen)
    if (json.isMember("span") && json["span"].isArray() && json["span"].size() > 0) {
        for (const auto& subArray : json["span"]) {
            if (!subArray.isArray() || subArray.size() != 2) {
                throw Json::LogicError("Incorrect 'span' sub-array format in JSON");
            }
            std::array<int, 4> mergePos;
            unsigned int index = 0;

            for (const auto& innerArray : subArray) {
                if (!innerArray.isArray() || innerArray.size() != 2) {
                    throw Json::LogicError("Incorrect 'span' inner-array format in JSON");
                }
                for (const auto& number : innerArray) {
                    if (index < mergePos.size()) { mergePos[index++] = number.asInt(); }
                }
            }

            for (int i = mergePos[0]; i <= mergePos[2]; i++) {
                for (int j = mergePos[1]; j <= mergePos[3]; j++) {
                    if (i == mergePos[0] && j == mergePos[1]) {
                        (*params)[i * cols + j]->width =
                            (mergePos[3] - mergePos[1] + 1) * gridWidth +
                            (mergePos[3] - mergePos[1]) * gapvPix;
                        (*params)[i * cols + j]->height =
                            (mergePos[2] - mergePos[0] + 1) * gridHeight +
                            (mergePos[2] - mergePos[0]) * gaphPix;
                    } else {
                        (*params)[i * cols + j] = nullptr;
                    }
                }
            }
        }
    }
    return params;
}

bool VideoStackManager::loadBgImg(const std::string& path) {
    _bgImg = std::make_shared<mediakit::FFmpegFrame>();

    _bgImg->get()->width = 1280;
    _bgImg->get()->height = 720;
    _bgImg->get()->format = AV_PIX_FMT_YUV420P;

    av_frame_get_buffer(_bgImg->get(), 32);

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { return false; }

    file.read((char*)_bgImg->get()->data[0],
              _bgImg->get()->linesize[0] * _bgImg->get()->height);// Y
    file.read((char*)_bgImg->get()->data[1],
              _bgImg->get()->linesize[1] * _bgImg->get()->height / 2);// U
    file.read((char*)_bgImg->get()->data[2],
              _bgImg->get()->linesize[2] * _bgImg->get()->height / 2);// V
    return true;
}

void VideoStackManager::clear() { _stackMap.clear(); }

VideoChannel::Ptr VideoStackManager::createChannel(const std::string& id, int width, int height,
                                              AVPixelFormat pixfmt) {

    std::lock_guard<std::recursive_mutex> lock(_mx);
    StackPlayer::Ptr player;
    auto it = _playerMap.find(id);
    if (it != _playerMap.end()) {
        player = it->second->acquire();
    } else {
        player = createPlayer(id);
    }

    auto refChn = std::make_shared<RefWrapper<VideoChannel::Ptr>>(
        std::make_shared<VideoChannel>(id, width, height, pixfmt));
    auto chn = refChn->acquire();
    player->addVideoChannel(chn);

    _channelMap[id + std::to_string(width) + std::to_string(height) + std::to_string(pixfmt)] =
        refChn;
    return chn;
}

StackPlayer::Ptr VideoStackManager::createPlayer(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(_mx);
    auto refPlayer =
        std::make_shared<RefWrapper<StackPlayer::Ptr>>(std::make_shared<StackPlayer>(id));
    _playerMap[id] = refPlayer;

    auto player = refPlayer->acquire();
    if (!id.empty()) { player->play(); }

    return player;
}
#endif
