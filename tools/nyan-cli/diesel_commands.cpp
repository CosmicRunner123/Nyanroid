// nyan-cli's Diesel Engine commands (Full Metal Daemon Muramasa, You and Me and Her).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "commands.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/common/sjis.hpp"
#include "nyan/diesel/crypto.hpp"
#include "nyan/diesel/game_data.hpp"
#include "nyan/diesel/image.hpp"
#include "nyan/audio/mixer.hpp"
#include "nyan/diesel/save_store.hpp"
#include "nyan/diesel/script_host.hpp"
#include "../../core/src/diesel/natives_table.hpp"
#include "nyan/diesel/sound_player.hpp"
#include "nyan/diesel/asf_info.hpp"
#include "nyan/diesel/movie_frames.hpp"
#include "stb_image_write.h"

namespace fs = std::filesystem;
using namespace nyan;
using namespace nyan::diesel;

namespace {

void usage() {
    std::puts(
        "  Diesel Engine folders (Muramasa, You and Me and Her, Phantom of Inferno, Mojika, The Song of Saya):\n"
        "  nyan-cli info    <game-dir>                   The exe, the archive key, setting.ini, the archives\n"
        "  nyan-cli list    <game-dir> [filter]          Archive entries in search order (* = hidden by a\n"
        "                                                later archive with the same name)\n"
        "  nyan-cli extract <game-dir> <out-dir> [filter] [--png]\n"
        "                                                Extract the entries the engine finds to <out-dir>/<name>;\n"
        "                                                --png also writes each picture as <name>.png.png\n"
        "  nyan-cli boot    <game-dir> [frames] [--press N] [--autoclick N] [--click F:X,Y]... [--move F:X,Y]... [--down F:X,Y]...\n"
        "                  [--up F:X,Y]... [--eval F:CODE]...\n"
        "                  [--draw] [--watch F1-F2:TEXT]... [--targets] [--sounds] [--wav F] [--key F:K]... [--saves DIR]\n"
        "                                                Run main.nut headless for that many frames (default\n"
        "                                                600); report the log, unhandled natives, missing files,\n"
        "                                                the cursor's shape when it changes.\n"
        "                                                --press N: Return every N frames; --autoclick N: a click\n"
        "                                                every N frames going round playall's points; --click: the pointer to\n"
        "                                                X,Y and a left click at frame F; --move: the pointer to X,Y\n"
        "                                                at frame F; --down / --up: the pointer there and the left\n"
        "                                                button down / up at frame F; --draw: the last frame's\n"
        "                                                draw list; --sounds: the sound device calls;\n"
        "                                                --wav: the sound mixed in step, to a WAV file;\n"
        "                                                --eval: Squirrel run before frame F, its result shown;\n"
        "                                                --watch: quads of the draw items whose texture holds TEXT;\n"
        "                                                --targets: render targets and masks when they change;\n"
        "                                                --key: key K (a letter, digit or key code) at frame F;\n"
        "                                                --saves: the save store in DIR (else in memory)\n"
        "  nyan-cli playall <game-dir> [scene] [--frames N] [--threads N] [--roundtrip N] [--natives FILE]\n"
        "                                                Run every scene (media/script/nut, or nps) headless from its\n"
        "                                                start, Return / clicks to advance; report stuck scenes,\n"
        "                                                script errors, unhandled natives, missing files (one\n"
        "                                                scene: its whole log, then where its threads wait);\n"
        "                                                --roundtrip N: a quick save and load every N frames,\n"
        "                                                the draw lists at the save and after the load compared;\n"
        "                                                --natives FILE: every native's scenes and calls, to FILE\n"
        "  nyan-cli movie   <game-dir> <file> <seconds> <out.png> [--alpha]\n"
        "                                                The picture a movie's texture holds at that time\n"
        "  nyan-cli natives <game-dir>                   Every native the exes register: this exe's address,\n"
        "                                                Nyanroid's implementation, script files naming it\n"
        "  nyan-cli verify  <game-dir> [--threads N]     Read every entry, check sizes, decode every picture;\n"
        "                                                count entries whose stored SHA-256 differs (Mware\n"
        "                                                never checks it)");
}

bool isDieselDir(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    try {
        return detectEngine(DirectoryGameFolder(dir)) == Engine::Diesel;
    } catch (const std::exception&) {
        return false;
    }
}

bool matches(const std::string& name, const std::string& filter) {
    return filter.empty() || toLowerAscii(name).find(toLowerAscii(filter)) != std::string::npos;
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

std::string extensionOf(const std::string& name) {
    auto slash = name.rfind('/');
    auto dot = name.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return toLowerAscii(name.substr(dot));
}

std::string utcTime(uint32_t t) {
    std::time_t tt = t;
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// An entry is hidden if an archive mounted later (searched earlier) has the same name.
std::set<std::pair<const NpkArchive*, const NpkEntry*>> visibleEntries(const Archives& a) {
    std::set<std::pair<const NpkArchive*, const NpkEntry*>> out;
    for (const auto& arc : a.archives()) {
        for (const auto& e : arc->entries()) {
            auto f = a.find(e.name);
            if (f && f->entry == &e) out.insert({arc.get(), &e});
        }
    }
    return out;
}

int cmdInfo(const std::string& dir) {
    DirectoryGameFolder folder(dir);
    GameData g = GameData::open(folder);
    std::printf("Exe:      %s (%zu bytes, PE time %s UTC)\n", g.exe.fileName.c_str(), g.exe.image.size(),
                utcTime(g.exe.timestamp).c_str());
    std::printf("Key:      %s\n", hex(g.key.data(), g.key.size()).c_str());
    std::printf("Window:   %s\n", g.windowName().c_str());
    std::printf("Screen:   %d x %d, code page %s\n", g.screenWidth(), g.screenHeight(), g.utf8() ? "UTF8" : "SJIS");
    for (const auto& [face, file] : g.settings->section("Font")) {
        std::printf("Font:     %s = %s\n", sjisToUtf8(face).c_str(), file.c_str());
    }
    std::printf("Archives (mount order; the last is searched first):\n");
    auto visible = visibleEntries(*g.archives);
    for (const auto& arc : g.archives->archives()) {
        std::map<std::string, std::pair<int, uint64_t>> byExt;
        uint64_t total = 0;
        int hidden = 0;
        for (const auto& e : arc->entries()) {
            auto& v = byExt[extensionOf(e.name)];
            v.first++;
            v.second += e.size;
            total += e.size;
            if (!visible.count({arc.get(), &e})) ++hidden;
        }
        std::printf("  %-22s %6zu entries %10.1f MB", arc->name().c_str(), arc->entries().size(), total / 1e6);
        if (hidden) std::printf(" (%d hidden)", hidden);
        std::printf(":");
        for (const auto& [ext, v] : byExt) std::printf(" %s %d", ext.empty() ? "(none)" : ext.c_str(), v.first);
        std::printf("\n");
    }
    return 0;
}

int cmdList(const std::string& dir, const std::string& filter) {
    DirectoryGameFolder folder(dir);
    GameData g = GameData::open(folder);
    auto visible = visibleEntries(*g.archives);
    const auto& arcs = g.archives->archives();
    for (auto it = arcs.rbegin(); it != arcs.rend(); ++it) {
        for (const auto& e : (*it)->entries()) {
            if (!matches(e.name, filter)) continue;
            bool shown = visible.count({it->get(), &e}) > 0;
            std::printf("%s %-20s %10u %3zu  %s\n", shown ? " " : "*", (*it)->name().c_str(), e.size,
                        e.segments.size(), e.name.c_str());
        }
    }
    return 0;
}

int cmdExtract(const std::string& dir, const std::string& out, const std::string& filter, bool png) {
    DirectoryGameFolder folder(dir);
    GameData g = GameData::open(folder);
    auto visible = visibleEntries(*g.archives);
    int files = 0, pictures = 0;
    for (const auto& [arc, e] : visible) {
        if (!matches(e->name, filter)) continue;
        Bytes data = arc->read(*e);
        fs::path p = fs::path(out) / fs::path(std::u8string(e->name.begin(), e->name.end()));
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
        ++files;
        if (png && imageFormat(data)[0]) {
            Image img = decodeImage(data);
            std::string target = p.string() + ".png";
            stbi_write_png(target.c_str(), img.width, img.height, 4, img.rgba.data(), img.width * 4);
            ++pictures;
        }
    }
    std::printf("%d files extracted to %s", files, out.c_str());
    if (png) std::printf(", %d pictures as PNG", pictures);
    std::printf("\n");
    return 0;
}

int cmdVerify(const std::string& dir, int threads) {
    DirectoryGameFolder folder(dir);
    GameData g = GameData::open(folder);
    struct Job {
        const NpkArchive* arc;
        const NpkEntry* entry;
    };
    std::vector<Job> jobs;
    for (const auto& arc : g.archives->archives()) {
        for (const auto& e : arc->entries()) jobs.push_back({arc.get(), &e});
    }
    std::atomic<size_t> next{0};
    std::mutex mu;
    std::vector<std::string> failures;
    std::map<std::string, int> formats;
    std::map<std::string, int> digestDiffers;  // archive + extension -> entries
    std::atomic<uint64_t> bytes{0};
    auto start = std::chrono::steady_clock::now();
    auto work = [&] {
        for (;;) {
            size_t i = next++;
            if (i >= jobs.size()) return;
            const Job& j = jobs[i];
            std::string problem;
            std::string format;
            try {
                Bytes data = j.arc->read(*j.entry);
                bytes += data.size();
                if (data.size() != j.entry->size) problem = "size";
                else {
                    if (sha256(data) != j.entry->sha256) {
                        std::lock_guard<std::mutex> lock(mu);
                        digestDiffers[j.arc->name() + " " + extensionOf(j.entry->name)]++;
                    }
                    format = imageFormat(data);
                    if (!format.empty()) {
                        Image img = decodeImage(data);
                        if (img.width <= 0 || img.height <= 0) problem = "empty picture";
                    }
                }
            } catch (const std::exception& e) {
                problem = e.what();
            }
            std::lock_guard<std::mutex> lock(mu);
            if (!problem.empty()) failures.push_back(j.arc->name() + ": " + j.entry->name + ": " + problem);
            formats[format.empty() ? extensionOf(j.entry->name) : "picture " + format]++;
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < std::max(1, threads); ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%zu entries in %zu archives, %.1f GB in %.0f s: %zu failures\n", jobs.size(),
                g.archives->archives().size(), bytes / 1e9, secs, failures.size());
    for (const auto& [f, n] : formats) std::printf("  %-14s %d\n", f.empty() ? "(none)" : f.c_str(), n);
    for (const auto& [what, n] : digestDiffers) std::printf("  stored SHA-256 differs: %s %d\n", what.c_str(), n);
    for (const auto& f : failures) std::printf("FAIL %s\n", f.c_str());
    return failures.empty() ? 0 : 1;
}

struct Click {
    uint64_t frame;
    int x, y;
};

// A 16-bit stereo WAV of interleaved float samples.
void writeWav(const std::string& path, const std::vector<float>& samples, int rate) {
    Bytes out;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto u16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    uint32_t bytes = static_cast<uint32_t>(samples.size() * 2);
    for (char c : std::string("RIFF")) out.push_back(static_cast<uint8_t>(c));
    u32(36 + bytes);
    for (char c : std::string("WAVEfmt ")) out.push_back(static_cast<uint8_t>(c));
    u32(16), u16(1), u16(2), u32(static_cast<uint32_t>(rate)), u32(static_cast<uint32_t>(rate) * 4), u16(4), u16(16);
    for (char c : std::string("data")) out.push_back(static_cast<uint8_t>(c));
    u32(bytes);
    for (float f : samples) u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f))));
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return;
    std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
}

