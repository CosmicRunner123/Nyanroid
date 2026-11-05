#include "nyan/diesel/exe_info.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace nyan::diesel {

namespace {

uint64_t readLE64(const uint8_t* p) { return readLE32(p) | (static_cast<uint64_t>(readLE32(p + 4)) << 32); }

bool containsAscii(const Bytes& data, const char* needle) {
    size_t n = std::strlen(needle);
    auto it = std::search(data.begin(), data.end(), needle, needle + n,
                          [](uint8_t a, char b) { return std::tolower(a) == std::tolower(static_cast<unsigned char>(b)); });
    return it != data.end();
}

std::optional<ExeInfo> parse(Bytes image, std::string name) {
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return std::nullopt;
    uint32_t pe = readLE32(&image[0x3c]);
    if (pe + 0x108 > image.size() || std::memcmp(&image[pe], "PE\0\0", 4) != 0) return std::nullopt;
    uint16_t machine = readLE16(&image[pe + 4]);
    if (machine != 0x14c && machine != 0x8664) return std::nullopt;  // i386, x86-64
    ExeInfo info;
    info.fileName = std::move(name);
    info.is64 = machine == 0x8664;
    info.timestamp = readLE32(&image[pe + 8]);
    // The optional header's ImageBase: a dword at +0x1c (PE32), a qword at +0x18 (PE32+).
    info.imageBase = info.is64 ? readLE64(&image[pe + 0x18 + 0x18]) : readLE32(&image[pe + 0x18 + 0x1c]);
    uint16_t sections = readLE16(&image[pe + 6]);
    uint16_t optSize = readLE16(&image[pe + 0x14]);
    size_t sh = pe + 0x18 + optSize;
    for (uint16_t i = 0; i < sections; ++i, sh += 0x28) {
        if (sh + 0x28 > image.size()) return std::nullopt;
        PeSection s;
        size_t len = 0;
        while (len < 8 && image[sh + len]) ++len;
        s.name.assign(reinterpret_cast<const char*>(&image[sh]), len);
        s.virtualSize = readLE32(&image[sh + 8]);
        s.virtualAddress = readLE32(&image[sh + 12]);
        s.fileSize = readLE32(&image[sh + 16]);
        s.fileOffset = readLE32(&image[sh + 20]);
        info.sections.push_back(s);
    }
    info.image = std::move(image);
    return info;
}

}  // namespace

