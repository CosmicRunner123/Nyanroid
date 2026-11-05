// nyan-cli's CatSystem2 commands: archives, images, scene and FE scripts, headless play, PSB / E-mote.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "commands.hpp"
#include "nyan/audio/mixer.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/common/sjis.hpp"
#include "nyan/cs2/asset_index.hpp"
#include "nyan/cs2/cst_scene.hpp"
#include "nyan/cs2/emote.hpp"
#include "nyan/cs2/emote_player.hpp"
#include "nyan/cs2/psb.hpp"
#include "nyan/cs2/hg_image.hpp"
#include "nyan/cs2/scene_player.hpp"
#include "nyan/cs2/game.hpp"
#include "nyan/cs2/window_theme.hpp"
#include "nyan/media/ffmpeg_video.hpp"
#include "nyan/media/mpeg_video.hpp"
#include "stb_image_write.h"

namespace fs = std::filesystem;
using namespace nyan;
using namespace nyan::cs2;

namespace {

void usage() {
    std::puts(
        "  nyan-cli info    <game-dir>                   Show V_CODEs and archive summary\n"
        "  nyan-cli list    <game-dir> [filter]          List assets (filter = substring)\n"
        "  nyan-cli extract <game-dir> <out-dir> [filter] [--png|--png-only] [--regex RE]\n"
        "                                                Extract assets; --png also converts HG images,\n"
        "                                                --png-only writes just the PNGs\n"
        "  nyan-cli hg      <game-dir|file> <name> <out-dir>\n"
        "                                                Decode one HG-2/HG-3 image to PNG frames\n"
        "  nyan-cli verify  <game-dir> [--threads N]     Read every asset, decode every HG image\n"
        "  nyan-cli cst     <game-dir> <scene>           Print a scene script's lines\n"
        "  nyan-cli fes     <game-dir> <script>          Print an FE script's source (UTF-8)\n"
        "  nyan-cli font    <game-dir> [font-dir...]     Show which font the game's text uses and its font list\n"
        "  nyan-cli cursor  <game-dir> [name] [out.png]  The mouse cursor the game shows (cursor.xml name)\n"
        "  nyan-cli cststats <game-dir>                  Count command usage across all scene scripts\n"
        "  nyan-cli play    <game-dir> [scene] [--frames N] [--verbose] [--lang L]\n"
        "                                                Run scenes headless, auto-clicking, first choice\n"
        "  nyan-cli playall <game-dir>                   Run every scene once (no jumps) and report problems\n"
        "  nyan-cli wintrace <game-dir> [scene...]       Play scenes through the whole game (its message window\n"
        "                                                too), clicking at each input; print what the window\n"
        "                                                shows at each message (images, texts, the name)\n"
        "  nyan-cli mixwav  <game-dir> <name.ogg> <out.wav> [seconds]\n"
        "  nyan-cli movie   <game-dir> <name.mpg> [seconds]      decode speed and sound check\n"
        "                                                Render a sound through the mixer to a WAV file");
}

std::string extensionOf(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos ? "" : toLowerAscii(name.substr(dot + 1));
}

bool matches(const std::string& utf8Name, const std::string& filter) {
    return filter.empty() || toLowerAscii(utf8Name).find(toLowerAscii(filter)) != std::string::npos;
}

// Every asset that is actually reachable by name (i.e. not shadowed by an update archive).
std::vector<std::pair<std::string, AssetRef>> visibleAssets(const AssetIndex& index) {
    std::vector<std::pair<std::string, AssetRef>> out;
    for (const auto& archive : index.archives()) {
        for (const auto& entry : archive->entries()) {
            auto ref = index.find(entry.name);
            if (ref && ref->entry == &entry) out.emplace_back(sjisToUtf8(entry.name), *ref);
        }
    }
    return out;
}

void writePng(const fs::path& path, const RgbaImage& img) {
    fs::create_directories(path.parent_path());
    if (!stbi_write_png(path.string().c_str(), static_cast<int>(img.width), static_cast<int>(img.height), 4,
                        img.pixels.data(), static_cast<int>(img.width * 4))) {
        fail("Failed to write " + path.string());
    }
}

void writeFile(const fs::path& path, const Bytes& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) fail("Failed to write " + path.string());
}

// Writes each frame of an HG image as <stem>[_<id>].png. Returns frames written.
size_t dumpHg(const Bytes& data, const fs::path& outDir, const std::string& stem) {
    HgImage img = HgImage::parse(data);
    const auto& frames = img.frames();
    for (size_t i = 0; i < frames.size(); ++i) {
        // Frame IDs are not unique within a file, so name multi-frame output by index.
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "_%03zu", i);
        std::string name = frames.size() == 1 ? stem : stem + suffix;
        writePng(outDir / (name + ".png"), img.decodeFull(i));
    }
    return frames.size();
}

int cmdInfo(const std::string& dir) {
    DirectoryGameFolder folder(dir);
    auto t0 = std::chrono::steady_clock::now();
    AssetIndex index = AssetIndex::load(folder);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    if (index.vcodes()) {
        std::printf("Executable : %s\nV_CODE     : %s\nV_CODE2    : %s\nTOC seed   : 0x%08X\n",
                    index.vcodes()->sourceFile.c_str(), index.vcodes()->vcode.c_str(),
                    index.vcodes()->vcode2.c_str(), index.vcodes()->tocSeed());
    } else {
        std::puts("V_CODE2    : (not found)");
    }
    std::printf("\n%-16s %9s  %s\n", "Archive", "Entries", "Encrypted");
    size_t total = 0;
    for (const auto& a : index.archives()) {
        std::printf("%-16s %9zu  %s\n", a->name().c_str(), a->entries().size(), a->encrypted() ? "yes" : "no");
        total += a->entries().size();
    }
    std::printf("\n%zu archives, %zu entries, %zu unique assets, indexed in %lld ms\n", index.archives().size(),
                total, index.size(), static_cast<long long>(ms));
    return 0;
}

int cmdList(const std::string& dir, const std::string& filter) {
    DirectoryGameFolder folder(dir);
    AssetIndex index = AssetIndex::load(folder);
    for (const auto& [name, ref] : visibleAssets(index)) {
        if (!matches(name, filter)) continue;
        std::printf("%-14s %10u  %s\n", ref.archive->name().c_str(), ref.entry->length, name.c_str());
    }
    return 0;
}

