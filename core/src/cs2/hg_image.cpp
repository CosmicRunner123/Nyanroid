#include "nyan/cs2/hg_image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#include "nyan/common/zlib_util.hpp"
#include "stb_image.h"

namespace nyan::cs2 {
namespace {

// LSB-first bit reader over the zero-run command stream.
class BitReader {
public:
    explicit BitReader(ByteSpan data) : data_(data) {}

    uint32_t bit() {
        if (pos_ >= data_.size() * 8) fail("HG command stream exhausted");
        uint32_t b = (data_[pos_ >> 3] >> (pos_ & 7)) & 1;
        ++pos_;
        return b;
    }

    uint32_t eliasGamma() {
        int digits = 0;
        while (!bit()) {
            if (++digits > 31) fail("HG command stream has an invalid Elias gamma code");
        }
        uint32_t value = 1u << digits;
        while (digits--) {
            if (bit()) value |= 1u << digits;
        }
        return value;
    }

private:
    ByteSpan data_;
    size_t pos_ = 0;
};

Bytes unrle(ByteSpan data, ByteSpan cmd) {
    BitReader bits(cmd);
    bool copy = bits.bit() != 0;
    uint32_t total = bits.eliasGamma();
    if (total > 512u * 1024 * 1024) fail("HG image too large");

    Bytes out(total, 0);
    size_t outPos = 0, dataPos = 0;
    while (outPos < total) {
        uint32_t n = bits.eliasGamma();
        if (n > total - outPos) fail("HG zero-run overflows output");
        if (copy) {
            if (n > data.size() - dataPos) fail("HG zero-run overflows input");
            std::memcpy(out.data() + outPos, data.data() + dataPos, n);
            dataPos += n;
        }
        outPos += n;
        copy = !copy;
    }
    return out;
}

struct WeightTables {
    std::array<uint32_t, 256> t[4];
    WeightTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = ((i & 0xC0) << 18) | ((i & 0x30) << 12) | ((i & 0x0C) << 6) | (i & 0x03);
            t[3][i] = v;
            t[2][i] = v << 2;
            t[1][i] = v << 4;
            t[0][i] = v << 6;
        }
    }
};

inline uint8_t unpackSigned(uint8_t c) {
    return (c & 1) ? static_cast<uint8_t>((c >> 1) ^ 0xFF) : static_cast<uint8_t>(c >> 1);
}

uint32_t strideFor(uint32_t width, uint32_t depthBytes) {
    return (width * depthBytes + 3) & ~3u;
}

}  // namespace

Bytes hgProcessImage(ByteSpan rleData, ByteSpan rleCmd, uint32_t width, uint32_t height, uint32_t depthBytes) {
    if (width == 0 || height == 0) fail("HG image has zero size");
    if (depthBytes < 3 || depthBytes > 4) fail("HG image has unsupported bit depth");

    Bytes unrled = unrle(rleData, rleCmd);
    uint32_t stride = strideFor(width, depthBytes);
    size_t expected = static_cast<size_t>(stride) * height;
    if (unrled.size() < expected || unrled.size() % 4 != 0) fail("HG image data size mismatch");

    static const WeightTables tables;
    Bytes out(unrled.size());
    size_t quarter = unrled.size() / 4;
    const uint8_t* s1 = unrled.data();
    const uint8_t* s2 = s1 + quarter;
    const uint8_t* s3 = s2 + quarter;
    const uint8_t* s4 = s3 + quarter;
    uint8_t* o = out.data();
    for (size_t i = 0; i < quarter; ++i) {
        uint32_t v = tables.t[0][s1[i]] | tables.t[1][s2[i]] | tables.t[2][s3[i]] | tables.t[3][s4[i]];
        *o++ = unpackSigned(static_cast<uint8_t>(v));
        *o++ = unpackSigned(static_cast<uint8_t>(v >> 8));
        *o++ = unpackSigned(static_cast<uint8_t>(v >> 16));
        *o++ = unpackSigned(static_cast<uint8_t>(v >> 24));
    }

    // Undo delta filtering: first row against the pixel to the left, later rows against the row above.
    for (uint32_t x = depthBytes; x < stride; ++x) out[x] += out[x - depthBytes];
    for (uint32_t y = 1; y < height; ++y) {
        uint8_t* line = out.data() + static_cast<size_t>(y) * stride;
        const uint8_t* prev = line - stride;
        for (uint32_t x = 0; x < stride; ++x) line[x] += prev[x];
    }
    out.resize(expected);
    return out;
}

HgImage HgImage::parse(Bytes data) {
    HgImage img;
    img.data_ = std::move(data);
    ByteReader r(img.data_, "HG image");
    std::string sig = r.fixedString(4);
    uint32_t headerSize = r.u32();
    uint32_t version = r.u32();
    if (headerSize < 12) fail("HG image header too small");
    r.seek(headerSize);
    if (sig == "HG-3") {
        img.version_ = 3;
        parseHg3(img, r);
    } else if (sig == "HG-2") {
        img.version_ = 2;
        parseHg2(img, r, version & 0xFFFF);
    } else {
        fail("Not an HG-2/HG-3 image");
    }
    if (img.frames_.empty()) fail("HG image has no frames");
    return img;
}

