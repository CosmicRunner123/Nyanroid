#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nyan/common/file_source.hpp"
#include "nyan/diesel/draw_item.hpp"

namespace nyan::diesel {

// The pictures a Diesel game's movie textures hold, as Mware fills them (Mware.dll: the frame
// conversion 0x1006c8b0, the texture copy 0x1006da90, the thread's frame drop 0x1006d240):
//  - the frame Media Foundation decodes as RGB32 (here FFmpeg's, nyan::media::FfmpegVideo, BT.601),
//    opaque; with createMovie's alpha flag the texture is the frame's upper half, each pixel's
//    alpha the blue of the pixel half the frame's height below it;
//  - the frame shown is the first whose time is at or after the movie's position (the thread drops
//    the queued frames before it while another follows), or the pass's last one. FfmpegVideo shows
//    the last frame at or before a time, so it is asked for the position plus one frame's time
//    (Fitted: exact for a steady frame rate).
// Each movie decodes on a thread of its own, as Mware's does: the caller gets the frame its position
// asks for (waiting only if it is not decoded yet) and the following frame is decoded meanwhile.
class MovieFrames {
public:
    using OpenFile = std::function<std::shared_ptr<RandomAccessFile>(const std::string&)>;
    using Log = std::function<void(const std::string&)>;
    explicit MovieFrames(OpenFile open, Log log = nullptr);
    ~MovieFrames();
    MovieFrames(const MovieFrames&) = delete;
    MovieFrames& operator=(const MovieFrames&) = delete;

    struct Picture {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;  // top row first
        uint64_t serial = 0;        // changes whenever the picture does
    };
    // The picture of a Movie draw item now; null if the movie cannot be decoded (or before its
    // first frame).
    const Picture* picture(const DrawItem& d);
    // After each frame: movies not drawn for a while have their decoders closed.
    void endFrame();
    static constexpr int kIdleFrames = 300;

private:
    struct Movie;
    OpenFile open_;
    Log log_;
    std::map<uint32_t, std::unique_ptr<Movie>> movies_;
    uint64_t frame_ = 0, serial_ = 0;
};

}  // namespace nyan::diesel