int cmdExtract(const std::string& dir, const std::string& outDir, const std::string& filter, bool png,
               const std::string& regex, bool pngOnly) {
    DirectoryGameFolder folder(dir);
    AssetIndex index = AssetIndex::load(folder);
    std::optional<std::regex> re;
    if (!regex.empty()) re.emplace(regex, std::regex::ECMAScript | std::regex::icase);
    size_t count = 0, images = 0;
    for (const auto& [name, ref] : visibleAssets(index)) {
        if (!matches(name, filter)) continue;
        if (re && !std::regex_search(name, *re)) continue;
        std::string ext0 = extensionOf(name);
        if (pngOnly && ext0 != "hg3" && ext0 != "hg2") continue;
        Bytes data = ref.archive->read(*ref.entry);
        fs::path archiveDir = fs::path(outDir) / fs::path(ref.archive->name()).stem();
        if (pngOnly) archiveDir = fs::path(outDir);
        else writeFile(archiveDir / name, data);
        std::string ext = extensionOf(name);
        if (png && (ext == "hg3" || ext == "hg2")) {
            try {
                images += dumpHg(data, archiveDir, fs::path(name).stem().string());
            } catch (const Error& e) {
                std::fprintf(stderr, "warning: %s: %s\n", name.c_str(), e.what());
            }
        }
        ++count;
    }
    std::printf("Extracted %zu assets (%zu PNG frames) to %s\n", count, images, outDir.c_str());
    return 0;
}

// Prints part of a PSB tree (E-mote model metadata etc.): `psb <game-dir> <name.psz> [path [depth]]`,
// path members separated by '/', list elements by index.
void printPsb(const cs2::PsbValue& v, int depth, int indent) {
    using T = cs2::PsbValue::Type;
    std::string pad(static_cast<size_t>(indent) * 2, ' ');
    switch (v.type) {
        case T::Null: std::printf("null"); break;
        case T::Bool: case T::Int: std::printf("%lld", static_cast<long long>(v.i)); break;
        case T::Float: std::printf("%g", v.f); break;
        case T::String: std::printf("\"%s\"", v.s.c_str()); break;
        case T::Resource: std::printf("<resource %lld>", static_cast<long long>(v.i)); break;
        case T::IntArray:
            std::printf("[");
            for (size_t i = 0; i < v.ints.size(); ++i) std::printf("%s%lld", i ? "," : "", static_cast<long long>(v.ints[i]));
            std::printf("]");
            break;
        case T::List:
            if (depth <= 0) { std::printf("[%zu items]", v.list.size()); break; }
            std::printf("[\n");
            for (size_t i = 0; i < v.list.size(); ++i) {
                std::printf("%s  %zu: ", pad.c_str(), i);
                printPsb(v.list[i], depth - 1, indent + 1);
                std::printf("\n");
            }
            std::printf("%s]", pad.c_str());
            break;
        case T::Object:
            if (depth <= 0) { std::printf("{%zu members}", v.object.size()); break; }
            std::printf("{\n");
            for (const auto& [k, c] : v.object) {
                std::printf("%s  %s: ", pad.c_str(), k.c_str());
                printPsb(c, depth - 1, indent + 1);
                std::printf("\n");
            }
            std::printf("%s}", pad.c_str());
            break;
    }
}

int cmdPsb(const std::string& dir, const std::string& name, const std::string& path, int depth) {
    AssetIndex index = AssetIndex::load(DirectoryGameFolder(dir));
    Bytes file = index.read(name);
    std::ifstream lib(fs::path(dir) / "emotedriver.dll", std::ios::binary);
    Bytes dll((std::istreambuf_iterator<char>(lib)), {});
    std::optional<cs2::Psb> psb;
    for (uint32_t k : cs2::Psb::keyCandidates(dll)) {
        if (cs2::Psb::keyFits(file, k) && (psb = cs2::Psb::load(file, k))) break;
    }
    if (!psb) psb = cs2::Psb::load(file, 0);
    if (!psb) fail("not a PSB: " + name);
    const cs2::PsbValue* v = &psb->root();
    size_t start = 0;
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? path.size() : slash + 1;
        if (part.empty()) continue;
        if (v->type == cs2::PsbValue::Type::List) {
            size_t i = std::stoul(part);
            if (i >= v->list.size()) fail("no element " + part);
            v = &v->list[i];
        } else {
            v = v->get(part);
            if (!v) fail("no member " + part);
        }
    }
    printPsb(*v, depth, 0);
    std::printf("\n");
    return 0;
}