// movie: the picture a movie's texture holds at a time (MovieFrames), as a PNG.
// natives: every native the exes register (core/src/diesel/natives_table.cpp) with this exe's address
// (0: not registered by it), whether Nyanroid implements it ("1"), has only the stub ("0") or does
// not register the class at all ("-"), and how many of the game's script files name it (a string
// literal equal to the method's name: SQObject OT_STRING 0x08000010, its length in 4 bytes, or 8 in a
// 64-bit build, then the bytes). Tab-separated.
int cmdNatives(const std::string& dir) {
    DirectoryGameFolder folder(dir);
    GameData g = GameData::open(folder);
    ScriptHost host(g, nullptr);
    const std::map<std::string, bool>& impl = host.natives();
    // The literals of every script file.
    std::map<std::string, int> files;  // literal -> script files holding it
    size_t scripts = 0;
    for (const auto& arc : g.archives->archives()) {
        for (const auto& e : arc->entries()) {
            if (e.name.rfind("media/script/", 0) != 0 || e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".nut") != 0)
                continue;
            auto data = host.readGameFile(e.name);
            if (!data) continue;
            ++scripts;
            std::set<std::string> seen;
            const Bytes& b = *data;
            size_t lenSize = g.exe.is64 ? 8 : 4;
            for (size_t i = 0; i + 4 + lenSize <= b.size(); ++i) {
                if (b[i] != 0x10 || b[i + 1] != 0 || b[i + 2] != 0 || b[i + 3] != 0x08) continue;
                uint64_t n = 0;
                for (size_t k = 0; k < lenSize; ++k) n |= static_cast<uint64_t>(b[i + 4 + k]) << (8 * k);
                if (n == 0 || n > 128 || i + 4 + lenSize + n > b.size()) continue;
                std::string s(reinterpret_cast<const char*>(b.data() + i + 4 + lenSize), static_cast<size_t>(n));
                bool ident = std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_';
                for (char c : s) ident = ident && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
                if (ident) seen.insert(s);
            }
            for (const auto& s : seen) files[s]++;
        }
    }
    const NativeExe which = nativeExe(g.exe.fileName, g.exe.is64);
    auto address = [&](const NativeName& n) -> uint64_t { return nativeAddress(n, which); };
    std::printf("# %s: %zu script files\n# class\tname\taddress\timplemented\tscript_files\n", g.exe.fileName.c_str(), scripts);
    auto row = [&](const std::string& cls, const NativeName& n) {
        std::string full = cls.empty() ? n.name : cls + "." + n.name;
        auto it = impl.find(full);
        const char* state = it == impl.end() ? "-" : it->second ? "1" : "0";
        auto f = files.find(n.name);
        std::printf("%s\t%s\t0x%llx\t%s\t%d\n", cls.empty() ? "(global)" : cls.c_str(), n.name,
                    static_cast<unsigned long long>(address(n)), state, f == files.end() ? 0 : f->second);
    };
    for (const NativeName* n = kGlobals; n->name[0]; ++n) row("", *n);
    for (const NativeClass* c = kNativeClasses; c->name[0]; ++c) {
        for (const NativeName* n = c->methods; n->name[0]; ++n) {
            if (std::strcmp(n->name, "constructor") != 0) row(c->name, *n);
        }
    }
    // Implemented names the table does not list (the exes' own globals, natives_app.cpp).
    for (const auto& [full, on] : impl) {
        bool listed = false;
        size_t dot = full.find('.');
        std::string cls = dot == std::string::npos ? "" : full.substr(0, dot), name = dot == std::string::npos ? full : full.substr(dot + 1);
        if (cls.empty()) {
            for (const NativeName* n = kGlobals; n->name[0] && !listed; ++n) listed = name == n->name;
        } else {
            for (const NativeClass* c = kNativeClasses; c->name[0] && !listed; ++c) {
                if (cls != c->name) continue;
                for (const NativeName* n = c->methods; n->name[0] && !listed; ++n) listed = name == n->name;
            }
        }
        if (!listed) {
            auto f = files.find(name);
            std::printf("%s\t%s\t?\t%d\t%d\n", cls.empty() ? "(global)" : cls.c_str(), name.c_str(), on ? 1 : 0, f == files.end() ? 0 : f->second);
        }
    }
    return 0;
}

int cmdMovie(const std::string& dir, const std::string& path, double seconds, const std::string& out, bool alpha) {
    DirectoryGameFolder folder(dir);
    GameData g = GameData::open(folder);
    std::string name = path.find("media/") == 0 ? path : "media/" + path;
    auto file = g.archives->open(name);
    if (!file) {
        std::fprintf(stderr, "%s: not in the archives\n", name.c_str());
        return 1;
    }
    auto info = readAsfInfo(*file);
    if (info) {
        std::printf("%s: %dx%d, %.3f s%s\n", name.c_str(), info->width, info->height,
                    static_cast<double>(info->duration()) / 1e7, info->hasAudio ? ", sound" : "");
    }
    MovieFrames frames([&](const std::string& p) -> std::shared_ptr<RandomAccessFile> { return g.archives->open(p); });
    DrawItem d;
    d.kind = DrawItem::Kind::Movie;
    d.texture = name;
    d.movie = 1;
    d.movieTime = static_cast<int64_t>(seconds * 1e7);
    d.movieAlpha = alpha;
    const MovieFrames::Picture* p = frames.picture(d);
    if (!p) {
        std::fprintf(stderr, "%s: no picture\n", name.c_str());
        return 1;
    }
    stbi_write_png(out.c_str(), p->width, p->height, 4, p->rgba.data(), p->width * 4);
    std::printf("== wrote %s (%dx%d)\n", out.c_str(), p->width, p->height);
    return 0;
}

struct Eval {
    uint64_t frame = 0;
    std::string code;
};

// The texture filters a draw item runs (DrawItem::filter), for --draw and --watch.
std::string filterText(const TextureFilter& f) {
    std::string out;
    char buf[160];
    if (f.ripple()) {
        std::snprintf(buf, sizeof buf, " ripple (%.1f,%.1f) power %.2f freq %.2f phase %.3f", f.rippleCenter[0],
                      f.rippleCenter[1], f.ripplePower, f.rippleFrequency, f.ripplePhase);
        out += buf;
    }
    if (f.raster()) {
        std::snprintf(buf, sizeof buf, " raster rot %.1f power %.2f freq %.2f phase %.3f", f.rasterRotate, f.rasterPower,
                      f.rasterFrequency, f.rasterPhase);
        out += buf;
    }
    if (f.radial()) {
        std::snprintf(buf, sizeof buf, " radial (%.1f,%.1f) power %.2f", f.radialCenter[0], f.radialCenter[1], f.radialPower);
        out += buf;
    }
    if (f.directionalBlur()) {
        std::snprintf(buf, sizeof buf, " directional (%.2f,%.2f) power %.2f", f.directional[0], f.directional[1],
                      f.directionalPower);
        out += buf;
    }
    if (f.lens()) {
        std::snprintf(buf, sizeof buf, " lens (%.1f,%.1f) radius %.1f distance %.2f", f.lensCenter[0], f.lensCenter[1],
                      f.lensRadius, f.lensDistance);
        out += buf;
    }
    if (f.shading()) {
        std::snprintf(buf, sizeof buf, " shading power %.2f level %d density %.1f", f.shadingPower, f.shadingLevel,
                      f.shadingDensity);
        out += buf;
    }
    if (f.negaposiOn()) out += " negaposi " + std::to_string(f.negaposi);
    if (f.monochromeOn()) {
        std::snprintf(buf, sizeof buf, " monochrome %.2f (%.2f,%.2f)", f.monochrome[0], f.monochrome[1], f.monochrome[2]);
        out += buf;
    }
    if (f.sepiaOn()) out += " sepia " + std::to_string(f.sepia);
    if (f.colorLevelOn()) {
        std::snprintf(buf, sizeof buf, " level %.2f..%.2f ^%.2f", f.colorLevel[0], f.colorLevel[1], f.colorLevel[2]);
        out += buf;
    }
    if (f.hsvOn()) {
        std::snprintf(buf, sizeof buf, " hsv %.2f %.2f %.2f glow %.2f", f.hsv[0], f.hsv[1], f.hsv[2], f.hsv[3]);
        out += buf;
    }
    return out;
}

