// Single translation unit holding the stb and pl_mpeg implementations used by the core.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "stb_vorbis.c"

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#undef PL_MPEG_IMPLEMENTATION

#include <algorithm>
#include <exception>

#include "media/plm_stream.hpp"

namespace nyan::media {

// pl_mpeg's FILE buffer (plm_buffer_create_with_file) over a RandomAccessFile. It is defined here
// because it sets the buffer's fields, which only the implementation declares.
plm_buffer_t* createPlmBuffer(PlmStream& stream) {
    auto load = [](plm_buffer_t* self, void* user) {
        auto& s = *static_cast<PlmStream*>(user);
        if (self->discard_read_bytes) plm_buffer_discard_read_bytes(self);
        const uint64_t size = s.file->size();
        size_t n = static_cast<size_t>(std::min<uint64_t>(self->capacity - self->length, size - std::min(s.pos, size)));
        try {
            if (n > 0) s.file->readAt(s.pos, self->bytes + self->length, n);
        } catch (const std::exception&) {
            n = 0;  // a read error ends the movie
        }
        s.pos += n;
        self->length += n;
        if (n == 0) self->has_ended = TRUE;
    };
    auto seek = [](plm_buffer_t*, size_t offset, void* user) { static_cast<PlmStream*>(user)->pos = offset; };
    auto tell = [](plm_buffer_t*, void* user) { return static_cast<size_t>(static_cast<PlmStream*>(user)->pos); };
    plm_buffer_t* buffer =
        plm_buffer_create_with_callbacks(load, seek, tell, static_cast<size_t>(stream.file->size()), &stream);
    buffer->discard_read_bytes = TRUE;
    return buffer;
}

}  // namespace nyan::media