// Renders an E-mote model's pose to a PNG with a simple software rasteriser (for checking the
// evaluation without the renderer): `emote <game-dir> <name.psz> <out.png> [var=value ...]`.
int cmdEmote(const std::string& dir, const std::string& name, const std::string& outPath,
             const std::vector<std::string>& assigns) {
    AssetIndex index = AssetIndex::load(DirectoryGameFolder(dir));
    Bytes file = index.read(name);
    std::ifstream lib(fs::path(dir) / "emotedriver.dll", std::ios::binary);
    Bytes dll((std::istreambuf_iterator<char>(lib)), {});
    uint32_t key = 0;
    for (uint32_t k : Psb::keyCandidates(dll)) {
        if (Psb::keyFits(file, k)) {
            key = k;
            break;
        }
    }
    std::printf("key %u\n", key);
    auto model = EmoteModel::load(file, key);
    if (!model) {
        std::fprintf(stderr, "not an E-mote model\n");
        return 1;
    }
    std::map<std::string, float> vars;
    float time = 0;
    std::string traceTimeline, traceVar;
    int traceFrames = 0;
    uint32_t traceFlags = cs2::EmotePlayer::kTimelineParallel;
    int moveAt = -1;
    float moveX = 0, moveY = 0;
    for (const auto& a : assigns) {
        size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "time") time = std::stof(v);
        else if (k == "timeline") traceTimeline = v;  // trace: play this timeline...
        else if (k == "trace") traceVar = v;          // ...and print this variable each frame
        else if (k == "frames") traceFrames = std::stoi(v);
        else if (k == "flags") traceFlags = static_cast<uint32_t>(std::stoul(v));  // PlayTimeline's (2: difference)
        else if (k == "move") std::sscanf(v.c_str(), "%d:%f:%f", &moveAt, &moveX, &moveY);  // from frame n, at (x, y)
        else vars[k] = std::stof(v);
    }
    if (!traceVar.empty()) {
        cs2::EmotePlayer player(std::make_shared<const EmoteModel>(*model));
        for (const auto& [k, v] : vars) player.setVariable(k, v);
        if (!traceTimeline.empty()) player.playTimeline(traceTimeline, traceFlags);
        for (int f = 0; f < traceFrames; ++f) {
            bool moved = moveAt >= 0 && f >= moveAt;
            player.step(1, moved ? moveX : 0, moved ? moveY : 0);
            auto vals = player.values();
            std::printf("%d %.3f\n", f + 1, vals.count(traceVar) ? vals[traceVar] : 0.0f);
        }
        return 0;
    }
    auto draws = model->evaluate(vars, time);
    const int W = 900, H = 1100;
    const float OX = 450, OY = 520;
    std::vector<float> img(static_cast<size_t>(W) * H * 4, 0.0f);
    const Bytes& tex = model->rgba();
    const int TW = model->textureWidth(), TH = model->textureHeight();
    auto tri = [&](const float* P, const float* T, float alpha) {
        float x0 = std::min({P[0], P[2], P[4]}), x1 = std::max({P[0], P[2], P[4]});
        float y0 = std::min({P[1], P[3], P[5]}), y1 = std::max({P[1], P[3], P[5]});
        float den = (P[3] - P[5]) * (P[0] - P[4]) + (P[4] - P[2]) * (P[1] - P[5]);
        if (std::fabs(den) < 1e-9f) return;
        for (int y = std::max(0, static_cast<int>(y0)); y <= std::min(H - 1, static_cast<int>(y1) + 1); ++y) {
            for (int x = std::max(0, static_cast<int>(x0)); x <= std::min(W - 1, static_cast<int>(x1) + 1); ++x) {
                float px = x + 0.5f, py = y + 0.5f;
                float l1 = ((P[3] - P[5]) * (px - P[4]) + (P[4] - P[2]) * (py - P[5])) / den;
                float l2 = ((P[5] - P[1]) * (px - P[4]) + (P[0] - P[4]) * (py - P[5])) / den;
                float l3 = 1 - l1 - l2;
                if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
                int tu = std::clamp(static_cast<int>(l1 * T[0] + l2 * T[2] + l3 * T[4]), 0, TW - 1);
                int tv = std::clamp(static_cast<int>(l1 * T[1] + l2 * T[3] + l3 * T[5]), 0, TH - 1);
                const uint8_t* s = &tex[(static_cast<size_t>(tv) * TW + tu) * 4];
                float a = s[3] / 255.0f * alpha;
                float* d = &img[(static_cast<size_t>(y) * W + x) * 4];
                for (int c = 0; c < 3; ++c) d[c] = s[c] / 255.0f * a + d[c] * (1 - a);
                d[3] = a + d[3] * (1 - a);
            }
        }
    };
    for (const auto& d : draws) {
        int g = d.grid;
        for (int j = 0; j < g; ++j) {
            for (int i = 0; i < g; ++i) {
                int k[4] = {j * (g + 1) + i, j * (g + 1) + i + 1, (j + 1) * (g + 1) + i + 1, (j + 1) * (g + 1) + i};
                float P[8], T[8];
                for (int q = 0; q < 4; ++q) {
                    P[q * 2] = d.xy[k[q] * 2] + OX, P[q * 2 + 1] = d.xy[k[q] * 2 + 1] + OY;
                    int ii = (k[q] % (g + 1)), jj = (k[q] / (g + 1));
                    T[q * 2] = d.icon->left + d.icon->width * ii / g;
                    T[q * 2 + 1] = d.icon->top + d.icon->height * jj / g;
                }
                float P1[6] = {P[0], P[1], P[2], P[3], P[6], P[7]}, T1[6] = {T[0], T[1], T[2], T[3], T[6], T[7]};
                float P2[6] = {P[2], P[3], P[4], P[5], P[6], P[7]}, T2[6] = {T[2], T[3], T[4], T[5], T[6], T[7]};
                tri(P1, T1, d.opacity);
                tri(P2, T2, d.opacity);
            }
        }
    }
    RgbaImage out;
    out.width = W, out.height = H;
    out.pixels.resize(static_cast<size_t>(W) * H * 4);
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        float a = img[i + 3];
        for (int c = 0; c < 3; ++c) out.pixels[i + c] = static_cast<uint8_t>(std::clamp((img[i + c] + (1 - a) * 0.35f) * 255.0f, 0.0f, 255.0f));
        out.pixels[i + 3] = 255;
    }
    writePng(outPath, out);
    std::printf("%zu images\n", draws.size());
    return 0;
}

int cmdHg(const std::string& source, const std::string& name, const std::string& outDir) {
    Bytes data;
    if (fs::is_directory(source)) {
        DirectoryGameFolder folder(source);
        data = AssetIndex::load(folder).read(name);
    } else {
        auto file = FdFile::open(source);
        if (!file) fail("Cannot open " + source);
        data = file->readAll();
    }
    HgImage img = HgImage::parse(data);
    for (size_t i = 0; i < img.frames().size(); ++i) {
        const auto& f = img.frames()[i];
        std::printf("frame %zu id=%u  %ux%u @(%d,%d) in %ux%u  depth=%u base=(%d,%d) attrs=%zu\n", i, f.id,
                    f.width, f.height, f.offsetX, f.offsetY, f.totalWidth, f.totalHeight, f.bitDepth, f.baseX,
                    f.baseY, f.attributes.size());
        for (const auto& a : f.attributes) {
            std::printf("    attr %u: (%d,%d) %ux%u color=%08X\n", a.id, a.x, a.y, a.width, a.height, a.color);
        }
    }
    size_t n = dumpHg(data, outDir, fs::path(name).stem().string());
    std::printf("Wrote %zu frame(s) to %s\n", n, outDir.c_str());
    return 0;
}

// CST text is Shift-JIS in Japanese releases; some translations store UTF-8.
std::string cstText(const std::string& s) {
    bool ascii = true;
    for (char c : s) ascii &= static_cast<unsigned char>(c) < 0x80;
    return ascii ? s : sjisToUtf8(s);
}

int cmdCst(const std::string& dir, std::string name) {
    DirectoryGameFolder folder(dir);
    AssetIndex index = AssetIndex::load(folder);
    // `archive.int:scene` reads a specific archive's copy, ignoring update overrides.
    std::string archive;
    if (auto colon = name.find(':'); colon != std::string::npos) {
        archive = name.substr(0, colon);
        name = name.substr(colon + 1);
    }
    if (extensionOf(name).empty()) name += ".cst";
    Bytes data;
    if (archive.empty()) {
        data = index.read(name);
    } else {
        for (const auto& a : index.archives()) {
            if (toLowerAscii(a->name()) != toLowerAscii(archive)) continue;
            for (const auto& e : a->entries()) {
                if (AssetIndex::normalizeName(e.name) == AssetIndex::normalizeName(name)) data = a->read(e);
            }
        }
        if (data.empty()) fail(name + " not found in " + archive);
    }
    CstScene scene = CstScene::parse(data);
    for (size_t i = 0; i < scene.lines().size(); ++i) {
        const auto& line = scene.lines()[i];
        std::printf("%5zu %-8s %s\n", i, toString(line.type), cstText(line.text).c_str());
    }
    return 0;
}