void HgImage::parseHg3(HgImage& img, ByteReader& r) {
    for (size_t frameStart = r.pos();;) {
        r.seek(frameStart);
        uint32_t frameNext = r.u32();
        HgFrame f;
        f.id = r.u32();
        bool haveStdinfo = false;

        for (size_t tagStart = r.pos();;) {
            r.seek(tagStart);
            std::string tag = r.fixedString(8);
            uint32_t tagNext = r.u32();
            uint32_t tagLength = r.u32();
            size_t body = r.pos();
            if (body + tagLength > r.size()) fail("HG-3 tag '" + tag + "' runs past end of file");

            if (tag == "stdinfo") {
                f.width = r.u32();
                f.height = r.u32();
                f.bitDepth = r.u32();
                f.offsetX = r.i32();
                f.offsetY = r.i32();
                f.totalWidth = r.u32();
                f.totalHeight = r.u32();
                f.transparent = r.u32() != 0;
                f.baseX = r.i32();
                f.baseY = r.i32();
                haveStdinfo = true;
            } else if (tag.size() == 7 && tag.starts_with("img") && std::isdigit(static_cast<unsigned char>(tag[3]))) {
                uint32_t sliceStart = r.u32();
                uint32_t sliceLength = r.u32();
                f.standard.compressedData = r.u32();
                f.standard.decompressedData = r.u32();
                f.standard.compressedCmd = r.u32();
                f.standard.decompressedCmd = r.u32();
                f.standard.dataOffset = r.pos();
                if (sliceStart != 0 || (haveStdinfo && sliceLength != f.height)) {
                    f.encoding = HgEncoding::Unsupported;
                    f.unsupportedTag = "multi-slice " + tag;
                } else {
                    f.encoding = HgEncoding::Standard;
                }
            } else if (tag == "img_jpg") {
                f.encoding = HgEncoding::Jpeg;
                f.blobOffset = body;
                f.blobLength = tagLength;
            } else if (tag == "img_al") {
                f.alphaCompressed = r.u32();
                f.alphaDecompressed = r.u32();
                f.alphaOffset = r.pos();
                if (f.encoding == HgEncoding::Jpeg) f.encoding = HgEncoding::JpegWithAlpha;
            } else if (tag == "img_wbp") {
                f.encoding = HgEncoding::WebP;
                f.unsupportedTag = tag;
                f.blobOffset = body;
                f.blobLength = tagLength;
            } else if (tag.starts_with("ats") && tag.size() == 7) {
                HgAttribute a{};
                a.id = static_cast<uint32_t>(std::stoul(tag.substr(3)));
                a.x = r.i32();
                a.y = r.i32();
                a.width = r.u32();
                a.height = r.u32();
                a.color = r.u32();
                f.attributes.push_back(a);
            } else if (tag == "img_jpa") {
                f.encoding = HgEncoding::Unsupported;
                f.unsupportedTag = tag;
            }
            // cptype, imgmode and unknown tags carry nothing we need yet.

            if (tagNext == 0) break;
            tagStart += tagNext;
        }

        if (!haveStdinfo) fail("HG-3 frame is missing its stdinfo tag");
        img.frames_.push_back(std::move(f));
        if (frameNext == 0) break;
        frameStart += frameNext;
    }
}

void HgImage::parseHg2(HgImage& img, ByteReader& r, uint32_t type) {
    if (type != 0x10 && type != 0x20 && type != 0x25) fail("Unknown HG-2 version " + std::to_string(type));
    for (size_t frameStart = r.pos();;) {
        r.seek(frameStart);
        HgFrame f;
        f.width = r.u32();
        f.height = r.u32();
        f.bitDepth = r.u16();
        r.skip(2 + 8);  // depth/planes, unused slice fields
        f.standard.compressedData = r.u32();
        f.standard.decompressedData = r.u32();
        f.standard.compressedCmd = r.u32();
        f.standard.decompressedCmd = r.u32();
        f.totalWidth = f.width;
        f.totalHeight = f.height;
        uint32_t next = 0;
        uint32_t dataOffset = 0;
        if (type >= 0x20) {
            dataOffset = r.u32();
            f.id = r.u32();
            f.totalWidth = r.u32();
            f.totalHeight = r.u32();
            f.offsetX = r.i32();
            f.offsetY = r.i32();
            f.transparent = r.u32() != 0;
            next = r.u32();
        }
        if (type >= 0x25) {
            f.baseX = r.i32();
            f.baseY = r.i32();
        }
        f.standard.dataOffset = frameStart + 0x24 + dataOffset;
        f.encoding = HgEncoding::Standard;
        img.frames_.push_back(std::move(f));
        if (next == 0) break;
        frameStart += next;
    }
}