std::optional<size_t> ExeInfo::offsetOf(uint64_t va) const {
    if (va < imageBase || va - imageBase > 0xffffffffu) return std::nullopt;
    uint32_t rva = static_cast<uint32_t>(va - imageBase);
    for (const auto& s : sections) {
        if (rva >= s.virtualAddress && rva < s.virtualAddress + std::min(s.virtualSize, s.fileSize)) {
            return size_t(s.fileOffset) + (rva - s.virtualAddress);
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> ExeInfo::addressOf(size_t offset) const {
    for (const auto& s : sections) {
        if (offset >= s.fileOffset && offset < size_t(s.fileOffset) + std::min(s.virtualSize, s.fileSize)) {
            return imageBase + s.virtualAddress + (offset - s.fileOffset);
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> ExeInfo::read32(uint64_t va) const {
    auto o = offsetOf(va);
    if (!o || *o + 4 > image.size()) return std::nullopt;
    return readLE32(&image[*o]);
}

std::optional<uint64_t> ExeInfo::readPointer(uint64_t va) const {
    if (!is64) return read32(va);
    auto o = offsetOf(va);
    if (!o || *o + 8 > image.size()) return std::nullopt;
    return readLE64(&image[*o]);
}

bool ExeInfo::holdsName(const std::string& text) const {
    if (containsString(text)) return true;
    const PeSection* code = section(".text");
    if (is64 || !code) return false;
    const auto codeBegin = image.begin() + code->fileOffset;
    const auto codeEnd = codeBegin + std::min<size_t>(code->fileSize, image.size() - code->fileOffset);
    std::string needle = text;
    needle.push_back('\0');
    for (auto it = image.begin();; ++it) {
        it = std::search(it, image.end(), needle.begin(), needle.end(),
                         [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); });
        if (it == image.end()) return false;
        auto va = addressOf(static_cast<size_t>(it - image.begin()));
        if (!va) continue;
        uint8_t le[4];
        writeLE32(le, static_cast<uint32_t>(*va));
        if (std::search(codeBegin, codeEnd, le, le + 4) != codeEnd) return true;
    }
}

bool ExeInfo::containsString(const std::string& text) const {
    std::string needle = text;
    needle.push_back('\0');
    auto it = image.begin();
    while (true) {
        it = std::search(it, image.end(), needle.begin(), needle.end(),
                         [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); });
        if (it == image.end()) return false;
        if (it == image.begin() || *(it - 1) == 0) return true;
        ++it;
    }
}

std::vector<std::string> ExeInfo::imports() const {
    std::vector<std::string> out;
    // The optional header's data directories (PE32 +0x60, PE32+ +0x70); the import directory is
    // the second, an array of 20-byte descriptors ending with a zeroed one, the DLL's name at +12.
    uint32_t pe = readLE32(&image[0x3c]);
    size_t dir = size_t{pe} + 0x18 + (is64 ? 0x70 : 0x60) + 8;
    if (dir + 8 > image.size()) return out;
    uint32_t rva = readLE32(&image[dir]);
    auto o = rva ? offsetOf(imageBase + rva) : std::nullopt;
    for (size_t d = o ? *o : image.size(); d + 20 <= image.size(); d += 20) {
        uint32_t name = readLE32(&image[d + 12]);
        if (name == 0) break;
        auto n = offsetOf(imageBase + name);
        if (!n) break;
        std::string s;
        for (size_t i = *n; i < image.size() && image[i] != 0 && s.size() < 256; ++i) s.push_back(static_cast<char>(image[i]));
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<uint64_t> ExeInfo::vtable(const std::string& rttiName) const {
    std::string needle = rttiName;
    needle.push_back('\0');
    auto it = std::search(image.begin(), image.end(), needle.begin(), needle.end(),
                          [](uint8_t a, char b) { return a == static_cast<uint8_t>(b); });
    if (it == image.end()) return std::nullopt;
    auto nameVa = addressOf(static_cast<size_t>(it - image.begin()));
    if (!nameVa) return std::nullopt;
    // The type descriptor: {vftable of type_info, spare, name}, pointer-sized fields.
    uint64_t typeDescriptor = *nameVa - 2 * pointerSize();
    // The locator: {signature (0, or 1 with image-relative offsets), offset 0, cd offset, type
    // descriptor, class hierarchy[, itself]}.
    uint32_t tdField = is64 ? static_cast<uint32_t>(typeDescriptor - imageBase) : static_cast<uint32_t>(typeDescriptor);
    uint32_t signature = is64 ? 1 : 0;
    auto findDword = [&](uint32_t value, size_t from) -> std::optional<size_t> {
        for (size_t o = from; o + 4 <= image.size(); o += 4) {
            if (readLE32(&image[o]) == value) return o;
        }
        return std::nullopt;
    };
    auto findPointer = [&](uint64_t value) -> std::optional<size_t> {
        if (!is64) return findDword(static_cast<uint32_t>(value), 0);
        for (size_t o = 0; o + 8 <= image.size(); o += 8) {
            if (readLE64(&image[o]) == value) return o;
        }
        return std::nullopt;
    };
    for (auto o = findDword(tdField, 0); o; o = findDword(tdField, *o + 4)) {
        if (*o < 12 || readLE32(&image[*o - 12]) != signature || readLE32(&image[*o - 8]) != 0) continue;
        auto locator = addressOf(*o - 12);
        if (!locator) continue;
        if (auto ref = findPointer(*locator)) {
            if (auto at = addressOf(*ref)) return *at + pointerSize();
        }
    }
    return std::nullopt;
}

const PeSection* ExeInfo::section(const std::string& name) const {
    for (const auto& s : sections) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::optional<ExeInfo> findExe(const GameFolder& folder) {
    std::vector<std::string> names = folder.list();
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        if (!toLowerAscii(n).ends_with(".exe")) continue;
        auto f = folder.open(n);
        if (!f || f->size() > 64u * 1024 * 1024) continue;
        Bytes image = f->readAll();
        if (!containsAscii(image, "Mware.dll")) continue;
        if (auto info = parse(std::move(image), n)) return info;
    }
    return std::nullopt;
}

std::optional<ExeInfo> readPe(const GameFolder& folder, const std::string& name) {
    auto f = folder.open(name);
    if (!f || f->size() > 64u * 1024 * 1024) return std::nullopt;
    return parse(f->readAll(), name);
}

std::map<uint64_t, std::vector<uint32_t>> pushedConstants(const ExeInfo& exe) {
    std::map<uint64_t, std::vector<uint32_t>> out;
    const PeSection* text = exe.section(".text");
    if (!text) return out;
    const uint8_t* b = exe.image.data();
    size_t begin = text->fileOffset, end = std::min<size_t>(exe.image.size(), size_t{text->fileOffset} + text->fileSize);
    auto rd32 = [b](size_t at) {
        return static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8 | static_cast<uint32_t>(b[at + 2]) << 16 |
               static_cast<uint32_t>(b[at + 3]) << 24;
    };
    const size_t load = exe.is64 ? 7 : 5;  // the load's length; then test (3 / 2) and je short (2)
    const size_t test = exe.is64 ? 3 : 2;
    for (size_t i = begin + load + test + 2; i + 6 <= end; ++i) {
        if (b[i] != 0xc7 || b[i + 1] != 0x00) continue;
        size_t j = i - 2;  // je short
        if (b[j] != 0x74) continue;
        j -= test;
        if (exe.is64 ? (b[j] != 0x48 || b[j + 1] != 0x85 || b[j + 2] != 0xc0) : (b[j] != 0x85 || b[j + 1] != 0xc0)) continue;
        j -= load;
        uint64_t addr = 0;
        if (exe.is64) {
            if (b[j] != 0x48 || b[j + 1] != 0x8b || b[j + 2] != 0x05) continue;
            auto va = exe.addressOf(j + 7);
            if (!va) continue;
            addr = *va + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(rd32(j + 3))));
        } else {
            if (b[j] != 0xa1) continue;
            addr = rd32(j + 1);
        }
        out[addr].push_back(rd32(i + 2));
    }
    return out;
}

NpkKey findArchiveKey(const ExeInfo& exe, const RandomAccessFile& probe) {
    const PeSection* text = exe.section(".text");
    if (!text) fail(exe.fileName + ": no .text section");
    const uint8_t* code = exe.image.data() + text->fileOffset;
    size_t size = std::min<size_t>(text->fileSize, exe.image.size() - text->fileOffset);

    // Stores of an immediate dword to the frame: C7 45 d8 / C7 85 d32 ([ebp/rbp + d]) and C7 44 24 d8 /
    // C7 84 24 d32 ([esp/rsp + d]), then the imm32.
    struct Store {
        size_t at, end;
        int base;  // 0 ebp/rbp, 1 esp/rsp
        int32_t disp;
        uint32_t value;
    };
    auto storeAt = [&](size_t i) -> std::optional<Store> {
        if (code[i] != 0xc7) return std::nullopt;
        if (i + 7 <= size && code[i + 1] == 0x45) {
            return Store{i, i + 7, 0, static_cast<int8_t>(code[i + 2]), readLE32(code + i + 3)};
        }
        if (i + 10 <= size && code[i + 1] == 0x85) {
            return Store{i, i + 10, 0, static_cast<int32_t>(readLE32(code + i + 2)), readLE32(code + i + 6)};
        }
        if (i + 8 <= size && code[i + 1] == 0x44 && code[i + 2] == 0x24) {
            return Store{i, i + 8, 1, static_cast<int8_t>(code[i + 3]), readLE32(code + i + 4)};
        }
        if (i + 11 <= size && code[i + 1] == 0x84 && code[i + 2] == 0x24) {
            return Store{i, i + 11, 1, static_cast<int32_t>(readLE32(code + i + 3)), readLE32(code + i + 7)};
        }
        return std::nullopt;
    };
    std::vector<Store> stores;
    for (size_t i = 0; i < size; ++i) {
        if (auto s = storeAt(i)) stores.push_back(*s);
    }
    // From each store, the stores to the next seven dwords of the same frame within a window (the
    // first one found for each); a set of eight is a candidate.
    constexpr size_t kWindow = 256;
    std::vector<NpkKey> tried;
    for (size_t a = 0; a < stores.size(); ++a) {
        const Store& first = stores[a];
        std::array<const Store*, 8> slot{};
        slot[0] = &first;
        int found = 1;
        for (size_t b = a + 1; b < stores.size() && stores[b].at < first.end + kWindow && found < 8; ++b) {
            const Store& s = stores[b];
            if (s.base != first.base) continue;
            int64_t k = (static_cast<int64_t>(s.disp) - first.disp) / 4;
            if ((static_cast<int64_t>(s.disp) - first.disp) % 4 != 0 || k < 1 || k > 7 || slot[static_cast<size_t>(k)]) continue;
            slot[static_cast<size_t>(k)] = &s;
            ++found;
        }
        if (found < 8) continue;
        NpkKey key{};
        for (size_t k = 0; k < 8; ++k) writeLE32(&key[4 * k], slot[k]->value);
        if (std::find(tried.begin(), tried.end(), key) != tried.end()) continue;
        tried.push_back(key);
        if (NpkArchive::keyOpens(probe, key)) return key;
    }
    fail(exe.fileName + ": no archive key found (" + std::to_string(tried.size()) + " candidates tried)");
}

}  // namespace nyan::diesel