int cmdFes(const std::string& dir, std::string name) {
    DirectoryGameFolder folder(dir);
    AssetIndex index = AssetIndex::load(folder);
    if (extensionOf(name).empty()) name += ".fes";
    std::string text = decodeFes(index.read(name));
    std::fputs(sjisToUtf8(text).c_str(), stdout);
    return 0;
}

int cmdFont(const std::string& dir, std::vector<std::string> fontDirs) {
    // What a front end asks before starting the game (the library's missing-font warning).
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::string> missing = Game::missingFonts(DirectoryGameFolder(dir), fontDirs);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("missing:");
    for (const auto& m : missing) std::printf(" [%s]", m.c_str());
    std::printf("%s (checked in %lld ms)\n", missing.empty() ? " none" : "", static_cast<long long>(ms));
    Game game(std::make_unique<DirectoryGameFolder>(dir), nullptr);
    game.player().log = [](const std::string& m) { std::printf("%s\n", m.c_str()); };
    game.setFontDirectories(std::move(fontDirs));
    int face = 0;
    Bytes font = game.loadFont(&face);
    std::printf("%zu bytes, face %d\n", font.size(), face);
    // Bold (the config's `fontweight`): the family's bold face, or the face emboldened.
    bool fake = false;
    Bytes bold = game.loadFont(&face, "", 700, &fake);
    std::printf("bold: %zu bytes, face %d%s\n", bold.size(), face, fake ? ", emboldened" : "");
    // The config font list (fontcom): Shift-JIS faces, then every face.
    for (int32_t charset : {128, -1}) {
        std::printf("fonts (charset %d):", charset);
        for (const auto& n : game.fontList(charset)) std::printf(" [%s]", n.c_str());
        std::printf("\n");
    }
    uint64_t v = game.engineVersion();
    std::printf("engine %u.%u.%u.%u\n", unsigned(v >> 48), unsigned(v >> 32 & 0xFFFF), unsigned(v >> 16 & 0xFFFF), unsigned(v & 0xFFFF));
    return 0;
}