RgbaImage HgImage::decode(size_t frameIndex) const {
    if (frameIndex >= frames_.size()) fail("HG frame index out of range");
    const HgFrame& f = frames_[frameIndex];
    ByteSpan all(data_);
    auto slice = [&](size_t off, size_t len) {
        if (off > all.size() || len > all.size() - off) fail("HG frame data runs past end of file");
        return all.subspan(off, len);
    };

    RgbaImage out;
    out.width = f.width;
    out.height = f.height;
    out.pixels.resize(static_cast<size_t>(f.width) * f.height * 4);

    switch (f.encoding) {
    case HgEncoding::Standard: {
        const auto& s = f.standard;
        Bytes rleData = zlibInflate(slice(s.dataOffset, s.compressedData), s.decompressedData);
        Bytes rleCmd = zlibInflate(slice(s.dataOffset + s.compressedData, s.compressedCmd), s.decompressedCmd);
        uint32_t depthBytes = (f.bitDepth + 7) / 8;
        Bytes bgr = hgProcessImage(rleData, rleCmd, f.width, f.height, depthBytes);
        uint32_t stride = strideFor(f.width, depthBytes);
        for (uint32_t y = 0; y < f.height; ++y) {
            const uint8_t* src = bgr.data() + static_cast<size_t>(f.height - 1 - y) * stride;
            uint8_t* dst = out.pixels.data() + static_cast<size_t>(y) * f.width * 4;
            for (uint32_t x = 0; x < f.width; ++x, src += depthBytes, dst += 4) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = depthBytes == 4 ? src[3] : 0xFF;
            }
        }
        break;
    }
    case HgEncoding::Jpeg:
    case HgEncoding::JpegWithAlpha: {
        auto jpeg = slice(f.blobOffset, f.blobLength);
        int w = 0, h = 0, comp = 0;
        uint8_t* px = stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h, &comp, 4);
        if (!px) fail(std::string("HG-3 JPEG decode failed: ") + stbi_failure_reason());
        bool ok = static_cast<uint32_t>(w) == f.width && static_cast<uint32_t>(h) == f.height;
        // The engine copies its JPEG decoder's bytes straight into its BGRA image (cs2.exe
        // 0x6a1330), so the files hold blue first: as plain RGB the channels come out swapped
        // (Nekopara's CG3_04a_anime frames against CG3_04a, the same picture in img0000: R / B
        // 182 / 212 against 212 / 182).
        if (ok) {
            for (size_t i = 0; i < out.pixels.size(); i += 4) {
                out.pixels[i] = px[i + 2];
                out.pixels[i + 1] = px[i + 1];
                out.pixels[i + 2] = px[i];
                out.pixels[i + 3] = px[i + 3];
            }
        }
        stbi_image_free(px);
        if (!ok) fail("HG-3 JPEG size does not match stdinfo");
        if (f.encoding == HgEncoding::JpegWithAlpha) {
            Bytes alpha = zlibInflate(slice(f.alphaOffset, f.alphaCompressed), f.alphaDecompressed);
            if (alpha.size() < static_cast<size_t>(f.width) * f.height) fail("HG-3 alpha mask too small");
            for (size_t i = 0; i < static_cast<size_t>(f.width) * f.height; ++i) out.pixels[i * 4 + 3] = alpha[i];
        }
        break;
    }
    case HgEncoding::WebP:
    case HgEncoding::Unsupported:
        fail("Unsupported HG frame encoding: " + f.unsupportedTag);
    }
    return out;
}

RgbaImage HgImage::decodeFull(size_t frameIndex) const {
    RgbaImage part = decode(frameIndex);
    const HgFrame& f = frames_[frameIndex];
    RgbaImage full;
    full.width = std::max(f.totalWidth, 1u);
    full.height = std::max(f.totalHeight, 1u);
    full.pixels.assign(static_cast<size_t>(full.width) * full.height * 4, 0);
    for (uint32_t y = 0; y < part.height; ++y) {
        int64_t dy = static_cast<int64_t>(y) + f.offsetY;
        if (dy < 0 || dy >= full.height) continue;
        int64_t x0 = std::max<int64_t>(0, -static_cast<int64_t>(f.offsetX));
        int64_t x1 = std::min<int64_t>(part.width, static_cast<int64_t>(full.width) - f.offsetX);
        if (x1 <= x0) continue;
        std::memcpy(full.pixels.data() + (static_cast<size_t>(dy) * full.width + f.offsetX + x0) * 4,
                    part.pixels.data() + (static_cast<size_t>(y) * part.width + x0) * 4,
                    static_cast<size_t>(x1 - x0) * 4);
    }
    return full;
}

}  // namespace nyan::cs2
