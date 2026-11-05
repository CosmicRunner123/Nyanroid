// Unit tests for the Diesel Engine core, on data made up here. Run via `ctest`.

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "nyan/common/engine.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/diesel/bytecode.hpp"
#include "nyan/diesel/action.hpp"
#include "nyan/diesel/archives.hpp"
#include "nyan/diesel/crypto.hpp"
#include "nyan/diesel/exe_info.hpp"
#include "nyan/diesel/game_data.hpp"
#include "nyan/diesel/image.hpp"
#include "nyan/diesel/input.hpp"
#include "nyan/diesel/npk_archive.hpp"
#include "nyan/diesel/ogg_info.hpp"
#include "nyan/diesel/particle.hpp"
#include "nyan/diesel/split.hpp"
#include "nyan/diesel/random.hpp"
#include "nyan/diesel/save_store.hpp"
#include "nyan/diesel/script_host.hpp"
#include "nyan/diesel/setting_ini.hpp"
#include "nyan/diesel/sound_player.hpp"
#include "nyan/diesel/asf_info.hpp"
#include "nyan/diesel/movie_clock.hpp"
#include "nyan/diesel/shake.hpp"
#include "nyan/diesel/movie_frames.hpp"
#include "nyan/media/ffmpeg_video.hpp"
#include "nyan/audio/mixer.hpp"
#include "nyan/diesel/sprite_geometry.hpp"
#include "nyan/diesel/system_config.hpp"
// The reverb as the public sound player runs it (included by sound_player.cpp only).
#include "../core/src/diesel/faudio_reverb.inc"
#include "nyan/diesel/text_layout.hpp"
#include "nyan/diesel/text_tokenizer.hpp"

namespace {
using nyan::Bytes;

using nyan::writeLE32;

using namespace nyan::diesel;

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

#define CHECK_EQ(actual, expected) CHECK((actual) == (expected))

Bytes bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }

std::string text(const Bytes& b) { return std::string(b.begin(), b.end()); }

Bytes unhex(std::string_view h) {
    Bytes out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
    return out;
}

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

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

// Paths with '/' (one level of subfolders), matched in either case.
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
        for (const auto& [p, d] : files_) {
            if (nyan::toLowerAscii(p) == want) return std::make_unique<MemoryFile>(d);
        }
        return nullptr;
    }
    std::string displayName() const override { return "memory"; }

private:
    std::map<std::string, Bytes> files_;
};

class ListedFolder final : public nyan::GameFolder {
public:
    explicit ListedFolder(std::vector<std::string> names) : names_(std::move(names)) {}
    std::vector<std::string> list() const override { return names_; }
    std::unique_ptr<nyan::RandomAccessFile> open(const std::string&) const override { return nullptr; }
    std::string displayName() const override { return "listed"; }

private:
    std::vector<std::string> names_;
};

const NpkKey kKey = [] {
    NpkKey k{};
    for (int i = 0; i < 32; ++i) k[i] = static_cast<uint8_t>(0xa0 + i * 7);
    return k;
}();

void encryptCbc(const Aes256& aes, Bytes& data, const uint8_t iv[16]) {
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t b = 0; b < data.size() / 16; ++b) {
        uint8_t* p = &data[16 * b];
        for (int i = 0; i < 16; ++i) p[i] ^= prev[i];
        aes.encryptBlock(p, p);
        std::memcpy(prev, p, 16);
    }
}

void put32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// A 31-byte zstd frame of "Diesel Engine, Diesel Engine, Diesel Engine, Diesel Engine!" (59 bytes).
const uint8_t kZstdFrame[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x3b, 0xb5, 0x00, 0x00, 0x80, 0x44,
                              0x69, 0x65, 0x73, 0x65, 0x6c, 0x20, 0x45, 0x6e, 0x67, 0x69, 0x6e,
                              0x65, 0x2c, 0x20, 0x21, 0x01, 0x00, 0x88, 0x39, 0xeb};

const char kZstdText[] = "Diesel Engine, Diesel Engine, Diesel Engine, Diesel Engine!";

struct FakeEntry {
    std::string name;
    Bytes data;          // the file
    Bytes stored;        // what the segment holds before encryption (data, or a zstd frame)
    uint32_t size = 0;   // the segment's size field
};