// The mouse cursor the engine shows (startup.xml <CURSOR>, cursor.xml), by name for type 1;
// written as a PNG when an output path is given.
int cmdCursor(const std::string& dir, const std::string& name, const std::string& out) {
    Game game(std::make_unique<DirectoryGameFolder>(dir), nullptr);
    game.player().log = [](const std::string& m) { std::printf("%s\n", m.c_str()); };
    if (!name.empty()) game.changeCursor(name);
    const CursorImage* c = game.cursor();
    if (!c) {
        std::printf("Windows' arrow\n");
        return 0;
    }
    std::printf("%dx%d, hotspot %d,%d\n", c->width, c->height, c->hotX, c->hotY);
    if (!out.empty()) {
        RgbaImage img;
        img.width = static_cast<uint32_t>(c->width), img.height = static_cast<uint32_t>(c->height);
        img.pixels.resize(c->argb.size() * 4);
        for (size_t i = 0; i < c->argb.size(); ++i) {
            const uint32_t v = c->argb[i];
            img.pixels[i * 4] = static_cast<uint8_t>(v >> 16), img.pixels[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
            img.pixels[i * 4 + 2] = static_cast<uint8_t>(v), img.pixels[i * 4 + 3] = static_cast<uint8_t>(v >> 24);
        }
        writePng(out, img);
    }
    return 0;
}

int cmdCstStats(const std::string& dir) {
    DirectoryGameFolder folder(dir);
    AssetIndex index = AssetIndex::load(folder);
    std::map<std::string, size_t> commands;
    std::map<std::string, std::string> examples;
    std::map<std::string, size_t> types;
    size_t scenes = 0;
    for (const auto& [name, ref] : visibleAssets(index)) {
        if (extensionOf(name) != "cst") continue;
        CstScene scene = CstScene::parse(ref.archive->read(*ref.entry));
        ++scenes;
        for (const auto& line : scene.lines()) {
            ++types[toString(line.type)];
            if (line.type != CstLineType::Command) continue;
            std::string word = line.text.substr(0, line.text.find_first_of(" \t"));
            if (!word.empty() && (word[0] == '#' || word[0] == '@')) word = word.substr(0, 1) + "...";
            if (commands[word]++ == 0) examples[word] = cstText(line.text);
        }
    }
    std::printf("%zu scenes\n", scenes);
    for (const auto& [t, n] : types) std::printf("  %-10s %8zu\n", t.c_str(), n);
    std::vector<std::pair<size_t, std::string>> sorted;
    for (const auto& [w, n] : commands) sorted.emplace_back(n, w);
    std::sort(sorted.rbegin(), sorted.rend());
    for (const auto& [n, w] : sorted) std::printf("%8zu  %-12s e.g. %s\n", n, w.c_str(), examples[w].c_str());
    return 0;
}

void printDiagnostics(const ScenePlayer& player) {
    std::vector<std::pair<size_t, std::string>> u;
    for (const auto& [k, n] : player.unhandledCommands()) u.emplace_back(n, k);
    std::sort(u.rbegin(), u.rend());
    std::printf("\nUnhandled commands (%zu kinds):\n", u.size());
    for (const auto& [n, k] : u) std::printf("  %7zu  %s\n", n, k.c_str());
    std::printf("Missing assets: %zu\n", player.missingAssets().size());
    size_t shown = 0;
    for (const auto& [k, n] : player.missingAssets()) {
        if (shown++ < 40) std::printf("  %5zu  %s\n", n, sjisToUtf8(k).c_str());
    }
}

int cmdPlay(const std::string& dir, const std::string& scene, uint64_t maxFrames, bool verbose, const std::string& lang) {
    DirectoryGameFolder folder(dir);
    AssetIndex index = AssetIndex::load(folder);
    VarStore vars;
    ScenePlayer player(index, vars);
    player.setTextTiming(0, 5, true);  // text at once
    player.log = [&](const std::string& m) {
        if (verbose || m.starts_with("scene:") || m.starts_with("end of")) {
            std::printf("[%7llu] %s\n", static_cast<unsigned long long>(player.frame()), m.c_str());
        }
    };
    // `--lang <prefix>`: the scenes' translation tables (`.cstl`) in that language (en, cn, jp, ...).
    if (!lang.empty()) player.setTextLanguage(lang);
    // NYAN_PLAY_REPLAY=1: as the extras' replay (sscript boot mode -2), which `end_of_kaisou` ends.
    if (const char* r = std::getenv("NYAN_PLAY_REPLAY"); r && *r == '1') player.setReplaying(true);
    // NYAN_PLAY_VARS="<n>=<value>,...": game variables set before the scene starts (route points).
    if (const char* v = std::getenv("NYAN_PLAY_VARS")) {
        std::string all = v;
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            const std::string kv = all.substr(at, end - at);
            if (const size_t eq = kv.find('='); eq != std::string::npos) vars.set(std::atoi(kv.c_str()), std::atoi(kv.c_str() + eq + 1));
            at = end + 1;
        }
    }
    player.start(scene);
    size_t messages = 0, choices = 0;
    // NYAN_MEDIA_TRACE=1: each sound command as the player hands it to the mixer.
    const bool mediaTrace = std::getenv("NYAN_MEDIA_TRACE") != nullptr;
    static const char* const kMediaKinds[] = {"PlayBgm", "StopBgm", "BgmVolume", "PlaySe",  "StopSe",  "SeVolume", "PlayVoice",
                                              "StopVoice", "PlayMovie", "VoiceVolume", "BgmPan", "SePan", "VoicePan"};
    // NYAN_PLAY_HOLD=<frames>: each message stays that long before the click (voices and lips).
    const uint64_t hold = std::getenv("NYAN_PLAY_HOLD") ? std::strtoull(std::getenv("NYAN_PLAY_HOLD"), nullptr, 10) : 0;
    uint64_t heldSince = 0;
    size_t heldLine = SIZE_MAX;
    while (player.frame() < maxFrames && player.state() != ScenePlayer::State::Ended) {
        player.tick();
        if (hold && player.state() == ScenePlayer::State::Input) {
            if (heldLine != player.lineIndex()) heldLine = player.lineIndex(), heldSince = player.frame();
            if (player.frame() < heldSince + hold) continue;
        }
        for (const auto& m : player.takeMedia()) {
            if (!mediaTrace) continue;
            std::printf("[%7llu] MEDIA %s ch %d %s%s vol %d from %d fade %u%s\n", static_cast<unsigned long long>(player.frame()),
                        kMediaKinds[static_cast<size_t>(m.kind)], m.channel, m.name.c_str(), m.loop ? " loop" : "", m.volume,
                        m.fromVolume, m.fadeFrames,
                        m.spareVoices ? (" spares " + std::to_string(m.spareVoices)).c_str() : "");
        }
        switch (player.state()) {
        case ScenePlayer::State::Input:
            ++messages;
            if (verbose) {
                const auto& w = player.stage().window;
                std::printf("    %s:%zu  %s%s%s\n", player.sceneName().c_str(), player.lineIndex(), w.name.empty() ? "" : w.name.c_str(),
                            w.name.empty() ? "" : ": ", w.text.plain().c_str());
            }
            player.advance();
            break;
        case ScenePlayer::State::WaitKey:
            player.advance();  // `wait key`: clicked through like a message
            break;
        case ScenePlayer::State::Choice:
            ++choices;
            std::printf("[%7llu] choice:", static_cast<unsigned long long>(player.frame()));
            for (const auto& c : player.stage().choices) std::printf(" [%s -> %s]", c.prompt.c_str(), c.target.c_str());
            std::printf("\n");
            player.choose(0);
            break;
        case ScenePlayer::State::Movie:
            player.movieFinished();
            break;
        default:
            break;
        }
    }
    double minutes = player.frame() / 3600.0;
    std::printf("\nStopped in state '%s' at %s:%zu after %llu frames (%.1f min of game time), %zu messages, %zu choices\n",
                toString(player.state()), player.sceneName().c_str(), player.lineIndex(),
                static_cast<unsigned long long>(player.frame()), minutes, messages, choices);
    // The parts seen (`end_of_kaisou <n>` sets #(900 + n); Cyanotype's base is 700).
    std::string seen;
    for (int32_t v = 700; v < 1000; ++v) {
        if ((v < 750 || v >= 900) && v < 950 && vars.get(v) != 0) seen += " #" + std::to_string(v) + "=" + std::to_string(vars.get(v));
    }
    if (!seen.empty()) std::printf("Variables 700-749 / 900-949 set:%s\n", seen.c_str());
    printDiagnostics(player);
    return 0;
}

int cmdPlayAll(const std::string& dir) {
    // Through the game, so scenes see its configuration (vram.xml layout, language).
    Game game(std::make_unique<DirectoryGameFolder>(dir), nullptr);
    const AssetIndex& index = game.assets();
    ScenePlayer& player = game.player();
    player.setTextTiming(0, 5, true);  // text at once
    size_t scenes = 0, stuck = 0;
    std::vector<std::string> errors;
    const bool atTrace = std::getenv("NYAN_AT_TRACE") != nullptr;  // the `@` trace (ScenePlayer::traceAt)
    player.log = [&](const std::string& m) {
        if (atTrace && m.starts_with("at ")) std::printf("%s\n", m.c_str());
        if (m.starts_with("bad ") || m.starts_with("too many") || m.starts_with("missing asset: 1.ogg") || m.starts_with("missing asset: 0.ogg") ||
            (std::getenv("NYAN_PLAYALL_MISSING") && m.starts_with("missing asset"))) errors.push_back(player.sceneName() + ": " + m);
    };
    for (const auto& [name, ref] : visibleAssets(index)) {
        if (extensionOf(name) != "cst") continue;
        std::string scene = name.substr(0, name.size() - 4);
        ++scenes;
        try {
            player.start(scene);
            uint64_t limit = player.frame() + 3600 * 60;  // an hour of game time
            uint64_t overlaySince = UINT64_MAX;
            int sweep = 0;
            while (player.state() != ScenePlayer::State::Ended && player.sceneName() == scene &&
                   player.frame() < limit) {
                player.tick();
                player.takeMedia();
                if (player.state() == ScenePlayer::State::Input || player.state() == ScenePlayer::State::WaitKey) player.advance();
                if (player.state() == ScenePlayer::State::Movie) player.movieFinished();
                if (player.state() == ScenePlayer::State::Choice) break;
                // Dialogs (name entry) are accepted as they stand. Other screens the scene runs (`fes`,
                // `fesjs`) that are still up after 300 frames wait for the player (Happiness 2's
                // notices, REBELLIONS' yes / no prompts): every 30 frames a click goes to the next
                // point of a 10 x 6 grid over the screen until one is taken.
                auto* o = player.overlay();
                if (o && o->dialog()) {
                    o->answerDialog(true, o->dialog()->value);
                } else if (o && player.state() == ScenePlayer::State::Fes) {
                    if (overlaySince == UINT64_MAX) overlaySince = player.frame(), sweep = 0;
                    if (player.frame() >= overlaySince + 300 && player.frame() % 30 == 0) {
                        o->click(64.0f + static_cast<float>(sweep % 10) * 128.0f, 60.0f + static_cast<float>((sweep / 10) % 6) * 120.0f);
                        ++sweep;
                    }
                } else {
                    overlaySince = UINT64_MAX;
                }
            }
            if (player.frame() >= limit) {
                ++stuck;
                errors.push_back(scene + ": did not finish");
            }
        } catch (const std::exception& e) {
            errors.push_back(scene + ": " + e.what());
        }
    }
    std::printf("Ran %zu scenes, %zu stuck, %zu errors\n", scenes, stuck, errors.size());
    for (size_t i = 0; i < errors.size() && (i < 60 || std::getenv("NYAN_PLAYALL_MISSING")); ++i) std::printf("  %s\n", errors[i].c_str());
    printDiagnostics(player);
    return 0;
}