// --key F:K: a key down at frame F and up at F + 1 (K: a letter or digit, or a Windows key code).
struct KeyPress {
    uint64_t frame = 0;
    int vk = 0;
};

struct Watch {
    uint64_t from = 0, to = 0;
    std::string text;  // part of the texture's name
};

// The k-th of the points automatic clicks go round, a click every half second: each height at the
// centre, then at the same height near the left edge (choices sit centred in Muramasa; left-aligned
// at the bottom in Totono, from x 76 of 1024 at y 485-515 for one option, a one-word option only 57
// pixels wide; some scenes ask dozens of those in a row).
void autoClickPoint(uint64_t k, int w, int h, int& x, int& y) {
    static const float kHeights[] = {0.5f, 0.87f, 0.42f, 0.6f, 0.33f, 0.7f, 0.25f, 0.8f, 0.93f};
    constexpr size_t n = sizeof kHeights / sizeof *kHeights;
    size_t i = static_cast<size_t>(k % (2 * n));
    x = static_cast<int>(static_cast<float>(w) * (i % 2 == 0 ? 0.5f : 0.09f));
    y = static_cast<int>(static_cast<float>(h) * kHeights[i / 2]);
}

int cmdBoot(const std::string& dir, int frames, int pressEvery, const std::vector<Click>& clicks,
            const std::vector<Click>& moves, const std::vector<Click>& downs, const std::vector<Click>& ups,
            bool draw, bool sounds, const std::string& wav,
            const std::vector<Eval>& evals, const std::vector<Watch>& watches, bool targets,
            const std::vector<KeyPress>& keys, const std::string& savesDir, int autoClick) {
    int shownCursor = 1;
    std::string lastTargets;
    DirectoryGameFolder folder(dir);
    GameData g = GameData::open(folder);
    // --saves DIR: the save store in a folder (kept between runs); else in memory.
    std::unique_ptr<SaveStore> saves = savesDir.empty() ? makeMemorySaveStore() : makeDirectorySaveStore(savesDir);
    ScriptHost host(g, [](const std::string& s) { std::printf("%s\n", s.c_str()); }, saves.get());
    audio::Mixer mixer(48000);
    std::unique_ptr<SoundPlayer> player;
    if (!wav.empty()) {
        player = std::make_unique<SoundPlayer>(
            &mixer, [&](const std::string& p) { return host.readGameFile(p); },
            [&](const std::string& p) { return host.openGameFile(p); });
    }
    std::vector<float> mixed;
    host.start();
    while (host.state() == ScriptHost::State::Suspended && host.frameNumber() < static_cast<uint64_t>(frames)) {
        uint64_t f = host.frameNumber();
        // --press N: Return pressed every N frames and released the next (VK_RETURN).
        if (pressEvery > 0) {
            if (f % static_cast<uint64_t>(pressEvery) == 0) host.keyEvent(0x0d, true);
            if (f % static_cast<uint64_t>(pressEvery) == 1) host.keyEvent(0x0d, false);
        }
        // --autoclick N: a left click every N frames, going round playall's points.
        if (autoClick > 0) {
            uint64_t n = static_cast<uint64_t>(autoClick);
            if (f % n == n / 2) {
                int x, y;
                autoClickPoint(f / n, g.screenWidth(), g.screenHeight(), x, y);
                host.pointerEvent(x, y);
                host.mouseButtonEvent(0, true);
            }
            if (f % n == n / 2 + 1) host.mouseButtonEvent(0, false);
        }
        // --click F:X,Y: the pointer there from frame F, the left button down at F and up at F + 1.
        for (const auto& c : clicks) {
            if (f == c.frame) {
                host.pointerEvent(c.x, c.y);
                host.mouseButtonEvent(0, true);
                std::printf("== frame %llu: click at %d,%d\n", static_cast<unsigned long long>(f), c.x, c.y);
            }
            if (f == c.frame + 1) host.mouseButtonEvent(0, false);
        }
        // --move F:X,Y: the pointer there from frame F, no button; --down / --up: and the left button.
        for (const auto& m : moves) {
            if (f == m.frame) {
                host.pointerEvent(m.x, m.y);
                std::printf("== frame %llu: pointer to %d,%d\n", static_cast<unsigned long long>(f), m.x, m.y);
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& m : pass == 0 ? downs : ups) {
                if (f != m.frame) continue;
                host.pointerEvent(m.x, m.y);
                host.mouseButtonEvent(0, pass == 0);
                std::printf("== frame %llu: left %s at %d,%d\n", static_cast<unsigned long long>(f), pass == 0 ? "down" : "up", m.x, m.y);
            }
        }
        for (const auto& k : keys) {
            if (f == k.frame) {
                host.keyEvent(k.vk, true);
                std::printf("== frame %llu: key %d\n", static_cast<unsigned long long>(f), k.vk);
            }
            if (f == k.frame + 1) host.keyEvent(k.vk, false);
        }
        // --eval F:code: Squirrel run before frame F (ScriptHost::evaluate), its result printed.
        for (const auto& e : evals) {
            if (f == e.frame) {
                std::printf("== frame %llu: eval %s -> %s\n", static_cast<unsigned long long>(f), e.code.c_str(),
                            host.evaluate(e.code).c_str());
            }
        }
        host.frame();
        // A message box the script opened (the PC's frame loop waits on it): answered with OK.
        if (auto m = host.messageBox()) {
            std::printf("== frame %llu: message box \"%s\": %s (type %d), answered OK\n", static_cast<unsigned long long>(f),
                        m->caption.c_str(), m->text.c_str(), m->type);
            host.answerMessageBox(1);
            host.frame();
        }
        // g_App.exit() (or the close button with no close callback): the exe's window closes here;
        // after Totono's RebootRequest the game starts again (the Diesel session does that; here a
        // later run with the same --saves is that start).
        if (host.quitRequested()) {
            std::printf("== frame %llu: %s\n", static_cast<unsigned long long>(f),
                        host.rebootRequested() ? "quit to restart (reboot)" : "quit");
            break;
        }
        // The pointer's shape as the window shows it (1 Windows' arrow, 7 its hand), when it changes.
        if (host.cursor() != shownCursor) {
            shownCursor = host.cursor();
            std::printf("== frame %llu: cursor %s\n", static_cast<unsigned long long>(f), shownCursor == 7 ? "hand" : "arrow");
        }
        // --targets: the render targets and masks whenever they change, with what draws into them.
        if (targets) {
            std::string now;
            for (const auto& t : host.renderTargets()) {
                size_t n = 0, masked = 0;
                for (const auto& d : host.drawItems()) n += d.target == t.id, masked += d.mask == t.id;
                now += (t.mask ? " mask " : " target ") + std::to_string(t.id) + " " + std::to_string(t.width) + "x" +
                       std::to_string(t.height) + (t.draw ? "" : " kept") + " (" + std::to_string(n) + " in, " +
                       std::to_string(masked) + " masked)";
            }
            if (now != lastTargets) {
                std::printf("== frame %llu: targets%s\n", static_cast<unsigned long long>(host.frameNumber()),
                            now.empty() ? " none" : now.c_str());
                lastTargets = now;
            }
        }
        // --watch F1-F2:TEXT: the quads of the draw items whose texture names hold TEXT, each frame.
        for (const auto& w : watches) {
            uint64_t now = host.frameNumber();
            if (now < w.from || now > w.to) continue;
            for (const auto& d : host.drawItems()) {
                if (d.texture.find(w.text) == std::string::npos && d.transitionRule.find(w.text) == std::string::npos) continue;
                const auto& q = d.quad.corner;
                std::printf("== frame %llu: %s (%.2f,%.2f) (%.2f,%.2f) a=%.2f", static_cast<unsigned long long>(now),
                            d.texture.c_str(), q[0][0], q[0][1], q[3][0], q[3][1], d.color[0][3]);
                if (d.target) std::printf(" on target %u", d.target);
                if (d.displacementMap) std::printf(" displaced by %u", d.displacementMap);
                if (!d.transitionRule.empty()) {
                    std::printf(" rule %s now %.3f rate %.3f", d.transitionRule.c_str(), d.transitionNow, d.transitionRate);
                }
                std::printf("%s\n", filterText(d.filter).c_str());
            }
        }
        auto events = host.takeSoundEvents();
        if (player) {
            player->apply(events, host.soundVoices());
            size_t n = static_cast<size_t>(48000 / std::max(1, host.frameRate()));
            size_t at = mixed.size();
            mixed.resize(at + n * 2);
            mixer.render(mixed.data() + at, n);
        }
        for (const auto& e : events) {
            if (!sounds) continue;
            const char* kinds[] = {"play", "stop", "pause", "resume", "loop"};
            std::printf("== frame %llu: sound %s voice %u", static_cast<unsigned long long>(host.frameNumber()),
                        kinds[static_cast<int>(e.kind)], e.voice);
            if (e.kind == SoundEvent::Kind::Play) {
                std::printf(" %s type %d plays %d loop %d..%d from %u%s", e.file.c_str(), e.type, e.plays, e.loopStartMs,
                            e.loopEndMs, e.startMs, e.movie ? " (movie)" : "");
            } else if (e.kind == SoundEvent::Kind::Loop) {
                std::printf(" plays %d loop %d..%d", e.plays, e.loopStartMs, e.loopEndMs);
            }
            std::printf("\n");
        }
    }
    if (sounds) {
        for (const auto& v : host.soundVoices()) {
            std::printf("== voice %u type %d gain %.3f pan %.2f frequency %.2f\n", v.voice, v.type, v.gain, v.pan,
                        v.frequency);
        }
    }
    if (!wav.empty()) {
        player.reset();
        writeWav(wav, mixed, 48000);
        std::printf("== wrote %s (%.1f s)\n", wav.c_str(), static_cast<double>(mixed.size()) / 2.0 / 48000.0);
    }
    if (draw) {
        const char* kinds[] = {"texture", "color", "screen", "movie", "glyph", "shadow", "target", "particles"};
        for (const auto& t : host.renderTargets()) {
            std::printf("== %s %u: %dx%d%s%s%s%s copy %u\n", t.mask ? "mask" : "render target", t.id, t.width, t.height,
                        t.reset ? " reset" : "", t.draw ? " draw" : " kept", t.clear ? " clear" : "", "", t.copyFrom);
        }
        std::printf("== draw list (%zu):\n", host.drawItems().size());
        for (const auto& d : host.drawItems()) {
            const auto& q = d.quad.corner;
            std::printf("  %-7s %s [%d,%d %dx%d] (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) a=%.2f blend=%d\n",
                        kinds[static_cast<int>(d.kind)], d.texture.c_str(), d.srcX, d.srcY, d.srcW, d.srcH, q[0][0],
                        q[0][1], q[1][0], q[1][1], q[2][0], q[2][1], q[3][0], q[3][1], d.color[0][3], d.blendMode);
            if (d.uvOffsetX != 0.0f || d.uvOffsetY != 0.0f) std::printf("          uv offset (%.4f,%.4f)\n", d.uvOffsetX, d.uvOffsetY);
            if (d.displacementMap) {
                std::printf("          displacement map %u scale (%.4f,%.4f)\n", d.displacementMap, d.displacementScaleX,
                            d.displacementScaleY);
            }
            if (d.kind == DrawItem::Kind::Screen) std::printf("          screen %u%s\n", d.screen, d.capture ? " capture" : "");
            if (d.kind == DrawItem::Kind::Particles && d.particles) {
                // The drawn particles' count and where their centres land on the target (clip space
                // to pixels, y down).
                const auto& vs = d.particles->vertices;
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                size_t shown = 0;
                for (size_t i = 0; i + 3 < vs.size(); i += 4) {
                    float cx = 0, cy = 0, cw = 0;
                    for (size_t k = 0; k < 4; ++k) cx += vs[i + k].x, cy += vs[i + k].y, cw += vs[i + k].w;
                    if (!(cw > 0.0f) || !(vs[i].color[3] > 0.0f)) continue;
                    float px = (cx / cw + 1.0f) * 0.5f * static_cast<float>(g.screenWidth());
                    float py = (1.0f - cy / cw) * 0.5f * static_cast<float>(g.screenHeight());
                    minX = std::min(minX, px), maxX = std::max(maxX, px), minY = std::min(minY, py), maxY = std::max(maxY, py);
                    ++shown;
                }
                std::printf("          %zu particles (%zu visible) colour mode %d, centres x %.0f..%.0f y %.0f..%.0f\n",
                            vs.size() / 4, shown, d.particles->colorMode, minX, maxX, minY, maxY);
            }
            if (d.kind == DrawItem::Kind::Movie) {
                std::printf("          movie %u at %.3f s pass %u%s\n", d.movie, static_cast<double>(d.movieTime) / 1e7,
                            d.moviePass, d.movieAlpha ? " alpha" : "");
            }
            if (d.target || d.mask || d.renderTarget) {
                std::printf("          on target %u, mask %u%s, shows target %u\n", d.target, d.mask,
                            d.maskInverse ? " (inverse)" : "", d.renderTarget);
            }
            if (d.filter.any()) std::printf("         %s\n", filterText(d.filter).c_str());
        }
    }
    const char* states[] = {"idle", "suspended", "finished", "failed"};
    std::printf("== after %llu frames: %s\n", static_cast<unsigned long long>(host.frameNumber()),
                states[static_cast<int>(host.state())]);
    if (host.state() == ScriptHost::State::Failed) std::printf("%s\n", host.lastError().c_str());
    std::printf("unhandled natives (%zu):", host.unhandled().size());
    for (const auto& [n, c] : host.unhandled()) std::printf(" %s(%d)", n.c_str(), c);
    std::printf("\nmissing files (%zu):", host.missing().size());
    for (const auto& [n, c] : host.missing()) std::printf(" %s(%d)", n.c_str(), c);
    std::printf("\n");
    return host.state() == ScriptHost::State::Failed ? 1 : 0;
}


