#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "nyan/common/file_source.hpp"

namespace nyan::media {

// A movie decoded with FFmpeg (the build's demuxers and decoders: ASF with Windows Media Video 7-9 /
// VC-1 and Windows Media Audio 1, 2 and Pro; cmake/ffmpeg.cmake). Same shape as MpegVideo: the
// picture is stepped by time on the caller's thread, and the sound comes from a second demuxer over
// the same file, pulled by the audio thread.
class FfmpegVideo {
public:
    // Whether this build has FFmpeg (else open() always fails).
    static bool available();
    // Nullptr if the file cannot be demuxed or has no video stream the build can decode.
    static std::unique_ptr<FfmpegVideo> open(std::shared_ptr<RandomAccessFile> file);
    ~FfmpegVideo();
    FfmpegVideo(const FfmpegVideo&) = delete;
    FfmpegVideo& operator=(const FfmpegVideo&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }
    double frameRate() const { return frameRate_; }
    double duration() const { return duration_; }  // seconds, 0 if unknown
    bool hasAudio() const { return sampleRate_ > 0; }
    int sampleRate() const { return sampleRate_; }

    // Decodes up to the frame shown at `seconds` from the start. True if the picture changed (then
    // `rgba()` holds it, width() * height() * 4 bytes, top row first).
    bool seek(double seconds);
    const std::vector<uint8_t>& rgba() const { return rgba_; }
    bool ended() const { return ended_; }
    void rewind();

    // A source of interleaved stereo float samples at sampleRate(): fills up to `frames` frames and
    // returns how many it wrote (0 at the end). Owns its own demuxer and decoder.
    std::function<int(float*, int)> audioSource(bool loop) const;

private:
    FfmpegVideo() = default;
    struct Demuxer;
    bool nextFrame();  // the next decoded picture into pending_; false at the end

    std::shared_ptr<RandomAccessFile> file_;
    std::unique_ptr<Demuxer> demux_;
    void* codec_ = nullptr;    // AVCodecContext
    void* frame_ = nullptr;    // AVFrame: the decoded picture after the shown one
    double pendingTime_ = -1;  // its time, -1 if none decoded yet
    bool drained_ = false;
    int videoStream_ = -1;
    int width_ = 0, height_ = 0, sampleRate_ = 0;
    double frameRate_ = 30, duration_ = 0, timeBase_ = 0;
    bool ended_ = false;
    std::vector<uint8_t> rgba_;
};

}  // namespace nyan::media