int cmdWinTrace(const std::string& dir, const std::vector<std::string>& only) {
    // Through Game::tick, as the front end runs the story (the window script hears everything the
    // scene sends it), with a click 30 frames into each wait for input and the first option at each
    // choice; each scene in a game of its own, so nothing one leaves (a `frameoff`) reaches the next.
    // One line a message: "<scene>:<input> spk=<the name line, - for none> name=<the name drawn> |
    // <image#frame> | T:<text>...".
    // NYAN_CONF="key=value,...": settings a direct start lacks (Himawari's flow sets wndcolor 50<<24
    // on first start; without it the panel and the name plate have no opacity).
    std::vector<std::pair<std::string, int32_t>> conf;
    if (const char* c = std::getenv("NYAN_CONF")) {
        std::string all = c;
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            const std::string kv = all.substr(at, end - at);
            if (const size_t eq = kv.find('='); eq != std::string::npos) conf.emplace_back(kv.substr(0, eq), std::atoi(kv.c_str() + eq + 1));
            at = end + 1;
        }
    }
    // NYAN_WINTRACE_LOAD="<save at>,<load at>" (scene:input keys): a quick save at the first message,
    // a quick load at the second; the line after it is the window as the load leaves it.
    std::string saveAt, loadAt;
    if (const char* l = std::getenv("NYAN_WINTRACE_LOAD")) {
        const std::string v = l;
        if (const size_t comma = v.find(','); comma != std::string::npos) saveAt = v.substr(0, comma), loadAt = v.substr(comma + 1);
    }
    std::vector<std::string> scenes = only;
    if (scenes.empty()) {
        Game game(std::make_unique<DirectoryGameFolder>(dir), nullptr);
        for (const auto& [name, ref] : visibleAssets(game.assets())) {
            if (extensionOf(name) == "cst") scenes.push_back(name.substr(0, name.size() - 4));
        }
    }
    for (const auto& scene : scenes) {
        try {
            Game game(std::make_unique<DirectoryGameFolder>(dir), nullptr);
            for (const auto& [k, v] : conf) game.config_store()[k] = v;
            ScenePlayer& player = game.player();
            player.setTextTiming(0, 5, true);  // text at once
            game.playScene(scene);
            const uint64_t limit = player.frame() + 60 * 60 * 60;
            std::string lastKey;
            uint64_t waitFrom = 0, overlaySince = UINT64_MAX;
            int sweep = 0;
            bool saved = false, loaded = false;
            while (player.state() != ScenePlayer::State::Ended && player.sceneName() == scene && player.frame() < limit) {
                game.tick();
                player.takeMedia();
                const auto st = player.state();
                if (st == ScenePlayer::State::Input && player.inputReady()) {
                    const std::string key = player.messageKey();
                    if (key != lastKey) lastKey = key, waitFrom = player.frame();
                    if (player.frame() >= waitFrom + 30) {
                        // The scene's own speaker for the message (its name line), then what the window shows.
                        std::string speaker = player.stage().window.speaker;
                        for (char& ch : speaker) if (ch == ' ' || ch == '|') ch = '_';
                        std::printf("%s spk=%s %s\n", key.c_str(), speaker.empty() ? "-" : speaker.c_str(),
                                    game.windowScript().traceSignature(player.frame()).c_str());
                        if (!saved && key == saveAt) saved = game.quickSave();
                        if (saved && !loaded && key == loadAt) {
                            loaded = game.quickLoad();
                            std::printf("(quick load)\n");
                            lastKey.clear();
                            continue;
                        }
                        if (loaded && key == loadAt) continue;  // the fade is still out: the load lands
                        player.advance();
                    }
                } else if (st == ScenePlayer::State::WaitKey) {
                    player.advance();
                } else if (st == ScenePlayer::State::Movie) {
                    player.movieFinished();
                } else if (st == ScenePlayer::State::Choice) {
                    player.choose(0);
                }
                auto* o = player.overlay();
                if (o && o->dialog()) {
                    o->answerDialog(true, o->dialog()->value);
                } else if (o && player.state() == ScenePlayer::State::Fes) {
                    if (overlaySince == UINT64_MAX) overlaySince = player.frame(), sweep = 0;
                    if (player.frame() >= overlaySince + 300 && player.frame() % 30 == 0) {
                        o->click(64.0f + static_cast<float>(sweep % 10) * 128.0f, 60.0f + static_cast<float>((sweep / 10) % 6) * 120.0f);
                        ++sweep;
                    }
                } else {
                    overlaySince = UINT64_MAX;
                }
            }
        } catch (const std::exception& e) {
            std::printf("%s: error %s\n", scene.c_str(), e.what());
        }
    }
    return 0;
}