// playall: every scene script (media/script/nut/*.nut but the boot loop) run once in a VM of its own,
// started as the framework starts a dropped file (the boot script: ::g_DroppedFileName names the
// scene; the boot loop calls it), Return pressed every 4 frames and a click every 60 at points going
// round the screen (text; choices, wherever their buttons are). A scene finishes when the next scene starts (the boot loop prints its name) or
// the game goes back to its title; else it is stuck after `frames`.
struct SceneRun {
    std::string scene;
    bool started = false, finished = false, failed = false;
    uint64_t frames = 0;
    std::string next;                      // the scene it went on to ("title" for the title)
    std::vector<std::string> errors;       // script errors, first line each
    std::string lastLine;                  // the last log line, for stuck scenes
    std::string waitsAt;                   // where a running scene's thread waits (outside the framework)
    std::map<std::string, int> unhandled, missing;
    std::map<std::string, int> traced;     // NYAN_DIESEL_TRACE's "call <name>" lines by name
    std::map<std::string, uint64_t> nativeCalls;  // --natives: implemented natives' calls
    struct RoundTrip {
        uint64_t saveFrame = 0, loadFrame = 0;  // the frames SaveVMState and the loaded scene's resume ran
        uint64_t loadVmFrame = 0;               // the frame LoadVMState ran
        uint64_t saveDoneAt = 0;                // the frame the story resumed after the save
        std::string outcome;                    // "same", "differs", "no save", "no load"
        std::vector<std::string> onlySaved, onlyLoaded;  // draw items (described) on one side only
                                                         // (saved: the story going on after the save)
        std::string movedBefore, moved;         // the last item that changed while waiting for rest
                                                // before the save (if it never rested)
    };
    std::vector<RoundTrip> trips;
};

// A draw item as text for comparing a frame before a save with one after the load: what it shows and
// where, in whole pixels and hundredths of colour (object ids differ after a load, so targets, masks,
// screens and movies are only there or not).
std::string describeItem(const DrawItem& d) {
    static const char* const kKinds[] = {"texture", "color", "screen", "movie", "glyph", "shadow", "target", "particles"};
    std::string s = kKinds[static_cast<int>(d.kind)];
    if (!d.texture.empty()) s += " " + d.texture;
    char b[160];
    if (d.glyph) {
        std::snprintf(b, sizeof b, " U+%04X", d.glyph->codepoint);
        s += b;
    }
    if (d.shadow) s += " glyphs " + std::to_string(d.shadow->glyphs.size());
    if (d.particles) s += " vertices " + std::to_string(d.particles->vertices.size());
    std::snprintf(b, sizeof b, " src %d,%d %dx%d blend %d", d.srcX, d.srcY, d.srcW, d.srcH, d.blendMode);
    s += b;
    s += " at";
    for (const auto& c : d.quad.corner) {
        std::snprintf(b, sizeof b, " %.0f,%.0f", c[0], c[1]);
        s += b;
    }
    s += " rgba";
    for (const auto& c : d.color) {
        std::snprintf(b, sizeof b, " %.2f,%.2f,%.2f,%.2f", c[0], c[1], c[2], c[3]);
        s += b;
    }
    if (!d.transitionRule.empty()) {
        std::snprintf(b, sizeof b, " transition %s %.2f", d.transitionRule.c_str(), d.transitionNow);
        s += b;
    }
    if (d.filter.any()) s += " filtered";
    if (d.target) s += " in-target";
    if (d.mask) s += d.maskInverse ? " masked-inverse" : " masked";
    if (d.kind == DrawItem::Kind::Screen) s += d.takeLastFrame ? " last-frame" : d.capture ? " live" : " frozen";
    return s;
}

// The items the renderer draws (it skips those with no alpha at any corner).
std::vector<std::string> describeFrame(const std::vector<DrawItem>& items) {
    std::vector<std::string> out;
    out.reserve(items.size());
    for (const auto& d : items) {
        bool visible = d.kind == DrawItem::Kind::Shadow || d.kind == DrawItem::Kind::Particles;
        for (const auto& c : d.color) visible = visible || c[3] > 0.0f;
        if (visible) out.push_back(describeItem(d));
    }
    return out;
}

// A description without its colours and transition progress: the same item, faded differently.
std::string withoutFade(const std::string& d) {
    std::string out = d;
    size_t a = out.find(" rgba ");
    if (a != std::string::npos) {
        size_t e = a + 6;
        for (int n = 0; n < 4 && e < out.size(); ++n) e = out.find(' ', e + 1) == std::string::npos ? out.size() : out.find(' ', e + 1);
        out.erase(a, e - a);
    }
    size_t t = out.find(" transition ");
    if (t != std::string::npos) {
        size_t sp = out.find(' ', t + 12);
        size_t e = sp == std::string::npos ? out.size() : out.find(' ', sp + 1);
        out.erase(sp, (e == std::string::npos ? out.size() : e) - sp);
    }
    return out;
}

