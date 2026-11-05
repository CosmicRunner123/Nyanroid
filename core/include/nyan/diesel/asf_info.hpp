#pragma once

#include <cstdint>
#include <optional>

#include "nyan/common/file_source.hpp"

namespace nyan::diesel {

// An ASF (Windows Media) file's picture size and length from its header, without decoding: what
// the host needs for a movie sprite's size and SQSprite's movie times. Media Foundation's source
// reader gives the duration as the File Properties Object's play duration less its preroll
// (MF_PD_DURATION, which Mware reads at open: Mware.dll 0x1006d070) and the frame size of the video
// stream (Mware +0xb4 / +0xb8).
struct AsfInfo {
    int width = 0, height = 0;   // the first video stream's (0 if none)
    uint32_t videoCodec = 0;     // its FOURCC ('WMV3', 'WVC1', ...)
    bool hasAudio = false;
    uint64_t playDuration = 0;   // 100 ns units
    uint64_t prerollMs = 0;
    // MF_PD_DURATION: 100 ns units.
    int64_t duration() const {
        int64_t d = static_cast<int64_t>(playDuration) - static_cast<int64_t>(prerollMs) * 10000;
        return d > 0 ? d : 0;
    }
};

// nullopt if the file does not start with an ASF header.
std::optional<AsfInfo> readAsfInfo(const RandomAccessFile& file);

}  // namespace nyan::diesel