int cmdMixWav(const std::string& dir, const std::string& name, const std::string& out, double seconds) {
    DirectoryGameFolder folder(dir);
    AssetIndex index = AssetIndex::load(folder);
    audio::Mixer mixer(48000);
    mixer.play(audio::Mixer::Bus::Bgm, 0, index.read(name), true, 500);
    size_t frames = static_cast<size_t>(seconds * 48000);
    std::vector<float> buf(frames * 2);
    for (size_t pos = 0; pos < frames; pos += 480) mixer.render(buf.data() + pos * 2, std::min<size_t>(480, frames - pos));
    double sum = 0;
    float peak = 0;
    std::vector<int16_t> pcm(buf.size());
    for (size_t i = 0; i < buf.size(); ++i) {
        sum += buf[i] * buf[i];
        peak = std::max(peak, std::abs(buf[i]));
        pcm[i] = static_cast<int16_t>(buf[i] * 32767);
    }
    std::ofstream f(out, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(48000);
    u32(48000 * 4); u16(4); u16(16); f.write("data", 4); u32(dataBytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
    std::printf("%.1fs rendered, RMS %.3f, peak %.3f -> %s\n", seconds, std::sqrt(sum / buf.size()), peak, out.c_str());
    return 0;
}

// Decodes a movie the way the game does (picture by time, sound through the mixer) and reports
// how fast: frames, decode time per frame, and the sound's level.
template <class Video>
int measureMovie(Video& video, double seconds) {
    std::printf("%dx%d %.2f fps, audio %d Hz\n", video.width(), video.height(), video.frameRate(), video.sampleRate());
    audio::Mixer mixer(48000);
    if (video.hasAudio()) mixer.playStream(audio::Mixer::Bus::Movie, 0, video.sampleRate(), video.audioSource(false));
    int pictures = 0;
    double worst = 0, total = 0;
    std::vector<float> buf(800 * 2);
    double sum = 0;
    size_t samples = 0;
    int frame = 0;
    for (; frame < static_cast<int>(seconds * 60) && !video.ended(); ++frame) {
        auto t0 = std::chrono::steady_clock::now();
        if (video.seek(frame / 60.0)) ++pictures;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, ms), total += ms;
        mixer.render(buf.data(), 800);
        for (float v : buf) sum += v * v;
        samples += buf.size();
    }
    std::printf("%d game frames, %d pictures, decode avg %.2f ms worst %.2f ms, sound RMS %.3f%s\n", frame, pictures,
                total / std::max(1, frame), worst, std::sqrt(sum / std::max<size_t>(1, samples)), video.ended() ? ", ended" : "");
    return 0;
}

// MPEG-1 through pl_mpeg, anything else (Nekopara's WMV) through FFmpeg, read from the archive.
int cmdMovie(const std::string& dir, const std::string& name, double seconds) {
    DirectoryGameFolder folder(dir);
    AssetIndex index = AssetIndex::load(folder);
    auto ref = index.find(name);
    if (!ref) {
        std::fprintf(stderr, "%s: not found\n", name.c_str());
        return 1;
    }
    if (name.ends_with(".mpg")) {
        auto video = media::MpegVideo::open(ref->archive->read(*ref->entry));
        if (!video) {
            std::fprintf(stderr, "%s: not an MPEG-1 stream\n", name.c_str());
            return 1;
        }
        return measureMovie(*video, seconds);
    }
    auto video = media::FfmpegVideo::open(ref->archive->openEntry(*ref->entry));
    if (!video) {
        std::fprintf(stderr, "%s: FFmpeg cannot decode it%s\n", name.c_str(),
                     media::FfmpegVideo::available() ? "" : " (built without FFmpeg)");
        return 1;
    }
    return measureMovie(*video, seconds);
}

int cmdTheme(const std::string& dir) {
    Game game(std::make_unique<DirectoryGameFolder>(dir), nullptr);
    std::printf("screen %dx%d\n", game.config().width, game.config().height);
    for (const auto& [name, L] : game.windowTheme().styles) {
        std::printf("%-14s img=%s bg#%d origin=(%d,%d) text=(%d,%d %dx%d) name=(%d,%d %dx%d) face=(%d,%d) clip=(%d,%d %dx%d) "
                    "sizes=%d/%d/%d/%d/%d name=%d\n",
                    name.c_str(), L.image.c_str(), L.backgroundFrames.empty() ? -1 : L.backgroundFrames.back(), L.originX, L.originY, L.text.x, L.text.y, L.text.w,
                    L.text.h, L.name.x, L.name.y, L.name.w, L.name.h, L.faceX, L.faceY, L.faceClip.x, L.faceClip.y,
                    L.faceClip.w, L.faceClip.h, L.fontSizes[0], L.fontSizes[1], L.fontSizes[2], L.fontSizes[3],
                    L.fontSizes[4], L.nameSize);
        // The window script as the engine starts it (theme images in $str900.., no MES_POS: the
        // sscript sends one only for a dragged position, 0x0038ec): where it puts its planes.
        if (L.script.empty()) continue;
        for (int i = 0; i < 3; ++i) game.vars().setStr(900 + i, L.images[i]);
        auto fes = FesInstance::load(game.assets(), game.vars(), L.script, 0, &game.config_store(), nullptr);
        if (!fes) continue;
        uint64_t f = 0;
        while (!fes->startedUp() && f < 600) fes->tick(++f);
        std::printf("    script %s (started %s): window plane %s", L.script.c_str(), fes->startedUp() ? "yes" : "no", L.windowPlane.c_str());
        for (const auto& n : fes->objectNames()) {
            int32_t x = 0, y = 0;
            std::string parent, type;
            if (!fes->objectPosition(n, f, x, y, &parent, &type) || type != "PLANE" || !parent.empty()) continue;
            std::printf("  %s=(%d,%d)", n.c_str(), x, y);
        }
        std::printf("\n");
    }
    return 0;
}

// Magic numbers for formats we can sanity-check after decryption.
const char* expectedMagic(const std::string& ext) {
    if (ext == "ogg") return "OggS";
    if (ext == "cst") return "CatScene";
    if (ext == "hg3") return "HG-3";
    if (ext == "hg2") return "HG-2";
    if (ext == "fes") return "FES";
    if (ext == "anm") return "ANM";
    return nullptr;
}

int cmdVerify(const std::string& dir, unsigned threads) {
    DirectoryGameFolder folder(dir);
    auto t0 = std::chrono::steady_clock::now();
    AssetIndex index = AssetIndex::load(folder);
    auto assets = visibleAssets(index);
    std::printf("Verifying %zu assets in %zu archives with %u threads...\n", assets.size(), index.archives().size(),
                threads);

    struct ExtStats {
        size_t count = 0, bytes = 0, badMagic = 0;
    };
    std::map<std::string, ExtStats> stats;
    std::map<std::string, size_t> hgEncodings;
    std::vector<std::string> failures;
    std::atomic<size_t> next{0}, hgFrames{0}, hgFailures{0}, done{0};
    std::mutex mu;

    auto worker = [&] {
        std::map<std::string, ExtStats> local;
        std::map<std::string, size_t> localEnc;
        for (size_t i; (i = next.fetch_add(1)) < assets.size();) {
            const auto& [name, ref] = assets[i];
            std::string ext = extensionOf(name);
            auto& s = local[ext];
            ++s.count;
            s.bytes += ref.entry->length;
            try {
                Bytes data = ref.archive->read(*ref.entry);
                if (const char* magic = expectedMagic(ext)) {
                    size_t len = std::strlen(magic);
                    if (data.size() < len || std::memcmp(data.data(), magic, len) != 0) {
                        ++s.badMagic;
                        std::lock_guard lock(mu);
                        failures.push_back(name + ": bad signature for ." + ext);
                    }
                }
                if (ext == "hg3" || ext == "hg2") {
                    HgImage img = HgImage::parse(std::move(data));
                    for (size_t f = 0; f < img.frames().size(); ++f) {
                        const auto& frame = img.frames()[f];
                        static const char* names[] = {"standard", "jpeg", "jpeg+alpha", "webp", "unsupported"};
                        ++localEnc[names[static_cast<int>(frame.encoding)]];
                        RgbaImage px = img.decode(f);
                        if (px.pixels.size() != static_cast<size_t>(frame.width) * frame.height * 4) {
                            fail("decoded size mismatch");
                        }
                        ++hgFrames;
                    }
                }
            } catch (const Error& e) {
                ++hgFailures;
                std::lock_guard lock(mu);
                failures.push_back(name + " (" + ref.archive->name() + "): " + e.what());
            }
            size_t d = ++done;
            if (d % 5000 == 0) {
                std::lock_guard lock(mu);
                std::printf("  %zu / %zu\n", d, assets.size());
                std::fflush(stdout);
            }
        }
        std::lock_guard lock(mu);
        for (auto& [k, v] : local) {
            stats[k].count += v.count;
            stats[k].bytes += v.bytes;
            stats[k].badMagic += v.badMagic;
        }
        for (auto& [k, v] : localEnc) hgEncodings[k] += v;
    };

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n%-8s %8s %12s %9s\n", "Ext", "Count", "MiB", "BadMagic");
    for (const auto& [ext, s] : stats) {
        std::printf("%-8s %8zu %12.1f %9zu\n", ext.c_str(), s.count, s.bytes / 1048576.0, s.badMagic);
    }
    std::printf("\nHG frames decoded: %zu", hgFrames.load());
    for (const auto& [enc, n] : hgEncodings) std::printf("  [%s: %zu]", enc.c_str(), n);
    std::printf("\nFailures: %zu   Time: %.1fs\n", failures.size(), secs);
    std::sort(failures.begin(), failures.end());
    for (size_t i = 0; i < failures.size() && i < 50; ++i) std::printf("  FAIL %s\n", failures[i].c_str());
    if (failures.size() > 50) std::printf("  ... and %zu more\n", failures.size() - 50);
    return failures.empty() ? 0 : 1;
}

std::optional<int> run(const std::vector<std::string>& args) {
    const std::string& cmd = args[0];
    if (cmd == "info" && args.size() == 2) return cmdInfo(args[1]);
    if (cmd == "list" && (args.size() == 2 || args.size() == 3)) {
        return cmdList(args[1], args.size() == 3 ? args[2] : "");
    }
    if (cmd == "extract" && args.size() >= 3) {
        bool png = false, pngOnly = false;
        std::string filter, regex;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--png") png = true;
            else if (args[i] == "--png-only") png = pngOnly = true;
            else if (args[i] == "--regex" && i + 1 < args.size()) regex = args[++i];
            else filter = args[i];
        }
        return cmdExtract(args[1], args[2], filter, png, regex, pngOnly);
    }
    if (cmd == "cst" && args.size() == 3) return cmdCst(args[1], args[2]);
    if (cmd == "cststats" && args.size() == 2) return cmdCstStats(args[1]);
    if (cmd == "fes" && args.size() == 3) return cmdFes(args[1], args[2]);
    if (cmd == "font" && args.size() >= 2) return cmdFont(args[1], {args.begin() + 2, args.end()});
    if (cmd == "cursor" && args.size() >= 2 && args.size() <= 4)
        return cmdCursor(args[1], args.size() >= 3 ? args[2] : "", args.size() >= 4 ? args[3] : "");
    if (cmd == "play" && args.size() >= 2) {
        std::string scene = "start";
        uint64_t frames = 60ull * 60 * 60 * 100;
        bool verbose = false;
        std::string lang;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--frames" && i + 1 < args.size()) frames = std::stoull(args[++i]);
            else if (args[i] == "--verbose") verbose = true;
            else if (args[i] == "--lang" && i + 1 < args.size()) lang = args[++i];
            else scene = args[i];
        }
        return cmdPlay(args[1], scene, frames, verbose, lang);
    }
    if (cmd == "movie" && args.size() >= 3) return cmdMovie(args[1], args[2], args.size() > 3 ? std::stod(args[3]) : 10);
    if (cmd == "mixwav" && args.size() >= 4) return cmdMixWav(args[1], args[2], args[3], args.size() > 4 ? std::stod(args[4]) : 10);
    if (cmd == "theme" && args.size() == 2) return cmdTheme(args[1]);
    if (cmd == "playall" && args.size() == 2) return cmdPlayAll(args[1]);
    if (cmd == "wintrace" && args.size() >= 2) return cmdWinTrace(args[1], {args.begin() + 2, args.end()});
    if (cmd == "hg" && args.size() == 4) return cmdHg(args[1], args[2], args[3]);
    if (cmd == "psb" && args.size() >= 3) {
        return cmdPsb(args[1], args[2], args.size() > 3 ? args[3] : "", args.size() > 4 ? std::stoi(args[4]) : 2);
    }
    if (cmd == "emote" && args.size() >= 4) return cmdEmote(args[1], args[2], args[3], {args.begin() + 4, args.end()});
    if (cmd == "verify" && args.size() >= 2) {
        unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        if (args.size() == 4 && args[2] == "--threads") threads = static_cast<unsigned>(std::stoul(args[3]));
        return cmdVerify(args[1], threads);
    }
    return std::nullopt;
}

}  // namespace

const nyan::cli::CommandSet nyan::cli::kCatSystem2Commands = {nyan::Engine::CatSystem2, usage, run};