// Two descriptions the same but for their numbers, each within a pixel (whole numbers) or 0.02
// (colours, the transition): a tween a fraction of a frame apart (timers are restored in
// milliseconds).
bool nearlySame(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    auto number = [](const std::string& s, size_t& k, double& v, bool& fraction) {
        size_t start = k;
        if (k < s.size() && s[k] == '-') ++k;
        while (k < s.size() && (std::isdigit(static_cast<unsigned char>(s[k])) || s[k] == '.')) ++k;
        fraction = s.find('.', start) < k;
        v = std::atof(s.substr(start, k - start).c_str());
    };
    while (i < a.size() && j < b.size()) {
        bool da = std::isdigit(static_cast<unsigned char>(a[i])) || (a[i] == '-' && i + 1 < a.size() && std::isdigit(static_cast<unsigned char>(a[i + 1])));
        bool db = std::isdigit(static_cast<unsigned char>(b[j])) || (b[j] == '-' && j + 1 < b.size() && std::isdigit(static_cast<unsigned char>(b[j + 1])));
        if (da && db && (i == 0 || a[i - 1] == ' ' || a[i - 1] == ',')) {
            double x, y;
            bool fx, fy;
            number(a, i, x, fx);
            number(b, j, y, fy);
            if (std::fabs(x - y) > (fx || fy ? 0.0201 : 1.0)) return false;
            continue;
        }
        if (a[i] != b[j]) return false;
        ++i, ++j;
    }
    return i == a.size() && j == b.size();
}

// The scene a log line says started, if it does: a .nut scene's boot prints its name between rules; The
// Song of Saya's tag scripts (.nps) start where SQN1ScriptReader reads them ("n1 read: path (n
// lines)"; its include files, .h, are not scenes, and menu.nps is its title).
std::optional<std::string> sceneStarted(const std::string& l) {
    const std::string mark = "━━ ";
    size_t at = l.find(mark);
    if (at != std::string::npos) {
        std::string name = l.substr(at + mark.size());
        name = name.substr(0, name.find(' '));
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".nut") == 0) name.resize(name.size() - 4);
        return name;
    }
    const std::string n1 = "n1 read: ";
    if (l.rfind(n1, 0) == 0) {
        std::string path = l.substr(n1.size(), l.find(" (") - n1.size());
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".nps") != 0) return std::nullopt;
        std::string name = path.substr(path.rfind('/') + 1);
        return name.substr(0, name.size() - 4);
    }
    return std::nullopt;
}

// Scenes started again and again without waiting a frame (Totono's credits pick what follows by the
// scene before them; started on their own, or after a scene they do not know, such as a leftover
// copy of a scene in the archive, they start themselves again) never return from the frame; the log
// stops them by throwing out of the VM, whose host is then left as it is (not freed).
struct SceneLoops {
    std::string name;  // the scene started again and again
};

// Whether the game locks saving / loading now: the framework's Status flags, or The Song of Saya's
// lock bits (its interpreter's IsLocked: n1_state.lock & N1_LOCK_SAVE / N1_LOCK_LOAD). Lines apart:
// Squirrel 2.2 wants a statement after a top-level `if` on a line of its own.
const char kLockedSave[] = "local r = getroottable()\nif (\"Status\" in r) return ::Status.lock_save ? 1 : 0\n"
                           "if (\"n1_state\" in r) return (::n1_state.lock & ::N1_LOCK_SAVE) != 0 ? 1 : 0\nreturn 0";
const char kLockedLoad[] = "local r = getroottable()\nif (\"Status\" in r) return ::Status.lock_load ? 1 : 0\n"
                           "if (\"n1_state\" in r) return (::n1_state.lock & ::N1_LOCK_LOAD) != 0 ? 1 : 0\nreturn 0";

