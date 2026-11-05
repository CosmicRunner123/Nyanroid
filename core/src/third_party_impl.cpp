// Single translation unit holding the stb implementations used by the core.

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
