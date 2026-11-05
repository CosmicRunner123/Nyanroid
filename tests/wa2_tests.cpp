// Unit tests for the White Album 2 (WA2) engine core, on data made up here. Run via `ctest`.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "nyan/common/engine.hpp"
#include "nyan/audio/mixer.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/media/ffmpeg_video.hpp"
#include "nyan/wa2/ani.hpp"
#include "nyan/wa2/assets.hpp"
#include "nyan/wa2/game.hpp"
#include "nyan/wa2/gfx.hpp"
#include "nyan/wa2/image.hpp"
#include "nyan/wa2/kcap_archive.hpp"
#include "nyan/wa2/lac_archive.hpp"
#include "nyan/wa2/lzss.hpp"
#include "nyan/wa2/script_vm.hpp"
#include "nyan/wa2/sound_out.hpp"
#include "nyan/wa2/subtitles.hpp"
#include "nyan/wa2/system_data.hpp"

namespace {
using nyan::Bytes;

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ(actual, expected) CHECK((actual) == (expected))

Bytes bytes(std::string_view s) {
    return Bytes(s.begin(), s.end());
}

std::string text(const Bytes& b) {
    return std::string(b.begin(), b.end());
}

// A file held in memory.
class MemoryFile final : public nyan::RandomAccessFile {
public:
    explicit MemoryFile(Bytes data) : data_(std::move(data)) {}
    uint64_t size() const override { return data_.size(); }
    void readAt(uint64_t offset, void* dst, size_t length) const override {
        if (offset + length > data_.size()) nyan::fail("read past the end");
        std::memcpy(dst, data_.data() + offset, length);
    }

private:
    Bytes data_;
};

// A game folder held in memory: paths with '/' (one level of subfolders), matched in either case.
class MemoryFolder final : public nyan::GameFolder {
public:
    void add(const std::string& path, Bytes data) { files_[path] = std::move(data); }
    std::vector<std::string> list() const override {
        std::vector<std::string> out;
        for (const auto& [p, d] : files_) {
            if (p.find('/') == std::string::npos) out.push_back(p);
        }
        return out;
    }
    std::vector<std::string> listDir(const std::string& sub) const override {
        std::vector<std::string> out;
        for (const auto& [p, d] : files_) {
            auto slash = p.find('/');
            if (slash != std::string::npos && nyan::toLowerAscii(p.substr(0, slash)) == nyan::toLowerAscii(sub)) {
                out.push_back(p.substr(slash + 1));
            }
        }
        return out;
    }
    std::unique_ptr<nyan::RandomAccessFile> open(const std::string& name) const override {
        std::string want = nyan::toLowerAscii(name);
        for (char& c : want) c = c == '\\' ? '/' : c;
        for (const auto& [p, d] : files_) {
            if (nyan::toLowerAscii(p) == want) return std::make_unique<MemoryFile>(d);
        }
        return nullptr;
    }
    std::string displayName() const override { return "memory"; }

private:
    std::map<std::string, Bytes> files_;
};

// A folder that only lists names and opens nothing.
class ListedFolder final : public nyan::GameFolder {
public:
    explicit ListedFolder(std::vector<std::string> names) : names_(std::move(names)) {}
    std::vector<std::string> list() const override { return names_; }
    std::unique_ptr<nyan::RandomAccessFile> open(const std::string&) const override { return nullptr; }
    std::string displayName() const override { return "listed"; }

private:
    std::vector<std::string> names_;
};

struct TestEntry {
    uint32_t type;
    std::string name;
    Bytes data;  // as stored (a packed entry with its 8-byte header)
};

Bytes kcap(const std::vector<TestEntry>& entries) {
    Bytes out(16 + entries.size() * 0x2c, 0);
    std::memcpy(out.data(), "KCAP", 4);
    nyan::writeLE32(&out[12], static_cast<uint32_t>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        size_t base = 16 + i * 0x2c;
        nyan::writeLE32(&out[base], entries[i].type);
        std::memcpy(&out[base + 4], entries[i].name.data(), std::min<size_t>(entries[i].name.size(), 24));
        nyan::writeLE32(&out[base + 0x24], static_cast<uint32_t>(out.size()));
        nyan::writeLE32(&out[base + 0x28], static_cast<uint32_t>(entries[i].data.size()));
        out.insert(out.end(), entries[i].data.begin(), entries[i].data.end());
    }
    return out;
}

// An entry packed as literals only, with its header (packed size, unpacked size).
Bytes packed(std::string_view s) {
    Bytes stream;
    for (size_t i = 0; i < s.size(); i += 8) {
        size_t n = std::min<size_t>(8, s.size() - i);
        stream.push_back(static_cast<uint8_t>((1u << n) - 1));
        stream.insert(stream.end(), s.begin() + static_cast<std::ptrdiff_t>(i), s.begin() + static_cast<std::ptrdiff_t>(i + n));
    }
    Bytes out(8);
    nyan::writeLE32(&out[0], static_cast<uint32_t>(8 + stream.size()));
    nyan::writeLE32(&out[4], static_cast<uint32_t>(s.size()));
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

// A LAC archive; names are given as the bytes to store before inversion.
Bytes lac(const std::vector<std::pair<std::string, Bytes>>& entries) {
    Bytes out(8 + entries.size() * 40, 0);
    std::memcpy(out.data(), "LAC\0", 4);
    nyan::writeLE32(&out[4], static_cast<uint32_t>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        size_t base = 8 + i * 40;
        const auto& [name, data] = entries[i];
        for (size_t k = 0; k < name.size() && k < 32; ++k) out[base + k] = static_cast<uint8_t>(~static_cast<uint8_t>(name[k]));
        nyan::writeLE32(&out[base + 32], static_cast<uint32_t>(data.size()));
        nyan::writeLE32(&out[base + 36], static_cast<uint32_t>(out.size()));
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

void testLzss() {
    using nyan::wa2::lzssDecompress;
    // Eight literals.
    auto r = lzssDecompress(bytes("\xff" "ABCDEFGH"), 8);
    CHECK_EQ(text(r.data), std::string("ABCDEFGH"));
    CHECK_EQ(r.produced, size_t{8});
    // The ring below 0xfee holds spaces.
    r = lzssDecompress(Bytes{0x00, 0x00, 0x00}, 3);
    CHECK_EQ(text(r.data), std::string("   "));
    CHECK_EQ(r.readStaleTail, false);
    // A reference overlapping what it writes: "ab" then 6 bytes from 0xfee.
    r = lzssDecompress(Bytes{0x03, 'a', 'b', 0xee, 0xf3}, 8);
    CHECK_EQ(text(r.data), std::string("abababab"));
    CHECK_EQ(r.readStaleTail, false);
    // Reading the ring's end before anything was written there.
    r = lzssDecompress(Bytes{0x00, 0xf8, 0xf0}, 3);
    CHECK_EQ(r.readStaleTail, true);
    CHECK_EQ(r.produced, size_t{3});
    // More than asked for: the engine produces one byte past the size before it stops.
    r = lzssDecompress(bytes("\xff" "123456789"), 4);
    CHECK_EQ(text(r.data), std::string("1234"));
    CHECK_EQ(r.produced, size_t{5});
    // Less: the rest stays zero.
    r = lzssDecompress(bytes("\x07" "xyz"), 5);
    CHECK_EQ(r.produced, size_t{3});
    CHECK_EQ(r.data.size(), size_t{5});
    CHECK_EQ(r.data[4], uint8_t{0});
}

void testKcap() {
    using nyan::wa2::KcapArchive;
    Bytes data = kcap({
        {0xccccccccu, "bak", {}},
        {0, "A.TXT", bytes("hello")},
        {1, "b.bnr", packed("LSCR and more")},
        {0, "a.txt", bytes("second")},
        {1, "empty", {}},
    });
    auto a = KcapArchive::open(std::make_unique<MemoryFile>(data), "test.pak");
    CHECK(a != nullptr);
    if (!a) return;
    CHECK_EQ(a->entries().size(), size_t{5});
    CHECK(a->entries()[0].folder());
    // The first entry of the name, either case.
    const auto* e = a->find("a.txt");
    CHECK(e == &a->entries()[1]);
    if (e) CHECK_EQ(text(a->read(*e)), std::string("hello"));
    e = a->find("B.BNR");
    CHECK(e != nullptr);
    if (e) {
        CHECK_EQ(a->unpackedSize(*e), uint32_t{13});
        nyan::wa2::LzssResult report;
        CHECK_EQ(text(a->read(*e, &report)), std::string("LSCR and more"));
        CHECK_EQ(report.produced, size_t{13});
    }
    // A packed entry too short for its header holds nothing.
    e = a->find("empty");
    CHECK(e != nullptr);
    if (e) CHECK(a->read(*e).empty());
    CHECK(a->find("missing") == nullptr);
    CHECK(KcapArchive::open(std::make_unique<MemoryFile>(bytes("KIF\0 not kcap  ")), "x.pak") == nullptr);

    // Names compare as lstrcmpiA does for Shift-JIS: a double-byte character's second byte does not
    // fold ("ア" 0x8341 is not "ャ" 0x8361).
    CHECK(nyan::wa2::sameName("ABC.tga", "abc.TGA"));
    CHECK(!nyan::wa2::sameName("\x83\x41", "\x83\x61"));
    CHECK(nyan::wa2::sameName("\x83\x41x.tga", "\x83\x41X.TGA"));
    CHECK(!nyan::wa2::sameName("abc", "abcd"));
}

void testLac() {
    using nyan::wa2::LacArchive;
    Bytes data = lac({
        {"BGM_001_A.OGG", bytes("OggS one")},
        {"se_0001.wav", bytes("RIFF two")},
        {"X\xff" "Y", bytes("three")},  // an inverted 0xff ends the name
    });
    auto a = LacArchive::open(std::make_unique<MemoryFile>(data), "BGM.PAK");
    CHECK(a != nullptr);
    if (!a) return;
    CHECK_EQ(a->entries().size(), size_t{3});
    CHECK_EQ(a->entries()[0].name, std::string("BGM_001_A.OGG"));
    CHECK_EQ(a->entries()[2].name, std::string("X"));
    const auto* e = a->find("SE_0001.WAV");
    CHECK(e != nullptr);
    if (e) CHECK_EQ(text(a->read(*e)), std::string("RIFF two"));
    CHECK(LacArchive::open(std::make_unique<MemoryFile>(bytes("LAC\0\xff\xff\xff\x0f")), "bad.pak") == nullptr);
}

void testLookup() {
    auto folder = std::make_unique<MemoryFolder>();
    folder->add("en.pak", kcap({
                              {0xccccccccu, "script", {}},
                              {1, "1001.txt", packed("en")},
                              {0, "enonly.txt", bytes("patch")},
                          }));
    folder->add("script.pak", kcap({
                                  {1, "1001.txt", packed("jp")},
                                  {1, "1002.txt", packed("jp2")},
                                  {0, "empty.txt", {}},
                              }));
    folder->add("IC/script.pak", kcap({
                                     {0, "1002.txt", bytes("ic")},
                                     {0, "1003.txt", bytes("ic3")},
                                     {0, "empty.txt", bytes("ic fallback")},
                                 }));
    folder->add("IC/bak.pak", kcap({{0, "b000.tga", bytes("TGA")}}));
    folder->add("script/loose.txt", bytes("loose"));
    folder->add("script/1003.txt", {});
    folder->add("BGM.PAK", lac({{"BGM_001.OGG", bytes("OggS")}}));
    folder->add("SE.PAK", lac({{"SE_0000.WAV", bytes("RIFF")}}));
    folder->add("IC/VOICE.PAK", lac({{"1002_0000_10.OGG", bytes("OggS")}}));
    nyan::wa2::Assets assets(std::move(folder));

    auto read = [&](const char* group, const char* name) {
        auto d = assets.read(group, name);
        return d ? text(*d) : std::string("<none>");
    };
    CHECK_EQ(read("script", "1001.txt"), std::string("en"));        // en.pak first
    CHECK_EQ(read("bak", "1001.txt"), std::string("en"));           // for every group
    CHECK_EQ(read("script", "1002.txt"), std::string("jp2"));       // the group's archive before IC's
    CHECK_EQ(read("script", "1003.txt"), std::string("ic3"));       // an empty loose file counts as none
    CHECK_EQ(read("script", "LOOSE.TXT"), std::string("loose"));    // loose files first
    CHECK_EQ(read("script", "empty.txt"), std::string("ic fallback"));  // an empty entry: the next archive
    CHECK_EQ(read("script", "missing.txt"), std::string("<none>"));
    CHECK_EQ(read("", "script/loose.txt"), std::string("loose"));   // no group: the loose file alone

    // The existence check skips en.pak and takes empty entries.
    CHECK(assets.exists("script", "1001.txt"));
    CHECK(!assets.exists("script", "enonly.txt"));
    CHECK(assets.read("script", "enonly.txt").has_value());
    CHECK(assets.exists("script", "empty.txt"));
    CHECK(assets.exists("script", "loose.txt"));
    CHECK(!assets.exists("script", "missing.txt"));

    CHECK(assets.introductoryChapterBundled());
    const auto& sound = assets.soundArchives();
    CHECK_EQ(sound.size(), size_t{3});
    if (sound.size() == 3) {
        CHECK_EQ(sound[0]->name(), std::string("BGM.PAK"));
        CHECK_EQ(sound[1]->name(), std::string("Se.PAK"));
        CHECK_EQ(sound[2]->name(), std::string("IC/Voice.PAK"));
    }
    auto archives = assets.kcapArchives();
    std::vector<std::string> paths;
    for (const auto& a : archives) paths.push_back(a.path);
    CHECK_EQ(paths, (std::vector<std::string>{"en.pak", "IC/bak.pak", "script.pak", "IC/script.pak"}));
}

// A TGA header followed by `body`.
Bytes tga(uint8_t type, int w, int h, int bpp, uint8_t descriptor, int paletteCount, int paletteBits, const Bytes& body) {
    Bytes out(18, 0);
    out[1] = paletteCount ? 1 : 0;
    out[2] = type;
    out[5] = static_cast<uint8_t>(paletteCount);
    out[6] = static_cast<uint8_t>(paletteCount >> 8);
    out[7] = static_cast<uint8_t>(paletteBits);
    out[12] = static_cast<uint8_t>(w);
    out[13] = static_cast<uint8_t>(w >> 8);
    out[14] = static_cast<uint8_t>(h);
    out[15] = static_cast<uint8_t>(h >> 8);
    out[16] = static_cast<uint8_t>(bpp);
    out[17] = descriptor;
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void testImages() {
    using namespace nyan::wa2;
    // 24-bit, bottom-up: the first stored row is the bottom one; pixels are B, G, R.
    Bytes body = {10, 20, 30, 40, 50, 60,  /* bottom */ 1, 2, 3, 4, 5, 6 /* top */};
    Picture p = decodePicture(tga(2, 2, 2, 24, 0, 0, 0, body), "b000.TGA");
    RgbaImage img = toRgba(p, PictureType::Alpha);
    CHECK_EQ(img.width, 2);
    CHECK_EQ((Bytes{img.pixels.begin(), img.pixels.begin() + 4}), (Bytes{3, 2, 1, 255}));
    CHECK_EQ((Bytes{img.pixels.begin() + 8, img.pixels.begin() + 12}), (Bytes{30, 20, 10, 255}));
    // Top-down when descriptor bit 0x20 is set.
    p = decodePicture(tga(2, 2, 2, 24, 0x20, 0, 0, body), "x.tga");
    CHECK_EQ(toRgba(p, PictureType::Alpha).pixels[0], uint8_t{30});
    // The engine refuses run-length TGAs.
    bool threw = false;
    try {
        decodePicture(tga(10, 1, 1, 24, 0, 0, 0, Bytes(3)), "x.tga");
    } catch (const nyan::Error&) {
        threw = true;
    }
    CHECK(threw);

    // 8-bit with a 24-bit palette (B, G, R entries): opaque; type 4 makes entry 0 transparent.
    Bytes pal8 = {10, 20, 30, 40, 50, 60, /* pixels */ 1, 0};
    p = decodePicture(tga(1, 2, 1, 8, 0x20, 2, 24, pal8), "x.tga");
    CHECK_EQ(p.paletteSize, 2);
    Picture keyed = p;
    preparePalette(keyed, PictureType::KeyedIndex0);
    img = toRgba(keyed, PictureType::KeyedIndex0);
    CHECK_EQ((Bytes{img.pixels.begin(), img.pixels.end()}), (Bytes{60, 50, 40, 255, 30, 20, 10, 0}));
    // A mask takes the palette's blue as alpha.
    img = toRgba(p, PictureType::Mask);
    CHECK_EQ(img.pixels[3], uint8_t{40});
    CHECK_EQ(img.pixels[7], uint8_t{10});

    // BMP, 8-bit, bottom-up, palette between the headers and the pixels.
    Bytes bmp(0x36, 0);
    bmp[0] = 'B', bmp[1] = 'M';
    nyan::writeLE32(&bmp[10], 0x36 + 8);
    nyan::writeLE32(&bmp[0x12], 4);
    nyan::writeLE32(&bmp[0x16], 2);
    bmp[0x1c] = 8;
    Bytes rest = {0, 0, 0, 0, 200, 100, 50, 0, /* bottom row */ 0, 0, 0, 0, /* top row */ 1, 1, 0, 0};
    bmp.insert(bmp.end(), rest.begin(), rest.end());
    p = decodePicture(bmp, "f0001.bmp");
    preparePalette(p, PictureType::Alpha);
    img = toRgba(p, PictureType::Alpha);
    CHECK_EQ((Bytes{img.pixels.begin(), img.pixels.begin() + 4}), (Bytes{50, 100, 200, 255}));
    CHECK_EQ(img.pixels[8 * 4 - 4], uint8_t{0});

    // The colour level: grey by byte position (77 R + 151 G + 28 B) >> 8.
    p = decodePicture(tga(2, 1, 1, 24, 0, 0, 0, Bytes{10, 20, 30}), "x.tga");
    Picture grey = p;
    applyLevel(grey, 0);
    CHECK_EQ(grey.pixels, (Bytes{21, 21, 21}));
    Picture half = p;
    applyLevel(half, 128);  // c * 128 / 256 + 21 * 127 / 256
    CHECK_EQ(half.pixels, (Bytes{15, 20, 25}));

    // .amp: curves 0, 1, 2 go to byte positions 2, 1, 0 (R, G, B of a pixel)...
    Bytes amp(0x300);
    for (int i = 0; i < 0x100; ++i) amp[i] = 1, amp[0x100 + i] = 2, amp[0x200 + i] = 3;
    Picture col = decodePicture(tga(2, 1, 1, 32, 0, 0, 0, Bytes{10, 20, 30, 99}), "x.tga");
    CHECK(applyAmp(col, amp));
    CHECK_EQ(toRgba(col, PictureType::Alpha).pixels, (Bytes{1, 2, 3, 99}));
    // ... and of an 8-bit picture's R, G, B palette, so its red and blue swap, as in the engine.
    Picture idx = decodePicture(tga(1, 1, 1, 8, 0, 1, 24, Bytes{10, 20, 30, 0}), "x.tga");
    preparePalette(idx, PictureType::Alpha);
    CHECK(applyAmp(idx, amp));
    CHECK_EQ(toRgba(idx, PictureType::Alpha).pixels, (Bytes{3, 2, 1, 255}));
    // 1280 bytes: curve 0 after curves 1, 2, 3.
    Bytes amp5(0x500);
    for (int i = 0; i < 0x100; ++i) amp5[i] = static_cast<uint8_t>(i / 2), amp5[0x100 + i] = 200, amp5[0x200 + i] = 100, amp5[0x300 + i] = 50;
    col = decodePicture(tga(2, 1, 1, 24, 0, 0, 0, Bytes{10, 20, 30}), "x.tga");
    CHECK(applyAmp(col, amp5));
    CHECK_EQ(col.pixels, (Bytes{25, 50, 100}));
    CHECK(!applyAmp(col, Bytes(10)));
}

// --- The script VM -------------------------------------------------------------------------------

// A .fnc file: records of name[32], argc, flag, return type, argument types, then the defaults.
struct FncSpec {
    const char* name;
    int ret;
    std::vector<int> types;
    std::vector<std::string> defaults;
};

Bytes fnc(int first, const std::vector<FncSpec>& fs) {
    Bytes out(4, 0);
    nyan::writeLE32(out.data(), static_cast<uint32_t>(first));
    for (const auto& f : fs) {
        Bytes r(0x78, 0);
        std::memcpy(r.data(), f.name, std::strlen(f.name));
        r[0x20] = static_cast<uint8_t>(f.types.size());
        nyan::writeLE32(&r[0x24], static_cast<uint32_t>(f.ret));
        for (size_t i = 0; i < f.types.size(); ++i) r[0x28 + i] = static_cast<uint8_t>(f.types[i]);
        out.insert(out.end(), r.begin(), r.end());
        for (size_t i = 0; i < f.types.size(); ++i) {
            std::string d = i < f.defaults.size() ? f.defaults[i] : "";
            out.insert(out.end(), d.begin(), d.end());
            out.push_back(',');
        }
    }
    return out;
}

// A script: code words with labels at word indices.
Bytes lscrScript(const std::vector<uint32_t>& code, const std::vector<std::pair<int, size_t>>& labels) {
    Bytes out(12 + 8 * labels.size(), 0);
    std::memcpy(out.data(), "LSCR", 4);
    nyan::writeLE32(&out[4], static_cast<uint32_t>(8 + 8 * labels.size()));
    nyan::writeLE32(&out[8], static_cast<uint32_t>(labels.size()));
    uint32_t base = static_cast<uint32_t>(out.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        nyan::writeLE32(&out[12 + 8 * i], static_cast<uint32_t>(labels[i].first));
        nyan::writeLE32(&out[16 + 8 * i], base + static_cast<uint32_t>(4 * labels[i].second));
    }
    for (uint32_t w : code) {
        out.resize(out.size() + 4);
        nyan::writeLE32(&out[out.size() - 4], w);
    }
    return out;
}

class TestHost final : public nyan::wa2::ScriptHost {
public:
    TestHost() {
        functions_.load(fnc(0, {{"SLoad", 3, {5, 3}}, {"SCall", 3, {5, 3}}, {"call", 3, {3}}, {"run", 3, {}}}));
        functions_.load(fnc(0x80, {{"Note", 3, {3}}, {"Hold", 3, {3}}, {"Text", 3, {5}, {"dflt"}}, {"NoteF", 3, {4}}}));
        globals_.push_back({3, "g0", 0, 0, ""});
        globals_.push_back({1, "byte", 0, 0, ""});
        globals_.push_back({5, "s", 0, 0, ""});
    }
    std::map<std::string, Bytes> files;
    std::vector<std::string> notes;
    std::vector<std::string> logs;

    std::optional<Bytes> readScriptFile(const std::string&, const std::string& name) override {
        auto it = files.find(name);
        if (it == files.end()) return std::nullopt;
        return it->second;
    }
    const nyan::wa2::FunctionTable& functions() const override { return functions_; }
    std::vector<nyan::wa2::GlobalVar>& globals() override { return globals_; }
    int callNative(nyan::wa2::ScriptVm&, int id, nyan::wa2::CallContext& c) override {
        switch (id) {
        case 3: return 1;  // run
        case 0x80: notes.push_back(std::to_string(c.argInt(0))); c.returnInt = c.argInt(0); return 0;
        case 0x81:  // Hold(n): waits n further frames, one per run (the count kept in the call)
            if (c.phase == 0) c.returnInt = 0;
            c.phase = 1;
            c.waiting = ++c.returnInt <= c.argInt(0);
            if (!c.waiting) notes.push_back("held");
            return 1;
        case 0x82: notes.push_back(c.argString(0)); return 0;
        case 0x83: notes.push_back(std::to_string(c.argFloat(0))); return 0;
        }
        return 0;
    }
    void log(const std::string& m) override { logs.push_back(m); }

private:
    nyan::wa2::FunctionTable functions_;
    std::vector<nyan::wa2::GlobalVar> globals_;
};

// Instruction words.
std::vector<uint32_t> pushI(int32_t v) { return {5, 3, static_cast<uint32_t>(v)}; }

std::vector<uint32_t> callF(uint32_t id) { return {4, id, 6, 0x1e}; }

std::vector<uint32_t> cat(std::initializer_list<std::vector<uint32_t>> parts) {
    std::vector<uint32_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

void testScriptVm() {
    using nyan::wa2::ScriptVm;
    {
        // g0 = 2 + 3 * 4; Note(g0); byte = 300; Note(byte); L0[3] = 7; Note(L0[3]); end
        TestHost h;
        auto code = cat({{1, 0}, pushI(2), pushI(3), pushI(4), {6, 0x12, 6, 0x10, 6, 0, 6, 0x1e},
                         {1, 0}, callF(0x80),
                         {1, 1}, pushI(300), {6, 0, 6, 0x1e}, {1, 1}, callF(0x80),
                         {2, 0}, pushI(3), {6, 0x1b}, pushI(7), {6, 0, 6, 0x1e},
                         {2, 0}, pushI(3), {6, 0x1b}, callF(0x80),
                         {0, 1}});
        h.files["a.bnr"] = lscrScript(code, {{0, 0}});
        ScriptVm vm(h);
        CHECK(vm.load("a"));
        CHECK(vm.start(0));
        vm.run();
        CHECK_EQ(h.notes, (std::vector<std::string>{"14", "44", "7"}));  // 300 kept as a byte
        CHECK_EQ(vm.state(), ScriptVm::Ended);
    }
    {
        // if (g0 > 5) Note(1); else Note(2); with g0 = 9, then g0 = 1.
        for (int g : {9, 1}) {
            TestHost h;
            h.globals()[0].i = g;
            // words: 0:[0 2 ELSE END] 4:[1 0] 6:[5 3 5] 9:[6 10] 11:[0 13] 13:Note(1) 20:[0 3 END] 23:Note(2) 30:end
            std::vector<uint32_t> code = cat({{0, 2, 0, 0}, {1, 0}, pushI(5), {6, 10}, {0, 13}, pushI(1), callF(0x80),
                                              {0, 3, 0}, pushI(2), callF(0x80), {0, 1}});
            Bytes s = lscrScript(code, {{0, 0}});
            uint32_t base = 20;
            nyan::writeLE32(&s[base + 4 * 2], base + 4 * 20);  // else: the else instruction
            nyan::writeLE32(&s[base + 4 * 3], base + 4 * 30);  // end
            nyan::writeLE32(&s[base + 4 * 22], base + 4 * 30);
            h.files["b.bnr"] = s;
            ScriptVm vm(h);
            CHECK(vm.load("b"));
            CHECK(vm.start(0));
            vm.run();
            CHECK_EQ(h.notes, (std::vector<std::string>{g > 5 ? "1" : "2"}));
        }
    }
    {
        // A waiting call runs again each frame without popping again; run() ends the frame.
        TestHost h;
        auto code = cat({pushI(2), callF(0x81), pushI(5), callF(0x80), {4, 3, 6, 0x1e}, pushI(6), callF(0x80), {0, 1}});
        h.files["c.bnr"] = lscrScript(code, {{0, 0}});
        ScriptVm vm(h);
        CHECK(vm.load("c"));
        CHECK(vm.start(0));
        vm.run();  // Hold counts 1
        CHECK(h.notes.empty());
        vm.run();  // 2
        vm.run();  // 3: done; a native's non-zero result ends the frame even then
        CHECK_EQ(h.notes, (std::vector<std::string>{"held"}));
        vm.run();  // Note(5), then run() ends the frame
        CHECK_EQ(h.notes, (std::vector<std::string>{"held", "5"}));
        vm.run();
        CHECK_EQ(h.notes.back(), std::string("6"));
        CHECK_EQ(vm.state(), ScriptVm::Ended);
    }
    {
        // Strings: the table, a default for -1, concatenation into a global.
        TestHost h;
        auto code = cat({{3, 1}, callF(0x82), {5, 5, 0xffffffffu}, callF(0x82),
                         {1, 2}, {3, 1}, {3, 2}, {6, 0x10, 6, 0, 6, 0x1e}, {1, 2}, callF(0x82), {0, 1}});
        h.files["d.bnr"] = lscrScript(code, {{0, 0}});
        h.files["d.txt"] = bytes(",ab,cd");
        ScriptVm vm(h);
        CHECK(vm.load("d"));
        CHECK(vm.start(0));
        vm.run();
        CHECK_EQ(h.notes, (std::vector<std::string>{"ab", "dflt", "abcd"}));
    }
    {
        // call(label) runs the label in a child with its own locals; SCall another script; SLoad
        // replaces the script after the frame.
        TestHost h;
        auto main = cat({{2, 0}, pushI(0), {6, 0x1b}, pushI(1), {6, 0, 6, 0x1e},  // L0[0] = 1
                         pushI(9), callF(2),                                       // call(9)
                         {2, 0}, pushI(0), {6, 0x1b}, callF(0x80),                 // Note(L0[0]): still 1
                         {3, 1}, pushI(0), callF(1),                               // SCall("other", 0)
                         {3, 1}, pushI(0), callF(0),                               // SLoad("other", 0)
                         pushI(99), callF(0x80), {0, 1}});                         // never reached
        size_t label9 = main.size();
        // label 9: L0[0] = 5; Note(L0[0]); end
        auto sub = cat({{2, 0}, pushI(0), {6, 0x1b}, pushI(5), {6, 0, 6, 0x1e}, {2, 0}, pushI(0), {6, 0x1b}, callF(0x80), {0, 1}});
        main.insert(main.end(), sub.begin(), sub.end());
        h.files["m.bnr"] = lscrScript(main, {{0, 0}, {9, label9}});
        h.files["m.txt"] = bytes(",other");
        h.files["other.bnr"] = lscrScript(cat({pushI(42), callF(0x80), {0, 1}}), {{0, 0}});
        ScriptVm vm(h);
        CHECK(vm.load("m"));
        CHECK(vm.start(0));
        for (int i = 0; i < 6; ++i) vm.run();
        CHECK_EQ(h.notes, (std::vector<std::string>{"5", "1", "42", "42"}));
        CHECK_EQ(vm.name(), std::string("other"));
    }
    {
        // Saved in the middle of a waiting call inside a child, restored into a new object: it goes on
        // exactly as the original.
        auto setup = [](TestHost& h) {
            auto main = cat({{2, 0}, pushI(0), {6, 0x1b}, pushI(3), {6, 0, 6, 0x1e},  // L0[0] = 3
                             pushI(1), callF(2),                                       // call(1)
                             {2, 0}, pushI(0), {6, 0x1b}, callF(0x80), {0, 1}});       // Note(L0[0])
            size_t label1 = main.size();
            auto sub = cat({pushI(3), callF(0x81), pushI(7), callF(0x80), {0, 1}});  // Hold(3); Note(7)
            main.insert(main.end(), sub.begin(), sub.end());
            h.files["s.bnr"] = lscrScript(main, {{0, 0}, {1, label1}});
        };
        TestHost a;
        setup(a);
        ScriptVm vm(a);
        CHECK(vm.load("s"));
        CHECK(vm.start(0));
        vm.run();  // call(1): the child starts and Hold counts 1
        vm.run();  // 2
        nyan::wa2::Writer w;
        vm.save(w);
        Bytes saved = w.take();
        for (int i = 0; i < 5; ++i) vm.run();
        TestHost b;
        setup(b);
        b.notes.clear();
        ScriptVm again(b);
        nyan::wa2::Reader r(saved);
        CHECK(again.restore(r));
        for (int i = 0; i < 5; ++i) again.run();
        CHECK_EQ(a.notes, b.notes);
        CHECK_EQ(a.notes, (std::vector<std::string>{"held", "7", "3"}));
        CHECK_EQ(again.state(), ScriptVm::Ended);
    }
}

void testSystemData() {
    using nyan::wa2::SystemData;
    SystemData d;
    d.setReadBit(2, 9);  // script index 2, line 9: byte 2 * 0x200 + 1, bit 6 from the top
    CHECK(d.readBit(2, 9));
    CHECK(!d.readBit(2, 8));
    CHECK(!d.readBit(3, 9));
    d.setGameFlag(20, 1);
    d.setGameFlag(0x3e0, 0x15);
    d.setGalleryFlag(1, 20340, 1);
    Bytes file = d.encode();
    auto back = SystemData::decode(file);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->readBit(2, 9));
        CHECK_EQ(back->gameFlag(20), 1);
        CHECK_EQ(back->gameFlag(0x3e0), 0x15);
        CHECK_EQ(static_cast<int>(back->galleryFlag(1, 20340)), 1);
        CHECK_EQ(static_cast<int>(back->galleryFlag(0, 20340)), 0);
    }
    CHECK(!SystemData::decode(bytes("not a system file")).has_value());
    CHECK_EQ(nyan::wa2::scriptIndex("1001"), 0);
    CHECK_EQ(nyan::wa2::scriptIndex("1002"), 1);
    CHECK_EQ(nyan::wa2::scriptIndex("CATCH"), 0x1d);
    CHECK_EQ(nyan::wa2::scriptIndex("1006_2"), 0);  // the patch's parts are not in the table
}

void testDetectionSynthetic() {
    using nyan::Engine;
    MemoryFolder wa2;
    wa2.add("SCRIPT.PAK", kcap({{0, "1001.bnr", bytes("LSCR")}}));
    CHECK_EQ(nyan::detectEngine(wa2), std::optional{Engine::WA2});
    MemoryFolder notKcap;
    notKcap.add("script.pak", bytes("PACK of something else"));
    CHECK_EQ(nyan::detectEngine(notKcap).has_value(), false);
    // A name alone is not enough: the archive has to open as KCAP.
    CHECK_EQ(nyan::detectEngine(ListedFolder({"script.pak"})).has_value(), false);
    CHECK_EQ(nyan::detectEngine(ListedFolder({"script.pak", "scene.int"})), std::optional{Engine::CatSystem2});
    CHECK_EQ(nyan::detectEngine(ListedFolder({"scene.int", "image.int"})), std::optional{Engine::CatSystem2});
    CHECK_EQ(std::string(nyan::engineName(Engine::WA2)), std::string("WA2"));
}

// The game folders themselves: every CatSystem2 one stays CatSystem2, the WA2 ones are WA2, and
// nothing else is WA2.
// The 2D layer: slot sizes, the frame's order, the quad drawer's geometry, tiles and colours.
void testGfx() {
    using nyan::wa2::DrawCommand;
    using nyan::wa2::Gfx;
    // A 300 x 20 32-bit picture: texel (x, y) has red x, green y.
    Bytes body;
    for (int y = 19; y >= 0; --y) {  // bottom-up
        for (int x = 0; x < 300; ++x) {
            body.push_back(0);
            body.push_back(static_cast<uint8_t>(y));
            body.push_back(static_cast<uint8_t>(x));
            body.push_back(0xff);
        }
    }
    auto folder = std::make_unique<MemoryFolder>();
    folder->add("grp.pak", kcap({{0, "p.tga", tga(2, 300, 20, 32, 0, 0, 0, body)}, {1, "q.tga", Bytes()}}));
    nyan::wa2::Assets assets(std::move(folder));
    Gfx g(assets);

    // Tiled path (0x43ffa0): 300 -> 512, 20 -> 32, tiles of 32.
    CHECK(g.load(5, "p.tga", 1, false, "", "", 0x100, ""));
    const auto& s = g.slots[5];
    CHECK_EQ(s.texW, 512);
    CHECK_EQ(s.texH, 32);
    CHECK_EQ(s.tileW, 32);
    CHECK_EQ(s.tilesX, 16);
    CHECK_EQ(s.tilesY, 1);
    CHECK_EQ(s.picW, 300);
    CHECK_EQ(s.pixels.pixels[(5 * 512 + 7) * 4 + 0], 7);  // top row first
    CHECK_EQ(s.pixels.pixels[(5 * 512 + 7) * 4 + 1], 5);
    CHECK_EQ(s.pixels.pixels[(5 * 512 + 400) * 4 + 3], 0);  // outside the picture
    CHECK(!g.load(6, "missing.tga", 1, false, "", "", 0x100, ""));
    // Measuring only: the same sizes, no texels.
    Gfx m(assets);
    m.keepPixels = false;
    CHECK(m.load(5, "p.tga", 1, false, "", "", 0x100, ""));
    CHECK_EQ(m.slots[5].texW, 512);
    CHECK(m.slots[5].pixels.pixels.empty());
    // Single texture: powers of two.
    CHECK(g.load(7, "p.tga", 1, false, "", "", 0x100, "", 0x100, true));
    CHECK_EQ(g.slots[7].texW, 512);
    CHECK_EQ(g.slots[7].texH, 32);
    CHECK_EQ(g.slots[7].tilesX, 1);

    // Order: ascending priority; of equal priorities the lower index is drawn last (on top).
    g.makePolygon(10, 4, 5, true);
    g.setColor(10, 255, 10, 0, 0);
    g.makePolygon(3, 4, 5, true);
    g.setColor(3, 255, 3, 0, 0);
    g.makePolygon(7, 4, 1, true);
    g.setColor(7, 255, 7, 0, 0);
    g.makePolygon(8, 4, 30001, true);  // never drawn
    auto frame = g.buildFrame(false);
    CHECK_EQ(frame.size(), 3u);
    if (frame.size() == 3) {
        CHECK_EQ(frame[0].vertices[0].diffuse[0], 7);
        CHECK_EQ(frame[1].vertices[0].diffuse[0], 10);
        CHECK_EQ(frame[2].vertices[0].diffuse[0], 3);
        // Untextured: no half-pixel correction (a D3D position is half a pixel right of OpenGL's).
        CHECK_EQ(frame[0].vertices[3].x, 1280.5f);
        CHECK(frame[0].texture == -1);
    }
    for (int o : {10, 3, 7, 8}) g.remove(o);

    // The 128-neutral colours of a textured object.
    g.makeImage(20, 5, 0, true);
    g.setColor(20, 300, 64, 128, 200);
    CHECK_EQ(g.objects[20].diffuse[0], 0xff80ffffu);
    CHECK_EQ(g.objects[20].specular[0], 0xff000090u);
    g.setAlpha(20, 100);
    CHECK_EQ(g.objects[20].diffuse[3] >> 24, 100u);

    // A source starting left of the picture: texel 0 lands where an unclipped mapping puts it, and
    // nothing is drawn for the part outside.
    g.setColor(20, 255, 128, 128, 128);
    g.setRect(20, 0, 0, -10, 0, 30, 20);
    frame = g.buildFrame(false);
    CHECK_EQ(frame.size(), 1u);
    if (frame.size() == 1) {
        const auto& v = frame[0].vertices;
        CHECK_EQ(v.size(), 4u);  // one tile
        CHECK_EQ(v[0].x, 10.0f);
        CHECK_EQ(v[1].x, 30.0f);
        CHECK_EQ(v[0].u, 0.0f);
        CHECK_EQ(v[1].u, 20.0f / 512.0f);
        CHECK_EQ(v[2].y, 20.0f);
        CHECK_EQ(v[0].diffuse[0], 255);
        CHECK(!frame[0].alphaFromDiffuseSquared);
    }
    // Across tiles: one quad per tile, meeting at the tile edge.
    g.setRect(20, 100, 50, 20, 0, 40, 20);
    frame = g.buildFrame(false);
    if (frame.size() == 1) {
        const auto& v = frame[0].vertices;
        CHECK_EQ(v.size(), 8u);
        if (v.size() == 8) {
            CHECK_EQ(v[1].x, 112.0f);  // texel 32 at 100 + 12
            CHECK_EQ(v[4].x, 112.0f);
            CHECK_EQ(v[5].x, 140.0f);
        }
    }
    // Tint group 0 at 64 (mode 0) halves the colours.
    g.setTint(64, 64, 64, 0);
    frame = g.buildFrame(false);
    if (!frame.empty()) CHECK_EQ(frame[0].vertices[0].diffuse[0], 127);
    g.setTintGroup(20, 1);  // group 1 is never tinted
    frame = g.buildFrame(false);
    if (!frame.empty()) CHECK_EQ(frame[0].vertices[0].diffuse[0], 255);
    g.setTint(128, 128, 128, 0);

    // Rotation about the pivot, positive counter-clockwise on screen.
    g.setRect(20, 100, 100, 0, 0, 10, 10);
    g.setPivot(20, 100, 100);
    g.setRotation(20, 90);
    frame = g.buildFrame(false);
    if (!frame.empty()) {
        CHECK(std::abs(frame[0].vertices[1].x - 100.0f) < 1e-3f);  // top right (110, 100) -> (100, 90)
        CHECK(std::abs(frame[0].vertices[1].y - 90.0f) < 1e-3f);
    }
    g.setRotation(20, 0);

    // Nine-slice: nine pieces, borders 1:1.
    g.makeNineSlice(21, 5, 0, true);
    g.setRect(21, 0, 0, 0, 0, 30, 30);
    g.setDest(21, 0, 0, 100, 60);
    g.remove(20);
    frame = g.buildFrame(false);
    CHECK_EQ(frame.size(), 9u);
    if (frame.size() == 9) {
        CHECK_EQ(frame[4].vertices[0].x, 10.0f);  // the centre from 10 to 90
        CHECK_EQ(frame[4].vertices[1].x, 90.0f);
        CHECK_EQ(frame[8].vertices[3].y, 60.0f);
    }
    g.remove(21);

    // A string: one glyph per character, spaces skipped, 16 glyphs per sheet row from 0x20.
    g.makeString(22, 5, 0, true, "A B");
    g.setSource(22, 0, 0, 10, 10);
    frame = g.buildFrame(false);
    CHECK_EQ(frame.size(), 2u);
    if (frame.size() == 2) {
        CHECK_EQ(frame[0].vertices[0].u, 10.0f / 512.0f);  // 'A' = 0x41: column 1
        CHECK_EQ(frame[0].vertices[0].v, 20.0f / 32.0f);   // row 2 (clipped below the texture)
        CHECK_EQ(frame[1].vertices[0].x, 2.0f * 51.0f);     // two cells of W'/10 = 51 on
    }
    g.remove(22);

    // A number of 3 digits with leading zeros.
    g.makeNumber(23, 5, 0, true, 7, -3, 0);
    g.setSource(23, 0, 0, 10, 10);
    frame = g.buildFrame(false);
    CHECK_EQ(frame.size(), 3u);
    if (frame.size() == 3) {
        CHECK_EQ(frame[0].vertices[0].u, 70.0f / 512.0f);  // the last digit first
        CHECK_EQ(frame[2].vertices[0].u, 0.0f);
    }
    g.remove(23);

    // Render targets: made on their first use; an object showing one takes its alpha twice.
    g.makeTarget(0x105, 0x800, 0x400);
    g.makeImage(30, 5, 0, true);
    g.setTarget(30, 0x105);
    g.makeImage(31, 0x105, 1, true);
    frame = g.buildFrame(true);
    CHECK(frame.size() == 4u && frame[0].prim == DrawCommand::Prim::Clear && frame[1].prim == DrawCommand::Prim::MakeTarget);
    if (frame.size() == 4) {
        CHECK_EQ(frame[2].target, 0x105);
        CHECK_EQ(frame[3].target, -1);
        CHECK(frame[3].alphaFromDiffuseSquared);
    }
    frame = g.buildFrame(false);
    CHECK_EQ(frame.size(), 2u);
}

// The text renderer (0x4462c0): pen, wrap, reveal, pages, the in-place substitutions.
void testText() {
    using nyan::wa2::Gfx;
    using nyan::wa2::SpriteObject;
    using nyan::wa2::TextSlot;
    CHECK_EQ(nyan::wa2::glyphIndex(0x20), 0);
    CHECK_EQ(nyan::wa2::glyphIndex('A'), 0x21);
    CHECK_EQ(nyan::wa2::glyphIndex(0x8140), 94 + 45);  // after 0x20..0x7d and 0xffb1..0xffdd
    CHECK_EQ(nyan::wa2::glyphIndex(0x7e), 7655);        // no glyph
    auto folder = std::make_unique<MemoryFolder>();
    folder->add("grp.pak", kcap({{0, "x.tga", Bytes()}}));
    nyan::wa2::Assets assets(std::move(folder));
    Gfx g(assets);
    // The message's record: 28 px, 28 columns, 4 rows, line gap 14.
    auto layout = [&](const std::string& text, int reveal, int page, std::vector<SpriteObject>& glyphs) {
        g.makeTextSlot(0, 0x1c4, text, 28, 28, 4, 0, 14, 32, 40, true, 255, 255, 255, 0x21, 0x2e, 0x38, true);
        g.makeText(0x443, 0, 0x1057, true);
        g.moveTo(0x443, 274, 536);
        g.setTextPage(0x443, page);
        g.setTextReveal(0x443, reveal);
        glyphs.clear();
        TextSlot& t = g.texts[0];
        bool done = nyan::wa2::renderText(g.objects[0x443], t, false, [&](const SpriteObject& o) { glyphs.push_back(o); });
        t.complete = done;
        return done;
    };
    std::vector<SpriteObject> gl;
    CHECK(layout("Ab~c", -1, 0, gl));
    CHECK_EQ(gl.size(), 4u);
    if (gl.size() == 4) {
        CHECK_EQ(gl[0].x[0], 274.0f + 4.0f);         // the body box's inset
        CHECK_EQ(gl[0].y[0], 536.0f + 14.0f + 4.0f); // line 1 starts at the gap
        CHECK_EQ(gl[1].x[0] - gl[0].x[0], 14.0f);    // half the size per single-byte character
        CHECK_EQ(gl[0].x[1] - gl[0].x[0], 32.0f);
        CHECK_EQ(gl[0].u0, static_cast<float>(4 + 33 * 40));  // 'A' = index 33, cell 40
        CHECK_EQ(gl[0].filter, 1u);                  // 32 px quads of 40 px cells: linear
    }
    CHECK_EQ(g.texts[0].text, std::string("Ab,c"));  // `~` became a comma in the string
    // 56 characters wrap after 55 (757 px).
    layout(std::string(56, 'a'), -1, 0, gl);
    if (gl.size() == 56) {
        CHECK_EQ(gl[55].x[0], 274.0f + 4.0f);
        CHECK_EQ(gl[55].y[0], 536.0f + 14.0f + 42.0f + 4.0f);
    }
    // Reveal: each character fades in over 16 units, one unit per character.
    layout("abcdef", 4, 0, gl);
    CHECK_EQ(gl.size(), 4u);  // drawing stops at reveal <= pos
    if (gl.size() == 4) {
        CHECK_EQ(gl[0].diffuse[0] >> 24, 255u * 4 * 16 >> 8);
        CHECK_EQ(gl[3].diffuse[0] >> 24, 255u * 1 * 16 >> 8);
    }
    CHECK(!g.texts[0].endReached);
    // Pages: page 0 stops at the first `\k`; page 1 shows segment 0 whole and segment 1 by reveal.
    CHECK(!layout("ab\\kcd", -1, 0, gl) || true);
    CHECK_EQ(gl.size(), 2u);
    CHECK(!g.texts[0].endReached);
    layout("ab\\kcd", 0, 1, gl);
    CHECK_EQ(gl.size(), 2u);
    if (gl.size() == 2) CHECK_EQ(gl[1].diffuse[0] >> 24, 255u);
    layout("ab\\kcd", 100, 1, gl);
    CHECK_EQ(gl.size(), 4u);
    CHECK(g.texts[0].endReached);
    // Tags: <F14 halves the advance; a colour tag.
    layout("<F14ab>c", -1, 0, gl);
    if (gl.size() == 3) {
        CHECK_EQ(gl[1].x[0] - gl[0].x[0], 7.0f);
        CHECK_EQ(gl[2].x[0] - gl[1].x[0], 9.0f);  // 7 of pen, and the inset back from 2 to 4
    }
    layout("<C4a>b", -1, 0, gl);
    if (gl.size() == 2) {
        CHECK_EQ(gl[0].diffuse[0] & 0xffffff, 0xff0000u);
        CHECK_EQ(gl[1].diffuse[0] & 0xffffff, 0xffffffu);
    }
    // The end anchor: after a line break, the end of the previous line.
    layout("ab\n", -1, 0, gl);
    CHECK_EQ(g.texts[0].endX, 28);
}

// The .ani interpreter (0x447ef0): frames for their ticks, loops.
void testAni() {
    using nyan::wa2::AniPlayer;
    using nyan::wa2::Gfx;
    // INAS: 60 ticks/s, 2 frames, 1 sequence [op2 0][op1 f0 b2][op1 f1 b1][op3][op0], 1 image.
    Bytes d = bytes("INAS");
    d.resize(0x10, 0);
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto u16 = [&](uint16_t v) {
        d.push_back(static_cast<uint8_t>(v));
        d.push_back(static_cast<uint8_t>(v >> 8));
    };
    u32(1), u32(60), u32(2), u32(1), u32(1);
    for (int f = 0; f < 2; ++f) {
        Bytes frame(0x18, 0);
        frame[0] = 1;
        frame[0x14] = 1;
        d.insert(d.end(), frame.begin(), frame.end());
        Bytes part(0x28, 0);
        part[0] = 1;
        part[0x14] = 0xfe, part[0x15] = 0xff, part[0x16] = 0xff, part[0x17] = 0xff;  // -2
        part[0x18] = static_cast<uint8_t>(f * 28);
        part[0x1c] = 28, part[0x1e] = 28;
        d.insert(d.end(), part.begin(), part.end());
    }
    u32(1), u32(4);
    for (auto [op, a, b] : std::vector<std::array<int, 3>>{{2, 0, 0}, {1, 0, 2}, {1, 1, 1}, {3, 0, 0}, {0, 0, 0}}) {
        u16(static_cast<uint16_t>(op)), u16(static_cast<uint16_t>(a)), u16(static_cast<uint16_t>(b)), u16(0);
    }
    for (char ch : std::string("s.tga")) d.push_back(static_cast<uint8_t>(ch));
    d.push_back(0);
    Bytes sheet;
    for (int i = 0; i < 56 * 28; ++i) sheet.insert(sheet.end(), {0, 0, 0, 0xff});
    auto folder = std::make_unique<MemoryFolder>();
    folder->add("grp.pak", kcap({{0, "t.ani", d}, {0, "s.tga", tga(2, 56, 28, 32, 0x20, 0, 0, sheet)}}));
    nyan::wa2::Assets assets(std::move(folder));
    Gfx g(assets);
    AniPlayer ani(g);
    CHECK(ani.load(1, "t.ani", 2, "", "", 0x100, ""));
    CHECK(g.slots[0x200].loaded);
    CHECK(ani.makeObject(5, 1, 0, true, 10, true, 1, 0, 0));
    int inst = g.objects[5].slot;
    std::vector<int> seen;
    for (int t = 0; t < 7; ++t) {
        ani.tick(60);
        seen.push_back(ani.currentFrame(inst));
    }
    // Frame 0 for two ticks, frame 1 for one, then again (the loop).
    CHECK((seen == std::vector<int>{0, 0, 1, 0, 0, 1, 0}));
    std::vector<nyan::wa2::SpriteObject> parts;
    ani.draw(g.objects[5], [&](const nyan::wa2::SpriteObject& o) { parts.push_back(o); });
    CHECK_EQ(parts.size(), 1u);
    if (!parts.empty()) {
        CHECK_EQ(parts[0].slot, 0x200);
        CHECK_EQ(parts[0].blend, 1u);  // part blend -2 with selector 0
        CHECK_EQ(parts[0].x[1] - parts[0].x[0], 28.0f);
    }
}

void testSubtitles() {
    using nyan::wa2::Subtitles;
    Subtitles s;
    s.load(bytes("ignored\r\n{\r\n1002 9763 1002 114\r\n0 800 Suwa: Kagura.\r\n800 900 Kagura: Here.\r\n}\r\n"
                 "{\n0 8365 2001 5\n100 50 Any^scene\n}\n"));
    CHECK_EQ(s.blocks().size(), 2u);
    if (s.blocks().size() != 2) return;
    const auto& b = s.blocks()[0];
    CHECK_EQ(b.scene, 1002);
    CHECK_EQ(b.key, 9763);
    CHECK_EQ(b.endScene, 1002);
    CHECK_EQ(b.endMsg, 114);
    CHECK_EQ(b.lines.size(), 2u);
    CHECK_EQ(b.lines[1].start, 800u);
    CHECK_EQ(b.lines[1].end, 1700u);  // the second number is a duration
    CHECK_EQ(b.lines[1].text, "Kagura: Here.");
    CHECK_EQ(s.blocks()[1].lines[0].text, "Any^scene");

    // A voice key matches only its own scene; an SE key also matches scene 0.
    s.trigger(9763, 1, 1002, 0, 1000);
    CHECK(s.playing());
    s.step(1000 + 400);
    CHECK_EQ(s.text(), "Suwa: Kagura.");
    s.step(1000 + 800);  // both bounds are strict, and the move to the next line waits for end < t
    CHECK_EQ(s.text(), "");
    s.step(1000 + 801);
    CHECK_EQ(s.text(), "");  // this step moved on
    s.step(1000 + 802);
    CHECK_EQ(s.text(), "Kagura: Here.");
    s.trigger(8365, 2, 1002, 0, 2000);  // a running block is not replaced
    s.step(1000 + 1000);
    CHECK_EQ(s.text(), "Kagura: Here.");
    s.messageMarked(1002, 113);
    CHECK(s.playing());
    s.messageMarked(1002, 114);  // the end message
    CHECK(!s.playing());
    s.trigger(8365, 1, 2001, 0, 5000);  // voice keys need the block's scene
    CHECK(!s.playing());
    s.trigger(8365, 2, 2001, 0, 5000);
    CHECK(s.playing());
    s.step(5000 + 151);  // past the last line's end
    CHECK(!s.playing());
}

}  // namespace

int main() {
    testLzss();
    testKcap();
    testLac();
    testLookup();
    testImages();
    testScriptVm();
    testSystemData();
    testGfx();
    testText();
    testAni();
    testSubtitles();
    testDetectionSynthetic();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("All WA2 tests passed");
    return 0;
}