// roundTripEvery > 0: every that many frames of the scene a quick save and a quick load (the games'
// S and L keys, the framework's ::System.ask turned off so the load does not ask), with no other input
// meanwhile. The scene is let come to rest first (its draw list the same 15 frames running, at most
// 240 frames), then saved; the story goes on 60 frames from where its threads resume after the save,
// then is loaded and goes on the same 60 frames from where they resume after the load: the two draw
// lists are compared.
SceneRun runScene(const GameData& g, const std::string& scene, uint64_t maxFrames, bool verbose,
                  uint64_t roundTripEvery = 0, bool n1 = false) {
    SceneRun r;
    enum class Trip { Idle, Settling, Saving, Loading } trip = Trip::Idle;
    uint64_t nextTrip = 0, tripStart = 0, saveDoneAt = 0, stableSince = 0;
    bool saveRan = false, saveDone = false, loadRan = false, askOff = false, moving = false;
    std::vector<std::string> savedFrame, lastFrame;
    SceneRun::RoundTrip current;
    ScriptHost* hostSeen = nullptr;
    auto drawn = [&]() -> const std::vector<DrawItem>& { return hostSeen->drawItems(); };
    // A scene at rest: its draw list the same 15 frames running (else, after `limit` frames, moving).
    auto settled = [&](uint64_t f, uint64_t since, uint64_t limit) {
        std::vector<std::string> now = describeFrame(drawn());
        if (now != lastFrame) {
            // The first item that changed, for the report.
            std::multiset<std::string> a(lastFrame.begin(), lastFrame.end()), b(now.begin(), now.end());
            std::vector<std::string> d;
            std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(d));
            current.moved = d.empty() ? "(an item gone)" : d.front();
            stableSince = f, lastFrame = std::move(now);
        }
        if (f >= stableSince + 15) return true;
        if (f >= since + limit) {
            moving = true;
            return true;
        }
        return false;
    };
    r.scene = scene;
    // The story going on after a save or load: the framework's "Resume:" lines; The Song of Saya's
    // interpreter resumes its tag script with N1_ResumeADV (traced: its dialogs end so).
    auto resumed = [&](const std::string& l) {
        return l.rfind("script: Resume:", 0) == 0 || (n1 && l.rfind("call N1_ResumeADV", 0) == 0);
    };
    std::vector<std::string> lines;
    auto saves = makeMemorySaveStore();
    int startsThisFrame = 0;
    auto hostPtr = std::make_unique<ScriptHost>(g, [&](const std::string& s) {
        lines.push_back(s);
        if (verbose) std::printf("%s\n", s.c_str());
        auto started = sceneStarted(s);
        if (started && ++startsThisFrame > 4) throw SceneLoops{*started};
    }, saves.get());
    ScriptHost& host = *hostPtr;
    hostSeen = &host;
    host.start();
    // (Saya's boot script takes the dropped name as its first tag script.)
    host.evaluate("::g_DroppedFileName <- \"" + (n1 ? "media/script/nps/" + scene + ".nps" : "media/script/nut/" + scene + ".nut") +
                  "\"; return 0");
    const std::string mark = "━━ ";
    size_t seen = 0;
    int w = g.screenWidth(), h = g.screenHeight();
    while (host.state() == ScriptHost::State::Suspended && host.frameNumber() < maxFrames) {
        uint64_t f = host.frameNumber();
        if (roundTripEvery && r.started && trip == Trip::Idle && f >= nextTrip) {
            // The framework's settings: loading without asking, and text drawn the same whether
            // read or not (else the line shown again after the load takes the read colour).
            if (!askOff) {
                // (The Song of Saya keeps them in its interpreter's config, InitN2VM.)
                // (Lines apart: Squirrel 2.2 wants a statement after a top-level `if` on a line of its own.)
                host.evaluate("local r = getroottable(); local s = null\n"
                              "if (\"System\" in r) s = ::System\n"
                              "else if (\"n1_state\" in r) s = ::n1_state.config\n"
                              "if (s != null) foreach (i, k in [\"ask\", \"read_text_color_change\"]) { if (k in s) s[k] = 0; }\n"
                              "return 0");
            }
            askOff = true;
            host.keyEvent(0x0d, false);
            host.mouseButtonEvent(0, false);
            trip = Trip::Settling;
            tripStart = f;
            saveRan = saveDone = loadRan = moving = false;
            lastFrame.clear();
            current = {};
        }
        if ((trip == Trip::Saving || trip == Trip::Loading) && f == tripStart + 1) host.keyEvent(trip == Trip::Saving ? 'S' : 'L', false);
        // Return as usual, and after the save to close its message (the story may go on: the load
        // brings it back).
        const bool input = trip == Trip::Idle || (trip == Trip::Saving && saveRan && !saveDone);
        if (input && f % 4 == 0) host.keyEvent(0x0d, true);
        if (input && f % 4 == 1) host.keyEvent(0x0d, false);
        if (trip == Trip::Idle && f % 30 == 15) {
            int x, y;
            autoClickPoint(f / 30, w, h, x, y);
            host.pointerEvent(x, y);
            host.mouseButtonEvent(0, true);
        }
        if (trip == Trip::Idle && f % 30 == 16) host.mouseButtonEvent(0, false);
        // A load that still asks (The Song of Saya's quick load always does: its dialog's
        // YES / NO): its YES clicked, as a player answers it.
        if (trip == Trip::Loading && !loadRan && f > tripStart + 10 && f % 30 == 0) {
            for (const auto& d : host.drawItems()) {
                std::string name = toLowerAscii(d.texture.substr(d.texture.rfind('/') + 1));
                if (name.find("yes") == std::string::npos || d.color[0][3] <= 0.0f) continue;
                float cx = 0, cy = 0;
                for (const auto& c : d.quad.corner) cx += c[0] / 4, cy += c[1] / 4;
                host.pointerEvent(static_cast<int>(cx), static_cast<int>(cy));
                host.mouseButtonEvent(0, true);
                break;
            }
        }
        if (trip == Trip::Loading && f % 30 == 1) host.mouseButtonEvent(0, false);
        try {
            startsThisFrame = 0;
            host.frame();
            // A message box (the load error's): answered OK, as a player would.
            if (host.messageBox()) {
                host.answerMessageBox(1);
                host.frame();
            }
        } catch (const SceneLoops& loop) {
            hostPtr.release();  // its VM was left mid-call
            saves.release();
            r.started = r.finished = true;
            r.next = loop.name + " (again and again)";
            r.unhandled = host.unhandled();
            r.missing = host.missing();
            r.nativeCalls = host.nativeCalls();
            r.frames = f;
            r.lastLine = lines.empty() ? "" : lines.back();
            return r;
        }
        for (; seen < lines.size(); ++seen) {
            const std::string& l = lines[seen];
            if (trip == Trip::Saving && l.rfind("call SaveVMState", 0) == 0 && !saveRan) {
                saveRan = true;
                current.saveFrame = f;
            } else if (trip == Trip::Saving && saveRan && !saveDone && resumed(l)) {
                saveDone = true;
                saveDoneAt = f;
                current.saveDoneAt = f;
            } else if (trip == Trip::Loading && l.rfind("call LoadVMState", 0) == 0) {
                loadRan = true;
                current.loadVmFrame = f;
            } else if (trip == Trip::Loading && loadRan && !current.loadFrame && resumed(l)) {
                current.loadFrame = f;
            }
            // The loaded scene's own start line comes again while it loads.
            if (trip == Trip::Loading && sceneStarted(l)) continue;
            // A script error: its message and the innermost script line it names.
            if (l.rfind("script error: ", 0) == 0) {
                std::string e = l.substr(14);
                size_t nl = e.find('\n');
                std::string first = e.substr(0, nl);
                if (nl != std::string::npos) {
                    size_t at = e.find("(", nl);
                    size_t close = at == std::string::npos ? at : e.find(')', at);
                    if (close != std::string::npos) first += "  [" + e.substr(at + 1, close - at - 1) + "]";
                }
                r.errors.push_back(first);
            }
            if (auto started = sceneStarted(l)) {
                const std::string& name = *started;
                if (n1 && r.started && name == "menu") {
                    r.finished = true;  // Saya's title
                    r.next = "title";
                } else if (!r.started) {
                    r.started = name == scene;
                    if (r.started) nextTrip = f + roundTripEvery;
                    if (!r.started) r.next = name;  // something else ran first
                } else {
                    // Another scene, or this one again.
                    r.finished = true;
                    r.next = name == scene ? scene + " (again)" : name;
                }
            } else if (r.started && (l == "script: TitleSet" || l == "script: LogoSet")) {
                r.finished = true;
                r.next = "title";
            }
        }
        if (std::getenv("NYAN_ROUNDTRIP_DEBUG") && ((trip == Trip::Saving && saveDone) || (trip == Trip::Loading && current.loadFrame))) {
            int typed = 0, typing = 0, hidden = 0;
            for (const auto& d : host.drawItems()) {
                if (d.kind != DrawItem::Kind::Glyph) continue;
                if (d.color[0][3] >= 0.999f) ++typed;
                else if (d.color[0][3] > 0.0f) ++typing;
                else ++hidden;
            }
            std::fprintf(stderr, "rt %s +%llu: %d typed, %d typing, %d hidden\n", trip == Trip::Saving ? "saved " : "loaded",
                         static_cast<unsigned long long>(f - (trip == Trip::Saving ? saveDoneAt : current.loadFrame)), typed, typing, hidden);
        }
        if (const char* e = std::getenv("NYAN_ROUNDTRIP_EVAL")) {
            if ((trip == Trip::Saving && saveDone && f == saveDoneAt + 30) || (trip == Trip::Loading && current.loadFrame && f == current.loadFrame + 30))
                std::fprintf(stderr, "rt eval %s: %s\n", trip == Trip::Saving ? "saved" : "loaded", host.evaluate(e).c_str());
        }
        if (trip == Trip::Settling && settled(f, tripStart, 240)) {
            // What the scene shows as the player saves (the save's dialog covers it from the next
            // frame on, the story's threads paused).
            if (moving) current.movedBefore = current.moved;
            current.moved.clear();
            if (const char* e = std::getenv("NYAN_ROUNDTRIP_EVAL")) std::fprintf(stderr, "rt eval before: %s\n", host.evaluate(e).c_str());
            moving = false;
            // The Song of Saya's quick load takes the newest quick save by its time(), in whole
            // seconds (its N1_QuickLoad), and a headless run's saves fall within one:
            // the list cleared first, so the load finds the save just made.
            if (n1) host.evaluate("local r = getroottable()\nif (\"n1_state\" in r) ::n1_state.quick_save_slot.clear()\nreturn 0");
            host.keyEvent('S', true);
            trip = Trip::Saving;
            tripStart = f + 1;
        } else if (trip == Trip::Saving) {
            // The story as it goes on after the save: 60 frames after its threads resume.
            if (saveDone && f == saveDoneAt) host.keyEvent(0x0d, false);
            if (saveDone && f == saveDoneAt + 60) savedFrame = describeFrame(host.drawItems());
            if (saveDone && f >= saveDoneAt + 61) {
                host.keyEvent('L', true);
                trip = Trip::Loading;
                tripStart = f + 1;
            } else if (f > tripStart + 600) {
                current.outcome = saveRan ? "save did not end" : "no save";
                // The game's own lock (the end roll, battles, death ends: saving is off there).
                if (!saveRan && host.evaluate(kLockedSave) == "1") current.outcome += " (locked)";
                r.trips.push_back(current);
                trip = Trip::Idle;
                nextTrip = f + roundTripEvery;
            }
        } else if (trip == Trip::Loading) {
            // The loaded story as it goes on, the same 60 frames after its threads resume.
            if (current.loadFrame && f == current.loadFrame + 60) {
                std::vector<std::string> loaded = describeFrame(host.drawItems());
                std::multiset<std::string> a(savedFrame.begin(), savedFrame.end()), b(loaded.begin(), loaded.end());
                std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(current.onlySaved));
                std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(current.onlyLoaded));
                // Pairs a fraction of a frame apart are the same.
                for (size_t i = 0; i < current.onlySaved.size();) {
                    auto& l = current.onlyLoaded;
                    auto it = std::find_if(l.begin(), l.end(), [&](const std::string& x) { return nearlySame(current.onlySaved[i], x); });
                    if (it == l.end()) {
                        ++i;
                        continue;
                    }
                    l.erase(it);
                    current.onlySaved.erase(current.onlySaved.begin() + static_cast<std::ptrdiff_t>(i));
                }
                // Only fades apart (the same items in the same places): the stories' timing.
                std::multiset<std::string> fa, fb;
                for (const auto& x : current.onlySaved) fa.insert(withoutFade(x));
                for (const auto& x : current.onlyLoaded) fb.insert(withoutFade(x));
                current.outcome = current.onlySaved.empty() && current.onlyLoaded.empty() ? "same"
                                  : fa == fb                                               ? "fade timing"
                                                                                           : "differs";
                if (!current.movedBefore.empty()) current.outcome += " (moving)";
            } else if (!current.loadFrame && f > tripStart + 600) {
                current.moved.clear();
                current.outcome = loadRan ? "load did not resume" : "no load";
                if (!loadRan && host.evaluate(kLockedLoad) == "1") current.outcome += " (locked)";
            }
            if (!current.outcome.empty()) {
                r.trips.push_back(current);
                savedFrame.clear();
                trip = Trip::Idle;
                nextTrip = f + roundTripEvery;
            }
        }
        // The game quit (exit(): the PC's window closes, no frame follows), or quit to start again.
        if (r.started && !r.finished && host.quitRequested()) {
            r.finished = true;
            r.next = host.rebootRequested() ? "reboot" : "quit";
        }
        if (r.finished || host.quitRequested()) break;
    }
    r.failed = host.state() == ScriptHost::State::Failed;
    if (r.failed && r.errors.empty()) r.errors.push_back(host.lastError());
    if (!r.finished && !r.failed && !n1) {
        // The scene's thread: its innermost call outside the framework, and the scene's own line.
        std::string stacks = host.threadStacks();
        if (verbose) std::printf("%s", stacks.c_str());
        const std::string own = "media/script/nut/" + scene + ".nut:";
        for (size_t at = 0; at < stacks.size();) {
            size_t end = stacks.find("thread:", at + 1);
            std::string block = stacks.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? stacks.size() : end;
            if (block.find(own) == std::string::npos) continue;
            std::string inner, line;
            for (size_t p = 0; p < block.size();) {
                size_t nl = block.find('\n', p);
                std::string l = block.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
                p = nl == std::string::npos ? block.size() : nl + 1;
                if (l.rfind("  at ", 0) != 0) continue;
                l = l.substr(5);
                if (inner.empty() && l.find("/framework/") == std::string::npos) inner = l;
                if (line.empty() && l.find(own) != std::string::npos) line = l.substr(l.find(own) + 17);
            }
            r.waitsAt = inner;
            if (!line.empty() && inner.find(own) == std::string::npos) r.waitsAt += " from " + line.substr(0, line.size() - 1);
            break;
        }
    }
    r.frames = host.frameNumber();
    for (const auto& l : lines) {
        if (l.rfind("call ", 0) == 0) r.traced[l.substr(5, l.find(' ', 5) - 5)]++;
    }
    if (!lines.empty()) r.lastLine = lines.back().substr(0, lines.back().find('\n'));
    r.unhandled = host.unhandled();
    r.missing = host.missing();
    r.nativeCalls = host.nativeCalls();
    return r;
}

