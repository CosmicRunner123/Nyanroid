#pragma once

#include <cstdint>
#include <memory>

#include "nyan/common/file_source.hpp"
#include "pl_mpeg.h"

namespace nyan::media {

// Where a streaming pl_mpeg buffer reads from: a file and the position of its next read.
struct PlmStream {
    std::shared_ptr<RandomAccessFile> file;
    uint64_t pos = 0;
};

// A pl_mpeg buffer that reads `stream` a buffer (128 KB) at a time, as pl_mpeg's own FILE buffer
// does, so a movie inside an archive is never read whole. `stream` must outlive the buffer.
plm_buffer_t* createPlmBuffer(PlmStream& stream);

}  // namespace nyan::media
