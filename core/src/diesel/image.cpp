#include "nyan/diesel/image.hpp"

#include <cstring>
#include <string>

#include <webp/decode.h>

#include "stb_image.h"

namespace nyan::diesel {

namespace {

bool isWebp(ByteSpan d) {
    return d.size() >= 16 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(d.data() + 8, "WEBP", 4) == 0;
}
bool isPng(ByteSpan d) { return d.size() >= 8 && std::memcmp(d.data(), "\x89PNG\r\n\x1a\n", 8) == 0; }
bool isJpeg(ByteSpan d) { return d.size() >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff; }

}  // namespace

const char* imageFormat(ByteSpan data) {
    if (isWebp(data)) return "webp";
    if (isPng(data)) return "png";
    if (isJpeg(data)) return "jpeg";
    return "";
}

bool imageSize(ByteSpan data, int& width, int& height) {
    if (isWebp(data)) return WebPGetInfo(data.data(), data.size(), &width, &height) != 0;
    if (isPng(data) || isJpeg(data)) {
        int comp = 0;
        return stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &width, &height, &comp) != 0;
    }
    return false;
}

Image decodeImage(ByteSpan data) {
    Image img;
    if (isWebp(data)) {
        uint8_t* px = WebPDecodeRGBA(data.data(), data.size(), &img.width, &img.height);
        if (!px) fail("broken WebP picture");
        img.rgba.assign(px, px + size_t(img.width) * img.height * 4);
        WebPFree(px);
        return img;
    }
    if (isPng(data) || isJpeg(data)) {
        int comp = 0;
        stbi_uc* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &img.width, &img.height, &comp, 4);
        if (!px) fail(std::string("broken picture: ") + stbi_failure_reason());
        img.rgba.assign(px, px + size_t(img.width) * img.height * 4);
        stbi_image_free(px);
        return img;
    }
    fail("not a picture (WebP, PNG or JPEG)");
}

}  // namespace nyan::diesel
