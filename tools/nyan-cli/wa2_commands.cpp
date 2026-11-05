// nyan-cli's White Album 2 (WA2) commands: archives, executables, file checks.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "commands.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/common/sjis.hpp"
#include "nyan/media/ffmpeg_video.hpp"
#include "nyan/wa2/assets.hpp"
#include "nyan/wa2/exe_info.hpp"
#include "nyan/wa2/game.hpp"
#include "nyan/wa2/image.hpp"
#include "nyan/wa2/script.hpp"
#include "stb_image_write.h"

namespace fs = std::filesystem;
using namespace nyan;
using namespace nyan::wa2;

namespace {

void usage() {
    std::puts(
        "  White Album 2 folders (WA2):\n"
        "  nyan-cli info    <game-dir>                   Executables, archives, the introductory chapter\n"
        "  nyan-cli list    <game-dir> [filter]          List archive entries (* = the engine never reaches it)\n"
        "  nyan-cli extract <game-dir> <out-dir> [filter] [--png]\n"
        "                                                Extract entries, unpacked, to <out-dir>/<archive>/;\n"
        "                                                --png also writes each picture as PNG\n"
        "  nyan-cli image   <game-dir> <group> <name> <out.png> [--type N] [--level N] [--amp NAME]...\n"
        "                                                One picture as an image slot loads it (type, colour\n"
        "                                                level, .amp tables from grp)\n"
        "  nyan-cli bnr     <game-dir> <script>          A script's listing (expressions and calls rebuilt)\n"
        "  nyan-cli movie   <game-dir> <file> [out-dir] [seconds...]\n"
        "                                                A movie's streams; frames at those times as PNG\n"
        "  nyan-cli scripts <game-dir>                   Decode every script the engine reaches; function use\n"
        "  nyan-cli playall <game-dir> [script]          Run every script (or one) headless, skipping, first\n"
        "                                                choice; report stuck scripts, errors, unhandled functions\n"
        "  nyan-cli verify  <game-dir> [--threads N]     Unpack every entry, check sizes and signatures,\n"
        "                                                decode every picture");
}

bool isWa2Dir(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    try {
        return detectEngine(DirectoryGameFolder(dir)) == Engine::WA2;
    } catch (const std::exception&) {
        return false;
    }
}

std::string extensionOf(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos ? "" : toLowerAscii(name.substr(dot + 1));
}

bool matches(const std::string& utf8Name, const std::string& filter) {
    return filter.empty() || toLowerAscii(utf8Name).find(toLowerAscii(filter)) != std::string::npos;
}

std::string utcTime(uint32_t t) {
    std::time_t tt = t;
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// The archive's path without `.pak` ("IC/bak"), where its entries are extracted.
std::string archiveDir(const std::string& path) {
    auto dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(0, dot);
}

// One KCAP entry as listed: the folder en.pak's packer put it in, and the group a lookup of it
// would be for (en.pak serves every group, so any one finds its entries).
struct Listed {
    const Assets::ArchiveInfo* info;
    const KcapEntry* entry;
    std::string folder;
};

std::vector<Listed> kcapEntries(const std::vector<Assets::ArchiveInfo>& archives) {
    std::vector<Listed> out;
    for (const auto& info : archives) {
        std::string folder;
        for (const auto& e : info.archive->entries()) {
            if (e.folder()) {
                folder = e.name;
                continue;
            }
            out.push_back({&info, &e, folder});
        }
    }
    return out;
}

int cmdInfo(const std::string& dir) {
    Assets assets(std::make_unique<DirectoryGameFolder>(dir));
    const GameFolder& folder = assets.folder();
    std::printf("Folder: %s\n\nExecutables:\n", dir.c_str());
    for (const auto& name : folder.list()) {
        std::string ext = extensionOf(name);
        if (ext != "exe" && ext != "dll") continue;
        Bytes image = folder.open(name)->readAll();
        auto info = readExeInfo(image);
        std::printf("  %-20s %9zu bytes", name.c_str(), image.size());
        if (!info) {
            std::printf("  (not a PE file)\n");
            continue;
        }
        std::printf("  linked %s UTC", utcTime(info->timestamp).c_str());
        auto get = [&](const char* k) {
            auto it = info->strings.find(k);
            return it == info->strings.end() ? std::string() : it->second;
        };
        if (!get("ProductName").empty()) {
            std::printf("  %s %s", get("ProductName").c_str(), get("ProductVersion").c_str());
            if (!get("CompanyName").empty()) std::printf(" (%s)", get("CompanyName").c_str());
        } else {
            std::printf("  no version strings");
        }
        std::printf("\n");
    }
    std::printf("\nIntroductory chapter: %s\n", assets.introductoryChapterBundled() ? "bundled in IC/" : "not bundled");

    std::printf("\nKCAP archives (pictures, scripts, fonts), in lookup order:\n");
    std::printf("  %-16s %-8s %8s %8s %8s %8s %8s\n", "Archive", "Group", "Entries", "Folders", "Packed", "Stored", "Empty");
    size_t total = 0;
    for (const auto& a : assets.kcapArchives()) {
        size_t folders = 0, packed = 0, stored = 0, empty = 0;
        for (const auto& e : a.archive->entries()) {
            if (e.folder()) ++folders;
            else if (e.size == 0) ++empty;
            else if (e.packed()) ++packed;
            else ++stored;
        }
        total += a.archive->entries().size() - folders;
        std::printf("  %-16s %-8s %8zu %8zu %8zu %8zu %8zu\n", a.path.c_str(), a.group.empty() ? "(all)" : a.group.c_str(),
                    a.archive->entries().size() - folders, folders, packed, stored, empty);
    }
    std::printf("  %zu entries\n", total);

    std::printf("\nSound archives (LAC), in the engine's order:\n");
    size_t index = 0;
    for (const auto& a : assets.soundArchives()) {
        std::printf("  %zu  %-16s %8zu entries\n", index++, a->name().c_str(), a->entries().size());
    }

    // The rest of the .pak files: the movies are ASF files under that name.
    static const uint8_t kAsf[16] = {0x30, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11,
                                     0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c};
    std::vector<std::string> others;
    auto scan = [&](const std::string& sub, const std::vector<std::string>& names) {
        for (const auto& n : names) {
            if (extensionOf(n) != "pak") continue;
            std::string path = sub.empty() ? n : sub + "/" + n;
            auto f = folder.open(path);
            if (!f || f->size() < 16) continue;
            Bytes head = f->readAt(0, 16);
            if (!std::memcmp(head.data(), "KCAP", 4) || !std::memcmp(head.data(), "LAC\0", 4)) continue;
            char line[160];
            std::snprintf(line, sizeof line, "  %-16s %11llu bytes  %s", path.c_str(), static_cast<unsigned long long>(f->size()),
                          std::memcmp(head.data(), kAsf, 16) == 0 ? "ASF (movie)" : "unknown");
            others.push_back(line);
        }
    };
    scan("", folder.list());
    scan("IC", folder.listDir("IC"));
    if (!others.empty()) {
        std::printf("\nOther .pak files:\n");
        for (const auto& l : others) std::printf("%s\n", l.c_str());
    }
    return 0;
}

int cmdList(const std::string& dir, const std::string& filter) {
    Assets assets(std::make_unique<DirectoryGameFolder>(dir));
    auto archives = assets.kcapArchives();
    for (const auto& l : kcapEntries(archives)) {
        std::string shown = sjisToUtf8(l.folder.empty() ? l.entry->name : l.folder + "/" + l.entry->name);
        if (!matches(shown, filter)) continue;
        // Reached: what the engine's lookup for this name in the archive's group finds is this entry.
        std::string group = l.info->group.empty() ? (l.folder.empty() ? "-" : l.folder) : l.info->group;
        auto hit = assets.locate(group, l.entry->name);
        bool reached = hit && hit->entry == l.entry;
        uint32_t unpacked = l.info->archive->unpackedSize(*l.entry);
        std::printf("%c %-14s %10u %10u  %s\n", reached ? ' ' : '*', l.info->path.c_str(), l.entry->size, unpacked,
                    shown.c_str());
    }
    for (const auto& a : assets.soundArchives()) {
        for (const auto& e : a->entries()) {
            std::string shown = sjisToUtf8(e.name);
            if (!matches(shown, filter)) continue;
            std::printf("  %-14s %10u %10u  %s\n", a->name().c_str(), e.size, e.size, shown.c_str());
        }
    }
    return 0;
}

bool isPicture(const std::string& name) {
    std::string ext = extensionOf(name);
    return ext == "tga" || ext == "bmp";
}

void writePng(const fs::path& path, const RgbaImage& img) {
    fs::create_directories(path.parent_path());
    if (!stbi_write_png(path.string().c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4)) {
        fail("Failed to write " + path.string());
    }
}

void writeFile(const fs::path& path, const Bytes& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) fail("Failed to write " + path.string());
}

int cmdExtract(const std::string& dir, const std::string& outDir, const std::string& filter, bool png) {
    Assets assets(std::make_unique<DirectoryGameFolder>(dir));
    auto archives = assets.kcapArchives();
    size_t count = 0, pictures = 0;
    for (const auto& l : kcapEntries(archives)) {
        std::string rel = sjisToUtf8(l.folder.empty() ? l.entry->name : l.folder + "/" + l.entry->name);
        if (!matches(rel, filter) || l.entry->size == 0) continue;
        Bytes data = l.info->archive->read(*l.entry);
        fs::path path = fs::path(outDir) / archiveDir(l.info->path) / rel;
        writeFile(path, data);
        if (png && isPicture(rel)) {
            // As stored: alpha where the file has it (an A8R8G8B8 slot).
            try {
                Picture p = decodePicture(data, l.entry->name);
                preparePalette(p, PictureType::Alpha);
                writePng(path.string() + ".png", toRgba(p, PictureType::Alpha));
                ++pictures;
            } catch (const Error& e) {
                std::fprintf(stderr, "warning: %s: %s\n", rel.c_str(), e.what());
            }
        }
        ++count;
    }
    for (const auto& a : assets.soundArchives()) {
        for (const auto& e : a->entries()) {
            std::string rel = sjisToUtf8(e.name);
            if (!matches(rel, filter) || e.size == 0) continue;
            writeFile(fs::path(outDir) / archiveDir(a->name()) / rel, a->read(e));
            ++count;
        }
    }
    std::printf("Extracted %zu entries (%zu pictures as PNG) to %s\n", count, pictures, outDir.c_str());
    return 0;
}

// Whether `d` starts as a file of this extension does, for the formats known so far (others pass);
// `expected` says what it should start with.
bool signatureOk(const std::string& ext, const Bytes& d, std::string& expected) {
    auto starts = [&](const char* m, size_t at = 0) {
        size_t n = std::strlen(m);
        return d.size() >= at + n && std::memcmp(d.data() + at, m, n) == 0;
    };
    static const std::map<std::string, const char*> kMagic = {
        {"bnr", "LSCR"}, {"ogg", "OggS"}, {"bmp", "BM"}, {"ani", "INAS"}};
    if (auto it = kMagic.find(ext); it != kMagic.end()) {
        expected = it->second;
        return starts(it->second);
    }
    if (ext == "wav") {
        expected = "RIFF....WAVE";
        return starts("RIFF") && starts("WAVE", 8);
    }
    if (ext == "tga") {
        // Truevision TGA: a colour map type of 0 or 1, an image type of 1-3 or 9-11, a size.
        expected = "a TGA header";
        return d.size() >= 18 && d[1] <= 1 && ((d[2] >= 1 && d[2] <= 3) || (d[2] >= 9 && d[2] <= 11)) &&
               readLE16(&d[12]) > 0 && readLE16(&d[14]) > 0;
    }
    return true;
}

int cmdVerify(const std::string& dir, unsigned threads) {
    auto t0 = std::chrono::steady_clock::now();
    Assets assets(std::make_unique<DirectoryGameFolder>(dir));
    auto archives = assets.kcapArchives();
    auto kcap = kcapEntries(archives);
    struct SoundItem {
        const LacArchive* archive;
        const LacEntry* entry;
    };
    std::vector<SoundItem> sound;
    for (const auto& a : assets.soundArchives()) {
        for (const auto& e : a->entries()) sound.push_back({a.get(), &e});
    }
    std::printf("Verifying %zu KCAP entries in %zu archives and %zu sound entries with %u threads...\n", kcap.size(),
                archives.size(), sound.size(), threads);

    struct Stats {
        size_t count = 0, bytes = 0, bad = 0;
    };
    std::map<std::string, Stats> stats;
    std::vector<std::string> failures, staleTail;
    std::map<std::string, size_t> pictureKinds;
    std::atomic<size_t> next{0};
    std::mutex mu;
    size_t total = kcap.size() + sound.size();
    auto worker = [&] {
        std::map<std::string, Stats> local;
        for (size_t i; (i = next.fetch_add(1)) < total;) {
            std::string name, where, problem;
            Bytes data;
            try {
                if (i < kcap.size()) {
                    const auto& l = kcap[i];
                    name = sjisToUtf8(l.entry->name);
                    where = l.info->path;
                    if (l.entry->size == 0) continue;  // folder names packed as empty files
                    LzssResult report;
                    data = l.info->archive->read(*l.entry, &report);
                    if (l.entry->packed()) {
                        uint32_t want = l.info->archive->unpackedSize(*l.entry);
                        if (report.produced != want) {
                            problem = "unpacked " + std::to_string(report.produced) + " of " + std::to_string(want) + " bytes";
                        }
                        if (report.readStaleTail) {
                            std::lock_guard lock(mu);
                            staleTail.push_back(where + ": " + name);
                        }
                    }
                } else {
                    const auto& s = sound[i - kcap.size()];
                    name = sjisToUtf8(s.entry->name);
                    where = s.archive->name();
                    data = s.archive->read(*s.entry);
                }
                std::string expected;
                std::string ext = extensionOf(name);
                if (problem.empty() && !signatureOk(ext, data, expected)) problem = "does not start with " + expected;
                if (problem.empty() && (ext == "tga" || ext == "bmp")) {
                    try {
                        Picture p = decodePicture(data, name);
                        // Indices past the palette the file gives would read whatever follows it.
                        if (p.bpp == 8) {
                            uint8_t top = *std::max_element(p.pixels.begin(), p.pixels.end());
                            if (top >= p.paletteSize) problem = "index " + std::to_string(top) + " past a palette of " + std::to_string(p.paletteSize);
                        }
                        std::lock_guard lock(mu);
                        ++pictureKinds[ext + " " + std::to_string(p.bpp) + "-bit" + (p.bpp == 8 && p.paletteAlpha ? " (palette alpha)" : "")];
                    } catch (const Error& e) {
                        problem = e.what();
                    }
                }
                auto& st = local[ext];
                ++st.count;
                st.bytes += data.size();
                if (!problem.empty()) {
                    ++st.bad;
                    std::lock_guard lock(mu);
                    failures.push_back(where + ": " + name + ": " + problem);
                }
            } catch (const std::exception& e) {
                std::lock_guard lock(mu);
                failures.push_back(where + ": " + name + ": " + e.what());
            }
        }
        std::lock_guard lock(mu);
        for (const auto& [k, v] : local) {
            stats[k].count += v.count;
            stats[k].bytes += v.bytes;
            stats[k].bad += v.bad;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n%-8s %8s %12s %6s\n", "Ext", "Count", "MiB", "Bad");
    for (const auto& [ext, s] : stats) {
        std::printf("%-8s %8zu %12.1f %6zu\n", ext.empty() ? "(none)" : ext.c_str(), s.count, s.bytes / 1048576.0, s.bad);
    }
    std::printf("\nPictures decoded:");
    for (const auto& [k, n] : pictureKinds) std::printf("  [%s: %zu]", k.c_str(), n);
    std::printf("\nPacked entries reading the ring's unwritten end: %zu\n", staleTail.size());
    std::sort(staleTail.begin(), staleTail.end());
    for (size_t i = 0; i < staleTail.size() && i < 20; ++i) std::printf("  %s\n", staleTail[i].c_str());
    std::printf("Failures: %zu   Time: %.1fs\n", failures.size(), secs);
    std::sort(failures.begin(), failures.end());
    for (size_t i = 0; i < failures.size() && i < 50; ++i) std::printf("  FAIL %s\n", failures[i].c_str());
    if (failures.size() > 50) std::printf("  ... and %zu more\n", failures.size() - 50);
    return failures.empty() ? 0 : 1;
}

// One picture as an image slot loads it (0x43ffa0): its palette for the slot type, the colour level,
// then each .amp table in turn.
int cmdImage(const std::string& dir, const std::string& group, const std::string& name, const std::string& out,
             int type, int level, const std::vector<std::string>& amps) {
    Assets assets(std::make_unique<DirectoryGameFolder>(dir));
    std::string sjisName = utf8ToSjis(name);
    auto data = assets.read(group, sjisName);
    if (!data) fail(group + "/" + name + ": not found");
    Picture p = decodePicture(*data, sjisName);
    std::printf("%s: %dx%d, %d-bit", name.c_str(), p.width, p.height, p.bpp);
    if (p.bpp == 8) std::printf(", %d palette entries%s", p.paletteSize, p.paletteAlpha ? " with alpha" : "");
    std::printf("\n");
    auto t = static_cast<PictureType>(type);
    preparePalette(p, t);
    applyLevel(p, level);
    for (const auto& a : amps) {
        auto table = assets.read("grp", utf8ToSjis(a));
        if (!table) fail(a + ": not found in grp");
        if (!applyAmp(p, *table)) std::printf("%s: %zu bytes, not a colour table (skipped)\n", a.c_str(), table->size());
    }
    RgbaImage img = toRgba(p, t);
    if (img.pixels.empty()) fail("a mask (type 3) needs an 8-bit picture");
    writePng(out, img);
    std::printf("wrote %s\n", out.c_str());
    return 0;
}

FunctionTable loadFunctions(const Assets& assets) {
    FunctionTable t;
    for (const char* name : {"LFScriptFunc.fnc", "LFScriptFuncEx.fnc"}) {
        auto d = assets.read("script", name);
        if (!d) fail(std::string(name) + ": not found");
        t.load(*d);
    }
    return t;
}

// A movie file (mvNN0.pak: ASF) through the shared FFmpeg decoder: its streams, frames as PNG, and the
// whole sound decoded once to count it.
int cmdMovie(const std::string& dir, const std::string& file, const std::string& outDir, std::vector<double> times) {
    if (!nyan::media::FfmpegVideo::available()) fail("this build has no FFmpeg");
    DirectoryGameFolder folder(dir);
    std::shared_ptr<RandomAccessFile> f = folder.open(file);
    if (!f) fail(file + ": not found");
    auto movie = nyan::media::FfmpegVideo::open(f);
    if (!movie) fail(file + ": cannot decode");
    std::printf("%s: %dx%d, %.3f fps, %.2f s, sound %d Hz\n", file.c_str(), movie->width(), movie->height(),
                movie->frameRate(), movie->duration(), movie->sampleRate());
    std::sort(times.begin(), times.end());
    size_t pictures = 0;
    for (double t = 0; !movie->ended(); t += 1.0 / movie->frameRate()) {
        if (movie->seek(t)) ++pictures;
        while (!times.empty() && times.front() <= t) {
            if (!outDir.empty() && !movie->rgba().empty()) {
                fs::create_directories(outDir);
                char name[64];
                std::snprintf(name, sizeof name, "/%s_%08.3f.png", fs::path(file).stem().string().c_str(), times.front());
                writePng(outDir + name, RgbaImage{movie->width(), movie->height(), Bytes(movie->rgba().begin(), movie->rgba().end())});
            }
            times.erase(times.begin());
        }
    }
    std::printf("%zu pictures\n", pictures);
    if (movie->hasAudio()) {
        auto pull = movie->audioSource(false);
        std::vector<float> buf(4096 * 2);
        size_t frames = 0;
        double peak = 0;
        for (int n; (n = pull(buf.data(), 4096)) > 0;) {
            frames += static_cast<size_t>(n);
            for (int i = 0; i < n * 2; ++i) peak = std::max(peak, static_cast<double>(std::abs(buf[static_cast<size_t>(i)])));
        }
        std::printf("sound: %.2f s, peak %.3f\n", static_cast<double>(frames) / movie->sampleRate(), peak);
    }
    return 0;
}

int cmdBnr(const std::string& dir, std::string name) {
    Assets assets(std::make_unique<DirectoryGameFolder>(dir));
    if (extensionOf(name) == "bnr") name = name.substr(0, name.size() - 4);
    auto code = assets.read("script", utf8ToSjis(name) + ".bnr");
    if (!code) fail(name + ".bnr: not found");
    auto text = assets.read("script", utf8ToSjis(name) + ".txt");
    std::vector<std::string> strings;
    if (text) strings = splitStrings(std::string(text->begin(), text->end()));
    Script script = Script::parse(std::move(*code));
    std::fputs(disassemble(script, strings, loadFunctions(assets)).c_str(), stdout);
    return 0;
}

// Every script name the engine can reach: the .bnr names of en.pak and the script archives.
std::vector<std::string> scriptNames(const Assets& assets) {
    std::map<std::string, std::string> names;  // folded -> as stored
    for (const auto& a : assets.kcapArchives()) {
        if (!a.group.empty() && a.group != "script") continue;
        for (const auto& e : a.archive->entries()) {
            std::string n = e.name;
            if (e.folder() || toLowerAscii(n).size() < 4 || !toLowerAscii(n).ends_with(".bnr")) continue;
            names.emplace(toLowerAscii(n), n.substr(0, n.size() - 4));
        }
    }
    std::vector<std::string> out;
    for (const auto& [k, v] : names) out.push_back(v);
    return out;
}

int cmdScripts(const std::string& dir) {
    Assets assets(std::make_unique<DirectoryGameFolder>(dir));
    FunctionTable functions = loadFunctions(assets);
    std::map<int, size_t> calls;
    std::map<uint32_t, size_t> controls, operators;
    size_t scripts = 0, instructions = 0;
    std::vector<std::string> problems;
    for (const auto& name : scriptNames(assets)) {
        auto code = assets.read("script", name + ".bnr");
        if (!code) continue;
        ++scripts;
        try {
            Script s = Script::parse(std::move(*code));
            for (uint32_t at = s.codeStart; at < s.data.size();) {
                Instruction in = decodeInstruction(s, at);
                ++instructions;
                if (in.op == Op::Call) ++calls[static_cast<int>(in.operands[0])];
                if (in.op == Op::Control) ++controls[in.operands[0]];
                if (in.op == Op::Operator) ++operators[in.operands[0]];
                at += in.size();
            }
            if (!assets.read("script", name + ".txt")) problems.push_back(sjisToUtf8(name) + ": no text table");
        } catch (const std::exception& e) {
            problems.push_back(sjisToUtf8(name) + ": " + e.what());
        }
    }
    std::printf("%zu scripts, %zu instructions decoded\n\nControl sub-ops:", scripts, instructions);
    for (const auto& [k, n] : controls) std::printf(" %u:%zu", k, n);
    std::printf("\nOperators:");
    for (const auto& [k, n] : operators) std::printf(" 0x%x:%zu", k, n);
    std::printf("\n\nCalls:\n");
    for (const auto& [id, n] : calls) {
        const ScriptFunction* f = functions.find(id);
        std::printf("  0x%02x %-16s %8zu\n", id, f ? f->name.c_str() : "?", n);
    }
    std::printf("Functions never called:");
    for (const auto& [id, f] : functions.all()) {
        if (!calls.count(id)) std::printf(" %s", f.name.c_str());
    }
    std::printf("\n\nProblems: %zu\n", problems.size());
    for (const auto& p : problems) std::printf("  %s\n", p.c_str());
    return problems.empty() ? 0 : 1;
}

// Every script run on its own from its first label (0 where it has one), in skip mode with skipping
// of unread text, picking the first option at choices, until it ends, loads another script, goes
// to the title, or an hour of game time passes.
int cmdPlayAll(const std::string& dir, const std::string& only) {
    Game game(std::make_unique<DirectoryGameFolder>(dir));
    game.gfx().keepPixels = false;
    std::vector<std::string> errors;
    std::string current;
    std::map<std::string, size_t> logCounts;
    game.logSink = [&](const std::string& m) {
        std::string key = m.substr(0, m.find(" at "));
        if (logCounts[key]++ < 3) errors.push_back(current + ": " + m);
    };
    game.choose = [](const std::vector<std::string>&) { return 0; };
    game.modes.skip = true;
    game.modes.skipUnread = true;
    size_t scripts = 0, stuck = 0;
    for (const auto& name : scriptNames(game.assets())) {
        if (!only.empty() && toLowerAscii(name) != toLowerAscii(utf8ToSjis(only))) continue;
        current = sjisToUtf8(name);
        auto code = game.assets().read("script", name + ".bnr");
        if (!code) continue;
        Script s = Script::parse(std::move(*code));
        int label = s.labels.count(0) || s.labels.empty() ? 0 : s.labels.begin()->first;
        ++scripts;
        game.startStory(name, label);
        uint64_t limit = game.frame() + 60ull * 60 * 60;
        int maxDepth = 0;
        while (game.storyRunning() && !game.titleRequested() && game.frame() < limit &&
               toLowerAscii(game.vm()->name()) == toLowerAscii(name)) {
            game.modes.skip = true;  // movies' ends and clicks switch it off
            game.tick();
            maxDepth = std::max(maxDepth, game.vm()->stackDepth());
        }
        if (maxDepth > 200) errors.push_back(current + ": " + std::to_string(maxDepth) + " values on the stack (the engine refuses past 254)");
        if (game.frame() >= limit) {
            ++stuck;
            errors.push_back(current + ": did not finish (" + sjisToUtf8(game.currentScript()) + ")");
        }
    }
    std::printf("Ran %zu scripts, %zu stuck, %zu errors\n", scripts, stuck, errors.size());
    for (size_t i = 0; i < errors.size() && i < 60; ++i) std::printf("  %s\n", errors[i].c_str());
    std::vector<std::pair<size_t, std::string>> u;
    for (const auto& [k, n] : game.unhandled()) u.emplace_back(n, k);
    std::sort(u.rbegin(), u.rend());
    std::printf("\nUnhandled functions (%zu kinds):\n", u.size());
    for (const auto& [n, k] : u) std::printf("  %7zu  %s\n", n, k.c_str());
    std::printf("Missing assets: %zu\n", game.missing().size());
    size_t shown = 0;
    for (const auto& [k, n] : game.missing()) {
        if (shown++ < 40) std::printf("  %5zu  %s\n", n, sjisToUtf8(k).c_str());
    }
    return 0;
}

std::optional<int> run(const std::vector<std::string>& args) {
    const std::string& cmd = args[0];
    if (args.size() < 2 || !isWa2Dir(args[1])) return std::nullopt;
    if (cmd == "info" && args.size() == 2) return cmdInfo(args[1]);
    if (cmd == "list" && (args.size() == 2 || args.size() == 3)) return cmdList(args[1], args.size() == 3 ? args[2] : "");
    if (cmd == "extract" && args.size() >= 3) {
        std::string filter;
        bool png = false;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--png") png = true;
            else filter = args[i];
        }
        return cmdExtract(args[1], args[2], filter, png);
    }
    if (cmd == "bnr" && args.size() == 3) return cmdBnr(args[1], args[2]);
    if (cmd == "movie" && args.size() >= 3) {
        std::vector<double> times;
        for (size_t i = 4; i < args.size(); ++i) times.push_back(std::stod(args[i]));
        return cmdMovie(args[1], args[2], args.size() >= 4 ? args[3] : "", times);
    }
    if (cmd == "scripts" && args.size() == 2) return cmdScripts(args[1]);
    if (cmd == "playall" && (args.size() == 2 || args.size() == 3)) return cmdPlayAll(args[1], args.size() == 3 ? args[2] : "");
    if (cmd == "image" && args.size() >= 5) {
        int type = 1, level = 0x100;
        std::vector<std::string> amps;
        for (size_t i = 5; i + 1 < args.size(); i += 2) {
            if (args[i] == "--type") type = std::stoi(args[i + 1]);
            else if (args[i] == "--level") level = std::stoi(args[i + 1], nullptr, 0);
            else if (args[i] == "--amp") amps.push_back(args[i + 1]);
            else return std::nullopt;
        }
        return cmdImage(args[1], args[2], args[3], args[4], type, level, amps);
    }
    if (cmd == "verify" && (args.size() == 2 || args.size() == 4)) {
        unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        if (args.size() == 4) {
            if (args[2] != "--threads") return std::nullopt;
            threads = std::max(1u, static_cast<unsigned>(std::stoul(args[3])));
        }
        return cmdVerify(args[1], threads);
    }
    return std::nullopt;
}

}  // namespace

const nyan::cli::CommandSet nyan::cli::kWa2Commands = {nyan::Engine::WA2, usage, run};
