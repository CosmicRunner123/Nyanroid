#include "nyan/cs2/pe_resources.hpp"

#include <algorithm>
#include <vector>

namespace nyan::cs2 {
namespace {

struct Section {
    uint32_t virtualAddress;
    uint32_t virtualSize;
    uint32_t rawOffset;
    uint32_t rawSize;
};

class PeImage {
public:
    explicit PeImage(ByteSpan image) : image_(image) {
        ByteReader r(image, "PE image");
        if (r.u16() != 0x5A4D) fail("Not a PE executable (missing MZ)");
        r.seek(0x3C);
        uint32_t peOffset = r.u32();
        r.seek(peOffset);
        if (r.u32() != 0x00004550) fail("Not a PE executable (missing PE signature)");
        r.skip(2);  // machine
        uint16_t sectionCount = r.u16();
        r.skip(12);
        uint16_t optionalSize = r.u16();
        r.skip(2);  // characteristics
        size_t optionalStart = r.pos();
        uint16_t magic = r.u16();
        size_t dataDirOffset;
        if (magic == 0x10B) dataDirOffset = 96;        // PE32
        else if (magic == 0x20B) dataDirOffset = 112;  // PE32+
        else fail("Unknown PE optional header magic");
        r.seek(optionalStart + dataDirOffset - 4);
        uint32_t dirCount = r.u32();
        if (dirCount > 2) {
            r.seek(optionalStart + dataDirOffset + 2 * 8);  // IMAGE_DIRECTORY_ENTRY_RESOURCE
            resourceRva_ = r.u32();
            resourceSize_ = r.u32();
        }
        r.seek(optionalStart + optionalSize);
        for (uint16_t i = 0; i < sectionCount; ++i) {
            r.skip(8);  // name
            Section s{};
            s.virtualSize = r.u32();
            s.virtualAddress = r.u32();
            s.rawSize = r.u32();
            s.rawOffset = r.u32();
            r.skip(16);
            sections_.push_back(s);
        }
    }

    uint32_t resourceRva() const { return resourceRva_; }
    uint32_t resourceSize() const { return resourceSize_; }

    size_t rvaToOffset(uint32_t rva) const {
        for (const auto& s : sections_) {
            uint32_t extent = std::max(s.virtualSize, s.rawSize);
            if (rva >= s.virtualAddress && rva < s.virtualAddress + extent) {
                return static_cast<size_t>(rva - s.virtualAddress) + s.rawOffset;
            }
        }
        fail("PE RVA out of range");
    }

private:
    ByteSpan image_;
    uint32_t resourceRva_ = 0;
    uint32_t resourceSize_ = 0;
    std::vector<Section> sections_;
};

// Reads a UTF-16 length-prefixed resource name, returning it as ASCII (non-ASCII -> '?').
std::string readResourceName(ByteReader& r, size_t offset) {
    r.seek(offset);
    uint16_t len = r.u16();
    std::string s;
    for (uint16_t i = 0; i < len; ++i) {
        uint16_t ch = r.u16();
        s += ch < 0x80 ? static_cast<char>(ch) : '?';
    }
    return s;
}

// Returns the offset-in-resource-section of the child entry matching `name`
// (or the first entry when name is empty), with its high "is directory" bit preserved.
std::optional<uint32_t> findChild(ByteReader& r, size_t dirOffset, const std::string& name) {
    r.seek(dirOffset + 12);
    uint16_t namedCount = r.u16();
    uint16_t idCount = r.u16();
    size_t entriesStart = r.pos();
    for (uint32_t i = 0; i < static_cast<uint32_t>(namedCount) + idCount; ++i) {
        r.seek(entriesStart + i * 8);
        uint32_t nameField = r.u32();
        uint32_t target = r.u32();
        if (name.empty()) return target;
        if (nameField & 0x80000000) {
            size_t save = r.pos();
            std::string entryName = readResourceName(r, nameField & 0x7FFFFFFF);
            r.seek(save);
            if (entryName == name) return target;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<Bytes> findPeResource(ByteSpan image, const std::string& type, const std::string& name) {
    PeImage pe(image);
    if (pe.resourceRva() == 0) return std::nullopt;
    size_t base = pe.rvaToOffset(pe.resourceRva());
    if (base >= image.size()) fail("PE resource section out of range");
    ByteReader r(image.subspan(base), "PE resources");

    auto typeEntry = findChild(r, 0, type);
    if (!typeEntry || !(*typeEntry & 0x80000000)) return std::nullopt;
    auto nameEntry = findChild(r, *typeEntry & 0x7FFFFFFF, name);
    if (!nameEntry || !(*nameEntry & 0x80000000)) return std::nullopt;
    auto langEntry = findChild(r, *nameEntry & 0x7FFFFFFF, "");
    if (!langEntry || (*langEntry & 0x80000000)) return std::nullopt;

    r.seek(*langEntry);
    uint32_t dataRva = r.u32();
    uint32_t dataSize = r.u32();
    size_t offset = pe.rvaToOffset(dataRva);
    if (offset + dataSize > image.size()) fail("PE resource data out of range");
    return Bytes(image.begin() + offset, image.begin() + offset + dataSize);
}

}  // namespace nyan::cs2