// An NPK3 archive as the games' packer writes it (one segment per entry).
Bytes makeNpk(const std::vector<FakeEntry>& entries) {
    Aes256 aes(kKey);
    uint8_t iv[16];
    for (int i = 0; i < 16; ++i) iv[i] = static_cast<uint8_t>(i * 13 + 1);
    // Data first, to know the offsets: lay the index size out with a first pass.
    Bytes index;
    std::vector<Bytes> blobs;
    size_t indexSize = 0;
    for (int pass = 0; pass < 2; ++pass) {
        index.clear();
        blobs.clear();
        uint64_t offset = 0x20 + indexSize;
        for (const auto& e : entries) {
            Bytes blob = e.stored;
            blob.resize((blob.size() + 15) / 16 * 16, 0);
            index.push_back(1);
            index.push_back(static_cast<uint8_t>(e.name.size()));
            index.push_back(static_cast<uint8_t>(e.name.size() >> 8));
            index.insert(index.end(), e.name.begin(), e.name.end());
            put32(index, static_cast<uint32_t>(e.data.size()));
            auto digest = sha256(e.data);
            index.insert(index.end(), digest.begin(), digest.end());
            put32(index, 1);
            put32(index, static_cast<uint32_t>(offset));
            put32(index, static_cast<uint32_t>(offset >> 32));
            put32(index, static_cast<uint32_t>(blob.size()));
            put32(index, e.size);
            put32(index, static_cast<uint32_t>(e.data.size()));
            offset += blob.size();
            blobs.push_back(std::move(blob));
        }
        uint8_t pad = static_cast<uint8_t>(16 - index.size() % 16);
        index.insert(index.end(), pad, pad);
        indexSize = index.size();
    }
    encryptCbc(aes, index, iv);
    Bytes out = bytes("NPK3");
    put32(out, 1);
    out.insert(out.end(), iv, iv + 16);
    put32(out, static_cast<uint32_t>(entries.size()));
    put32(out, static_cast<uint32_t>(index.size()));
    out.insert(out.end(), index.begin(), index.end());
    for (auto& b : blobs) {
        encryptCbc(aes, b, iv);
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}

FakeEntry plain(const std::string& name, const std::string& content, uint32_t extra = 0) {
    FakeEntry e{name, bytes(content), bytes(content), static_cast<uint32_t>(content.size()) + extra};
    return e;
}

void testCrypto() {
    // FIPS-197 appendix C.3.
    NpkKey key{};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    Aes256 aes(key);
    Bytes pt = unhex("00112233445566778899aabbccddeeff");
    uint8_t ct[16], back[16];
    aes.encryptBlock(pt.data(), ct);
    CHECK_EQ(hex(ct, 16), std::string("8ea2b7ca516745bfeafc49904b496089"));
    aes.decryptBlock(ct, back);
    CHECK_EQ(hex(back, 16), std::string("00112233445566778899aabbccddeeff"));
    // CBC both ways over several blocks.
    Bytes data = bytes("0123456789abcdefFEDCBA9876543210the third block....");
    data.resize(48);
    Bytes copy = data;
    uint8_t iv[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
    encryptCbc(aes, data, iv);
    CHECK(data != copy);
    aes.decryptCbc(data.data(), 3, iv);
    CHECK(data == copy);

    auto h = sha256(bytes("abc"));
    CHECK_EQ(hex(h.data(), 32), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    h = sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
    CHECK_EQ(hex(h.data(), 32), std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    h = sha256({});
    CHECK_EQ(hex(h.data(), 32), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

void testNpk() {
    FakeEntry z{"media/z.txt", bytes(kZstdText), Bytes(kZstdFrame, kZstdFrame + sizeof kZstdFrame),
                static_cast<uint32_t>(sizeof kZstdFrame)};
    // A stored segment whose size field is above its unpacked size, as many of the games' are.
    Bytes npk = makeNpk({plain("Media/Script/Main.nut", "main script", 10), z, plain("empty.dat", "")});
    MemoryFile probe(npk);
    CHECK(NpkArchive::keyOpens(probe, kKey));
    NpkKey wrong = kKey;
    wrong[5] ^= 1;
    CHECK(!NpkArchive::keyOpens(probe, wrong));

    NpkArchive arc(std::make_unique<MemoryFile>(npk), "test.npk", kKey);
    CHECK_EQ(arc.entries().size(), size_t(3));
    const NpkEntry* main = arc.find(normalizeName("media/script/MAIN.NUT"));
    CHECK(main != nullptr);
    if (main) {
        CHECK_EQ(main->segments.size(), size_t(1));
        CHECK(!main->segments[0].compressed());
        CHECK_EQ(text(arc.read(*main)), std::string("main script"));
    }
    const NpkEntry* ze = arc.find("media/z.txt");
    CHECK(ze != nullptr);
    if (ze) {
        CHECK(ze->segments[0].compressed());
        Bytes got = arc.read(*ze);
        CHECK_EQ(text(got), std::string(kZstdText));
        CHECK(sha256(got) == ze->sha256);
    }
    CHECK_EQ(normalizeName("Media/CG/\xe7\xa9\xba.PNG"), std::string("media/cg/\xe7\xa9\xba.png"));

    bool threw = false;
    try {
        NpkArchive bad(std::make_unique<MemoryFile>(bytes("NPK2 not this one")), "bad.npk", kKey);
    } catch (const nyan::Error&) {
        threw = true;
    }
    CHECK(threw);

    // NPK2 (The Song of Saya): the same layout, version 2, packed segments raw deflate.
    Bytes text2 = bytes(kZstdText);
    Bytes deflated(256);
    z_stream zs{};
    deflateInit2(&zs, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = text2.data();
    zs.avail_in = static_cast<uInt>(text2.size());
    zs.next_out = deflated.data();
    zs.avail_out = static_cast<uInt>(deflated.size());
    deflate(&zs, Z_FINISH);
    deflated.resize(zs.total_out);
    deflateEnd(&zs);
    Bytes npk2 = makeNpk({plain("setting.ini", "[Init]\r\n"),
                          FakeEntry{"media/d.txt", text2, deflated, static_cast<uint32_t>(deflated.size())}});
    std::memcpy(npk2.data(), "NPK2", 4);
    npk2[4] = 2;
    MemoryFile probe2(npk2);
    CHECK(isNpkArchive(probe2));
    CHECK(NpkArchive::keyOpens(probe2, kKey));
    NpkArchive arc2(std::make_unique<MemoryFile>(npk2), "npk2.npk", kKey);
    const NpkEntry* de = arc2.find("media/d.txt");
    CHECK(de != nullptr && de->segments[0].compressed());
    if (de) {
        CHECK_EQ(text(arc2.read(*de)), std::string(kZstdText));
        CHECK_EQ(text(arc2.readSegment(*de, 0)), std::string(kZstdText));
    }
    // A version other than the one each magic goes with is not read.
    npk2[4] = 1;
    CHECK(!isNpkArchive(MemoryFile(npk2)));
}

void testArchivesOrder() {
    MemoryFolder f;
    f.add("script.npk", makeNpk({plain("media/script/main.nut", "base"), plain("media/a.txt", "script a")}));
    f.add("system.npk", makeNpk({plain("setting.ini", "[Init]\r\nSCREEN_WIDTH=1024\r\n"), plain("media/a.txt", "system a")}));
    f.add("cg.npk", makeNpk({plain("media/cg/x.png", "old x")}));
    f.add("patch/patch002.npk", makeNpk({plain("media/cg/x.png", "x from 002"), plain("media/cg/y.png", "y 002")}));
    f.add("patch/PATCH001.npk", makeNpk({plain("media/cg/x.png", "x from 001"), plain("media/cg/y.png", "y 001")}));
    auto order = mountList(f);
    CHECK_EQ(order.size(), size_t(5));
    if (order.size() == 5) {
        CHECK_EQ(order[0], std::string("script.npk"));
        CHECK_EQ(order[1], std::string("cg.npk"));
        CHECK_EQ(order[2], std::string("system.npk"));
        CHECK_EQ(order[3], std::string("patch/patch001.npk"));
        CHECK_EQ(order[4], std::string("patch/patch002.npk"));
    }
    Archives a(f, kKey);
    CHECK_EQ(text(*a.read("media/cg/x.png")), std::string("x from 002"));
    CHECK_EQ(text(*a.read("MEDIA/CG/Y.PNG")), std::string("y 002"));
    CHECK_EQ(text(*a.read("media/a.txt")), std::string("system a"));  // system mounted after script
    CHECK(!a.read("media/none.txt").has_value());
}

void testSettingIni() {
    SettingIni ini(bytes("[Init]\r\nSCREEN_WIDTH=1024\r\nSCREEN_HEIGHT=576\r\nWINDOW_NAME=\"A # B\"\r\n"
                         "#DOCKING=0#comment\r\nDOCKING=1#\x83h\x83" "b\x83N\r\nCODE_PAGE=\"UTF8\"\r\n\r\n[Font]\r\n"
                         "Droid Serif=\"media/font/DroidSerif-Regular.ttf\"\r\nOpen Sans=\"media/font/o.ttf\"\r\n"));
    CHECK_EQ(ini.getInt("init", "SCREEN_WIDTH", 0), 1024);
    CHECK_EQ(ini.getInt("Init", "screen_height", 0), 576);
    CHECK_EQ(*ini.get("Init", "WINDOW_NAME"), std::string("A # B"));
    CHECK_EQ(ini.getInt("Init", "DOCKING", -1), 1);
    CHECK_EQ(*ini.get("Init", "CODE_PAGE"), std::string("UTF8"));
    auto fonts = ini.section("Font");
    CHECK_EQ(fonts.size(), size_t(2));
    if (fonts.size() == 2) {
        CHECK_EQ(fonts[0].first, std::string("Droid Serif"));
        CHECK_EQ(fonts[1].second, std::string("media/font/o.ttf"));
    }
    // The Song of Saya's begins with a UTF-8 byte-order mark, which the exes' reader skips.
    SettingIni bom(bytes("\xEF\xBB\xBF[Init]\r\nSCREEN_WIDTH=1280\r\nSCREEN_HEIGHT=960\r\nWINDOW_NAME=\"Song of Saya\"\r\n"));
    CHECK_EQ(bom.getInt("Init", "SCREEN_WIDTH", 0), 1280);
    CHECK_EQ(bom.getInt("Init", "SCREEN_HEIGHT", 0), 960);
    CHECK_EQ(*bom.get("Init", "WINDOW_NAME"), std::string("Song of Saya"));
}

// A minimal i386 PE whose .text builds `key` on the stack as the games' exes do.
Bytes makeExe(const NpkKey& key) {
    Bytes code = {0x55, 0x8b, 0xec};  // push ebp; mov ebp, esp
    // A decoy run with the wrong values, then the real one with an xor in between, as in the exes.
    for (int i = 0; i < 8; ++i) {
        code.insert(code.end(), {0xc7, 0x45, static_cast<uint8_t>(0x90 + 4 * i)});
        put32(code, 0x11111111u * i);
    }
    code.insert(code.end(), {0x90, 0x90});
    for (int i = 0; i < 8; ++i) {
        code.insert(code.end(), {0xc7, 0x45, static_cast<uint8_t>(0xd0 + 4 * i)});
        code.insert(code.end(), key.begin() + 4 * i, key.begin() + 4 * i + 4);
        if (i == 0) code.insert(code.end(), {0x33, 0xff});
    }
    code.push_back(0xc3);
    Bytes exe(0x400, 0);
    exe[0] = 'M';
    exe[1] = 'Z';
    writeLE32(&exe[0x3c], 0x80);
    std::memcpy(&exe[0x80], "PE\0\0", 4);
    exe[0x84] = 0x4c;
    exe[0x85] = 0x01;  // i386
    exe[0x86] = 1;     // one section
    writeLE32(&exe[0x88], 0x5cd1454d);
    exe[0x94] = 0xe0;  // optional header size
    writeLE32(&exe[0x98 + 0x1c], 0x400000);
    size_t sh = 0x98 + 0xe0;
    std::memcpy(&exe[sh], ".text", 5);
    writeLE32(&exe[sh + 8], static_cast<uint32_t>(code.size()));
    writeLE32(&exe[sh + 12], 0x1000);
    writeLE32(&exe[sh + 16], static_cast<uint32_t>(code.size()));
    writeLE32(&exe[sh + 20], 0x400);
    std::string imp = "Mware.dll";
    std::memcpy(&exe[0x300], imp.data(), imp.size());
    exe.insert(exe.end(), code.begin(), code.end());
    return exe;
}

void testExeKey() {
    MemoryFolder f;
    f.add("game_en.exe", makeExe(kKey));
    f.add("other.exe", bytes("MZ not a game exe"));
    f.add("system.npk", makeNpk({plain("setting.ini", "[Init]\r\nSCREEN_WIDTH=1024\r\n")}));
    auto exe = findExe(f);
    CHECK(exe.has_value());
    if (!exe) return;
    CHECK_EQ(exe->fileName, std::string("game_en.exe"));
    CHECK_EQ(exe->timestamp, 0x5cd1454du);
    CHECK_EQ(exe->offsetOf(0x401000), std::optional<size_t>(0x400));
    auto probe = f.open("system.npk");
    CHECK(findArchiveKey(*exe, *probe) == kKey);
    MemoryFolder none;
    none.add("readme.txt", bytes("no exe"));
    CHECK(!findExe(none).has_value());
}

void testDetectionSynthetic() {
    using nyan::Engine;
    MemoryFolder d;
    d.add("script.npk", makeNpk({plain("media/script/main.nut", "x")}));
    d.add("system.npk", makeNpk({plain("setting.ini", "")}));
    CHECK_EQ(nyan::detectEngine(d), std::optional{Engine::Diesel});
    // Not an archive at all: not Diesel. (NPK2 archives are: The Song of Saya, testNpk.)
    MemoryFolder npk2;
    npk2.add("script.npk", bytes("NPK2 older archives"));
    npk2.add("system.npk", bytes("NPK2"));
    CHECK(nyan::detectEngine(npk2) != std::optional{Engine::Diesel});
    CHECK(nyan::detectEngine(ListedFolder({"script.npk", "system.npk"})) != std::optional{Engine::Diesel});
    CHECK_EQ(nyan::detectEngine(ListedFolder({"scene.int", "image.int"})), std::optional{Engine::CatSystem2});
    CHECK_EQ(std::string(nyan::engineName(Engine::Diesel)), std::string("Diesel"));
}

void testEasingAndAction() {
    // Curve 0 is linear, 1 quadratic in (muramasa_en.exe 0x430550, 0x4306b0).
    CHECK_EQ(easing(0, 10.0f, 20.0f, 100.0f, 25.0f), 12.5f);
    CHECK_EQ(easing(1, 0.0f, 100.0f, 100.0f, 50.0f), 25.0f);
    CHECK_EQ(easing(500, 10.0f, 20.0f, 100.0f, 25.0f), 12.5f);  // out of range: curve 0
    // Every curve starts at src and ends at dst: the elastic ones (65..80) within their 2^-10 residue,
    // the rest within float rounding. (Checks the lifting too: a broken branch gave 0 at mid-run.)
    int bad = 0;
    for (int i = 0; i < 161; ++i) {
        for (int t = 0; t <= 1000; t += 50) {
            float v = easing(i, 3.0f, 7.0f, 1000.0f, static_cast<float>(t));
            if (!std::isfinite(v) || v < 1.0f || v > 9.0f) ++bad;
        }
        float a = easing(i, 3.0f, 7.0f, 1000.0f, 0.0f), b = easing(i, 3.0f, 7.0f, 1000.0f, 1000.0f);
        if (std::fabs(a - 3.0f) > 5e-3f || std::fabs(b - 7.0f) > 5e-3f) ++bad;
    }
    CHECK_EQ(bad, 0);

    FrameClock clock;
    Action1D a;
    a.set(0.0f, 100.0f, 100, 0);
    CHECK(a.active);
    CHECK_EQ(a.now, 0.0f);
    // 60 frames a second: 16 / 17 ms steps, each frame's speed from its own length.
    uint32_t frame = 0;
    auto tick = [&] {
        ++frame;
        clock.setTime(frame * 1000 / 60);
        clock.beginFrame();
        a.step(clock);
    };
    tick();  // 16 ms since the clock's start: 16 * 0.06 * 16.666666 ~ 16
    CHECK(a.timer > 15.9f && a.timer < 16.1f);
    for (int i = 0; i < 10; ++i) tick();
    CHECK(!a.active);
    CHECK_EQ(a.now, 100.0f);
    CHECK_EQ(a.timer, 100.0f);
    // A zero time jumps to dst at once.
    Action1D b;
    b.set(5.0f, 9.0f, 0, 3);
    CHECK(!b.active);
    CHECK_EQ(b.now, 9.0f);
    // finish() jumps to dst; power scales now.
    Action1D c;
    c.setPower(2.0f);
    c.set(1.0f, 4.0f, 1000, 0);
    CHECK(c.finish());
    CHECK_EQ(c.now, 8.0f);
    CHECK(!c.finish());
}

// The save store (case-insensitive paths, as on Windows) and Mware's compressed file format.
void testSaveStore() {
    auto store = makeMemorySaveStore();
    Bytes data = {1, 2, 3};
    CHECK(store->write("Common/Val.npf", data));
    CHECK(store->exists("common/VAL.NPF"));
    CHECK(store->isDirectory("COMMON"));
    CHECK(store->read("common/val.npf") == data);
    CHECK(store->remove("COMMON/val.npf"));
    CHECK(!store->exists("common/val.npf"));
    CHECK(!store->read("missing").has_value());

    // MWCF: a header, then blocks; ours are zstd frames of raw blocks (128 KB each at most).
    Bytes big(300000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7 + (i >> 9));
    Bytes packed = encodeCompressedFile(big);
    CHECK(std::memcmp(packed.data(), "MWCF", 5) == 0);
    CHECK(std::memcmp(packed.data() + 9, "BLCK", 5) == 0);
    auto back = decodeCompressedFile(packed);
    CHECK(back.has_value() && *back == big);
    Bytes empty;
    auto none = decodeCompressedFile(encodeCompressedFile(empty));
    CHECK(none.has_value() && none->empty());
    packed[0] = 'X';
    CHECK(!decodeCompressedFile(packed).has_value());
}

// Mware's keyboard and mouse queues and the task's key repeat.
void testInput() {
    CHECK_EQ(keyCodeForVk(0x41), 0x1e);  // A
    CHECK_EQ(keyCodeForVk(0x0d), 0x1c);  // Return
    CHECK_EQ(keyCodeForVk(0x12), 0xb8);  // Alt: right Alt's code
    CHECK_EQ(keyCodeForVk(0x26), 0xc8);  // Up
    CHECK_EQ(keyCodeForVk(0xff), 0);

    Keyboard kb;
    kb.keyDown(0x1c);
    CHECK_EQ(kb.state(0x1c), 1);  // before any update: just pressed
    kb.update();
    CHECK_EQ(kb.state(0x1c), 1);
    CHECK(kb.isDown(0x1c, true));
    kb.update();
    CHECK_EQ(kb.state(0x1c), 2);  // held
    CHECK(kb.isDown(0x1c, false) && !kb.isDown(0x1c, true));
    kb.keyDown(0x1c);  // auto-repeat adds nothing
    kb.keyUp(0x1c);
    CHECK(kb.isUp(0x1c));  // the seen press gives way to the release
    kb.update();
    CHECK(kb.isUp(0x1c) && kb.state(0x1c) == 3);
    kb.update();
    CHECK_EQ(kb.state(0x1c), 0);
    // A tap between two updates: the press shows for a frame, then the release.
    kb.keyDown(0x39);
    kb.keyUp(0x39);
    kb.update();
    CHECK_EQ(kb.state(0x39), 1);
    kb.update();
    CHECK_EQ(kb.state(0x39), 2);  // the press counts on while the release waits behind it
    kb.update();
    CHECK(kb.isUp(0x39));
    kb.update();
    kb.update();
    CHECK_EQ(kb.state(0x39), 0);
    // Right-hand modifiers ask about the left-hand key; getKeyState takes the code as given.
    kb.keyDown(0x38);
    kb.update();
    CHECK(kb.isDown(0xb8, false));
    CHECK_EQ(kb.state(0xb8), 0);
    kb.releaseAll();
    kb.update();
    kb.update();
    CHECK_EQ(kb.state(0x38), 0);

    // Key repeat: true at once, then not within the interval, then again; a release resets it.
    Keyboard k2;
    KeyRepeat rep;
    k2.keyDown(0x1c);
    k2.update();
    uint32_t now = 1000;
    rep.update(k2, now);
    CHECK(rep.isKeyDown(k2, 0x1c, now, 100));
    k2.update();
    now += 50;
    rep.update(k2, now);
    CHECK(!rep.isKeyDown(k2, 0x1c, now, 100));
    now += 120;
    rep.update(k2, now);
    CHECK(rep.isKeyDown(k2, 0x1c, now, 100));

    Mouse m;
    m.moveTo(10, 20);
    m.update();
    CHECK(m.x() == 10 && m.y() == 20 && m.moveX() == 10);
    m.moveTo(15, 18);
    m.update();
    CHECK(m.moveX() == 5 && m.moveY() == -2);
    CHECK_EQ(m.buttonState(Mouse::Left), 0);
    m.button(Mouse::Left, true);
    m.update();
    CHECK_EQ(m.buttonState(Mouse::Left), 2);
    m.update();
    CHECK_EQ(m.buttonState(Mouse::Left), 3);
    m.button(Mouse::Left, false);
    m.update();
    CHECK_EQ(m.buttonState(Mouse::Left), 1);
    m.update();
    CHECK_EQ(m.buttonState(Mouse::Left), 0);
    CHECK_EQ(m.wheelState(), 4);
    m.wheel(120);
    m.update();
    CHECK_EQ(m.wheelState(), 6);
    m.update();
    CHECK_EQ(m.wheelState(), 4);
    m.wheel(-120);
    CHECK_EQ(m.wheelState(), 5);
    // clearMouseInput hides buttons until every queue is empty.
    m.update();
    m.button(Mouse::Right, true);
    m.clear();
    m.update();
    CHECK_EQ(m.buttonState(Mouse::Right), 0);
    m.button(Mouse::Right, false);
    m.update();
    m.update();
    m.update();
    m.button(Mouse::Right, true);
    m.update();
    CHECK_EQ(m.buttonState(Mouse::Right), 2);
}

// system.dat: the fields, the launcher's options, and the reader's fallbacks.
void testSystemConfig() {
    SystemConfig a;
    a.setLauncherOptionCount(3);
    a.fullScreen = 1;
    a.frameRate = 30;
    a.unknown68 = 1.5f;
    a.launcherOptions[1] = 1;
    a.launcherOptions[2] = 1;
    Bytes file = a.serialize();
    CHECK_EQ(file.size(), size_t(7 + 4 + 8 + 18 * 4 + 9 + 3 * 4));
    SystemConfig b;
    b.setLauncherOptionCount(3);
    b.deserialize(file);
    CHECK(b.userSetting == 1 && b.fullScreen == 1 && b.frameRate == 30 && b.unknown68 == 1.5f);
    CHECK(b.launcherOptions == a.launcherOptions);
    // A two-option exe reading a three-option file takes the first two.
    SystemConfig c;
    c.deserialize(file);
    CHECK(c.launcherOptions.size() == 2 && c.launcherOptions[1] == 1);
    // A bad header: the reader's fallbacks, and no user setting.
    file[0] = 'X';
    SystemConfig d;
    d.useHDR = 1;
    d.deserialize(file);
    CHECK(d.userSetting == 0 && d.useHDR == 0 && d.antiAliasType == 2 && d.pointLightNum == 4);
    SystemConfig e;
    e.applyPreset(3);
    CHECK(e.useShadow == 0 && e.pointLightNum == 2 && e.frameRate == 60);
}

// Mware's twister with signed shifts (values from a separate model of its decompiled code).
void testRandom() {
    MwareRandom r(12345);
    const int32_t first[] = {823399721, 1544576112, 334004963, 429909554, 1436439020};
    for (int32_t want : first) CHECK_EQ(r.next(), want);
    for (int i = 0; i < 695; ++i) r.next();
    CHECK_EQ(r.next(), 1594040317);  // after the second twist
    MwareRandom ri(777);
    const int32_t ints[] = {13, 7, 12, 7, 10, 7};
    for (int32_t want : ints) CHECK_EQ(ri.nextInt(5, 14), want);
    MwareRandom rf(777);
    const float floats[] = {2.8864097595214844f, 2.202864170074463f, 1.8322727680206299f};
    for (float want : floats) CHECK_EQ(rf.nextFloat(1.0f, 3.0f), want);
}

// The message text tokenizer (CTextTokenizer) on the markup the scripts use.
void testTextTokenizer() {
    auto t = tokenizeText("あ<RUBY text='かん'>漢</RUBY>字\n", true);
    CHECK(t.has_value() && t->size() == 1);
    if (t && t->size() == 1) {
        const auto& l = (*t)[0];
        CHECK_EQ(l.size(), size_t(5));
        if (l.size() == 5) {
            CHECK(l[0].name.empty() && l[0].text == "あ");
            CHECK(l[1].name == "RUBY" && l[1].attributes.size() == 1 && l[1].attributes[0].first == "TEXT" &&
                  l[1].attributes[0].second == "かん");
            CHECK(l[2].text == "漢");
            CHECK(l[3].name == "/RUBY" && l[3].text.empty());
            CHECK(l[4].text == "字\n");
        }
    }
    // Comment lines go; line breaks end lines (and stay in the last text); tags without
    // attributes; the input's last byte is text.
    t = tokenizeText("\r\n//comment\r\nabc<k>def\r\n\r\nxyz", true);
    CHECK(t.has_value() && t->size() == 4);
    if (t && t->size() == 4) {
        CHECK((*t)[0].size() == 1 && (*t)[0][0].text == "\n");
        CHECK((*t)[1].size() == 3 && (*t)[1][0].text == "abc" && (*t)[1][1].name == "K" && (*t)[1][2].text == "def\n");
        CHECK((*t)[2].size() == 1 && (*t)[2][0].text == "\n");
        CHECK((*t)[3].size() == 1 && (*t)[3][0].text == "xyz");
    }
    // <NAME=value> keeps the value as the tag's text; quoted attributes; '&' escapes.
    t = tokenizeText("<Wait=300><voice name='a b' file='c'>x&<y\n", true);
    CHECK(t.has_value() && t->size() == 1);
    if (t && t->size() == 1 && (*t)[0].size() == 3) {
        const auto& l = (*t)[0];
        CHECK(l[0].name == "WAIT" && l[0].text == "300");
        CHECK(l[1].name == "VOICE" && l[1].attributes.size() == 2 && l[1].attributes[0].second == "a b" &&
              l[1].attributes[1].first == "FILE" && l[1].attributes[1].second == "c");
        CHECK(l[2].text == "x<y\n");
    } else {
        CHECK(false);
    }
    auto keep = tokenizeText("x&<y\n", false);
    CHECK(keep && keep->size() == 1 && (*keep)[0].size() == 1 && (*keep)[0][0].text == "x&<y\n");
    // The ruby shorthand, and its failure when unclosed.
    t = tokenizeText("#{ろまん}浪漫#だ\n", true);
    CHECK(t && t->size() == 1 && (*t)[0].size() == 4 && (*t)[0][0].name == "RUBY" &&
          (*t)[0][0].attributes[0].second == "ろまん" && (*t)[0][1].text == "浪漫" && (*t)[0][2].name == "/RUBY");
    CHECK(!tokenizeText("#{abc", true).has_value());
    CHECK(!tokenizeText("#{abc}def", true).has_value());
    // A '<' inside a tag drops the line, tokens read before it too.
    t = tokenizeText("ab<c<d>ef\ngh\n", true);
    CHECK(t && t->size() == 1 && (*t)[0].size() == 1 && (*t)[0][0].text == "gh\n");
    CHECK_EQ(countUtf16Units("a\tb\r\nあ\U0001F600"), 5);
    CHECK_EQ(countUtf16Units("\xff"), 1);
}

// Ogg Vorbis lengths from the identification header and the last page's granule position.
void testOggInfo() {
    auto page = [](int64_t granule, const Bytes& body) {
        Bytes p = {'O', 'g', 'g', 'S', 0, 0};
        for (int i = 0; i < 8; ++i) p.push_back(static_cast<uint8_t>(granule >> (8 * i)));
        p.resize(26, 0);
        p.push_back(1);
        p.push_back(static_cast<uint8_t>(body.size()));
        p.insert(p.end(), body.begin(), body.end());
        return p;
    };
    Bytes id = {1, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 2, 0x44, 0xac, 0, 0, 0, 0, 0, 0};
    Bytes file = page(0, id);
    Bytes more = page(44100 * 3 + 22050, Bytes(10, 0));
    file.insert(file.end(), more.begin(), more.end());
    auto info = readOggInfo(file);
    CHECK(info && info->sampleRate == 44100 && info->channels == 2 && info->milliseconds() == 3500);
    CHECK(!readOggInfo(Bytes(40, 0)).has_value());
}

// Sprite quads (0x455b10 / 0x456400) and the hit test's point lookup (0x4584a0).
void testSpriteGeometry() {
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    auto corner = [&](const SpriteQuad& q, size_t i, float x, float y) {
        return near(q.corner[i][0], x) && near(q.corner[i][1], y);
    };
    SpriteImage s;
    s.source = SpriteImage::Source::Color;
    s.srcW = 100, s.srcH = 50;
    s.move = Tween(3, 10.0f, 20.0f, 0.0f);
    auto q = spriteQuad(s, 1280, 720);
    CHECK(q && corner(*q, 0, 10, 20) && corner(*q, 1, 110, 20) && corner(*q, 2, 10, 70) && corner(*q, 3, 110, 70));
    // Points back to (u, v); outside, none.
    auto uv = quadPointToUv(*q, 60, 45);
    CHECK(uv && near((*uv)[0], 0.5f) && near((*uv)[1], 0.5f));
    uv = quadPointToUv(*q, 10, 70);
    CHECK(uv && near((*uv)[0], 0.0f) && near((*uv)[1], 1.0f));
    CHECK(!quadPointToUv(*q, 5, 5));
    CHECK(!quadPointToUv(*q, 111, 30));
    // Scaled about the centre (the default anchor).
    s.scale = Tween(2, 2.0f, 2.0f);
    q = spriteQuad(s, 1280, 720);
    CHECK(q && corner(*q, 0, -40, -5) && corner(*q, 3, 160, 95));
    // A quarter turn about z: the top-left corner goes to the top right (clockwise on the screen).
    s.scale = Tween(2, 1.0f, 1.0f);
    s.rotate = Tween(3, 0.0f, 0.0f, 90.0f);  // degrees, as Mware's RotationZXY takes them
    q = spriteQuad(s, 1280, 720);
    CHECK(q && corner(*q, 0, 85, -5) && corner(*q, 3, 35, 95));
    // The anchor: centre (0, 0) turns about the top-left corner.
    s.centerX = s.centerY = 0.0f;
    q = spriteQuad(s, 1280, 720);
    CHECK(q && corner(*q, 0, 10, 20) && corner(*q, 2, -40, 20));
    // A child is placed in its parent's part, from its top-left corner.
    SpriteImage parent;
    parent.srcW = parent.srcH = 200;
    parent.move = Tween(3, 100.0f, 100.0f, 0.0f);
    SpriteImage child;
    child.srcW = child.srcH = 20;
    child.move = Tween(3, 10.0f, 10.0f, 0.0f);
    child.setParent(&parent);
    q = spriteQuad(child, 1280, 720);
    CHECK(q && corner(*q, 0, 110, 110) && corner(*q, 3, 130, 130));
    child.setParent(nullptr);
    // A corner moved bends the quad; the others stay.
    SpriteImage b;
    b.srcW = 100, b.srcH = 50;
    b.vertexMove[0] = Tween(2, 10.0f, 0.0f);
    q = spriteQuad(b, 1280, 720);
    CHECK(q && corner(*q, 0, 10, 0) && corner(*q, 1, 100, 0) && corner(*q, 2, 0, 50) && corner(*q, 3, 100, 50));
    uv = q ? quadPointToUv(*q, 55, 25) : std::nullopt;
    CHECK(uv.has_value());
    CHECK(q && !quadPointToUv(*q, 5, 1));
}

// CTextEnglish's layout (0x4ca640) with a font of fixed cells: letters 10 wide, a space 4, all 20
// high (the "ruby" font half that).
struct FixedFont final : nyan::diesel::TextFont {
    int unit;
    explicit FixedFont(int u) : unit(u) {}
    nyan::diesel::TextGlyph glyph(uint32_t code, bool) override {
        nyan::diesel::TextGlyph g;
        g.cellWidth = code == ' ' ? unit * 2 / 5 : unit;
        g.cellHeight = unit * 2;
        return g;
    }
    int height() const override { return unit * 2; }
};

void testTextLayout() {
    using nyan::diesel::TextLayout;
    using nyan::diesel::TextLayoutSettings;
    FixedFont main(10), ruby(5);
    auto xs = [](const TextLayout& l) {
        std::vector<int> out;
        for (const auto& c : l.chars) out.push_back(static_cast<int>(c.x));
        return out;
    };
    TextLayoutSettings s;
    s.fieldWidth = 1000, s.fieldHeight = 500;
    nyan::diesel::LineBreakTables breaks;
    TextLayout l;
    CHECK(l.layout("ab cd", "ab cd", main, ruby, s, breaks));
    CHECK((xs(l) == std::vector<int>{0, 10, 20, 24, 34}));
    CHECK(l.x == 44 && l.maxX == 44 && l.totalHeight == 20 && l.lines == 1);
    // Each unit read before a character holds it back 48 alpha units.
    CHECK(l.chars.size() == 5 && l.chars[4].shown == -192.0f && l.chars[4].typingStart == -192.0f);
    // A word that crosses the field goes to the next line; the trailing space may hang over.
    s.fieldWidth = 35;
    l.clear();
    CHECK(l.layout("ab cd", "ab cd", main, ruby, s, breaks));
    CHECK((xs(l) == std::vector<int>{0, 10, 20, 0, 10}));
    CHECK(l.chars[3].y == 20.0f && l.lines == 2 && l.x == 20 && l.maxX == 24);
    // Laid out again (an empty text): places change, the typing state stays.
    l.chars[0].shown = 255.0f;
    s.fieldWidth = 1000;
    CHECK(l.layout("", "ab cd", main, ruby, s, breaks));
    CHECK((xs(l) == std::vector<int>{0, 10, 20, 24, 34}) && l.chars[0].shown == 255.0f && l.chars[3].y == 0.0f);
    // A line break.
    l.clear();
    CHECK(l.layout("a\nb", "a\nb", main, ruby, s, breaks));
    CHECK(l.chars.size() == 2 && l.chars[1].x == 0.0f && l.chars[1].y == 20.0f);
    // CENTER ... /CENTER centres the run in the field; the pen goes after it.
    s.fieldWidth = 100;
    l.clear();
    CHECK(l.layout("<CENTER>ab</CENTER>", "", main, ruby, s, breaks));
    CHECK((xs(l) == std::vector<int>{40, 50}) && l.x == 60);
    // SMALL text takes the ruby font.
    l.clear();
    CHECK(l.layout("<SMALL>a</SMALL>b", "", main, ruby, s, breaks));
    CHECK(l.chars.size() == 2 && l.chars[0].small && l.chars[0].cellWidth == 5 && l.chars[1].x == 5.0f && !l.chars[1].small);
    // The pitch: setPitchSpace's second value before a half-width character, and before printed text.
    s.pitchHalf = 3;
    l.clear();
    CHECK(l.layout("ab", "ab", main, ruby, s, breaks));
    CHECK((xs(l) == std::vector<int>{0, 13}) && l.x == 23);
    l.pitchBeforePrint("c", s);
    CHECK(l.x == 26);
    // UTF-8 to UTF-16 as MultiByteToWideChar gives it.
    CHECK(nyan::diesel::utf8ToUtf16("a\xc3\xa9\xf0\x9f\x98\x80") == std::u16string(u"a\u00e9\U0001F600"));
    CHECK(nyan::diesel::utf8ToUtf16("\xff") == std::u16string(u"\ufffd"));

    // CText's Japanese layout (0x4d08c0): a character at a time, the pitch before each but a line's
    // first. create clears the text, which leaves +0x4d4 unset: its first character takes a pitch;
    // laid out again (an empty text) it does not.
    breaks.headOfLine = {0x3002};
    breaks.smallKana = {0x3063};
    breaks.endOfLine = {0x300c};
    TextLayout j;
    j.type = 0;
    s = TextLayoutSettings{};
    s.fieldWidth = 1000, s.fieldHeight = 500, s.pitchHalf = 3;
    j.clear();
    CHECK(j.layout("ab", "ab", main, ruby, s, breaks));
    CHECK((xs(j) == std::vector<int>{3, 16}) && j.x == 26);
    CHECK(j.layout("", "ab", main, ruby, s, breaks));
    CHECK((xs(j) == std::vector<int>{0, 13}));
    // A character that would cross the field starts the next line (no words) ...
    s.pitchHalf = 0, s.fieldWidth = 25;
    j.clear();
    CHECK(j.layout("abc", "abc", main, ruby, s, breaks));
    CHECK((xs(j) == std::vector<int>{0, 10, 0}) && j.chars[2].y == 20.0f && j.lines == 2);
    // ... unless a line may not begin with it (it hangs past the field), small kana too.
    j.clear();
    CHECK(j.layout("ab\xe3\x80\x82", "", main, ruby, s, breaks));  // 。
    CHECK((xs(j) == std::vector<int>{0, 10, 20}) && j.lines == 1);
    j.clear();
    CHECK(j.layout("ab\xe3\x81\xa3", "", main, ruby, s, breaks));  // っ
    CHECK((xs(j) == std::vector<int>{0, 10, 20}) && j.lines == 1);
    // One a line may not end with goes on when a full-width character would not fit after it.
    j.clear();
    CHECK(j.layout("a\xe3\x80\x8c", "", main, ruby, s, breaks));  // 「
    CHECK((xs(j) == std::vector<int>{0, 0}) && j.chars[1].y == 20.0f);
    // Ruby, one character over each: centred on the cell, the ruby font's height above, a quarter
    // of the cell down; typed with its character.
    s.fieldWidth = 1000;
    j.clear();
    CHECK(j.layout("<RUBY text='xy'>ab</RUBY>", "", main, ruby, s, breaks));
    CHECK(j.rubies.size() == 2 && j.rubies[0].x == 3.0f && j.rubies[0].y == -4.0f && j.rubies[1].x == 13.0f);
    CHECK(j.rubies[1].typingStart == -48.0f && j.rubies[0].ruby && j.pool.size() == 4);
    // Spread over its characters: spaced out (at most half their width between two), centred.
    j.clear();
    CHECK(j.layout("<RUBY text='xy'>abc</RUBY>", "", main, ruby, s, breaks));
    CHECK(j.rubies.size() == 2 && j.rubies[0].x == 2.0f && j.rubies[1].x == 22.0f && j.rubies[1].y == -4.0f);
    CHECK(j.rubies[0].typingStart == 0.0f && j.rubies[1].typingStart == -96.0f);
    // A single ruby character over two divides by no gaps: NaN, and the exe's int of it.
    j.clear();
    CHECK(j.layout("<RUBY text='x'>ab</RUBY>", "", main, ruby, s, breaks));
    CHECK(j.rubies.size() == 1 && j.rubies[0].x == -2147483648.0f);
    // A fix rule moves the first character's right corners (vtable +0x88, 0x4d29f0).
    j.clear();
    CHECK(j.layout("ab", "", main, ruby, s, breaks));
    j.cornerOffsets(s, {{'a', 'b', 3}});
    CHECK(j.chars[0].corner[1][0] == 3.0f && j.chars[0].corner[3][0] == 3.0f && j.chars[0].corner[0][0] == 0.0f &&
          j.chars[1].corner[1][0] == 0.0f);

    // You and Me and Her's older layouts. English: no SMALL; a word in a token without letters wraps too (not
    // at a line's start).
    TextLayout e;
    e.older = true;
    s = TextLayoutSettings{};
    s.fieldWidth = 1000, s.fieldHeight = 500;
    e.clear();
    CHECK(e.layout("<SMALL>a</SMALL>b", "", main, ruby, s, breaks));
    CHECK(e.chars.size() == 2 && !e.chars[0].small && e.chars[1].x == 10.0f);
    s.fieldWidth = 35;
    e.clear();
    CHECK(e.layout("ab <I>--</I>", "", main, ruby, s, breaks));
    CHECK(e.chars.size() == 5 && e.chars[3].x == 0.0f && e.chars[3].y == 20.0f);
    l.clear();
    CHECK(l.layout("ab <I>--</I>", "", main, ruby, s, breaks));
    CHECK(l.chars[3].y == 0.0f && l.chars[3].x == 24.0f);  // Muramasa's: no letter in its token, it stays
    // Japanese: small kana do not hang; ruby spreads row by row, each row's share of it.
    TextLayout o;
    o.type = 0, o.older = true;
    s.fieldWidth = 25;
    o.clear();
    CHECK(o.layout("ab\xe3\x81\xa3", "", main, ruby, s, breaks));  // っ
    CHECK((xs(o) == std::vector<int>{0, 10, 0}) && o.lines == 2);
    s.fieldWidth = 30;
    o.clear();
    CHECK(o.layout("<RUBY text='wxyz'>abcd</RUBY>", "", main, ruby, s, breaks));
    CHECK(o.chars[3].y == 20.0f && o.rubies.size() == 4);
    // Four over four: one over each, the second row's under it.
    CHECK(o.rubies[3].x == 3.0f && o.rubies[3].y == 15.0f);
    o.clear();
    CHECK(o.layout("<RUBY text='xyz'>abcd</RUBY>", "", main, ruby, s, breaks));
    // CTextVertical (0x4ccb30): down the column, turned right to left; a character that would pass
    // the field's foot starts the next column (to the left).
    TextLayout v;
    v.type = 2;
    s = TextLayoutSettings{};
    s.fieldWidth = 100, s.fieldHeight = 1000;
    v.clear();
    CHECK(v.layout("ab", "ab", main, ruby, s, breaks));
    CHECK(v.chars.size() == 2 && v.chars[0].x == 90.0f && v.chars[0].y == 0.0f && v.chars[1].x == 90.0f &&
          v.chars[1].y == 20.0f && v.maxX == 10 && v.totalHeight == 40);
    s.fieldHeight = 30;
    v.clear();
    CHECK(v.layout("abc", "abc", main, ruby, s, breaks));
    CHECK(v.chars[1].x == 80.0f && v.chars[1].y == 0.0f && v.chars[2].x == 70.0f && v.lines == 3);
    // Three over three then one: trunc(3 x 3 / 4) = 2 for the first row, the last one for the second.
    CHECK(o.rubies.size() == 3 && o.rubies[0].y == -4.0f && o.rubies[1].x == 23.0f && o.rubies[2].y == 15.0f);
}

// CEffectShake's way: 0 to -amplitude over half a swing, side to side every `frequency` ms, back to
// 0 at the end; the random one stays within the amplitude; setTimer lands on the same swing side.
void testShake() {
    auto run = [](Shake& s, int frames, std::vector<float>& xs) {
        FrameClock clock;
        clock.milliTime = 1000;
        clock.lastMilli = 1000;
        for (int i = 1; i <= frames; ++i) {
            clock.milliTime = 1000 + static_cast<uint32_t>(i * 1000 / 60);
            clock.beginFrame();
            if (s.shaking()) s.update(clock);
            xs.push_back(s.offset()[0]);
        }
    };
    Shake s(false);
    s.set({10, 0, 0}, {10, 0, 0}, 100, 1, 1000, 0, 0);
    CHECK(s.shaking() && s.offset()[0] == 0.0f);
    std::vector<float> xs;
    run(s, 90, xs);
    // About 50 ms in: at -10; then it crosses to +10 within the next swing.
    CHECK(xs[2] < -5.0f);
    float most = *std::max_element(xs.begin(), xs.begin() + 60), least = *std::min_element(xs.begin(), xs.begin() + 60);
    CHECK(most > 9.0f && most <= 10.0f && least < -9.0f && least >= -10.0f);
    int crossings = 0;
    for (size_t i = 1; i < 60; ++i) crossings += (xs[i - 1] < 0) != (xs[i] < 0);
    CHECK(crossings >= 6);
    CHECK(!s.shaking() && std::fabs(xs.back()) < 0.001f);
    // Without interpolation the offset is a swing's target: only 0 and the sides.
    Shake jump(false);
    jump.set({10, 0, 0}, {10, 0, 0}, 100, 0, 1000, 0, 0);
    xs.clear();
    run(jump, 40, xs);
    for (float x : xs) CHECK(x == 0.0f || std::fabs(std::fabs(x) - 10.0f) < 0.01f);
    // The amplitude moves from start to end.
    Shake grow(false);
    grow.set({0, 0, 0}, {20, 0, 0}, 50, 1, 1000, 0, 0);
    xs.clear();
    run(grow, 60, xs);
    float early = 0, late = 0;
    for (size_t i = 0; i < 15; ++i) early = std::max(early, std::fabs(xs[i]));
    for (size_t i = 35; i < 55; ++i) late = std::max(late, std::fabs(xs[i]));
    CHECK(early < late);
    // Random: within the amplitude, back to 0 at the end.
    Shake r(true, 1234);
    r.set({8, 4, 0}, {8, 4, 0}, 50, 1, 600, 0, 0);
    xs.clear();
    run(r, 50, xs);
    for (float x : xs) CHECK(std::fabs(x) <= 8.0f);
    CHECK(!r.shaking() && std::fabs(xs.back()) < 0.001f);
    CHECK(std::any_of(xs.begin(), xs.end(), [](float x) { return std::fabs(x) > 1.0f; }));
    // setTimer: set back to 0 the shake starts again; into a later swing, the side follows its parity.
    Shake t(false);
    t.set({10, 0, 0}, {10, 0, 0}, 100, 1, 1000, 0, 0);
    t.setTimer(0.0f);
    CHECK(t.timer() == 0.0f && t.shaking() && t.offset()[0] == 0.0f);
    t.setTimer(100.0f);  // halfway through the second swing (from -10 to +10)
    CHECK(t.timer() == 100.0f && std::fabs(t.offset()[0]) < 2.0f);
    t.setTimer(990.0f);  // within the swing back to 0
    CHECK(std::fabs(t.offset()[0]) < 10.0f && t.shaking());
}

// Mware's movie clock: passes, plays left, pause, stop, seek and the time scale's limit.
void testMovieClock() {
    MovieClock c;
    c.duration = 1000 * 10000;  // 1 s
    c.play(0);
    CHECK(!c.step(500 * 10000) && c.positionMs() == 500 && c.playing);
    CHECK(c.step(600 * 10000) && !c.playing && c.position == 0);  // 0: once
    c.play(1);
    CHECK(c.step(1000 * 10000) && !c.playing);  // 1: once
    c.play(2);
    CHECK(!c.step(1000 * 10000) && c.playing && c.position == 0 && c.pass == 1);
    CHECK(c.step(1000 * 10000) && !c.playing);
    c.play(-1);
    for (int i = 0; i < 5; ++i) CHECK(!c.step(1000 * 10000));
    CHECK(c.playing && c.pass == 6);
    CHECK(c.pause() && c.paused);
    c.step(400 * 10000);
    CHECK(c.position == 0);
    CHECK(c.resume() && !c.paused);
    c.step(400 * 10000);
    CHECK(c.positionMs() == 400);
    c.seek(700);
    CHECK(c.playing && c.positionMs() == 700);
    c.stop();
    CHECK(!c.playing && c.position == 0 && !c.pause());
    c.seek(300);
    CHECK(!c.playing && c.positionMs() == 300);
    c.setTimeScale(2.0f);
    CHECK(c.timeScale == 1.0f);
    c.setTimeScale(0.5f);
    c.play(0);
    c.step(400 * 10000);
    CHECK(c.positionMs() == 500);
    c.setTimeScale(-1.0f);
    CHECK(c.timeScale == 0.0f);
}

// Split faces (section 5r): the grid, its cuts, groups, an action's motion and its timeline.
void testSplitFace() {
    // No distortion: even cells (the remainder a pixel each from the first), no jag, one group each.
    SplitFace f;
    f.split(2, 3, 0.0f, 1, 1, 100, 60);
    CHECK_EQ(f.pieces.size(), size_t{6});
    CHECK_EQ(f.groups.size(), size_t{6});
    if (f.pieces.size() == 6) {
        CHECK(f.pieces[0].w == 34.0f && f.pieces[1].w == 33.0f && f.pieces[2].x == 67.0f && f.pieces[3].y == 30.0f);
        CHECK(f.pieces[4].jag[0][0] == 0.0f && f.pieces[4].jag[3][1] == 0.0f);
        CHECK(f.pieces[0].own[0] == 17.0f - 50.0f && f.pieces[0].center[0] == f.pieces[0].own[0]);
    }
    // The same arguments again change nothing; cells under 15 pixels are not allowed.
    f.split(2, 3, 0.0f, 1, 1, 100, 60);
    CHECK_EQ(f.pieces.size(), size_t{6});
    SplitFace small;
    small.split(10, 10, 0.0f, 1, 1, 60, 45);
    CHECK(small.cols == 4 && small.rows == 3);
    // Distorted cuts cover the picture, each at least 5 pixels; the outer corners stay put.
    SplitFace d;
    d.split(5, 5, 0.0f, 7, 0, 200, 150);
    float wsum = 0.0f, hsum = 0.0f;
    bool wide = true;
    for (int32_t i = 0; i < d.cols; ++i) wsum += d.pieces[static_cast<size_t>(i)].w, wide = wide && d.pieces[static_cast<size_t>(i)].w >= 5.0f;
    for (int32_t j = 0; j < d.rows; ++j) hsum += d.pieces[static_cast<size_t>(j * d.cols)].h;
    CHECK(wsum == 200.0f && hsum == 150.0f && wide);
    CHECK(d.pieces[0].jag[0][0] == 0.0f && d.pieces[0].jag[0][1] == 0.0f);
    // A link rate of 1 joins everything into one group around the picture's centre.
    SplitFace all;
    all.split(2, 2, 1.0f, 3, 1, 100, 100);
    CHECK_EQ(all.groups.size(), size_t{1});
    for (const auto& p : all.pieces) CHECK(p.group == 0 && p.count == 4 && std::fabs(p.center[0]) < 1e-4f);
    // An action: every group moves outward at 100 a second; at the end of its second, without
    // resistance, it has gone 100 (x and y on the picture, z the dome's).
    SplitFace::ActionSpec a;
    a.time = 1.0f;
    a.vel.on = 1;
    a.vel.distMin = a.vel.distMax = 100.0f;
    f.push(a, 100, 60);
    CHECK_EQ(f.actions.size(), size_t{1});
    f.begin(0.0f, 1.0f, 0, 0, 0);
    CHECK(!f.t.active && f.t.now[0] == 1000.0f);
    f.dirty = 1;
    f.place();
    if (f.pieces.size() == 6) {
        const auto& p = f.pieces[0];
        float px = p.center[0] / 60.0f * 2.0f, py = p.center[1] / 60.0f * 2.0f;
        float l = std::sqrt(px * px + py * py);
        CHECK(std::fabs(p.offset[0] - px / l * 100.0f) < 1e-3f && std::fabs(p.offset[1] - py / l * 100.0f) < 1e-3f);
        CHECK(std::fabs(p.offset[2] - std::sqrt(std::max(1.0f - l * l, 0.0f)) * 100.0f) < 1e-3f);
    }
    // With resistance k the distance is v0 (1 - e^-k) / k after a second.
    SplitFace r;
    r.split(1, 2, 0.0f, 1, 1, 60, 30);
    a.regist = 2.0f;
    r.push(a, 60, 30);
    r.begin(0.0f, 1.0f, 0, 0, 0);
    r.dirty = 1;
    r.place();
    if (r.pieces.size() == 2) {
        const auto& p = r.pieces[1];  // centre (15, 0): straight right
        CHECK(std::fabs(p.offset[0] - 100.0f * (1.0f - std::exp(-2.0f)) / 2.0f) < 1e-3f && std::fabs(p.offset[1]) < 1e-4f);
    }
    // The timer: past the tween with keepGoing it runs on.
    r.begin(0.0f, 0.5f, 1000, 0, 1);
    r.setTimer(1500.0f);
    CHECK(r.timer() == 1500.0f && r.t.timer == 1000.0f);
}

// The particle system on a made-up file (field offsets: core/src/diesel/particle_layout.inc): four
// particles from a point, 100 ms apart, one life of a second each, rising 2 units a frame.
std::vector<uint8_t> particleRecord() {
    std::vector<uint8_t> raw(ParticleFile::kSize, 0);
    auto i32 = [&raw](size_t off, int32_t v) { std::memcpy(raw.data() + off, &v, 4); };
    auto f32 = [&raw](size_t off, float v) { std::memcpy(raw.data() + off, &v, 4); };
    i32(0x0, 1);                              // is_use_particle
    std::memcpy(raw.data() + 0x68, "dot.png", 7);  // texture_filename
    f32(0x284, 1.0f), f32(0x288, 1.0f);       // sprite.size.start: the texture's size
    for (size_t c = 0; c < 4; ++c) i32(0x300 + 4 * c, 255);  // sprite.color.start
    i32(0x33c, 6);                            // sprite.color.middle.tempo
    i32(0x398, 4);                            // occur.particle_num.num
    i32(0x3a4, 1), i32(0x3a8, 1000);          // occur.use_particle_age, .time
    i32(0x3b8, 1);                            // occur.count (patturn 0: lives counted)
    i32(0x3c0, 1), i32(0x3c4, 100);           // occur.use_delay, delay.time (order 0: x index + 1)
    i32(0x758, 1);                            // location.use_bounding_point
    i32(0x7ec, 2), i32(0x834, 2), i32(0x87c, 2);  // rotate.z / x / y: no rotation
    f32(0x8d4, 2.0f);                         // move.velocity.y
    i32(0x14f4, 4), i32(0x14f8, 1), i32(0x14fc, 1);  // version, texture_count, texture_use_count
    return raw;
}

void testParticles() {
    // A version 3 file's keyframe curves are renumbered (0x47e8e0: 6 -> 14).
    auto raw = particleRecord();
    int32_t three = 3;
    std::memcpy(raw.data() + 0x14f4, &three, 4);
    auto old = parseParticleFile(raw.data(), raw.size());
    CHECK(old && old->i(0x33c) == 14 && old->textures.size() == 1 && old->textures[0] == "dot.png");
    CHECK(!parseParticleFile(raw.data(), 0x14f3));

    raw = particleRecord();
    auto make = [&](const std::vector<uint8_t>& r) {
        auto f = parseParticleFile(r.data(), r.size());
        CHECK(f.has_value());
        ParticleSystem::Texture t{"dot.png", 10, 10};
        auto sys = std::make_unique<ParticleSystem>(std::move(*f), std::vector<ParticleSystem::Texture>{t}, 1234);
        sys->setCamera(nullptr, 512.0f, 45.0f, 1024, 576);
        return sys;
    };
    auto sys = make(raw);
    CHECK_EQ(sys->particleCount(), size_t{4});
    sys->reset();
    sys->start();
    CHECK(sys->active());
    // 150 ms: only the first is past its delay, 50 ms (3 frames) into its life: 6 units up.
    sys->advance(150.0f);
    CHECK_EQ(sys->aliveCount(), size_t{1});
    auto batch = sys->build(1024, 576);
    CHECK(batch && batch->vertices.size() == 4);  // only started particles are drawn
    if (batch && batch->vertices.size() == 4) {
        float cy = 0.0f, cw = 0.0f, width = 0.0f;
        for (size_t k = 0; k < 4; ++k) cy += batch->vertices[k].y, cw += batch->vertices[k].w;
        width = (batch->vertices[1].x - batch->vertices[0].x) / batch->vertices[0].w;
        // The default view: eye at z = -512, 45 degrees: y / w = 6 / (512 tan 22.5).
        CHECK(std::fabs(cy / cw - 6.0f / (512.0f * std::tan(0.392699093f))) < 1e-4f);
        // 10 units wide (the texture's 10 pixels times 1) at the distance 512, in NDC.
        CHECK(std::fabs(width - 10.0f / (512.0f * std::tan(0.392699093f)) * (576.0f / 1024.0f)) < 1e-4f);
        CHECK(batch->vertices[0].color[3] == 1.0f);
        CHECK(std::fabs(batch->vertices[0].u - 0.05f) < 1e-6f);  // half a texel in
    }
    // A seek kept before begin replaces the first update's time: 2 s, past every one's only life.
    sys = make(raw);
    sys->seek(2000.0f);
    sys->reset();
    sys->start();
    sys->advance(16.6f);
    CHECK_EQ(sys->aliveCount(), size_t{0});
    CHECK(!sys->active() && !sys->visible());
    // Endless lives (patturn 1) keep them all going; the fall of gravity (1/2 g t^2, frames).
    int32_t one = 1;
    std::memcpy(raw.data() + 0x3b4, &one, 4);   // occur.patturn
    std::memcpy(raw.data() + 0xa80, &one, 4);   // move.use_gravity
    float g = 1.0f, zero = 0.0f;
    std::memcpy(raw.data() + 0xa84, &g, 4);     // move.gravity.acceleration
    std::memcpy(raw.data() + 0xa88, &g, 4);     // move.gravity.factor
    std::memcpy(raw.data() + 0x8d4, &zero, 4);  // no velocity
    sys = make(raw);
    sys->reset();
    sys->start();
    sys->advance(700.0f);  // the first: 600 ms = 36 frames in: 648 down
    CHECK_EQ(sys->aliveCount(), size_t{4});
    batch = sys->build(1024, 576);
    if (batch && !batch->vertices.empty()) {
        float cy = 0.0f, cw = 0.0f;
        for (size_t k = 0; k < 4; ++k) cy += batch->vertices[k].y, cw += batch->vertices[k].w;
        CHECK(std::fabs(cy / cw + 648.0f / (512.0f * std::tan(0.392699093f))) < 1e-3f);
    }
    sys->advance(5000.0f);
    CHECK_EQ(sys->aliveCount(), size_t{4});
    CHECK(sys->active());
}

}  // namespace

// The reverb Diesel voices get (core/src/diesel/faudio_reverb.inc, FAudio's re-implementation of
// XAudio2's) for Mware's preset 26 (LARGEROOM): an impulse through twenty passes of 441 frames at
// 44.1 kHz. The reference is FAudio 26.11's own C code run the same way (the port matched it bit
// for bit for every preset, mono and stereo); the energy of each pass is compared within 1e-4
// relative, as another host's pow / tan may round the coefficients otherwise.
void testReverb() {
    static const double monoEnergy[20] = {
        0, 0, 4.82081437e-05, 0.000212597888, 3.4015671e-05, 5.35397588e-06, 8.85313366e-08, 8.70801209e-07,
        1.45137155e-07, 2.46452548e-07, 2.01712435e-06, 8.2563831e-06, 1.97699912e-05, 4.20405694e-05,
        5.20883733e-05, 3.77024167e-05, 3.04715158e-05, 3.74293446e-05, 3.26112502e-05, 3.06156462e-05,
    };
    static const double stereoEnergy[20] = {
        0, 0, 2.41040719e-05, 0.000106298944, 1.70078355e-05, 2.67698794e-06, 4.42656683e-08, 4.35400605e-07,
        7.25273256e-08, 1.04413263e-07, 9.19770742e-07, 3.75504435e-06, 1.14361674e-05, 1.96883387e-05,
        2.52148701e-05, 1.66806112e-05, 1.77449254e-05, 1.80548946e-05, 1.53351919e-05, 1.57856082e-05,
    };
    for (int ch = 1; ch <= 2; ++ch) {
        xa2reverb::Reverb rv(ch, 44100.0f);
        rv.setParameters(xa2reverb::convertPreset(26));
        std::vector<float> in(441 * 2), out(441 * 2), all;
        for (int pass = 0; pass < 20; ++pass) {
            std::fill(in.begin(), in.end(), 0.0f);
            if (pass == 0) in[0] = 1.0f;
            rv.process(in.data(), out.data(), 441, true);
            all.insert(all.end(), out.begin(), out.begin() + 441 * ch);
        }
        // Nothing until the preset's 20 ms reflections delay has passed.
        size_t first = 0;
        while (first < all.size() && std::fabs(all[first]) <= 1e-6f) ++first;
        CHECK_EQ(first / static_cast<size_t>(ch), size_t{882});
        const double* gold = ch == 1 ? monoEnergy : stereoEnergy;
        double worst = 0;
        for (int pass = 0; pass < 20; ++pass) {
            double e = 0;
            for (int k = 0; k < 441 * ch; ++k) e += static_cast<double>(all[pass * 441 * ch + k]) * all[pass * 441 * ch + k];
            worst = std::max(worst, std::fabs(e - gold[pass]) / std::max(gold[pass], 1e-12));
        }
        std::printf("reverb %s: worst relative energy difference from FAudio %g\n", ch == 1 ? "mono" : "stereo", worst);
        CHECK(worst < 1e-4);
    }
    // Turned off, the sound passes through as it is.
    xa2reverb::Reverb off(2, 48000.0f);
    const std::vector<float> dry = {0.25f, -0.5f, 0.125f, 1.0f};
    std::vector<float> wet(4);
    off.process(dry.data(), wet.data(), 2, false);
    CHECK(wet == dry);
    // The preset as the effect's parameters: the room and gains in hundredths of a decibel, the
    // delays in ms.
    xa2reverb::Native n = xa2reverb::convertPreset(26);
    CHECK(n.reflectionsDelay == 20 && n.reverbDelay == 40 && n.rearDelay == 5 && n.earlyDiffusion == 15);
    CHECK(std::fabs(n.roomFilterMain + 10.0f) < 1e-5f && std::fabs(n.reverbGain + 10.0f) < 1e-5f && n.lowEQGain == 8);
}

int main() {
    testCrypto();
    testNpk();
    testArchivesOrder();
    testSettingIni();
    testExeKey();
    testDetectionSynthetic();
    testEasingAndAction();
    testSaveStore();
    testInput();
    testSystemConfig();
    testRandom();
    testTextTokenizer();
    testOggInfo();
    testSpriteGeometry();
    testTextLayout();
    testMovieClock();
    testShake();
    testParticles();
    testSplitFace();
    testReverb();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("diesel tests passed");
    return 0;
}