int cmdPlayAll(const std::string& dir, const std::string& only, uint64_t maxFrames, int threads, uint64_t roundTrip,
               const std::string& nativesOut) {
    if (!nativesOut.empty()) setenv("NYAN_DIESEL_COUNT_NATIVES", "1", 1);
    if (roundTrip) {
        // The framework's save and load functions mark the frames the round trip compares.
        const char* t = std::getenv("NYAN_DIESEL_TRACE");
        std::string list = std::string(t ? t : "") + (t && *t ? "," : "") + "SaveVMState,LoadVMState";
        setenv("NYAN_DIESEL_TRACE", list.c_str(), 1);
    }
    DirectoryGameFolder folder(dir);
    GameData g = GameData::open(folder);
    std::vector<std::string> scenes;
    // The scenes: media/script/nut/*.nut, or The Song of Saya's tag scripts media/script/nps/*.nps
    // (its title, menu.nps, as well).
    std::set<std::string> names;
    bool n1 = false;
    for (const char* kind : {"nut", "nps"}) {
        const std::string prefix = std::string("media/script/") + kind + "/", ext = std::string(".") + kind;
        for (const auto& arc : g.archives->archives()) {
            for (const auto& e : arc->entries()) {
                if (e.name.rfind(prefix, 0) != 0 || e.name.size() < prefix.size() + 5) continue;
                if (e.name.compare(e.name.size() - 4, 4, ext) != 0) continue;
                std::string name = e.name.substr(prefix.size(), e.name.size() - prefix.size() - 4);
                if (name == "0_boot_start" || name.find('/') != std::string::npos) continue;
                if (only.empty() || name == only) names.insert(name);
            }
        }
        if (!names.empty()) {
            n1 = std::string(kind) == "nps";
            break;
        }
    }
    scenes.assign(names.begin(), names.end());
    if (roundTrip && n1) {
        std::string list = std::getenv("NYAN_DIESEL_TRACE");
        setenv("NYAN_DIESEL_TRACE", (list + ",N1_ResumeADV").c_str(), 1);
    }
    if (scenes.empty()) {
        std::fprintf(stderr, "no scene %s\n", only.c_str());
        return 1;
    }
    bool verbose = !only.empty();
    std::vector<SceneRun> runs(scenes.size());
    std::atomic<size_t> next{0}, done{0};
    auto start = std::chrono::steady_clock::now();
    auto work = [&] {
        // Each thread its own archives (their readers keep state).
        DirectoryGameFolder f(dir);
        GameData gd = GameData::open(f);
        for (;;) {
            size_t i = next++;
            if (i >= scenes.size()) return;
            if (std::getenv("NYAN_PLAYALL_TRACE")) std::fprintf(stderr, "start %s\n", scenes[i].c_str());
            auto t0 = std::chrono::steady_clock::now();
            try {
                runs[i] = runScene(gd, scenes[i], maxFrames, verbose, roundTrip, n1);
            } catch (const std::exception& e) {
                runs[i].scene = scenes[i];
                runs[i].failed = true;
                runs[i].errors.push_back(std::string("exception: ") + e.what());
            }
            if (std::getenv("NYAN_PLAYALL_TRACE")) {
                std::fprintf(stderr, "end %s %.1f s %llu frames\n", scenes[i].c_str(),
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                             static_cast<unsigned long long>(runs[i].frames));
            }
            size_t n = ++done;
            if (!verbose && n % 50 == 0) std::fprintf(stderr, "%zu / %zu scenes\n", n, scenes.size());
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < std::max(1, std::min<int>(threads, static_cast<int>(scenes.size()))); ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    size_t stuck = 0, failed = 0, withErrors = 0;
    uint64_t frames = 0;
    std::map<std::string, std::pair<int, int>> unhandled;  // name -> scenes, calls
    std::map<std::string, std::vector<std::string>> unhandledScenes;
    std::map<std::string, std::pair<int, int>> traced;  // NYAN_DIESEL_TRACE: name -> scenes, calls
    std::map<std::string, std::vector<std::string>> tracedScenes;
    std::map<std::string, std::vector<std::string>> missing;  // file -> scenes
    std::map<std::string, std::vector<std::string>> errorScenes;  // error -> scenes
    // A name the scripts look up and do not find, which the exe does not hold either (so no native of
    // the engine's): script state (route flags, functions) the earlier scenes set up, which a scene
    // started on its own lacks; not the engine's doing.
    auto definedElsewhere = [&](const std::string& error, const std::string&) {
        const std::string head = "the index '";
        if (error.rfind(head, 0) != 0) return false;
        size_t end = error.find('\'', head.size());
        if (end == std::string::npos) return false;
        return !g.exe.containsString(error.substr(head.size(), end - head.size()));
    };
    std::map<std::string, std::vector<std::string>> routeErrors;
    for (const auto& r : runs) {
        frames += r.frames;
        if (!r.finished) ++stuck;
        if (r.failed) ++failed;
        if (!r.errors.empty()) ++withErrors;
        for (const auto& [n, c] : r.unhandled) {
            unhandled[n].first++, unhandled[n].second += c;
            unhandledScenes[n].push_back(r.scene);
        }
        for (const auto& [n, c] : r.missing) missing[n].push_back(r.scene);
        for (const auto& [n, c] : r.traced) {
            traced[n].first++, traced[n].second += c;
            tracedScenes[n].push_back(r.scene);
        }
        std::set<std::string> once;
        for (const auto& e : r.errors) once.insert(e.substr(0, e.find("  [")));
        for (const auto& e : once) {
            if (definedElsewhere(e, r.scene)) routeErrors[e].push_back(r.scene);
            else errorScenes[e].push_back(r.scene);
        }
    }
    size_t routeOnly = 0;
    for (const auto& r : runs) {
        bool other = false;
        for (const auto& e : r.errors) other = other || !definedElsewhere(e.substr(0, e.find("  [")), r.scene);
        routeOnly += !r.errors.empty() && !other;
    }
    std::printf("Ran %zu scenes (%llu frames, %.0f s): %zu stuck, %zu failed, %zu with script errors (%zu of them only "
                "route state)\n", runs.size(), static_cast<unsigned long long>(frames), secs, stuck, failed, withErrors,
                routeOnly);
    if (stuck) {
        std::printf("\nDid not finish in %llu frames:\n", static_cast<unsigned long long>(maxFrames));
        for (const auto& r : runs) {
            if (r.finished) continue;
            if (!r.waitsAt.empty()) {
                std::printf("  %s: waits in %s\n", r.scene.c_str(), r.waitsAt.c_str());
                continue;
            }
            std::printf("  %s: %s%s (last: %s)\n", r.scene.c_str(), r.failed ? "failed" : r.started ? "running" : "never started",
                        !r.started && !r.next.empty() ? (", ran " + r.next).c_str() : "", r.lastLine.c_str());
        }
    }
    bool header = false;
    for (const auto& r : runs) {
        bool loop = r.next.find(" (again and again)") != std::string::npos;
        if (!loop && r.next.find(" (again)") == std::string::npos) continue;
        if (!header) std::printf("\nStarted themselves again, or a scene again and again within a frame (stopped there):\n");
        header = true;
        std::printf("  %s -> %s%s\n", r.scene.c_str(), r.next.substr(0, r.next.find(" (again")).c_str(),
                    loop ? " (again and again)" : " (again)");
    }
    // Scenes that quit the game (exit(): Totono's credits' ExitGame), and that quit to start it
    // again (Totono's Reboot: RebootRequest, then exit()), in rows of eight.
    for (const char* kind : {"quit", "reboot"}) {
        size_t quits = 0;
        for (const auto& r : runs) {
            if (r.next != kind) continue;
            if (quits == 0) {
                std::printf(r.next == "quit" ? "\nQuit the game (exit(): the PC's window closes there):\n"
                                             : "\nRestarted the game (RebootRequest, then exit()):\n");
            }
            std::printf(quits % 8 == 0 ? "  %s" : " %s", r.scene.c_str());
            if (++quits % 8 == 0) std::printf("\n");
        }
        if (quits % 8 != 0) std::printf("\n");
    }
    auto where = [&](const std::string& scene, const std::string& e) {
        for (const auto& r : runs) {
            if (r.scene != scene) continue;
            for (const auto& x : r.errors) {
                if (x.rfind(e, 0) == 0 && x.size() > e.size()) return x.substr(e.size());
            }
        }
        return std::string();
    };
    if (!routeErrors.empty()) {
        std::printf("\nScript names not found that the exe does not hold (route state from earlier scenes; %zu):\n",
                    routeErrors.size());
        for (const auto& [e, sc] : routeErrors) {
            std::printf("  %4zu  %s:", sc.size(), e.c_str());
            for (size_t i = 0; i < sc.size() && i < 6; ++i) std::printf(" %s", sc[i].c_str());
            std::printf(sc.size() > 6 ? " ...\n" : "\n");
        }
    }
    if (!errorScenes.empty()) {
        std::printf("\nScript errors (%zu kinds):\n", errorScenes.size());
        for (const auto& [e, sc] : errorScenes) {
            std::printf("  %4zu  %s%s\n        in", sc.size(), e.c_str(), where(sc[0], e).c_str());
            for (size_t i = 0; i < sc.size() && i < 8; ++i) std::printf(" %s", sc[i].c_str());
            if (sc.size() > 8) std::printf(" ...");
            std::printf("\n");
        }
    }
    std::printf("\nUnhandled natives (%zu kinds; scenes, calls):\n", unhandled.size());
    std::vector<std::pair<std::string, std::pair<int, int>>> u(unhandled.begin(), unhandled.end());
    std::sort(u.begin(), u.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    // Natives few scenes call name those scenes.
    for (const auto& [n, c] : u) {
        std::printf("  %4d %7d  %s", c.first, c.second, n.c_str());
        const auto& sc = unhandledScenes[n];
        if (sc.size() <= 4) {
            std::printf("  (");
            for (size_t i = 0; i < sc.size(); ++i) std::printf("%s%s", i ? " " : "", sc[i].c_str());
            std::printf(")");
        }
        std::printf("\n");
    }
    if (!traced.empty()) {
        std::printf("\nTraced calls (NYAN_DIESEL_TRACE; %zu names; scenes, calls):\n", traced.size());
        for (const auto& [n, c] : traced) {
            std::printf("  %4d %7d  %s", c.first, c.second, n.c_str());
            const auto& sc = tracedScenes[n];
            if (sc.size() <= 4) {
                std::printf("  (");
                for (size_t i = 0; i < sc.size(); ++i) std::printf("%s%s", i ? " " : "", sc[i].c_str());
                std::printf(")");
            }
            std::printf("\n");
        }
    }
    if (roundTrip) {
        // Round trips: how many came back the same, and the scenes whose draw lists differ.
        std::map<std::string, int> outcomes;
        for (const auto& r : runs) {
            for (const auto& t : r.trips) outcomes[t.outcome]++;
        }
        std::printf("\nSave / load round trips (every %llu frames):", static_cast<unsigned long long>(roundTrip));
        for (const auto& [o, n] : outcomes) std::printf(" %d %s", n, o.c_str());
        std::printf("\n");
        for (const auto& r : runs) {
            for (const auto& t : r.trips) {
                if (t.outcome == "same" || t.outcome == "same (moving)") continue;
                std::printf("  %s save at %llu: %s", r.scene.c_str(), static_cast<unsigned long long>(t.saveFrame), t.outcome.c_str());
                if (t.outcome.rfind("differs", 0) != 0) {
                    std::printf("\n");
                    continue;
                }
                std::printf(" (%zu only saved, %zu only loaded; resumed at %llu)\n", t.onlySaved.size(), t.onlyLoaded.size(),
                            static_cast<unsigned long long>(t.loadFrame));
                if (std::getenv("NYAN_ROUNDTRIP_DEBUG"))
                    std::printf("    frames from the save to its resume %lld, from the load to its resume %lld\n",
                                static_cast<long long>(t.saveDoneAt - t.saveFrame), static_cast<long long>(t.loadFrame - t.loadVmFrame));
                if (!t.movedBefore.empty()) std::printf("    moving before the save: %s\n", t.movedBefore.c_str());
                size_t shown = verbose ? SIZE_MAX : 3;
                for (size_t i = 0; i < t.onlySaved.size() && i < shown; ++i) std::printf("    - %s\n", t.onlySaved[i].c_str());
                for (size_t i = 0; i < t.onlyLoaded.size() && i < shown; ++i) std::printf("    + %s\n", t.onlyLoaded[i].c_str());
            }
        }
    }
    if (!nativesOut.empty()) {
        // Every native the scenes called: scenes and calls (implemented ones counted, stubs noted).
        std::map<std::string, std::pair<uint64_t, uint64_t>> calls;
        for (const auto& r : runs) {
            for (const auto& [n, c] : r.nativeCalls) calls[n].first++, calls[n].second += c;
            for (const auto& [n, c] : r.unhandled) calls[n].first++, calls[n].second += static_cast<uint64_t>(c);
        }
        if (FILE* f = std::fopen(nativesOut.c_str(), "w")) {
            std::fprintf(f, "# name\tscenes\tcalls (%zu scenes)\n", runs.size());
            for (const auto& [n, c] : calls) std::fprintf(f, "%s\t%llu\t%llu\n", n.c_str(), static_cast<unsigned long long>(c.first),
                                                          static_cast<unsigned long long>(c.second));
            std::fclose(f);
        }
    }
    std::printf("\nMissing files (%zu; scenes):\n", missing.size());
    for (const auto& [n, sc] : missing) {
        std::printf("  %4zu  %s  (", sc.size(), n.c_str());
        for (size_t i = 0; i < sc.size() && i < 4; ++i) std::printf("%s%s", i ? " " : "", sc[i].c_str());
        std::printf("%s)\n", sc.size() > 4 ? " ..." : "");
    }
    return stuck || failed ? 1 : 0;
}

std::optional<int> run(const std::vector<std::string>& args) {
    if (args.size() < 2 || !isDieselDir(args[1])) return std::nullopt;
    const std::string& cmd = args[0];
    if (cmd == "info" && args.size() == 2) return cmdInfo(args[1]);
    if (cmd == "list" && args.size() <= 3) return cmdList(args[1], args.size() == 3 ? args[2] : "");
    if (cmd == "extract" && args.size() >= 3) {
        std::string filter;
        bool png = false;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--png") png = true;
            else filter = args[i];
        }
        return cmdExtract(args[1], args[2], filter, png);
    }
    if (cmd == "boot") {
        int frames = 600, press = 0, autoClick = 0;
        bool draw = false, sounds = false;
        std::string wav;
        std::vector<Click> clicks, moves, downs, ups;
        std::vector<Eval> evals;
        std::vector<Watch> watches;
        std::vector<KeyPress> keys;
        std::string savesDir;
        bool targets = false;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--press" && i + 1 < args.size()) {
                press = std::atoi(args[++i].c_str());
            } else if (args[i] == "--autoclick" && i + 1 < args.size()) {
                autoClick = std::atoi(args[++i].c_str());
            } else if (args[i] == "--click" && i + 1 < args.size()) {
                unsigned long long f = 0;
                int x = 0, y = 0;
                if (std::sscanf(args[++i].c_str(), "%llu:%d,%d", &f, &x, &y) != 3) return std::nullopt;
                clicks.push_back({f, x, y});
            } else if ((args[i] == "--move" || args[i] == "--down" || args[i] == "--up") && i + 1 < args.size()) {
                auto& list = args[i] == "--move" ? moves : args[i] == "--down" ? downs : ups;
                unsigned long long f = 0;
                int x = 0, y = 0;
                if (std::sscanf(args[++i].c_str(), "%llu:%d,%d", &f, &x, &y) != 3) return std::nullopt;
                list.push_back({f, x, y});
            } else if (args[i] == "--eval" && i + 1 < args.size()) {
                const std::string& a = args[++i];
                size_t colon = a.find(':');
                if (colon == std::string::npos) return std::nullopt;
                evals.push_back({std::strtoull(a.c_str(), nullptr, 10), a.substr(colon + 1)});
            } else if (args[i] == "--watch" && i + 1 < args.size()) {
                const std::string& a = args[++i];
                Watch w;
                char* end = nullptr;
                w.from = std::strtoull(a.c_str(), &end, 10);
                w.to = end && *end == '-' ? std::strtoull(end + 1, &end, 10) : w.from;
                if (!end || *end != ':') return std::nullopt;
                w.text = end + 1;
                watches.push_back(w);
            } else if (args[i] == "--key" && i + 1 < args.size()) {
                const std::string& a = args[++i];
                size_t colon = a.find(':');
                if (colon == std::string::npos || colon + 1 >= a.size()) return std::nullopt;
                std::string k = a.substr(colon + 1);
                int vk = k.size() == 1 && std::isalnum(static_cast<unsigned char>(k[0]))
                             ? std::toupper(static_cast<unsigned char>(k[0]))
                             : std::atoi(k.c_str());
                keys.push_back({std::strtoull(a.c_str(), nullptr, 10), vk});
            } else if (args[i] == "--saves" && i + 1 < args.size()) {
                savesDir = args[++i];
            } else if (args[i] == "--targets") {
                targets = true;
            } else if (args[i] == "--draw") {
                draw = true;
            } else if (args[i] == "--sounds") {
                sounds = true;
            } else if (args[i] == "--wav" && i + 1 < args.size()) {
                wav = args[++i];
            } else {
                frames = std::atoi(args[i].c_str());
            }
        }
        return cmdBoot(args[1], frames, press, clicks, moves, downs, ups, draw, sounds, wav, evals, watches, targets, keys,
                       savesDir, autoClick);
    }
    if (cmd == "movie" && args.size() >= 5) {
        bool alpha = args.size() >= 6 && args[5] == "--alpha";
        return cmdMovie(args[1], args[2], std::atof(args[3].c_str()), args[4], alpha);
    }
    if (cmd == "playall") {
        std::string only;
        uint64_t frames = 60000, roundTrip = 0;
        std::string nativesOut;
        int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--threads" && i + 1 < args.size()) threads = std::atoi(args[++i].c_str());
            else if (args[i] == "--frames" && i + 1 < args.size()) frames = std::strtoull(args[++i].c_str(), nullptr, 10);
            else if (args[i] == "--roundtrip" && i + 1 < args.size()) roundTrip = std::strtoull(args[++i].c_str(), nullptr, 10);
            else if (args[i] == "--natives" && i + 1 < args.size()) nativesOut = args[++i];
            else only = args[i];
        }
        return cmdPlayAll(args[1], only, frames, threads, roundTrip, nativesOut);
    }
    if (cmd == "natives" && args.size() >= 2) return cmdNatives(args[1]);
    if (cmd == "verify") {
        int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2));
        for (size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "--threads") threads = std::atoi(args[i + 1].c_str());
        }
        return cmdVerify(args[1], threads);
    }
    return std::nullopt;
}

}  // namespace

namespace nyan::cli {
const CommandSet kDieselCommands{Engine::Diesel, usage, run};
}
