#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "nyan/common/file_source.hpp"

namespace nyan::media {

// An MPEG-1 program stream (the CatSystem2 games' .mpg movies: MPEG-1 video, MP2 audio), decoded
// with pl_mpeg from the file as it plays. A worker thread decodes the pictures a few ahead; the
// caller's thread steps the picture by time. The sound comes from a second decoder over the same
// file, pulled by the audio thread (pl_mpeg instances are not shared between threads).
class MpegVideo {
public:
    static std::unique_ptr<MpegVideo> open(std::shared_ptr<RandomAccessFile> file);
    ~MpegVideo();
    MpegVideo(const MpegVideo&) = delete;
    MpegVideo& operator=(const MpegVideo&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }
    double frameRate() const { return frameRate_; }
    bool hasAudio() const { return sampleRate_ > 0; }
    int sampleRate() const { return sampleRate_; }

    // Steps to the frame shown at `seconds` from the start, waiting for the worker if it is not
    // decoded yet. True if the picture changed (then `rgba()` holds it, width() * height() * 4 bytes).
    bool seek(double seconds);
    const std::vector<uint8_t>& rgba() const { return rgba_; }
    bool ended() const { return ended_; }
    // Starts again from the beginning (looping plane videos).
    void rewind();

    // A source of interleaved stereo float samples at sampleRate(): fills up to `frames` frames and
    // returns how many it wrote (0 at the end). Independent of the picture; owns its own decoder.
    std::function<int(float*, int)> audioSource(bool loop) const;

private:
    struct Decoder;
    MpegVideo() = default;
    std::shared_ptr<RandomAccessFile> file_;
    std::unique_ptr<Decoder> decoder_;
    int width_ = 0, height_ = 0, sampleRate_ = 0;
    double frameRate_ = 30;
    int64_t shown_ = -1;  // index of the frame in rgba_
    bool ended_ = false;
    std::vector<uint8_t> rgba_;
};

}  // namespace nyan::media
