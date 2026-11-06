#include "nyan/cs2/fes_runtime.hpp"

#include <bit>

#include "nyan/cs2/dictionary.hpp"
#include "nyan/cs2/anm.hpp"

#include <ctime>

#include "nyan/common/sjis.hpp"
#include "nyan/cs2/save_data.hpp"

#include <algorithm>
#include <limits>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nyan/common/sjis.hpp"
#include "nyan/cs2/hg_image.hpp"
#include "nyan/cs2/window_theme.hpp"

namespace nyan::cs2 {
namespace {

constexpr uint32_t kMaxStatementsPerTick = 20000;
constexpr uint64_t kClickFrames = 10;  // how long an unread click identifier is kept

bool isContainerType(const std::string& t) {
    return t == "PLANE" || t == "CPLANE" || t == "STRING" || t == "NVSTRING" || t == "LOGSTRING";
}
// The image kinds (cs2.exe's image load 0x5625e0 sets their base, +0xe0): CIMAGE, IMAGE, BUTTON,
// PANEL, SAVEPANEL, BGTABLE(2), CHTABLE(2/3), IMGWND, DIFIMAGE.
bool isImageKind(const std::string& t) {
    static const std::set<std::string> kImages = {"CIMAGE",  "IMAGE",    "BUTTON",   "PANEL",  "SAVEPANEL", "BGTABLE",
                                                  "BGTABLE2", "CHTABLE", "CHTABLE2", "CHTABLE3", "IMGWND", "DIFIMAGE"};
    return kImages.count(t) != 0;
}
bool isDrawableType(const std::string& t) {
    return t == "IMAGE" || t == "BUTTON" || t == "CIMAGE" || t == "DIFIMAGE";
}

bool pairOf(const std::string& v, int32_t& a, int32_t& b) {
    size_t comma = v.find(',');
    if (comma == std::string::npos) return false;
    a = std::atoi(v.c_str());
    b = std::atoi(v.c_str() + comma + 1);
    return true;
}

// C's sprintf over whole numbers: %[flags][width][.prec](d|i|u|x|X|o|c) take the next argument
// (0 once they run out), %% is a percent sign; anything else is copied as written.
std::string formatInts(const std::string& f, const int32_t* args, size_t n) {
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') { out += f[i]; continue; }
        size_t j = i + 1;
        while (j < f.size() && std::strchr("-+ #0", f[j])) ++j;
        while (j < f.size() && (std::isdigit(static_cast<unsigned char>(f[j])) || f[j] == '.')) ++j;
        if (j < f.size() && f[j] == '%') { out += '%'; i = j; continue; }
        if (j >= f.size() || !std::strchr("diuxXoc", f[j])) { out += f[i]; continue; }
        std::string spec = f.substr(i, j - i + 1);
        char buf[64];
        std::snprintf(buf, sizeof buf, spec.c_str(), next < n ? args[next] : 0);
        ++next;
        out += buf;
        i = j;
    }
    return out;
}

// The stand viewers' list files (Amakano 2's cs2.exe). Their text is taken as the game's (decoded
// to UTF-8), which leaves the ASCII separators the engine looks for where they were.

// A text file's lines (0x722b50): broken at "\r\n", "\r" or "\n"; empty ones kept.
std::vector<std::string> textLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < text.size()) {
        const size_t end = text.find_first_of("\r\n", i);
        if (end == std::string::npos) {
            lines.push_back(text.substr(i));
            break;
        }
        lines.push_back(text.substr(i, end - i));
        i = end + 1;
        if (text[end] == '\r' && i < text.size() && text[i] == '\n') ++i;
    }
    return lines;
}

// The pieces of a string between separators (0x63b290 / 0x63acb0): runs of the separator count as
// one, and nothing is made of leading or trailing ones.
std::vector<std::string> pieces(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == sep) {
            ++i;
            continue;
        }
        size_t end = s.find(sep, i);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(i, end - i));
        i = end;
    }
    return out;
}

// A CSV file (0x6c02b0, comma mode): rows end at a line break; a field (0x6c0a40) ends at a comma,
// or is quoted ("" for a quote, commas and line breaks inside it; the breaks are dropped), and
// anything after its closing quote up to the next comma is skipped. An empty line is a row with
// no fields.
std::vector<std::vector<std::string>> parseCsv(const std::string& s) {
    std::vector<std::vector<std::string>> rows;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        std::vector<std::string> row;
        while (i < n) {
            if (s[i] == '\n') {
                ++i;
                break;
            }
            if (s[i] == '\r') {
                ++i;
                if (i < n && s[i] == '\n') ++i;
                break;
            }
            std::string field;
            const bool quoted = s[i] == '"';
            if (quoted) ++i;
            bool done = false;
            while (i < n && s[i] != '\0' && !done) {
                const char c = s[i];
                if (c == '\n' || c == '\r') {
                    if (!quoted) break;
                    ++i;
                    if (i < n && s[i] == (c == '\n' ? '\r' : '\n')) ++i;
                } else if (c == '"') {
                    if (i + 1 < n && s[i + 1] == '"') field += '"', i += 2;
                    else done = true, ++i;
                } else if (c == ',' && !quoted) {
                    break;
                } else {
                    field += c, ++i;
                }
            }
            while (i < n && s[i] != '\0' && s[i] != '\r' && s[i] != '\n') {
                if (s[i++] == ',') break;
            }
            row.push_back(std::move(field));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// A BGTABLE2's background mark, the first field of a background's row (0x952e40).
constexpr const char* kBackgroundMark = "○";

// The row of the n-th background (0x727fd0), -1 if there is none.
int32_t backgroundRow(const std::vector<std::vector<std::string>>& rows, int32_t n) {
    int32_t seen = 0;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (rows[r].empty() || rows[r][0] != kBackgroundMark) continue;
        if (seen++ == n) return static_cast<int32_t>(r);
    }
    return -1;
}

// A background's picture (0x727eb0 / 0x727f30): field picture + 2 of its row; empty if the lists
// have none.
std::string backgroundPicture(const std::vector<std::vector<std::string>>& rows, int32_t backgrounds, int32_t bg,
                              int32_t picture) {
    if (bg >= backgrounds) return {};
    const int32_t row = backgroundRow(rows, bg);
    const int32_t field = picture + 2;
    if (row < 0 || field < 0 || static_cast<size_t>(field) >= rows[static_cast<size_t>(row)].size()) return {};
    return rows[static_cast<size_t>(row)][static_cast<size_t>(field)];
}

// The comments of a list (0x723220): `//` to the end of its line, `/* */` across lines.
void stripComments(std::vector<std::string>& lines) {
    bool block = false;
    for (std::string& line : lines) {
        size_t start = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            if (block) {
                if (line.compare(i, 2, "*/") == 0) {
                    line.erase(start, i + 2 - start);
                    block = false;
                    i = start;
                    if (i >= line.size()) break;
                    --i;
                }
            } else if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                block = true;
                start = i;
            } else if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
                line.erase(i);
                break;
            }
        }
        if (block) {
            line.erase(start);
            start = 0;
        }
    }
}

// The comma-separated parts of two names merged (0x72a900 -> 0x72adf0): the second's part where it
// has one other than "@", else the first's.
std::string mergeName(const std::string& a, const std::string& b) {
    const std::vector<std::string> pa = pieces(a, ','), pb = pieces(b, ',');
    std::string out;
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        if (i) out += ',';
        out += i < pa.size() && (i >= pb.size() || pb[i] == "@") ? pa[i] : pb[i];
    }
    return out;
}

// An object's opacity: a running mcfade sets it outright each frame (the engine's motion writes the
// plane's alpha), else its own value.
template <class O>
int32_t opacity(const O& o, uint64_t frame) {
    for (const Motion& m : o.motions)
        if (m.kind == Motion::Kind::Fade) return evaluate(o.motions, frame).setAlpha;
    return o.alpha.value(frame);
}

// The engine's FE objects hold what their motions do: a motion writes the object's position, scale
// and rotation each frame (the object's vtable+8, Happiness 2 cs2.exe 0x653a40, from the motion's
// values), a motion that starts on a still object starts from them (vtable+4, 0x653950), and a
// finished motion's end stays the object's. `stop` (0x67a2b0) writes the motions' current values
// one last time (0x652840 with no time step) and ends them (0x699680): the object stays where it
// is. Here a motion is an offset on the object's own tweens, so the motions are folded into them:
// the finished ones (`all` false) or every one (`all`, for `stop`, which also freezes the tweens).
template <class O>
void foldMotions(O& o, uint64_t at, bool all) {
    std::vector<Motion> fold, keep;
    for (const Motion& m : o.motions) (all || (!m.loop && !m.active(at)) ? fold : keep).push_back(m);
    if (fold.empty() && !all) return;
    const MotionState ms = evaluate(fold, at);
    auto shift = [](Tween& t, float d) {
        const int32_t di = static_cast<int32_t>(std::lround(d));
        t.from += di;
        t.to += di;
    };
    auto scale = [](Tween& t, float k) {
        t.from = static_cast<int32_t>(std::lround(t.from * k));
        t.to = static_cast<int32_t>(std::lround(t.to * k));
    };
    if (ms.hasPos) {
        o.x = Tween::constant(static_cast<int32_t>(std::lround(ms.x)));
        o.y = Tween::constant(static_cast<int32_t>(std::lround(ms.y)));
    }
    shift(o.x, ms.dx);
    shift(o.y, ms.dy);
    scale(o.scaleX, ms.scaleX);
    scale(o.scaleY, ms.scaleY);
    o.rotation += ms.rotation;
    o.rotX += ms.rotX;
    o.rotY += ms.rotY;
    o.motions = std::move(keep);
    if (all) {
        for (Tween* t : {&o.x, &o.y, &o.scaleX, &o.scaleY}) *t = Tween::constant(t->value(at));
    }
}

}  // namespace

void FesInstance::styleOf(const Object& o, FesQuad& q) {
    if (o.shadow[0] >= 0) {
        q.edgeRightDown = o.shadow[0] != 0 ? 1 : 0;
        q.edgeAround = o.shadow[1] != 0 ? 1 : 0;
    }
    if (o.weight >= 0) q.weight = o.weight;
}

FesQuad FesInstance::stringBox(const Object& c, uint64_t frame) const {
    FesQuad q;
    // Script text is Shift-JIS; engine-provided names are already UTF-8.
    q.text = isValidUtf8(c.text) ? c.text : sjisToUtf8(c.text);
    const float size = static_cast<float>(c.textSizes[2]);
    const float x = static_cast<float>(c.margin[0]), y = static_cast<float>(c.margin[1]);
    const float w = static_cast<float>(c.width - c.margin[0] - c.margin[2]), h = static_cast<float>(c.height);
    const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
    for (int k = 0; k < 4; ++k) q.x[k] = x + xs[k] * w, q.y[k] = y + ys[k] * h;
    q.textSize = size;
    auto col = c.colors.find("color");
    q.color = col != c.colors.end() && col->second ? col->second : 0xFFFFFFFF;
    // The layout colour's edge (`layout color r g b <edge r g b> ...`); black when unset.
    auto edge = c.colors.find("color#edge");
    q.outlineColor = edge != c.colors.end() ? edge->second : 0xFF000000;
    // One line, plus one per `\n` in the text (config.fes's three-line preview), the layout's
    // `move` apart as in the backlog; wrapped lines too, as many as the box holds (the string's
    // plane is the object's size, kcTaskStringPlane: lines past it are cut). Cyanotype's tips
    // wrap paragraphs of two or three breaks over six lines and more.
    const int breaks = 1 + static_cast<int>(std::count(q.text.begin(), q.text.end(), '\n'));
    const float pitch = size + static_cast<float>(c.moveY);
    const int fit = h >= size && pitch > 0 ? 1 + static_cast<int>((h - size) / pitch) : 1;
    q.maxLines = std::max(breaks, fit);
    q.align = c.textAlign;
    q.lineHeight = q.maxLines > 1 ? pitch : size;
    q.font = fontOf(c);
    q.proportional = !c.fontFixed;
    styleOf(c, q);
    if (c.revealStart >= 0 && frame >= static_cast<uint64_t>(c.revealStart)) {
        q.revealElapsed = static_cast<int64_t>(frame) - c.revealStart;
        q.revealSpeed = c.revealSpeed;
        q.scrollType = c.scrollType;
        q.wipeWidth = c.wipeWidth;
        q.revealFramesPerGlyph = host_ ? host_->framesPerGlyph(c.revealSpeed) : static_cast<float>(c.revealSpeed) * 0.5f;
    }
    return q;
}

int64_t FesInstance::revealFrames(const Object& o) const {
    if (o.revealStart < 0 || o.revealSpeed <= 0) return 0;
    const FesQuad q = stringBox(o, frame_);
    // Glyphs: UTF-8 lead bytes, line breaks left out.
    int64_t glyphs = 0, wide = 0;
    for (unsigned char b : q.text) {
        if ((b & 0xC0) == 0x80 || b == '\n') continue;
        ++glyphs;
        if (b >= 0xC0) ++wide;
    }
    if (o.scrollType == 1 && o.wipeWidth > 0) {
        // The wipe (grisaia.exe 0x54c2b0): size x frames x 2 / speed pixels along the lines; the
        // text is through once the trailing edge, wipewidth behind, passes the end. The length as
        // the renderer lays the text out, else full-width glyphs a size wide and the rest half.
        const float size = std::max(1.0f, q.textSize);
        float total = static_cast<float>(wide) * size + static_cast<float>(glyphs - wide) * size / 2;
        if (auto l = host_ ? host_->textWipeLength(q) : std::nullopt) total = *l;
        const double need = std::ceil(static_cast<double>(total) + o.wipeWidth);
        return static_cast<int64_t>(std::ceil(need * o.revealSpeed / (2.0 * size)));
    }
    const float per = host_ ? host_->framesPerGlyph(o.revealSpeed) : static_cast<float>(o.revealSpeed) * 0.5f;
    return static_cast<int64_t>(std::ceil(static_cast<double>(glyphs) * per));
}

int32_t FesInstance::stringDrawing(const Object& o) const {
    // `isdraw` (cs2.exe 0x775970 -> 0x752db0, the STRING8 object's +0x544; STRING 0x74e430,
    // NVSTRING 0x750930): `draw` sets it to 1, and each frame the object asks its plane (message
    // 0x1da, 0x663610) and keeps the answer: 1 while the text is being revealed, 0 once through.
    // `str` sets -1 (0x752eb0), which the next frame's answer replaces.
    if (o.revealStart >= 0 && static_cast<int64_t>(frame_) - o.revealStart < revealFrames(o)) return 1;
    return o.strFrame == frame_ ? -1 : 0;
}

void FesInstance::revealPlane(const Object& child) {
    // A plane hidden only by its #OBJECT row, which the script never shows itself (`disp 1`, `fade`),
    // shows once the script shows something on it: Cyanotype's title (an image shown on a plane declared
    // hidden) and Hatsumira's chapter titles (a string drawn there) have no other way to appear.
    // Planes the script shows itself wait for that (Fruit's menu: buttons first, then the plane's
    // `disp 1` with its fade-in), planes a command hid stay hidden, and colour planes (CPLANE) are not
    // revealed this way.
    auto it = byName_.find(child.plane);
    if (it == byName_.end()) return;
    Object& p = objects_[it->second];
    if (p.type != "PLANE" || p.disp || p.dispCommanded) return;
    std::string base = p.name.substr(0, p.name.find('['));
    if (shownByScript_.count(base)) return;
    p.disp = true;
}

bool FesInstance::Object::container() const {
    return isContainerType(type);
}

std::shared_ptr<const AnmScript> FesInstance::anmScript(const std::string& name) const {
    auto it = anmScripts_.find(name);
    if (it == anmScripts_.end()) {
        std::shared_ptr<const AnmScript> s;
        if (auto ref = assets_.find(name + ".anm")) {
            if (auto p = AnmScript::parse(ref->archive->read(*ref->entry))) s = std::make_shared<const AnmScript>(std::move(*p));
        }
        it = anmScripts_.emplace(name, std::move(s)).first;
    }
    return it->second;
}

int32_t FesInstance::Object::frame() const {
    // The image's index (setid, IDN, load's id; 0 at first) picks the frame: entry <index> of a
    // 32-entry table that starts as 0, 1, 2, ... once an ID.n column or settbl has written it,
    // else the index itself (cs2.exe 0x749ed0; table from the constructor 0x749ca0, written by
    // 0x749ea0). Index -1 is tested before that and draws the whole image (0x65dbf0).
    if (idIndex == -1) return -1;
    if (!idTable) return idIndex;
    if (idIndex < 0 || idIndex >= 32) return 0;
    auto it = ids.find(idIndex);
    return it != ids.end() ? it->second : idIndex;
}

void FesInstance::Object::setIndex(int32_t n) {
    // 0x749f00: with a table, an index outside it (-1 too) is ignored.
    if (idTable && (n < 0 || n >= 32)) return;
    idIndex = n;
}

FesInstance::Affine FesInstance::Affine::operator*(const Affine& o) const {
    return {a * o.a + c * o.b,       b * o.a + d * o.b,       a * o.c + c * o.d,
            b * o.c + d * o.d,       a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
}

void FesInstance::Affine::apply(float x, float y, float& ox, float& oy) const {
    ox = a * x + c * y + tx;
    oy = b * x + d * y + ty;
}

std::unique_ptr<FesInstance> FesInstance::load(const AssetIndex& assets, VarStore& globals, const std::string& name,
                                               uint64_t frame, Config* config, Host* host) {
    auto ref = assets.find(name + ".fes");
    if (!ref) return nullptr;
    std::string text = decodeFes(ref->archive->read(*ref->entry));
    return create(assets, globals, name, FesScript::parse(text), frame, config, host);
}

std::unique_ptr<FesInstance> FesInstance::create(const AssetIndex& assets, VarStore& globals, const std::string& name,
                                                 FesScript script, uint64_t frame, Config* config, Host* host) {
    auto inst = std::unique_ptr<FesInstance>(new FesInstance(assets, globals, name, std::move(script)));
    inst->frame_ = frame;
    inst->config_ = config;
    inst->host_ = host;
    inst->createObjects();
    inst->jump("START", false);
    return inst;
}

FesInstance::FesInstance(const AssetIndex& assets, VarStore& globals, std::string name, FesScript script)
    : assets_(assets), globals_(globals), expr_(globals, '$'), name_(std::move(name)), script_(std::move(script)) {
    for (const auto& st : script_.code) {
        // First two words: object (maybe indexed) and method.
        const std::string& t = st.text;
        size_t a = t.find(' ');
        if (a == std::string::npos) continue;
        size_t b = t.find(' ', a + 1);
        std::string method = t.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        std::string arg = b == std::string::npos ? std::string() : t.substr(b + 1, t.find(' ', b + 1) - b - 1);
        if (method == "fade" || (method == "disp" && arg != "0")) shownByScript_.insert(t.substr(0, t.find_first_of("[ ")));
        if (method == "layout" && (arg == "prof_enable" || arg == "profmode")) proportionalReset_ = true;
    }
    expr_.setLocals(&locals_, '\\');
    // The FE command handlers evaluate their arguments with cs2.exe 0x5f2260 (through 0x61ef10 for
    // the #SUBCOMMAND types, directly for `pos`, `arc`, ...), whose result for a root unary plus is
    // 0. Amakano 1's logo screen leans on it: `pos -900` then `move 120 +900` moves to 0 (the `px`
    // argument is the target less the position, 0x61de60), and `pos 0 -400`, `move 10 0 +400` to
    // y 0: the letters land on their places.
    expr_.setPlusRootIsZero(true);
    expr_.setIdentResolver([this](std::string_view id) -> std::optional<int32_t> {
        std::string s(id);
        if (s == "_CLICK_L_" || s == "_CLICK_R_") {
            if (clickL_ || clickR_) clickRead_ = true;
            return s == "_CLICK_L_" ? clickL_ : clickR_;
        }
        if (s == "_EXITCODE_") return exitCodeOfLaunched();
        if (s == "_SAVEOBJ_") return host_ ? host_->saveObject() : 0;
        // Frames since the script's first update (cs2.exe 0x756390: the base is that frame - 1).
        if (s == "_FRAME_") return static_cast<int32_t>(frameCount());
        if (s == "_SCRL_U_" || s == "_SCRL_D_" || s == "_PAGE_U_" || s == "_PAGE_D_") {
            int k = s == "_SCRL_U_" ? 0 : s == "_SCRL_D_" ? 1 : s == "_PAGE_U_" ? 2 : 3;
            if (scrollFlags_[k]) clickRead_ = true;
            return scrollFlags_[k];
        }
        if (s == "_MOUSE_X_") return static_cast<int32_t>(mouseX_);
        if (s == "_MOUSE_Y_") return static_cast<int32_t>(mouseY_);
        if (auto m = script_.macros.find(s); m != script_.macros.end()) {
            try {
                return expr_.eval(m->second);
            } catch (const Error&) {
                return 0;
            }
        }
        if (auto it = byName_.find(s); it != byName_.end()) return objects_[it->second].uid;
        // Array element with a computed index: `btn_hit[\0]`, `help[@]`.
        if (size_t open = s.find('['); open != std::string::npos && s.back() == ']') {
            int32_t idx = num(s.substr(open + 1, s.size() - open - 2));
            auto it = byName_.find(s.substr(0, open) + "[" + std::to_string(idx) + "]");
            if (it != byName_.end()) return objects_[it->second].uid;
            return 0;
        }
        if (s.size() > 1 && s.front() == '_' && s.back() == '_') return 0;  // engine flags not modelled
        return std::nullopt;
    });
}

void FesInstance::scriptError(const std::string& what) {
    // Scripts re-evaluate the same faulty line every frame (e.g. an identifier the game never
    // defined): report each one once.
    if (scriptErrors_.insert(what).second && log) log("fes " + name_ + ": " + what);
}

void FesInstance::unhandled(const std::string& what) {
    if (unhandled_[what]++ == 0 && log) log("fes " + name_ + ": unhandled " + what);
}

std::string FesInstance::startFace() const {
    // The config's font when the player chose another than the startup face; empty (the game's
    // font) otherwise.
    if (!host_) return {};
    std::string face = host_->configFont();
    return face == host_->defaultFont() ? std::string() : face;
}

std::string FesInstance::fontOf(const Object& o) const {
    // A string starts with the config's face (Nekopara cs2.exe 0x74f2f0, `reset` 0x74f4e0; Cyanotype
    // 0x658620) and keeps following it: a setconf has the next frame send every task message 0x13
    // (0x5dc080), and strings still flagged (+0x518) take the face again (0x74f690). A script's
    // `font` / `fontind` sets its own and ends that (0x74e860). The weight and the edge follow the
    // config the same way (Game::textWeight) until `layout weight` / `layout shadow` (styleOf).
    return o.fontFollows ? startFace() : o.fontName;
}

std::string FesInstance::str(const std::string& token) const {
    // `$str50` -> string variable 50 (also `$str900:3`, whose ":3" the caller strips). Text after
    // the number is appended (`$str900_%LANG%` -> sys_meswnd_en), and language placeholders expand.
    // Every `$strN` in the text expands (saveload.fes: `str $str487$str481\_$str482`).
    std::string out;
    size_t from = 0;
    const size_t colon = token.starts_with("$str") ? token.find(':') : std::string::npos;
    const std::string body = token.substr(0, colon);
    for (size_t at = body.find("$str"); at != std::string::npos; at = body.find("$str", from)) {
        size_t end = at + 4;
        while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end]))) ++end;
        if (end == at + 4) {
            out += body.substr(from, end - from);
        } else {
            out += body.substr(from, at - from) + globals_.str(std::atoi(body.c_str() + at + 4));
        }
        from = end;
    }
    out += body.substr(std::min(from, body.size()));
    return out.find('%') != std::string::npos ? assets_.expand(out) : out;
}

int32_t FesInstance::num(const std::string& e, int32_t current) {
    try {
        // In event handlers `@` is the triggering element's index (help[@], @==9).
        if (current_ && e.find('@') != std::string::npos) return expr_.eval(e, current_->index);
        return expr_.eval(e, current);
    } catch (const Error& err) {
        scriptError(err.what());
        return current;
    }
}

int32_t FesInstance::numAt(const std::string& e, int32_t current) {
    // In a position or motion argument `@` is where the object is now (`<object>[@] move 10 @+5 @+5`
    // in Amakano 2's map shifts a hovered portrait's shadow 5 pixels), while the `[@]`
    // and conditions of the same handler mean the element's index.
    try {
        return expr_.eval(e, current);
    } catch (const Error& err) {
        scriptError(err.what());
        return current;
    }
}

float FesInstance::percent(const std::string& e, float current) {
    if (!e.empty() && e.back() == '%') return static_cast<float>(num(e.substr(0, e.size() - 1), 0));
    return static_cast<float>(num(e, static_cast<int32_t>(current)));
}

const FesInstance::FrameInfo* FesInstance::frameInfo(const std::string& file, int32_t id) const {
    if (file.empty()) return nullptr;
    auto it = frames_.find(file);
    if (it == frames_.end()) {
        std::map<int32_t, FrameInfo> m;
        if (auto ref = assets_.find(file + ".hg3")) {
            try {
                HgImage img = HgImage::parse(ref->archive->read(*ref->entry));
                for (const auto& f : img.frames()) {
                    m.emplace(static_cast<int32_t>(f.id), FrameInfo{f.offsetX, f.offsetY, static_cast<int32_t>(f.width),
                                                                    static_cast<int32_t>(f.height), f.baseX, f.baseY});
                }
            } catch (const Error&) {
            }
        }
        it = frames_.emplace(file, std::move(m)).first;
    }
    auto f = it->second.find(id);
    return f == it->second.end() ? nullptr : &f->second;
}

void FesInstance::createObjects() {
    for (const auto& d : script_.declarations) {
        int n = std::max(d.count, 1);
        for (int i = 0; i < n; ++i) {
            Object o;
            o.name = d.count ? d.name + "[" + std::to_string(i) + "]" : d.name;
            o.type = d.type;
            if (o.type == "STRING" || o.type == "NVSTRING" || o.type == "LOGSTRING") o.fontFollows = true;
            o.uid = 1000 + static_cast<int32_t>(objects_.size());
            o.index = i;
            o.base = d.name;
            o.element = d.count > 0;
            o.named = !o.element;
            byName_[o.name] = objects_.size();
            objects_.push_back(std::move(o));
        }
    }
    for (const auto& row : script_.rows) {
        for (Object* o : resolve(row.object)) applyRow(*o, row.values);  // resolve marks an element it names
    }
    // An image or button without a PL has no plane of its own (the engine leaves the name empty). In
    // the newer engines (2.6.1.80 on) it draws at the origin of video memory: a plane whose VRAM is
    // 0,0 shows that area, so such objects appear in it, above or below its images by priority (the
    // title buttons of Love's Sweet Garnish and Cyanotype's titlea, in pl_bg with the background;
    // Cyanotype's name plate in the window's pl). Older engines draw them on the screen: Fruit's
    // title has an empty, hidden `pl[0]` at 0,0 and its plane-less buttons still show and click.
    // Elements nothing names in an old engine (fitted, see namedElementsOnly_): Ikinari Anata ni
    // Koishiteiru's window shows `btn` whole, but gives ids to all its buttons except 3 and 4; drawn
    // at index 0 they show the window picture's frame 0, an opaque black layer the size of the
    // window (the layout's base, also its plane's SIZE2), which PC screenshots do not show.
    const uint64_t engine = host_ ? host_->engineFileVersion() : 0;
    namedElementsOnly_ = engine != 0 && engine < 0x0002000600000000ull;
    vramPlanes_ = host_ && host_->engineFileVersion() >= 0x0002000600010050ull;
    if (!vramPlanes_) return;
    // Only a plane there that holds images of its own counts: shared include files declare empty
    // planes at 0,0 too (Fruit's title has `pl[0]`, unused, and draws everything without a plane).
    const Object* origin = nullptr;
    for (const auto& o : objects_) {
        if (o.type != "PLANE" || o.vramX != 0 || o.vramY != 0) continue;
        if (std::any_of(objects_.begin(), objects_.end(), [&](const Object& x) { return x.plane == o.name; })) {
            origin = &o;
            break;
        }
    }
    if (origin) {
        std::string name = origin->name;
        for (auto& o : objects_) {
            if (isDrawableType(o.type) && o.plane.empty() && !o.mask) {
                o.plane = name;
                o.originPlane = true;
            }
        }
    }
}

void FesInstance::applyRow(Object& o, const std::map<std::string, std::string>& values) {
    for (const auto& [col, v] : values) {
        int32_t a, b;
        if (col == "FILE") o.file = v;
        else if (col.starts_with("ID.")) {
            // The suffix is a number or a button state letter: N 0, D 1, F 2, P 3 (cs2.exe 0x759ae0;
            // the CG viewers' tool buttons use ID.N ID.D ID.F ID.P).
            static const std::string kStates = "NDFP";
            const char c = col.size() == 4 ? col[3] : '\0';
            const auto st = kStates.find(c);
            int n = c != '\0' && st != std::string::npos ? static_cast<int>(st) : std::atoi(col.c_str() + 3);
            if (n >= 0 && n < 32) o.ids[n] = num(v), o.idTable = true;
        }
        else if (col == "PRI") o.pri = num(v);
        else if (col == "DISP") o.disp = num(v) != 0, o.dispColumn = true;
        else if (col == "ENABLE") o.enable = num(v) != 0;
        // The object's opacity, 0..255 (as `blend`): objects declared see-through are faded in by
        // the script (Amakano 2's map buttons, Fruit's gallery and config screens).
        else if (col == "BLEND") o.alpha = Tween::constant(std::clamp(num(v), 0, 255));
        else if (col == "PL") o.plane = v;
        else if (col == "VIEWPORT") o.viewport = v;
        else if (col == "MASK") o.mask = true;
        else if (col == "COLOR") {
            // 0xAARRGGBB: the alpha byte is where the object's opacity starts, which `blend` / `fade`
            // then change (a black mask declared `0x00000000` is clear until its `blend 255`).
            auto argb = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
            o.colorFill = true;
            o.color = argb | 0xFF000000u;
            o.alpha = Tween::constant(static_cast<int32_t>(argb >> 24));
        }
        else if (col == "GROUP") o.group = v;
        else if (col == "STYPE") o.stype = num(v);
        else if (col == "IDN") {
            o.setIndex(num(v));  // 0x767be0 flag 0x400
            o.animate = num(v) < 0;
        }
        else if (col == "COUNT") o.logCount = std::max(1, num(v));
        else if (col == "PRI_P") o.pri = num(v);  // priority among planes (scroll bars outside any plane)
        else if (col == "PARENT") o.parent = v;
        else if (col == "POS" && pairOf(v, a, b)) o.x = Tween::constant(a), o.y = Tween::constant(b);
        else if (col == "BASE" && pairOf(v, a, b)) o.baseX = a, o.baseY = b, o.baseSet = true;
        else if (col == "BTYPE") o.btype = num(v);
        else if (col == "SIZE" && pairOf(v, a, b)) o.width = a, o.height = b;
        else if (col == "VRAM" && pairOf(v, a, b)) o.vramX = a, o.vramY = b;
        else if (col == "POS2" || col == "SIZE2") {
            // "$str900:3": position/size of frame 3 of the image named by string variable 900.
            size_t colon = v.find(':');
            if (colon == std::string::npos) continue;
            if (const FrameInfo* f = frameInfo(str(v.substr(0, colon)), std::atoi(v.c_str() + colon + 1))) {
                if (col == "POS2") {
                    o.x = Tween::constant(f->x), o.y = Tween::constant(f->y);
                    o.pos2File = str(v.substr(0, colon));
                    o.pos2Id = std::atoi(v.c_str() + colon + 1);
                    if (o.type == "SLIDER" || o.type == "SCROLL" || o.type == "SCROLL2") o.width = f->w, o.height = f->h;  // the track
                } else {
                    o.width = f->w, o.height = f->h;
                }
            }
        }
        else if (col == "TABLE") o.table = v;
        // TYPE: not needed for drawing yet.
    }
    // A table object reads its lists when its row names them (0x701c80 / 0x701d80, the TABLE
    // column's flag 0x8000000).
    if (values.count("TABLE") && (o.type == "BGTABLE2" || o.type == "CHTABLE3")) loadTable(o);
}

FesInstance::Object* FesInstance::find(const std::string& name) {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &objects_[it->second];
}

std::vector<FesInstance::Object*> FesInstance::resolve(const std::string& ref) {
    std::vector<Object*> out;
    size_t open = ref.find('[');
    if (open != std::string::npos && ref.back() == ']') {
        std::string inner = ref.substr(open + 1, ref.size() - open - 2);
        int32_t idx = num(inner);
        if (Object* o = find(ref.substr(0, open) + "[" + std::to_string(idx) + "]")) {
            o->named = true;
            out.push_back(o);
        }
        // Element 0 of an object declared without an array is the object (Amakano 2's title declares
        // one image that way and shows it as element 0).
        else if (idx == 0) {
            if (Object* o = find(ref.substr(0, open))) out.push_back(o);
        }
        return out;
    }
    if (Object* o = find(ref)) {
        out.push_back(o);
        return out;
    }
    // Array name without an index: every element.
    for (auto& o : objects_) {
        if (o.name.size() > ref.size() && o.name.starts_with(ref) && o.name[ref.size()] == '[') out.push_back(&o);
    }
    return out;
}

void FesInstance::jump(const std::string& target, bool call) {
    // The label may be a string variable: Amakano's config keeps where its confirmation leads in a
    // string variable (`strvar` ... `next $str<n>`).
    const std::string label = target.starts_with("$str") ? str(target) : target;
    size_t start, end;
    if (!script_.block(label, start, end)) {
        unhandled("label " + label);
        return;
    }
    if (log && std::getenv("NYAN_FES_TRACE")) log("fes " + name_ + ": " + (call ? "call " : "next ") + label);
    if (call) callStack_.emplace_back(pc_, end_);
    else blockStart_ = start, blockEnd_ = end;
    if (!call && label != "START") startedUp_ = true;
    pc_ = start;
    end_ = end;
    state_ = State::Running;
}

int32_t FesInstance::exitCodeOfLaunched() {
    if (launched_ == Launched::Fes) return child_ && child_->exited() ? child_->exitCode() : -1;
    if (launched_ == Launched::Kcs) return host_ ? host_->kcsExitCode(kcsProgram_) : -1;
    return -1;
}

std::vector<MediaCommand> FesInstance::takeMedia() {
    auto out = std::exchange(media_, {});
    if (child_) {
        for (auto& m : child_->takeMedia()) out.push_back(std::move(m));
    }
    return out;
}

void FesInstance::skipBlock(bool toElse) {
    int depth = 0;
    while (pc_ < end_) {
        const auto& t = script_.code[pc_++].tokens;
        if (t.empty()) continue;
        if (t[0] == "if") ++depth;
        else if (t[0] == "endif") {
            if (depth-- == 0) return;
        } else if (t[0] == "else" && depth == 0 && toElse) {
            return;
        }
    }
}

bool FesInstance::moving(const Object& o, uint64_t at, int depth) const {
    // A motion's running flag (+0x9c) is up from its start (0x63b480) until its script ends itself
    // (native 38, 0x63c340): a one-shot in the update that gives it its end value ("time >= frames
    // or stopped"), a loop (mcMove, mcRot, ...) only once stopped, by `stop` or a skip. The
    // `oneshot` / `continue` word of #SUBCOMMAND is only checked (0x612380), never kept.
    if (!o.x.done(at) || !o.y.done(at) || !o.alpha.done(at) || !o.scaleX.done(at) || !o.scaleY.done(at)) return true;
    for (const auto& m : o.motions) {
        if (m.active(at)) return true;
    }
    // The objects inside it (0x601d00: its children, each through 0x601be0).
    if (depth < 8) {
        for (const auto& c : objects_) {
            if (&c != &o && (c.plane == o.name || c.parent == o.name) && moving(c, at, depth + 1)) return true;
        }
    }
    return false;
}

bool FesInstance::animating(uint64_t at) const {
    if (wiping(at)) return true;
    for (const auto& o : objects_) {
        if (moving(o, at, 8)) return true;
    }
    return false;
}

void FesInstance::tick(uint64_t frame) {
    frame_ = frame;
    if (!frameBase_) frameBase_ = frame - 1;
    if (child_) child_->tick(frame);
    if (exited_) return;
    // A music-room track that ended by itself: its table's STOP event (the script moves on).
    for (auto& o : objects_) {
        if (o.type != "SNDTABLE" || !o.playing || o.repeat || frame_ < o.playFrom + 30) continue;
        if (host_ && !host_->bgmPlaying(7)) {
            o.playing = false;
            fireEvent(o, "STOP");
        }
    }
    if (rehover_) {
        rehover_ = false;
        if (pointerSeen_) pointerMove(mouseX_, mouseY_);
    }
    // The Force Skip key, asked at the start of each update (cs2.exe 0x52eb1b, before the step at
    // 0x52ebd3). A change is the input handler's event 0x13 (held: `_CLICK_L_` -1, the click flag
    // and the held flag +0x5b8 up) or 0x14 (let go: all three down) (0x60e480).
    if (const bool key = host_ && host_->forceSkipHeld(); key != skipKeyDown_) {
        skipKeyDown_ = key;
        skipClick_ = skipHeld_ = key;
        skipClickFromPress_ = false;
        clickL_ = key ? -1 : 0;
        clickFromHold_ = key;
        clickAt_ = frame_;
    }
    // The waits as the update has them (Amakano 1's cs2.exe 0x60de30, before the statements), with
    // the click flag (skipClick_). The checks see the motions as the last update left them: the
    // update (0x60e326) comes after the statements. With skipping on, `wait N` not yet due and
    // `wait frame N` (a held skip too) end at once after the skip (0x601c10), the script going on in
    // this update. A bare `wait` (-1) runs no statements in the update that checks it: with nothing
    // moving it only clears its state, the script going on in the next update; while something
    // moves, a click (or a held skip) with skipping on stops every motion, which this update's run
    // of them ends. `wait <object>` (-4) goes on in the update that finds its objects still, or on a
    // click (with a held skip only under `keyskip ctrl` or on), with no skip, whatever `keyskip` says.
    const uint64_t lastUpdate = frame_ ? frame_ - 1 : 0;
    switch (state_) {
    case State::WaitFrames:
        if (frame_ >= waitUntil_) state_ = State::Running;
        else if (keyskipOn_ && skipClick_) skipMotions(), state_ = State::Running;
        break;
    case State::WaitFrameCount:
        if (frameCount() >= waitFrameCount_) state_ = State::Running;
        else if (keyskipOn_ && (skipHeld_ || skipClick_)) skipMotions(), state_ = State::Running;
        break;
    case State::WaitAnimation:
        // Nothing moves: +0x4c = 0, and the next update's statements go on.
        if (!animating(lastUpdate)) state_ = State::WaitFrames, waitUntil_ = frame_ + 1;
        else if (keyskipOn_ && (skipHeld_ || skipClick_)) skipMotions();
        break;
    default:
        break;
    }
    const bool objectSkip = skipClick_ && (!skipHeld_ || keyskipCtrl_ || keyskipOn_);
    // Sounds that ended (cs2.exe 0x6285b0, each update: the sound task asked with message 0xb9).
    // From the frame after their `play`, when the engine has started them.
    for (auto& o : objects_) {
        if (o.soundOn && frame_ > o.playFrom && !(host_ && host_->soundPlaying(o.soundKind, o.soundChannel))) o.soundOn = false;
    }
    if (state_ == State::WaitSound) {
        auto objs = resolve(waitSound_);
        if (objectSkip || objs.empty() || !objs[0]->soundOn) state_ = State::Running;
    }
    if (state_ == State::WaitObject) {
        // `wait <object>` (state -4, 0x60df59): until none of the objects named moves.
        bool busy = false;
        for (const Object* o : resolve(waitSound_)) busy = busy || moving(*o, lastUpdate);
        if (objectSkip || !busy) state_ = State::Running;
    }
    const FesInstance* before = child_.get();
    if (state_ == State::Running) run();
    // A screen started now (`execfes`) runs its start straight away, in this same frame: the caller
    // has typically just hidden itself after fading to black, and the new screen covers the gap
    // (Hatsumira's settings tabs) instead of the game showing through for a frame.
    if (child_ && child_.get() != before) child_->tick(frame);
    // A click identifier lasts until the script has looked at it. A click that arrives while the
    // script is busy elsewhere (a hover handler's `wait 1`: a touch tap delivers hover and click
    // together) is kept a few frames for the loop that polls it, then dropped. Not the engine's:
    // it clears them at the end of the next update (+0x5c0); kept with the release-time click
    // (mouseUp) for touch, by the user's decision.
    if (clickRead_ || frame_ >= clickAt_ + kClickFrames) {
        // The update keeps `_CLICK_L_` under `keyskip ctrl` while the skip is held (0x60e2b5 /
        // 0x60e33d), and a held skip's -1 until an input event comes (+0x5c0).
        if (!clickFromHold_ && !(keyskipCtrl_ && skipHeld_)) clickL_ = 0;
        clickR_ = 0;
    }
    if (clickRead_ || frame_ >= scrollAt_ + kClickFrames) std::fill(std::begin(scrollFlags_), std::end(scrollFlags_), 0);
    clickRead_ = false;
    // The click flag lasts one update unless `keyskip cont` or a held skip keeps it (0x60e338).
    if (!keyskipCont_ && !skipHeld_) skipClick_ = false;
    skipClickFromPress_ = false;
}

void FesInstance::answerDialog(bool ok, const std::string& valueUtf8) {
    if (child_ && child_->dialog()) {
        child_->answerDialog(ok, valueUtf8);
        return;
    }
    if (!dialog_) return;
    if (dialog_->input && ok) {
        // The dialog (cs2.exe 0x62b5a0) limits the edit box to maxChars UTF-16 characters
        // (EM_LIMITTEXT) and converts what was typed to Shift-JIS (0x6a4f10, up to 1024 bytes).
        std::string u = valueUtf8;
        if (dialog_->maxChars > 0) {
            size_t units = 0, i = 0;
            while (i < u.size()) {
                const auto b = static_cast<unsigned char>(u[i]);
                const size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
                const size_t w = len == 4 ? 2 : 1;  // a character past U+FFFF is two UTF-16 units
                if (units + w > static_cast<size_t>(dialog_->maxChars)) break;
                units += w;
                i += len;
            }
            u.resize(std::min(i, u.size()));
        }
        std::string v = utf8ToSjis(u);
        if (v.size() > 1023) v.resize(1023);
        globals_.setStr(dialogValueVar_, v);
    }
    if (dialogResultVar_ >= 0) globals_.set(dialogResultVar_, ok ? 1 : 0);
    dialog_.reset();
    state_ = State::Running;
}

void FesInstance::run() {
    uint32_t guard = 0;
    while (state_ == State::Running && !exited_) {
        if (pc_ >= end_) {
            if (callStack_.empty()) {
                startedUp_ = true;
                // A block that ends without `next` polls: it runs again next frame.
                pc_ = blockStart_;
                end_ = blockEnd_;
                waitUntil_ = frame_ + 1;
                state_ = State::WaitFrames;
                return;
            }
            std::tie(pc_, end_) = callStack_.back();
            callStack_.pop_back();
            continue;
        }
        if (++guard > kMaxStatementsPerTick) return;  // continue next frame
        const auto& st = script_.code[pc_++];
        if (!execute(st)) return;
    }
}

bool FesInstance::execute(const FesScript::Statement& st) {
    const auto& t = st.tokens;
    if (log && std::getenv("NYAN_FES_TRACE") && std::string(std::getenv("NYAN_FES_TRACE")) == "2") {
        log("fes " + name_ + " @" + std::to_string(pc_ - 1) + " [" + std::to_string(end_) + "] f" + std::to_string(frame_) + ": " + st.text);
    }
    if (t.empty()) return true;
    const std::string& k = t[0];
    auto rest = [&](size_t from) {
        std::string r;
        for (size_t i = from; i < t.size(); ++i) r += (i > from ? " " : "") + t[i];
        return r;
    };

    if (k == "if" || k.starts_with("if(")) {
        size_t open = st.text.find('('), close = st.text.rfind(')');
        bool ok = open != std::string::npos && close != std::string::npos && close > open &&
                  num(st.text.substr(open + 1, close - open - 1)) != 0;
        if (!ok) skipBlock(true);
        return true;
    }
    if (k == "else") {
        skipBlock(false);  // the if-branch ran: skip to endif
        return true;
    }
    if (k == "endif") return true;
    if (k == "next" && t.size() >= 2) {
        jump(t[1], false);
        return true;
    }
    if (k == "call" && t.size() >= 2) {
        jump(t[1], true);
        return true;
    }
    if (k == "break") {
        pc_ = end_;
        return true;
    }
    if (k == "exit") {
        exitCode_ = t.size() >= 2 ? num(t[1]) : 0;
        exited_ = true;
        return false;
    }
    if (k == "wait" && t.size() >= 2 && t[1] == "frame") {
        // `wait frame <n>` (cs2.exe 0x77a060 -> mode -2, 0x7586b0; grisaia.exe 0x5ef320): until the
        // script's `_FRAME_` reaches n, checked at each update once `_FRAME_` is set (0x756390,
        // grisaia.exe 0x5d1500). Fruit's staff roll times its pictures to the music this way.
        waitFrameCount_ = t.size() >= 3 ? num(t[2]) : 0;
        if (frameCount() >= waitFrameCount_) return true;
        state_ = State::WaitFrameCount;
        return false;
    }
    if (k == "reset_frame") {
        // cs2.exe 0x77a820 / grisaia.exe 0x5ef9c0: the count's base back to 0 and `_FRAME_` = 0; the
        // next update sets the base to that frame - 1, so `_FRAME_` counts 1, 2, ... from there.
        frameBase_ = frame_;
        return true;
    }
    if (k == "wait") {
        auto objs = t.size() >= 2 && !std::isdigit(static_cast<unsigned char>(t[1][0])) ? resolve(t[1])
                                                                                          : std::vector<Object*>{};
        if (!objs.empty()) {
            // `wait <object>` (cs2.exe 0x77a060: wait state -4, checked at 0x756390): until that
            // object is done; a SOUND (type 9, 0x74c280) while its playing flag is up, which the
            // script's update clears once the sound task says the sound is over (0x6285b0).
            if (objs[0]->type == "SOUND") {
                if (!objs[0]->soundOn) return true;
                waitSound_ = objs[0]->name;
                state_ = State::WaitSound;
                return false;
            }
            // Another object: until it no longer moves (its motions: 0x601be0 on it).
            waitSound_ = t[1];
            state_ = State::WaitObject;
            return false;
        }
        if (t.size() >= 2) {
            waitUntil_ = frame_ + static_cast<uint64_t>(std::max(0, num(t[1])));
            state_ = State::WaitFrames;
        } else {
            state_ = State::WaitAnimation;
        }
        return false;
    }
    if (k == "baseimg") {
        // `baseimg [object]` (cs2.exe 0x77a850): a message on the script's queue (0x779d60) that the
        // task's update (0x629840) sends as 0x128 to the screen it draws on (its target, +0x198):
        // an image's picture (object types 1, 2, 6, 11, 12, 22), a plane's with its place (type 4:
        // messages 0x122 / 0x11d first), or none when bare. The screen keeps it after the script
        // ends: Cyanotype's title says `baseimg img_white` before handing over to config.fes, whose
        // `rdraw 20` then starts from white, and config says `baseimg cmask` before going back.
        std::vector<FesQuad> base;
        if (t.size() >= 2) {
            if (Object* o = find(str(t[1]))) {
                if (o->container()) {
                    base = drawContainer(o->name, frame_);  // the plane as it shows now, where it is
                } else if (o->colorFill) {
                    FesQuad q;  // a colour image's picture, from the screen's corner
                    q.color = o->color;
                    const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
                    for (int n = 0; n < 4; ++n) q.x[n] = xs[n] * static_cast<float>(o->width), q.y[n] = ys[n] * static_cast<float>(o->height);
                    base.push_back(std::move(q));
                } else {
                    unhandled("baseimg " + o->type + " with a file");
                    return true;
                }
            }
        }
        if (host_) host_->baseImage(std::move(base));
        return true;
    }
    if (k == "rwipe" || k == "rdraw") {
        // `rwipe [frames] [mask] [x] [y]` / `rdraw [frames] [x] [y]` (cs2.exe 0x780480 / 0x780280,
        // the FES table's entries): a render object on top that wipes (through the mask) or fades
        // from the screen as last drawn to the current one; ignored while one is running. Frames
        // default to 30 (`rdraw` at least 1). The x / y numbers only place it on a 3D plane.
        if (wiping(frame_)) return true;
        wipe_ = ScreenWipe{};
        wipe_.start = frame_;
        wipe_.frames = static_cast<uint32_t>(std::max(1, t.size() >= 2 ? num(t[1]) : 30));
        wipe_.mask = k == "rwipe" && t.size() >= 3 ? str(t[2]) : std::string();
        return true;
    }
    if (k == "rettitle") {
        if (host_) host_->retTitle();
        exited_ = true;
        exitCode_ = 0;
        return false;
    }
    if (k == "react") {
        // cs2.exe 0x77b870 -> message 12 (0x62a1ec): the focused object gets its FOCUS event again.
        if (hovered_) fireEvent(*hovered_, "FOCUS");
        return true;
    }
    if (k == "resetactive") {
        // 0x77e7d0 -> message 11 (0x62a10a): the focused object gets UNFOCUS and the task forgets
        // it; the input object is told to start over (command 0x91), so the button under the
        // pointer is focused afresh at the next update.
        if (hovered_) fireEvent(*std::exchange(hovered_, nullptr), "UNFOCUS");
        rehover_ = true;
        return true;
    }
    if (k == "decode" || k == "force_redraw" || k == "restoremove" || k == "lockactive" || k == "config_open" ||
        k == "btnsimulate") {
        // Nothing to do here (cs2.exe, the FE task's messages in 0x629840):
        // - `decode` (message 23) syncs and decodes every object at once; `force_redraw` (+0x824)
        //   flushes the drawing after an engine call-back (0x62cd76). Both happen every frame here.
        // - `restoremove` (message 10) moves the mouse cursor back, only with the auto-move
        //   setting (desktop pointer warping).
        // - `lockactive <n>` (message 22, +0x4a0) keeps the focused object after the pointer
        //   leaves all buttons (message 0x40), for keyboard navigation; only Fruit's window
        //   scripts use it, and the window is drawn natively.
        // - `config_open` (message 8) posts 0x465 to the game window: the PC's own settings
        //   (the window's Settings menu opens cs2confx.dll, 0x530230 -> 0x522e60).
        // - `btnsimulate <a> <b>` (message 29, input command 0x9a) presses a button for the
        //   developers' log simulation (Nekopara's logsim.fes).
        return true;
    }
    if (k == "deletebgbutton") {  // cs2.exe 0x77e820 -> message 17 (0x62a386)
        lowButton_ = false;
        return true;
    }
    if (k == "enablebgbutton") {  // 0x7820a0 -> message 31 (0x62a776, engine command 0x5c)
        lowButtonEnabled_ = t.size() < 2 || num(t[1]) != 0;
        return true;
    }
    if (k == "basebtnpri") {  // 0x77be20 -> message 14 (0x62a251, command 0x80)
        int32_t n = t.size() >= 2 ? num(t[1]) : -1;
        if (n == -1) lowButtonPri_.reset();
        else lowButtonPri_ = n;
        return true;
    }
    if (k == "xchgcursor") {
        // The FE task's message (Nekopara cs2.exe 0x62a69b) posts window message 0x46d with the name;
        // its handler (0x5316a0) takes that cursor.xml entry, `default` without a name.
        if (host_) host_->changeCursor(t.size() >= 2 ? str(t[1]) : std::string("default"));
        return true;
    }
    if (k == "sendbtn") {  // 0x780210: the script's +0x820, default 1
        sendBtn_ = t.size() >= 2 ? num(t[1]) : 1;
        return true;
    }
    if (k == "keyskip") {
        // cs2.exe 0x62c140 (Amakano 1): the words after it in any order: `on` / `off`, `ctrl`, `cont`,
        // `force <expr>` (a click and a held skip at once while true, `_CLICK_L_` -1), anything else
        // an expression for on / off. Each `keyskip` sets all three: on unless said, ctrl and cont
        // off unless named.
        bool on = true, ctrl = false, cont = false;
        for (size_t i = 1; i < t.size(); ++i) {
            std::string w = t[i];
            for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (w == "on") on = true;
            else if (w == "off") on = false;
            else if (w == "ctrl") ctrl = true;
            else if (w == "cont") cont = true;
            else if (w == "force") {
                const bool force = i + 1 < t.size() && num(t[++i]) != 0;
                if (force) {
                    skipClick_ = skipHeld_ = true;
                    skipClickFromPress_ = false;
                    clickL_ = -1;
                    clickFromHold_ = true;
                    clickAt_ = frame_;
                }
            } else {
                on = num(t[i]) != 0;
            }
        }
        keyskipOn_ = on, keyskipCtrl_ = ctrl, keyskipCont_ = cont;
        return true;
    }
    if (k == "execfes" && t.size() >= 2) {
        // `execfes <script> [n...]` (cs2.exe 0x77a620): up to 16 numbers after the name, each an
        // expression, go with the request to the new script, which starts with them in its locals
        // \0.. (Amakano 2's flow script passes 1 to config, whose start then plays no voice of its own;
        // the title's goes on).
        child_ = load(assets_, globals_, t[1], frame_, config_, host_);
        if (child_) {
            child_->log = log;
            for (size_t i = 2; i < t.size() && i < 18; ++i) child_->setLocal(static_cast<int32_t>(i - 2), num(t[i]));
        } else {
            unhandled("missing script " + t[1]);
        }
        launched_ = Launched::Fes;
        return true;
    }
    if (k == "endfes") {
        child_.reset();
        return true;
    }
    if (k == "execkcs" && t.size() >= 2) {
        if (log && std::getenv("NYAN_FES_TRACE")) log("fes " + name_ + ": execkcs " + t[1]);
        if (host_) host_->execKcs(t[1]);
        kcsProgram_ = t[1];
        launched_ = Launched::Kcs;
        kcsRunning_ = true;
        return true;
    }
    if (k == "endkcs") {
        // Ends the kcs program this script started with `execkcs`, if any (cs2.exe: request 5 of
        // the script's queue, 0x7155f0, carried out at 0x5b4cc0 on the script's own kcs slot).
        // A screen that started none (Amakano 2's config pages and message window, which say
        // `endkcs` before opening a dialog) leaves the story alone.
        if (std::exchange(kcsRunning_, false) && host_) host_->endKcs(kcsProgram_);
        return true;
    }
    if (k == "sysvoice" && t.size() >= 2) {
        // `sysvoice <var> [c...]` (cs2.exe 0x716cf0): one of the listed characters (all 48 when
        // none are) whose system voice is on (config `sysplay <c>`), at random; -1 for none.
        std::vector<int32_t> pick;
        auto on = [&](int32_t c) {
            if (!config_ || c < 0 || c >= 48) return false;
            auto it = config_->find("sysplay " + std::to_string(c));
            return it != config_->end() && it->second != 0;
        };
        if (t.size() > 2) {
            for (size_t i = 2; i < t.size(); ++i) if (on(num(t[i]))) pick.push_back(num(t[i]));
        } else {
            for (int32_t c = 0; c < 48; ++c) if (on(c)) pick.push_back(c);
        }
        globals_.set(num(t[1]), pick.empty() ? -1 : pick[static_cast<size_t>(randomBetween(0, static_cast<int32_t>(pick.size()) - 1))]);
        return true;
    }
    if (k == "sysvoiceex" && t.size() >= 2) {
        // `sysvoiceex <var> [part path...]` (0x71fb00): soundconf.xml's system voices.
        std::vector<std::vector<int32_t>> paths;
        for (size_t i = 2; i < t.size(); ++i) {
            std::vector<int32_t> path;
            std::string part;
            for (char ch : t[i] + ",") {
                if (ch != ',') { part += ch; continue; }
                if (!part.empty()) path.push_back(num(part));
                part.clear();
            }
            paths.push_back(std::move(path));
        }
        auto v = host_ ? host_->pickSystemVoice(paths) : std::nullopt;
        globals_.set(num(t[1]), v.value_or(-1));
        return true;
    }
    if ((k == "getsndconfex" || k == "setsndconfex" || k == "setsndconfexnonotify") && t.size() >= 4) {
        // `getsndconfex <var> <field>[_id] <name | ids...>` (0x71e100), `setsndconfex <value> ...`
        // (0x71eb00: ...nonotify, then the settings applied to playing sound).
        std::string field = t[2];
        std::string name;
        std::vector<int32_t> ids;
        if (field.ends_with("_id")) {
            field.resize(field.size() - 3);
            for (size_t i = 3; i < t.size(); ++i) ids.push_back(num(t[i]));
        } else {
            name = t[3];
        }
        if (field == "reset" && k != "getsndconfex") {
            // `setsndconfex <v> reset <part>` (cs2.exe 0x783a20): the part's settings as loaded.
            if (!host_ || !host_->resetSoundConf(name, ids, k == "setsndconfex")) unhandled(k + " reset without soundconf.xml");
            return true;
        }
        static const char* kFields[] = {"volume", "play", "sysplay", "mute", "voicebgm", "voicemute"};
        if (std::find(std::begin(kFields), std::end(kFields), field) == std::end(kFields) ||
            (field == "sysplay" && name == "master")) {
            unhandled(k + " " + t[2]);
            return true;
        }
        if (k == "getsndconfex") {
            auto v = host_ ? host_->soundConf(field, name, ids) : std::nullopt;
            if (!v) unhandled("getsndconfex without soundconf.xml");
            globals_.set(num(t[1]), v.value_or(field == "volume" ? 100 : field == "play" || field == "sysplay" ? 1 : 0));
        } else if (!host_ || !host_->setSoundConf(field, name, ids, num(t[1]), k == "setsndconfex")) {
            unhandled(k + " without soundconf.xml");
        }
        return true;
    }
    if (k == "m2_setconf" && t.size() == 2 && t[1] == "reset") {
        // `m2_setconf reset` (cs2.exe 0x786830 -> 0x5816b0): every character's motion, eff_dif,
        // eff_iner, eff_blink and eff_lip to 1 and fade to 0, interleave to 0, every group's iner_lv
        // to 100 and iner_enable to 1: the values the settings start with (0x580320).
        if (config_) {
            for (const char* key : {"motion", "eff_dif", "eff_iner", "eff_blink", "eff_lip"}) (*config_)[std::string("m2 ") + key] = 1;
            (*config_)["m2 fade"] = 0;
            (*config_)["m2 interleave"] = 0;
            for (auto& [key, v] : *config_) {
                if (key.starts_with("m2 iner_lv ")) v = 100;
                else if (key.starts_with("m2 iner_enable ")) v = 1;
            }
        }
        if (host_) host_->configSet("m2 reset");
        return true;
    }
    if ((k == "m2_getconf" || k == "m2_setconf") && t.size() >= 3) {
        // E-mote settings (Nekopara cs2.exe 0x786350 / 0x786830, Amakano 0x721c40 / 0x722180):
        // `m2_getconf <key> <var>`, `m2_setconf <key> <value> [chara]` for eff_dif, eff_iner, fade,
        // iner_lv, iner_enable, interleave, motion, eff_blink, eff_lip, applied to every character
        // (the config screen's one switch sets eff_blink, eff_lip and motion). Kept as config
        // entries "m2 <key>"; the values they start with (Nekopara 0x580320, Amakano's missing
        // m2setting.xml 0x4e0a90): fade 0, iner_lv 100, the rest 1, except interleave, 0 in Nekopara
        // and 1 in Amakano (nothing reads it). Unlike getconf, m2_getconf's <var> is the script's own variable
        // \<var> (0-511: 0x728c90, Amakano 0x6c1870), as the config scripts read it (`m2_getconf
        // iner_lv <n> <group>`, then `\<n>`). iner_lv / iner_enable name a group: kept per group;
        // the level is clamped to 0-200 (0x581240).
        std::string key = "m2 " + t[1];
        if ((t[1] == "iner_lv" || t[1] == "iner_enable") && t.size() >= 4) key += " " + t[3];
        const int32_t def = t[1] == "fade" ? 0 : t[1] == "iner_lv" ? 100 : 1;
        if (k == "m2_setconf") {
            int32_t v = num(t[2]);
            if (t[1] == "iner_lv") v = std::clamp(v, 0, 200);
            if (config_) (*config_)[key] = v;
            if (host_) host_->configSet(key);
            return true;
        }
        int32_t v = def;
        if (config_) {
            auto it = config_->find(key);
            if (it != config_->end()) v = it->second;
        }
        if (int32_t i = num(t[2]); i >= 0 && i < 512) locals_.set(i, v);
        return true;
    }
    if ((k == "getconf" || k == "setconf") && t.size() == 3 && t[2] == "font") {
        // The config's font (cs2.exe getconf 0x68c260 / setconf 0x68d240, the running settings'
        // +0xfcb4, 64 characters): a face name in a string variable.
        if (k == "getconf") globals_.setStr(num(t[1]), host_ ? host_->configFont() : std::string());
        else if (host_) host_->setConfigFont(globals_.str(num(t[1])).substr(0, 63));
        return true;
    }
    if (k == "fontcom" && t.size() >= 2) {
        // cs2.exe 0x68fb40: `init [charset]` makes the system font list (0x80 Shift-JIS when not
        // given; see Game::fontList), `count <var>`, `get <n> <string var>` ("" past the end),
        // `search <var> <name>` (its place, exact, -1 if absent), `default <string var>` (the
        // startup face), `add <file>` (AddFontResourceA). count / get / search need the list.
        const std::string& op = t[1];
        auto at = [&](size_t i) { return i < t.size() ? num(t[i]) : 0; };
        if (op == "init") {
            fontList_ = host_ ? host_->fontList(t.size() > 2 ? at(2) : 0x80) : std::vector<std::string>{};
            fontListMade_ = true;
            if (log && std::getenv("NYAN_FES_TRACE")) {
                std::string names;
                for (const auto& f : fontList_) names += (names.empty() ? "" : ", ") + f;
                log("fes " + name_ + ": fontcom init: " + std::to_string(fontList_.size()) + " fonts: " + names);
            }
        } else if (op == "count" && t.size() > 2 && fontListMade_) {
            globals_.set(at(2), static_cast<int32_t>(fontList_.size()));
        } else if (op == "get" && t.size() > 3 && fontListMade_) {
            const int32_t n = at(2);
            globals_.setStr(at(3), n >= 0 && n < static_cast<int32_t>(fontList_.size()) ? fontList_[static_cast<size_t>(n)] : std::string());
        } else if (op == "search" && t.size() > 3 && fontListMade_) {
            std::string name = str(t[3]);
            if (!isValidUtf8(name)) name = sjisToUtf8(name);
            auto it = std::find(fontList_.begin(), fontList_.end(), name);
            globals_.set(at(2), it == fontList_.end() ? -1 : static_cast<int32_t>(it - fontList_.begin()));
        } else if (op == "default" && t.size() > 2) {
            globals_.setStr(at(2), host_ ? host_->defaultFont() : std::string());
        } else if (op == "add" && t.size() > 2) {
            if (host_) host_->addFont(str(t[2]));
        }
        return true;
    }
    if ((k == "getconf" || k == "setconf") && t.size() >= 3) {
        // `getconf <var> <key> [sub...]`, `setconf <value> <key> [sub...]`, e.g. `getconf 450 play 3 7`.
        std::string key = t[2];
        for (size_t i = 3; i < t.size(); ++i) key += " " + std::to_string(num(t[i]));
        if (t[2] == "fshadow") {
            // `fshadow <n>`: the text edge's switches, n at most 2, which is the next setting, the
            // bold flag (cs2.exe 0x77c800 / 0x77d770 index +0xf8f8 by 4 n; see Game).
            const int32_t n = t.size() > 3 ? std::max(0, num(t[3])) : 0;
            key = n >= 2 ? "fontweight" : "fshadow " + std::to_string(n);
        }
        if (k == "setconf") {
            if (config_) (*config_)[key] = num(t[1]);
            if (host_) host_->configSet(key);
            return true;
        }
        // `play <group> [n]` switches (BGM, SE, voices, each character's voice) are on until set.
        int32_t v = key.starts_with("play ") ? 1 : 0;
        if (config_) {
            auto it = config_->find(key);
            if (it != config_->end()) v = it->second;
            else unhandled("config " + key);
        }
        globals_.set(num(t[1]), v);
        return true;
    }
    if (k == "send" && t.size() >= 2) {
        Send sd{static_cast<int64_t>(std::strtoll(t[1].c_str(), nullptr, 0)), t.size() > 2 ? num(t[2]) : 0,
                t.size() > 3 ? num(t[3]) : 0};
        if (!host_ || !host_->request(*this, sd.code, sd.a, sd.b)) sends_.push_back(sd);
        return true;
    }
    // CG gallery (amakanoPE.exe 0x62c8b0-0x62d020; cs2.exe 0x77ab00-0x77b170). `cgreg_init <global
    // file> <offset> <list>` gives the script a table of its own (made anew each time): the records
    // read back from the global file at the offset, where sscript writes the engine's own (native
    // 424 with 0x50 and m[0x800055cc]), so here the engine's records; and the groups of the list it
    // names, read as an archive entry (0x4dc990 mode 3; a missing list leaves the table without
    // groups). Every gallery names its list: Amakano 1's cglist_n.dat / cglist_i.dat (its
    // cglist.dat is another game's), REBELLIONS's list_cg.dat (it has no cglist.dat), LSG's
    // cglist2.dat, Nekopara's cglist_anm.dat. The commands ask the table and answer 0 without one:
    // `cgreg_t <var> <group>` 1 if an entry of the group is recorded; `cgregnum` / `cgregmax <group>
    // <var>` its entries recorded / in all; `cgregtbl <name> <var>` the group's number (-1 if none,
    // 0 with no list); `cgreg <var> <name>` 1 if that entry is recorded.
    if (k == "cgreg_init") {
        cgTable_.emplace();
        if (t.size() >= 4) {
            if (auto data = readFile(str(t[3]))) {
                try {
                    *cgTable_ = ScenePlayer::parseCgList(*data);
                } catch (const Error&) {
                }
            }
        }
        return true;
    }
    if ((k == "cgreg_t" && t.size() >= 3) || ((k == "cgregnum" || k == "cgregmax") && t.size() >= 3)) {
        int32_t seen = 0, total = 0;
        if (k == "cgreg_t") {
            bool ok = cgTable_ && cgTableCount(cgTableGroup(t[2]), seen, total);
            globals_.set(num(t[1]), ok && seen > 0 ? 1 : 0);
        } else {
            bool ok = cgTable_ && cgTableCount(cgTableGroup(t[1]), seen, total);
            globals_.set(num(t[2]), ok ? (k == "cgregnum" ? seen : total) : 0);
        }
        return true;
    }
    if (k == "cgregtbl" && t.size() >= 3) {
        int32_t g = 0;
        if (cgTable_ && !cgTable_->empty()) g = cgTableIndex(str(t[1]));
        globals_.set(num(t[2]), g);
        return true;
    }
    if (k == "cgreg" && t.size() >= 3) {
        globals_.set(num(t[1]), cgTable_ && host_ && host_->cgEntrySeen(str(t[2])) ? 1 : 0);
        return true;
    }
    if (k == "debug") {
        // `debug <n>` (0x71a000): the number written to the engine's debug log ("debug = %d").
        const int32_t v = t.size() >= 2 ? num(t[1]) : 0;
        if (log && t.size() >= 2 && std::getenv("NYAN_FES_TRACE")) log("fes " + name_ + ": debug = " + std::to_string(v));
        return true;
    }
    if (k == "getexvar" && t.size() >= 3) {
        auto v = host_ ? host_->exVar(t[1]) : std::nullopt;
        if (!v) unhandled("getexvar " + t[1]);
        globals_.set(num(t[2]), v.value_or(0));
        return true;
    }
    if (SaveStore* saves = host_ ? host_->saveStore() : nullptr) {
        // Save slots. `newsave <var> <from> <to>`: newest existing slot in range (-1 if none).
        if (k == "newsave" && t.size() >= 4) {
            globals_.set(num(t[1]), saves->newest(num(t[2]), num(t[3])));
            return true;
        }
        if (k == "saveexist" && t.size() >= 3) {
            globals_.set(num(t[2]), saves->exists(num(t[1])) ? 1 : 0);
            return true;
        }
        if (k == "savedelete" && t.size() >= 2) {
            saves->remove(num(t[1]));
            return true;
        }
        if (k == "savecopy" && t.size() >= 3) {
            // `savecopy <from> <to>` (cs2.exe 0x77ecf0 -> 0x78c250): save<from>.dat over save<to>.dat,
            // if it exists.
            saves->copy(num(t[1]), num(t[2]));
            return true;
        }
        if (k == "saveexchg" && t.size() >= 3) {
            saves->exchange(num(t[1]), num(t[2]));
            return true;
        }
        if (k == "saveapend_str" && t.size() >= 4) {
            // Script strings are Shift-JIS with `\ ` and `_` standing for spaces; saves keep UTF-8 text.
            std::string text;
            std::string raw = str(rest(3));
            for (size_t i = 0; i < raw.size(); ++i) {
                auto b = static_cast<unsigned char>(raw[i]);
                if (((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) && i + 1 < raw.size()) {
                    text += raw.substr(i++, 2);  // double-byte character: its second byte may be '\\' or '_'
                    continue;
                }
                if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == ' ') continue;
                text += raw[i] == '_' ? ' ' : raw[i];
            }
            saves->appendString(num(t[1]), static_cast<uint32_t>(num(t[2])), sjisToUtf8(text));
            return true;
        }
        if (k == "gettime") {
            // `gettime <year> <month> <day> <weekday> <hour> <minute> <second>` (cs2.exe 0x782870:
            // GetLocalTime): each variable given gets its field.
            std::time_t now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            const int32_t fields[7] = {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_wday, tm.tm_hour, tm.tm_min, tm.tm_sec};
            for (size_t i = 0; i < 7 && i + 1 < t.size(); ++i) globals_.set(num(t[i + 1]), fields[i]);
            return true;
        }
        if (k == "saveget_str" && t.size() >= 4) {
            std::string v;
            if (const SaveFile* f = saves->get(num(t[1]))) {
                auto it = f->strings.find(static_cast<uint32_t>(num(t[2])));
                if (it != f->strings.end()) v = it->second;
            }
            globals_.setStr(num(t[3]), v);
            return true;
        }
        if (k == "datasave" && t.size() >= 3) {
            if (!host_->dataSave(num(t[1]), num(t[2]))) unhandled("datasave without a game to save");
            return true;
        }
    }
    if (k == "rand" && t.size() >= 4) {
        // `rand <var> <min> <max>`: a random whole number in [min, max] (Labyrinth picks one of ten ticker
        // lines this way).
        globals_.set(num(t[1]), randomBetween(num(t[2]), num(t[3])));
        return true;
    }
    if (k.starts_with("dic_")) {
        // The glossary queries (cs2.exe 0x780af0..0x781170), all `dic_<x> <handle> ...` with the
        // handle the engine put in adv.xml's <var> (Cyanotype's $899); without one they do nothing.
        Dictionary* d = host_ ? host_->dictionary() : nullptr;
        auto arg = [&](size_t i) { return i < t.size() ? num(t[i]) : 0; };
        auto key = [&](size_t i) { return i < t.size() ? str(t[i]) : std::string(); };
        if (!d || arg(1) == 0) return true;
        if (k == "dic_find" && t.size() >= 7) {
            // `dic_find <h> <found var> <key str> <n> <from> <to>`: the n-th entry whose key starts
            // with a character from <from> to <to> (0x82b0a0).
            std::string found;
            bool ok = d->find(key(5), key(6), arg(4), found);
            globals_.setStr(arg(3), ok ? found : std::string());
            globals_.set(arg(2), ok ? 1 : 0);
        } else if (k == "dic_appear" && t.size() >= 4) {
            globals_.set(arg(2), static_cast<int32_t>(d->appear(key(3))));
        } else if (k == "dic_max_appear" && t.size() >= 4) {
            globals_.set(arg(2), static_cast<int32_t>(d->maxAppear(key(3))));
        } else if (k == "dic_title" && t.size() >= 5) {
            std::string a, b;
            d->title(key(4), a, b);
            globals_.setStr(arg(2), a);
            globals_.setStr(arg(3), b);
        } else if (k == "dic_ind" && t.size() >= 4) {
            globals_.setStr(arg(2), d->ind(key(3)));
        } else if (k == "dic_update" && t.size() >= 5) {
            int32_t fresh = 0, updated = 0;
            d->update(key(4), fresh, updated);
            globals_.set(arg(2), fresh);
            globals_.set(arg(3), updated);
        } else if (k == "dic_update_clear") {
            d->updateClear(t.size() >= 3 ? key(2) : std::string());
        } else if (k == "dic_id" && t.size() >= 4) {
            globals_.set(arg(2), d->position(key(3)));
        } else {
            unhandled(k);
        }
        return true;
    }
    if (k == "fileexist" && t.size() >= 3) {
        // `fileexist <path> <var>` (0x77c060 -> 0x5d4960): 1 if the engine can open the file,
        // `archive.int/entry` naming an entry of that archive (Nekopara's and LSG's flow scripts ask
        // whether the adult scenes are installed this way), else 0.
        const bool found = fileExists(str(t[1]));
        if (log && std::getenv("NYAN_FES_TRACE")) log("fes " + name_ + ": fileexist " + str(t[1]) + (found ? " yes" : " no"));
        globals_.set(num(t[2]), found ? 1 : 0);
        return true;
    }
    if (k == "replacespace" && t.size() >= 3) {
        // `replacespace <string var> <text>` (0x785660): every single-byte space becomes <text>
        // (`\_`, so the string stays one token for `saveapend_str`); double-byte characters stay.
        const std::string& src = globals_.str(num(t[1]));
        std::string out;
        for (size_t i = 0; i < src.size(); ++i) {
            auto b = static_cast<unsigned char>(src[i]);
            if (((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) && i + 1 < src.size()) {
                out += src.substr(i++, 2);
            } else {
                out += b == ' ' ? t[2] : std::string(1, src[i]);
            }
        }
        globals_.setStr(num(t[1]), out);
        return true;
    }
    if (k == "wndmenu") return true;  // 0x77fad0: message 0x1b, the window's full-screen / window menu
    if (k == "automove" && t.size() >= 2) {
        // `automove <object>`: the engine moves the mouse cursor onto the object (to its `keypos`
        // point), so it takes the focus and its FOCUS handler runs: Himawari's choice screen puts its
        // arrow on the first choice this way. The real pointer takes over at its next move.
        auto objs = resolve(t[1]);
        if (!objs.empty() && objs.front() != hovered_) {
            if (hovered_) fireEvent(*std::exchange(hovered_, nullptr), "UNFOCUS");
            hovered_ = objs.front();
            fireEvent(*objs.front(), "FOCUS");
        }
        return true;
    }
    if (k == "bitcount" && t.size() >= 3) {
        // `bitcount <var> <value>` (0x781300): the number of bits set.
        globals_.set(num(t[1]), static_cast<int32_t>(std::popcount(static_cast<uint32_t>(num(t[2])))));
        return true;
    }
    if (k == "strvar" && t.size() >= 4 && t[1] == "get") {
        // `strvar get <dst> <src>`: copy string variable <src> (an expression) into <dst>.
        globals_.setStr(num(t[2]), globals_.str(num(t[3])));
        return true;
    }
    if (k == "strvar" && t.size() >= 2) {
        // `strvar <dst> <format> [n...]` (cs2.exe 0x77c260): the format (its `$strN` expanded, so
        // `strvar 410 $str303` copies) printed with up to four whole-number expressions (a file name
        // with `%03d` in it, say). Numbers the format has no place for are dropped; no format is the
        // empty string.
        int32_t args[4]{};
        size_t n = t.size() > 3 ? std::min<size_t>(4, t.size() - 3) : 0;
        for (size_t i = 0; i < n; ++i) args[i] = num(t[3 + i]);
        globals_.setStr(num(t[1]), t.size() >= 3 ? formatInts(str(t[2]), args, n) : std::string());
        return true;
    }
    if (k == "clearstrvar" && t.size() >= 2) {
        // `clearstrvar <from> [<to>]` (Happiness 2 cs2.exe 0x6963b0): the string variables from..to
        // (only <from> without <to>) are taken out, so they read empty.
        const int32_t from = num(t[1]), to = t.size() >= 3 ? num(t[2]) : from;
        for (int32_t i = from; i <= to && i - from < 100000; ++i) globals_.setStr(i, std::string());
        return true;
    }
    if (k == "strcmp" && t.size() >= 3) {
        // `strcmp <var> <a> [b]`: 0 when the texts match, as C's strcmp (`if ($<var>==0)`).
        // Nyanroid keeps strings in Shift-JIS where it can; the UTF-8 engines (Happiness 2, Nekopara,
        // Amakano 2, Cyanotype, Yukikoi, LSG) hold them as UTF-8, scene lines included (Happiness 2
        // cs2.exe 0x639740 converts every command line, 0x5ae5c0). The same characters in the two
        // encodings are the same text: a script's UTF-8 names match a scene's Shift-JIS ones.
        const std::string a = str(t[2]), b = t.size() > 3 ? str(t[3]) : std::string();
        int c = a.compare(b);
        if (c != 0) {
            auto chars = [](const std::string& v) { return isValidUtf8(v) ? v : sjisToUtf8(v); };
            if (chars(a) == chars(b)) c = 0;
        }
        globals_.set(num(t[1]), c < 0 ? -1 : c > 0 ? 1 : 0);
        return true;
    }
    if (k == "dbgstr") return true;  // a debug message
    if ((k == "input_text" && t.size() >= 4) || (k == "messagebox" && t.size() >= 4)) {
        // `input_text <result> <prompt str> <text str> [max] [cancel]` (cs2.exe 0x77f350, request
        // 0x18 -> 0x62a4d5, dialog 0x62b5a0): the prompt as the title, the text in an edit box of
        // <max> characters (128 unless given), its Cancel button enabled unless <cancel> is 0
        // (Amakano 2's family name); OK sets <result> 1 and the text, Cancel <result> 0.
        // `messagebox <result> <type> <title> <text...>` (0x77f800, request 0x19 -> 0x62a594): type 0
        // OK, 1 OK / Cancel, 2 Yes / No (MessageBox 0, 1, 4); <result> 1 on OK / Yes, else 0, not
        // kept when it is -1. Both are Windows dialogs on PC; the platform shows its own and answers
        // `answerDialog`.
        static uint32_t serial = 0;
        auto utf8 = [](const std::string& v) { return isValidUtf8(v) ? v : sjisToUtf8(v); };
        Dialog d;
        d.serial = ++serial;
        d.input = k == "input_text";
        dialogResultVar_ = num(t[1]);
        if (d.input) {
            d.title = utf8(globals_.str(num(t[2])));
            dialogValueVar_ = num(t[3]);
            d.value = utf8(globals_.str(dialogValueVar_));
            d.maxChars = t.size() > 4 ? num(t[4]) : 128;
            d.cancel = t.size() > 5 ? num(t[5]) != 0 : true;
        } else {
            d.type = num(t[2]);
            d.title = utf8(str(t[3]));
            for (size_t i = 4; i < t.size(); ++i) d.message += (i > 4 ? " " : "") + utf8(str(t[i]));
        }
        dialog_ = std::move(d);
        state_ = State::Dialog;
        return false;
    }
    if (k[0] == '$' || k[0] == '\\') {
        try {
            // In event handlers `@` is the triggering element's index.
            std::optional<int32_t> at;
            if (current_) at = current_->index;
            if (!expr_.assign(st.text, at)) unhandled("statement " + k);
        } catch (const Error& e) {
            scriptError(e.what());
        }
        return true;
    }
    if (k == "@" && t.size() >= 2) {
        if (current_) method(*current_, t[1], {t.begin() + 2, t.end()});
        return true;
    }
    if (!std::isalpha(static_cast<unsigned char>(k[0])) && k[0] != '_' && k[0] != '@') {
        // A head that cannot name an object or a command (an operator, a number, a bracket): the
        // router (cs2.exe 0x7690d0; its first word is split at spaces, 0x767040) finds neither and
        // hands the line to the calculator (0x728ac0), which runs the last tree its pass makes
        // (lastExpressionTree). One of Yukikoi's extras scripts splits an `if` over two lines, the
        // second starting `!= 0)`: the engine runs the assignment after it, which unlocks a thumbnail.
        try {
            std::optional<int32_t> at;
            if (current_) at = current_->index;
            const std::string_view part = lastExpressionTree(st.text);
            if (!part.empty()) expr_.assign(part, at);
        } catch (const Error& e) {
            scriptError(e.what());
        }
        return true;
    }
    auto targets = resolve(k);
    if (!targets.empty() && t.size() >= 2) {
        std::vector<std::string> args(t.begin() + 2, t.end());
        if (t[1] == "ismot" && !args.empty()) {
            bool any = false;
            for (Object* o : targets) any = any || moving(*o, frame_ ? frame_ - 1 : 0);
            globals_.set(num(args[0]), any ? 1 : 0);
            return true;
        }
        for (Object* o : targets) method(*o, t[1], args);
        return true;
    }
    // `bgm stop` / `se stop` in a script without such an object: the engine's own channels.
    if ((k == "bgm" || k == "se") && t.size() >= 2 && t[1] == "stop") {
        uint32_t fade = t.size() >= 3 ? static_cast<uint32_t>(std::max(0, num(t[2]))) : 0;
        media_.push_back({k == "bgm" ? MediaCommand::Kind::StopBgm : MediaCommand::Kind::StopSe, -1, "", false, fade});
        return true;
    }
    // Engine services (config, saves, other scripts, KCS) are not modelled yet.
    unhandled(k);
    return true;
}

void FesInstance::method(Object& o, const std::string& m, const std::vector<std::string>& args) {
    auto arg = [&](size_t i, int32_t def = 0) { return i < args.size() ? num(args[i], def) : def; };
    motionAt_ = frame_ + static_cast<uint64_t>(std::max(0, o.delay));
    // What finished motions left is the object's own now (foldMotions): a new motion starts there.
    foldMotions(o, frame_, false);
    if (m == "delay") {
        o.delay = std::max(0, arg(0));
        return;
    }
    if (m == "ismot" && !args.empty()) {
        // `<object> ismot <var>` (Amakano 1's cs2.exe 0x629bc0; Happiness 2 0x6865e0): 1 while the
        // object moves (0x601be0, as `wait <object>` asks it), else 0. A name without an index is
        // every object of the array, their answers ORed (statement() asks them all).
        globals_.set(arg(0), moving(o, frame_ ? frame_ - 1 : 0) ? 1 : 0);
        return;
    }

    if (auto sc = script_.subcommands.find(m); sc != script_.subcommands.end()) {
        // Expand the subcommand's argument mapping with the call's arguments or the defaults.
        std::vector<std::string> prim;
        for (const auto& map : sc->second.mapping) {
            if (map.size() >= 3 && map.front() == '[' && map.back() == ']') {
                size_t n = static_cast<size_t>(std::atoi(map.c_str() + 1));
                prim.push_back(n < args.size() ? args[n] : n < sc->second.defaults.size() ? sc->second.defaults[n] : "0");
            } else {
                prim.push_back(map);
            }
        }
        // A SOUND object's opacity is its volume (`blend <v>`): the script's `fade` motion
        // (`oneshot fade motFade p[<frames>] p[<from>] p[<to>]`) fades the sound. Amakano 2's flow script fades the
        // title music out this way before starting the game.
        if (o.type == "SOUND" && (sc->second.primitive == "motFade" || sc->second.primitive == "pAlpha")) {
            using K = MediaCommand::Kind;
            bool bgm = o.group == "bgm" || (o.group.empty() && o.stype == 0);
            bool fade = sc->second.primitive == "motFade";
            MediaCommand c{bgm ? K::BgmVolume : K::SeVolume, 7};
            if (fade) {
                c.fadeFrames = static_cast<uint32_t>(std::max(0, num(prim.size() > 0 ? prim[0] : "0")));
                c.fromVolume = std::clamp(num(prim.size() > 1 ? prim[1] : "0"), 0, 100);
                c.volume = std::clamp(num(prim.size() > 2 ? prim[2] : "0"), 0, 100);
            } else {
                c.volume = std::clamp(num(prim.size() > 0 ? prim[0] : "0"), 0, 100);
            }
            media_.push_back(c);
            return;
        }
        primitive(o, sc->second.primitive, prim);
        return;
    }
    if ((o.type == "BGTABLE2" || o.type == "CHTABLE3") && tableMethod(o, m, args)) return;
    if (m == "arc" || m == "mcarc") {
        // `arc <frames> <deg0> <deg1> <radiusX> <radiusY>`: move along a sine arc, holding the end.
        // Defaults 30 frames, the rest 0 (cs2.exe 0x623f40 / mcarc 0x6241d0, motArc).
        Motion mo;
        mo.kind = Motion::Kind::Arc;
        mo.loop = m == "mcarc";
        mo.start = motionAt_;
        mo.period = static_cast<uint32_t>(std::max(1, arg(0, 30)));
        mo.a[0] = static_cast<float>(arg(1));
        mo.a[1] = static_cast<float>(arg(2, 0));
        mo.a[2] = static_cast<float>(arg(3));
        mo.a[3] = static_cast<float>(arg(4));
        std::erase_if(o.motions, [](const Motion& x) { return x.kind == Motion::Kind::Arc; });
        o.motions.push_back(mo);
        return;
    }
    if (o.type == "SNDTABLE") {
        // The music room's track list (`#OBJECT CHANNEL GROUP ...`: a BGM channel).
        using K = MediaCommand::Kind;
        const int32_t ch = 7;
        auto playCurrent = [&] {
            if (o.idIndex < 0 || o.idIndex >= static_cast<int32_t>(o.tracks.size())) return;
            const auto& t = o.tracks[static_cast<size_t>(o.idIndex)];
            if (t.first.empty()) return;
            MediaCommand c{K::PlayBgm, ch, t.first, o.repeat};
            // The sound sync (0x662630) stops the channel before a new file unless crossfade is on;
            // with it the new file starts with the crossfade's frames. The same file just plays.
            if (o.crossfade && o.playing && o.playingFile != t.first) c.crossfadeFrames = static_cast<uint32_t>(std::max(0, o.crossfadeFrames));
            media_.push_back(c);
            o.playing = true;
            o.playingFile = t.first;
            o.playFrom = frame_;
        };
        auto step = [&](int dir) {
            int32_t n = static_cast<int32_t>(o.tracks.size());
            if (n == 0) return;
            if (o.shuffle) {
                std::vector<int32_t> open;
                for (int32_t i = 0; i < n; ++i) {
                    if (o.tracks[static_cast<size_t>(i)].second && i != o.idIndex) open.push_back(i);
                }
                if (!open.empty()) o.idIndex = open[static_cast<size_t>(randomBetween(0, static_cast<int32_t>(open.size()) - 1))];
                return;
            }
            for (int32_t k = 1; k <= n; ++k) {
                int32_t i = ((o.idIndex + dir * k) % n + n) % n;
                if (o.tracks[static_cast<size_t>(i)].second) {
                    o.idIndex = i;
                    return;
                }
            }
        };
        if (m == "sndtbl" && args.size() >= 2) {
            int32_t i = arg(0);
            if (i >= 0 && i < 1000) {
                if (static_cast<int32_t>(o.tracks.size()) <= i) o.tracks.resize(static_cast<size_t>(i) + 1);
                o.tracks[static_cast<size_t>(i)] = {args[1], args.size() < 3 || arg(2, 1) != 0};
            }
        } else if (m == "setid") {
            o.idIndex = arg(0);
        } else if (m == "getid" && !args.empty()) {
            globals_.set(arg(0), o.idIndex);
        } else if (m == "play") {
            playCurrent();
        } else if (m == "stop") {
            o.playing = false;
            media_.push_back({K::StopBgm, ch, "", false, static_cast<uint32_t>(arg(0, 0))});
        } else if (m == "next" || m == "rev") {
            step(m == "next" ? 1 : -1);
            if (o.playing) playCurrent();
        } else if (m == "crossfade") {
            o.crossfade = arg(0, 0) != 0;
            o.crossfadeFrames = arg(1, 0);
        } else if (m == "shuffle") {
            o.shuffle = arg(0, 1) != 0;
        } else if (m == "repeat") {
            o.repeat = arg(0, 1) != 0;
        } else if (m == "blend" || m == "fade") {
            MediaCommand c{K::BgmVolume, ch};
            c.volume = std::clamp(arg(m == "fade" ? 2 : 0, 100), 0, 100);
            media_.push_back(c);
        } else {
            unhandled("SNDTABLE." + m);
        }
        return;
    }
    if (o.type == "SOUND" || o.type == "SNDTABLE") {
        using K = MediaCommand::Kind;
        // A BGM object: in the bgm group, or of sound type 0 (`#OBJECT CHANNEL STYPE` / `bgm 30 0`).
        bool bgm = o.group == "bgm" || (o.group.empty() && o.stype == 0), voice = o.group == "voice";
        int32_t ch = bgm ? 7 : 7;
        // The object has one sound of its own (the sound sync 0x662630 starts it with message 0xc8
        // on the object's handle); the group only picks the part its level comes from. So a voice
        // object's sound is a voice, once, and leaves the scene's voices playing.
        const K playKind = voice ? K::PlayVoice : bgm ? K::PlayBgm : K::PlaySe;
        if (m == "load" && !args.empty()) o.file = args[0];
        else if (m == "play" && !o.file.empty()) {
            // `play <count>`: 0 loops until stopped (title music), 1 plays once; without a count BGM
            // loops and other sounds play once.
            bool loop = args.empty() ? bgm : arg(0, 1) == 0;
            MediaCommand c{playKind, ch, str(o.file), loop};
            // The object's own volume (`blend`, 0x74c4b0) is the sound's level from the start: the
            // sync sends it (x the parts) each frame, message 0xba.
            c.volume = o.soundVolume;
            if (voice) c.spareVoices = ~(1u << ch);
            media_.push_back(c);
            if (o.type == "SOUND") {
                o.soundOn = true, o.playFrom = frame_, o.soundChannel = ch;
                o.soundKind = static_cast<int>(playKind);
            }
        } else if (m == "stop") {
            o.soundOn = false;
            media_.push_back({voice ? K::StopVoice : bgm ? K::StopBgm : K::StopSe, ch, "", false,
                              static_cast<uint32_t>(arg(0, 0))});
        } else if (m == "blend" || m == "fade") {
            MediaCommand c{voice ? K::VoiceVolume : bgm ? K::BgmVolume : K::SeVolume, ch};
            c.volume = std::clamp(arg(m == "fade" ? 2 : 0, 100), 0, 100);
            o.soundVolume = c.volume;
            media_.push_back(c);
        }
        return;
    }
    if (m == "color" && o.colorFill && !args.empty()) {
        // `color <0xAARRGGBB>` (amakanoPE.exe 0x620da0): a colour object's colour (0x604480 for a
        // CPLANE, 0x602c20 for a CIMAGE), then mot.int's pColor with the four bytes, which sets it
        // with its opacity, the byte `blend` / `fade` drive (as the COLOR column does). Amakano 1's
        // config turns its white mask black in game (`color 0xff000000`) and clear before fading it in
        // to leave (`color 0`); Nyanroid left it white, so the in-game config came in from white and
        // went out to white.
        const std::string& v = args[0];
        const auto argb = v.starts_with("0x") || v.starts_with("0X") ? static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0))
                                                                     : static_cast<uint32_t>(num(v));
        o.color = argb | 0xFF000000u;
        o.alpha = Tween::constant(static_cast<int32_t>(argb >> 24));
        return;
    }
    if (o.type == "PLANE" && m == "wipe") {
        // `<plane> wipe [frames] [mask]` (cs2.exe 0x76af60): a WIPE object `__wipeobj__` on the plane
        // shows its new contents through the mask over the frames (default 30); ignored while one
        // runs. The titles that use it (Cyanotype, Love's Sweet Garnish) wipe a full-screen plane,
        // so the screen's wipe stands in for it: what changes above the plane in the same frame
        // wipes with it (Love's Sweet Garnish's brand logo, which the engine hides at once).
        if (!wiping(frame_)) {
            wipe_ = ScreenWipe{};
            wipe_.start = frame_;
            wipe_.frames = static_cast<uint32_t>(std::max(1, arg(0, 30)));
            wipe_.mask = args.size() >= 2 ? str(args[1]) : std::string();
        }
        return;
    }
    if (o.container() && o.type != "STRING" && o.type != "NVSTRING" && o.type != "LOGSTRING" && m == "draw") {
        // `draw [frames]` (amakanoPE.exe 0x61f640). A hidden plane appears, fading in over the
        // frames. On a PLANE (type 4, 0x61f6c6) the engine adds a DRAW object (type 7, named
        // `__drawobj__`) to it, which takes the plane from how it was last drawn to what it holds
        // now over the frames (default 30, at least 1); nothing while one is there. It is the plane's
        // counterpart of `rdraw` (an RDRAW, type 26, on the screen), so the screen's fade stands in
        // for it inside the plane's box (fitted: what else changes in the box fades with it).
        // Amakano 1's title comes back from white this way, and its save screen leaves to white; both
        // were shown at once.
        if (!o.disp) {
            o.disp = true;
            int32_t frames = std::max(0, arg(0, 0));
            int32_t to = o.alpha.to;
            o.alpha = frames ? Tween{0, to, frame_, static_cast<uint32_t>(frames)} : Tween::constant(to);
        } else if (o.type == "PLANE" && !wiping(frame_)) {
            wipe_ = ScreenWipe{};
            wipe_.start = frame_;
            wipe_.frames = static_cast<uint32_t>(std::max(1, arg(0, 30)));
            if (o.width > 0 && o.height > 0) {
                const Affine w = worldTransform(o, frame_);
                float xs[4], ys[4];
                const float cx[4] = {0, 1, 1, 0}, cy[4] = {0, 0, 1, 1};
                for (int k = 0; k < 4; ++k) w.apply(cx[k] * static_cast<float>(o.width), cy[k] * static_cast<float>(o.height), xs[k], ys[k]);
                wipe_.box = true;
                wipe_.x0 = *std::min_element(xs, xs + 4), wipe_.x1 = *std::max_element(xs, xs + 4);
                wipe_.y0 = *std::min_element(ys, ys + 4), wipe_.y1 = *std::max_element(ys, ys + 4);
            }
        }
        return;
    }
    if (o.type == "SAVEPANEL") {
        if (m == "ptype" && args.size() >= 2) {
            Object::PanelPart part;
            part.kind = args[0];
            part.id = arg(1);
            if (part.kind == "SIMAGE") {
                part.mask = static_cast<uint32_t>(arg(2));
            } else if (part.kind == "SSTR") {
                // SSTR <id> <mask> <size> <chars per line> <color> <outline on> <outline color> <lines> <spacing>
                part.mask = static_cast<uint32_t>(std::strtoul(args.size() > 2 ? args[2].c_str() : "0", nullptr, 0));
                part.size = arg(3, 12);
                part.lineChars = arg(4, 0);
                part.color = static_cast<uint32_t>(std::strtoul(args.size() > 5 ? args[5].c_str() : "0xffffffff", nullptr, 0));
                bool outline = arg(6, 0) != 0;
                part.outline = outline ? static_cast<uint32_t>(std::strtoul(args.size() > 7 ? args[7].c_str() : "0", nullptr, 0)) : 0;
                part.lines = std::max(1, arg(8, 1));
            } else if ((part.kind == "NUM_O" || part.kind == "NUM_O2") && args.size() >= 4) {
                // The source (0x668f40): a name up to '.' or ':', a number after '.' (FLAG's), a
                // divisor after ':'. FLAG without a divisor has 0, a date -1; anything else is a
                // number (Amakano 2's slot numbers; Cyanotype's numbers, set later by `setnum`).
                part.font = args[2];
                const std::string& f = args[3];
                const size_t stop = f.find_first_of(".:");
                const std::string name = f.substr(0, stop);
                size_t colon = f.find(':');
                if (stop != std::string::npos && f[stop] == '.') part.flag = num(f.substr(stop + 1, colon == std::string::npos ? std::string::npos : colon - stop - 1));
                const bool hasDiv = colon != std::string::npos;
                const int32_t div = hasDiv ? num(f.substr(colon + 1)) : 0;
                if (name == "FLAG") {
                    part.field = name;
                    part.div = div;
                } else if (name == "DATE_Y" || name == "DATE_M" || name == "DATE_D" || name == "DATE_H" || name == "DATE_N" ||
                           name == "DATE_S") {
                    part.field = name;
                    part.div = hasDiv ? div : -1;
                } else {
                    part.value = num(name);
                }
            }
            // A part's type is set per id: setting it again replaces it (saveload.fes sets the
            // panels up again on every Save / Load tab change, and the slot number per slot).
            for (auto& p : o.parts) {
                if (p.id == part.id) {
                    part.visible = p.visible;
                    p = std::move(part);
                    return;
                }
            }
            o.parts.push_back(std::move(part));
            return;
        }
        if (m == "setsave" && !args.empty()) {
            o.saveSlot = arg(0, -1);
            return;
        }
        if (m == "dispid" && args.size() >= 2) {
            // dispid <0|1> <id or [from-to]> ...
            bool on = arg(0) != 0;
            for (size_t i = 1; i < args.size(); ++i) {
                int32_t from, to;
                const std::string& a = args[i];
                if (a.size() > 2 && a.front() == '[') {
                    from = std::atoi(a.c_str() + 1);
                    size_t dash = a.find('-', 2);
                    to = dash == std::string::npos ? from : std::atoi(a.c_str() + dash + 1);
                } else {
                    from = to = num(a);
                }
                for (auto& p : o.parts) {
                    if (p.id >= from && p.id <= to) p.visible = on;
                }
            }
            return;
        }
        if (m == "setnum" && args.size() >= 2) {
            // `setnum <id> <value>` (Nekopara 0x762580): the number a NUM part shows.
            for (auto& p : o.parts) {
                if (p.id == arg(0)) p.value = arg(1);
            }
            return;
        }
        if (m == "getflag" && args.size() >= 2) {
            // getflag <flag in the save> <var>
            int32_t v = 0;
            if (SaveStore* saves = host_ ? host_->saveStore() : nullptr) {
                if (const SaveFile* f = saves->get(o.saveSlot)) v = f->resume.vars.get(arg(0));
            }
            globals_.set(arg(1), v);
            return;
        }
        if (m == "getsavedate") {
            // `getsavedate <y> <m> <d> <h> <min> <s>` (0x7798a0 -> 0x762c80): the bound save's time,
            // each into its variable (Nekopara's quick-save notice prints it).
            std::tm tm{};
            if (SaveStore* saves = host_ ? host_->saveStore() : nullptr) {
                if (const SaveFile* f = saves->get(o.saveSlot)) {
                    const auto t = static_cast<std::time_t>(f->time);
                    if (const std::tm* lt = std::localtime(&t)) tm = *lt;
                    const int32_t v[6] = {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec};
                    for (size_t i = 0; i < 6 && i < args.size(); ++i) globals_.set(arg(i), v[i]);
                }
            }
            return;
        }
        if (m == "getsavestr" && args.size() >= 2) {
            // `getsavestr <string var> <id>` (cs2.exe 0x7791a0 -> 0x762c50 -> 0x78b780): the bound
            // save's record with that id, "" if none. sscript writes 0x200 (the message) and 0x201
            // (the scene title); Nekopara's saveload.fes adds the date and time (0x80000000 /
            // 0x80000001, `gettime` then `saveapend_str`).
            std::string text;
            if (SaveStore* saves = host_ ? host_->saveStore() : nullptr) {
                if (const SaveFile* f = saves->get(o.saveSlot)) {
                    const auto id = static_cast<uint32_t>(std::strtoul(args[1].c_str(), nullptr, 0));
                    if (auto it = f->strings.find(id); it != f->strings.end()) text = it->second;
                }
            }
            // Script strings are Shift-JIS; text Shift-JIS can't hold stays UTF-8 (drawn as such).
            std::string sjis = utf8ToSjis(text);
            globals_.setStr(arg(0), sjisToUtf8(sjis) == text ? sjis : text);
            return;
        }
        if (m == "userfont") return;  // custom glyph images for special characters: not drawn yet
    }
    if (o.type == "STRING" || o.type == "NVSTRING" || o.type == "LOGSTRING") {
        if (m == "layout") {
            layoutMethod(o, args);
            return;
        }
        if (m == "mag") {
            // `mag [n]` (Nekopara's cs2.exe 0x776af0; bare 10): the text's reveal speed in tenths,
            // which the windows' MES_SETMAG sets (Labyrinth: 20 while auto runs sped up).
            o.mag = arg(0, 10);
            return;
        }
        if (m == "reset" || m == "draw" || m == "userfont" || m == "userfontobj" || m == "clear" || m == "skip") {
            if (m == "clear") o.text.clear();
            // The newer string engine (the one with `prof_enable` / `profmode`) resets a string to
            // proportional spacing: its reset clears the fixed-pitch flag (Cyanotype's cinema window
            // sets no `fontfix 0` yet draws proportional text). Older engines keep fixed pitch.
            if (m == "reset" && proportionalReset_) o.fontFixed = false;
            // A string starts again with the config's font, and follows it (fontOf).
            if (m == "reset") o.fontName.clear(), o.fontFollows = true, o.shadow[0] = o.shadow[1] = -1, o.weight = -1;
            if (m == "draw" && o.disp) revealPlane(o);
            // `draw <n>` (cs2.exe 0x753610) has the plane reveal the text at the layout's `frame`
            // speed (-1: the config's messpd; 0: at once); `clear`, `skip` and `reset` end a reveal.
            if (m != "userfont" && m != "userfontobj") o.revealStart = -1;
            // On a string the host draws (the message), `skip` is for the host to act on: cs2.exe's
            // STRING `skip` (0x775a90 -> +0x510; NVSTRING +0x618) asks the object's sync to show the
            // whole text (0x6635c5: message 0x1c7 to its text).
            if (m == "skip" && hostStrings_.count(o.name)) hostSkipped_.insert(o.name);
            if (m == "draw" && o.type == "STRING" && !o.text.empty()) {
                int32_t speed = o.frameSet ? o.frameSpeed : -1;
                if (speed < 0) {
                    speed = 4;
                    if (config_) {
                        if (auto it = config_->find("messpd"); it != config_->end()) speed = it->second;
                    }
                }
                if (speed > 0) {
                    o.revealStart = static_cast<int64_t>(frame_);
                    o.revealSpeed = speed;
                }
            }
            return;
        }
        if (m == "isdraw") {
            locals_.set(0, o.type == "STRING" ? stringDrawing(o) : 0);
            return;
        }
        if (m == "str" || m == "apend") {
            // `str <text>` (cs2.exe 0x774cd0) sets the text; `apend <text>` (0x775650) runs the same
            // text processing and adds the result to the end (STRING8 0x752f80, STRING 0x74e600,
            // NVSTRING 0x750b00); both mark the plane's status unread (+0x544 = -1).
            o.strFrame = frame_;
            o.revealStart = -1;
            std::string v;
            for (size_t i = 0; i < args.size(); ++i) v += (i ? " " : "") + args[i];
            // Print prefixes: \pr right-aligned, \pc centred, \pl left (a text's start only).
            const bool append = m == "apend" && !o.text.empty();
            if (!append) o.textAlign = 0;
            if (v.starts_with("\\pr")) v = v.substr(3), o.textAlign = append ? o.textAlign : 1;
            else if (v.starts_with("\\pc")) v = v.substr(3), o.textAlign = append ? o.textAlign : 2;
            else if (v.starts_with("\\pl")) v = v.substr(3);
            const std::string text = str(v);
            // `\_` is a space (a space would end the token): `No.%02d\_:\_` shows "No.01 : ". `\n`
            // breaks the line, as in message text (config.fes's preview: `NEKOPARA\nThe Nekos'
            // Aromatissé\nThis is sample text.`). Shift-JIS lead bytes take the next byte along (it
            // may be 0x5C); harmless for UTF-8.
            std::string spaced;
            for (size_t i = 0; i < text.size(); ++i) {
                const auto c = static_cast<unsigned char>(text[i]);
                if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < text.size()) {
                    spaced += text.substr(i++, 2);
                } else if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'i' || text[i + 1] == 'I') &&
                           (i + 2 >= text.size() || (text[i + 2] != 'd' && text[i + 2] != 'D'))) {
                    // `\i` marks the indent for the lines after it (grisaia.exe 0x548bc0: control glyph
                    // 6, nothing drawn); Cyanotype's glossary puts one after each paragraph. The mark
                    // itself is not kept (indents are not modelled here).
                    ++i;
                } else if (c == '\\' && i + 1 < text.size() && (text[i + 1] == '_' || text[i + 1] == 'n')) {
                    spaced += text[i + 1] == 'n' ? '\n' : ' ';
                    ++i;
                } else {
                    spaced += text[i];
                }
            }
            o.text = append ? o.text + spaced : std::move(spaced);
            return;
        }
        static const char* kLogMethods[] = {"setlog", "max", "getpos", "next", "rev", "page", "pos", "blockcolor", "lineinfo",
                                            "blockinfo", "pathinfo", "remake"};
        if (o.type == "LOGSTRING" && std::find(std::begin(kLogMethods), std::end(kLogMethods), m) != std::end(kLogMethods)) {
            logMethod(o, m, args);
            return;
        }
    }
    if (o.type == "SLIDER" || o.type == "SCROLL" || o.type == "SCROLL2") {
        if (m == "attach") return;  // wheel routing to the bar: scroll inputs come from the engine
        if (m == "page") {
            // page <min> <max> [visible]: value range, and how much one view shows (thumb length).
            o.sliderLo = arg(0, 0);
            o.sliderHi = arg(1, 10);
            o.sliderPage = arg(2, 0);
            return;
        }
        if (m == "pos") {
            const int32_t top = sliderTop(o);
            o.sliderValue = std::clamp(arg(0, o.sliderValue), std::min(o.sliderLo, top), std::max(o.sliderLo, top));
            return;
        }
        if (m == "getpos" && !args.empty()) {
            globals_.set(arg(0), o.sliderValue);
            return;
        }
    }
    if (m == "attach") return;  // keyboard/wheel routing to a scroll bar: scroll inputs come from the engine
    if (m == "disp") {
        o.disp = arg(0, 1) != 0;
        o.dispCommanded = true;
        if (o.disp) revealPlane(o);
    }
    else if (m == "setid") o.setIndex(arg(0));
    else if (m == "enable") o.enable = arg(0, 1) != 0;
    else if (m == "blend") o.alpha = Tween::constant(std::clamp(arg(0, 255), 0, 255));
    else if (m == "pos") {
        if (!args.empty()) o.x = Tween::constant(numAt(args[0], o.x.value(frame_)));
        if (args.size() > 1) o.y = Tween::constant(numAt(args[1], o.y.value(frame_)));
    } else if (m == "pos2" && args.size() >= 2) {
        if (const FrameInfo* f = frameInfo(str(args[0]), arg(1))) o.x = Tween::constant(f->x), o.y = Tween::constant(f->y);
    } else if (m == "load" && (o.type == "IMAGE" || o.type == "BUTTON" || o.type == "PANEL" || o.type == "SAVEPANEL" ||
                                o.type == "IMGWND")) {
        // `load [file [id]]` (cs2.exe 0x76bae0): the file (none: emptied) and the index, 0 when not
        // given, through setid's setter.
        o.file = args.empty() ? std::string() : args[0];
        o.setIndex(args.size() >= 2 ? arg(1) : 0);
    } else if (m == "load" && o.type == "PLANE") {
        // A plane ignores `load` (Fruit's staff roll empties its planes with a bare one).
    } else if (m == "load" && !args.empty()) {
        o.file = args[0];
    } else if (m == "delete") {
        o.disp = false;
    } else if (m == "stop") {
        // cs2.exe 0x67a2b0: everything stays where the motions have it now (foldMotions); a stopped
        // fade keeps the opacity it reached (an mcfade the one it last set).
        o.delay = 0;
        o.alpha = Tween::constant(opacity(o, frame_));
        foldMotions(o, frame_, true);
    } else if (m == "getpos" && args.size() >= 2) {
        globals_.set(arg(0), o.x.value(frame_));
        globals_.set(arg(1), o.y.value(frame_));
    } else if (m == "getpos2" && args.size() >= 2) {
        // The frame the object was placed at (POS2), else its own image's frame.
        const FrameInfo* f = !o.pos2File.empty() ? frameInfo(o.pos2File, o.pos2Id) : frameInfo(str(o.file), o.frame());
        globals_.set(arg(0), f ? f->x : 0);
        globals_.set(arg(1), f ? f->y : 0);
    } else if (m == "getid" && !args.empty()) {
        globals_.set(arg(0), o.idIndex);
    } else if (m == "loadlist2" && args.size() >= 3) {
        // `loadlist2 <list> <group> <n> <flag>` (CG viewer, amakanoPE.exe 0x621880): the object
        // shows the n-th recorded picture of that group of cgreg_init's table (nothing without one).
        std::string img;
        int32_t group = cgTable_ ? cgTableGroup(args[1]) : -1;
        if (group >= 0 && static_cast<size_t>(group) < cgTable_->size() && host_) {
            int32_t n = arg(2);
            for (const auto& entry : (*cgTable_)[static_cast<size_t>(group)].images) {
                if (host_->cgEntrySeen(entry) && n-- == 0) {
                    img = entry;
                    break;
                }
            }
        }
        if (!img.empty()) {
            o.file = img;
            o.setIndex(0);
        }
    } else if (m == "endpos" && args.size() >= 2) {
        // `endpos <x var> <y var>`: the end of the message text (its last line's top), relative to the
        // string object; NVL windows put their click-wait mark there.
        globals_.set(arg(0), textEndX_);
        globals_.set(arg(1), textEndY_);
    } else if (m == "getsize" && args.size() >= 2) {
        // `getsize <w var> <h var>`: the object's SIZE / SIZE2, else its image frame's size (scripts
        // centre objects with `base $w/2 $h/2`).
        int32_t w = o.width, h = o.height;
        if ((w <= 0 || h <= 0) && !o.file.empty()) {
            if (const FrameInfo* f = frameInfo(str(o.file), o.frame())) w = f->w, h = f->h;
        }
        globals_.set(arg(0), std::max(0, w));
        globals_.set(arg(1), std::max(0, h));
    } else if (m == "getbase" || m == "getoffset") {
        // `getbase <x var> <y var> [<z var>]` / `getoffset ...` (cs2.exe 0x686da0 / 0x686f70). The
        // image kinds' base (+0xe0) is set when their image loads (0x5625e0): BTYPE 0 takes the HG
        // base less the frame's offset and moves the offset into the object (the image's offset
        // becomes 0 0), BTYPE 1 the HG base, its offset kept; Nyanroid keeps a script's BASE /
        // `base` before that (which wins is not traced; no game reads one back). The others (planes,
        // strings) give their base. z is 0 throughout.
        static const std::set<std::string> kOthers = {"CPLANE", "PLANE", "STRING", "NVSTRING", "RFMOS", "EFKPLANE", "STRING8", "NVSTRING8"};
        const bool image = isImageKind(o.type);
        if (!image && !(m == "getbase" && kOthers.count(o.type))) return;
        const FrameInfo* f = image && !o.file.empty() ? frameInfo(str(o.file), o.frame()) : nullptr;
        int32_t x = 0, y = 0;
        if (m == "getbase") {
            x = o.baseX, y = o.baseY;
            if (f && !o.baseSet) {
                x = f->baseX - (o.btype == 1 ? 0 : f->x);
                y = f->baseY - (o.btype == 1 ? 0 : f->y);
            }
        } else if (f && o.btype == 1) {
            x = f->x, y = f->y;
        }
        const int32_t v[3] = {x, y, 0};
        for (size_t i = 0; i < 3 && i < args.size(); ++i) globals_.set(arg(i), v[i]);
    } else if (m == "size" && args.size() >= 2) {
        // `size <w> <h>` (Happiness 2 cs2.exe 0x67afb0): an image kind's drawn size (CIMAGE, IMAGE,
        // BUTTON, PANEL, SAVEPANEL, IMGWND: 0x653880, +300 / +0x130; an IMGWND builds its window
        // that big), a plane's or string's box (CPLANE, PLANE, STRING, NVSTRING: 0x655170).
        static const std::set<std::string> kKinds = {"CIMAGE", "IMAGE",  "BUTTON", "PANEL",    "SAVEPANEL",
                                                     "IMGWND", "CPLANE", "PLANE",  "STRING",   "NVSTRING"};
        if (kKinds.count(o.type)) o.width = arg(0), o.height = arg(1);
    } else if (m == "drawarea" && !args.empty()) {
        // `drawarea <w var> [<h var>]` (0x689380, STRING / STRING8): the text as drawn, the largest
        // x + width and y + height of its glyphs (message 0x1db -> 0x59db80), margins included.
        if (o.type != "STRING") return;
        const FesQuad q = stringBox(o, frame_);
        float w = 0, h = 0;
        if (auto area = host_ ? host_->textDrawArea(q) : std::nullopt; area && area->first > 0) {
            w = static_cast<float>(o.margin[0]) + area->first;
            h = static_cast<float>(o.margin[1]) + area->second;
        }
        globals_.set(arg(0), static_cast<int32_t>(std::lround(w)));
        if (args.size() >= 2) globals_.set(arg(1), static_cast<int32_t>(std::lround(h)));
    } else if (m == "getfile" && !args.empty()) {
        // `getfile <string var>` (0x6879b0): an IMAGE's file (Amakano 2's CG viewer prints it).
        if (o.type == "IMAGE") globals_.setStr(arg(0), str(o.file));
    } else if (m == "linecnt" && !args.empty()) {
        // `linecnt <var>` (0x686cb0): a STRING's or STRING8's line count (message 0x1ea to its
        // plane -> 0x5a1860), wrapped lines included. Headless, the lines its breaks make.
        if (o.type != "STRING" && o.type != "STRING8") return;
        const FesQuad q = stringBox(o, frame_);
        std::optional<int32_t> n = host_ ? host_->textLineCount(q) : std::nullopt;
        if (!n) {
            int32_t lines = 1;
            for (size_t i = 0; i < q.text.size(); ++i) {
                if (q.text[i] == '\n' && i + 1 < q.text.size()) ++lines;
            }
            n = lines;
        }
        globals_.set(arg(0), *n);
        if (log && std::getenv("NYAN_FES_TRACE")) log("fes " + name_ + ": " + o.name + " linecnt " + std::to_string(*n));
    } else if (m == "settbl" && args.size() >= 2) {
        // `settbl <n> <frame id>`: replace entry n of the object's frame table (ID.n).
        if (arg(0) >= 0 && arg(0) < 32) o.ids[arg(0)] = arg(1), o.idTable = true;  // 0x749ea0
    } else if (m == "setanm" || m == "anm") {
        if (m == "setanm") {
            // A new script on a playing image starts at once (0x770e00 -> 0x650560 makes the image a
            // new ANM object).
            o.anmName = args.empty() ? std::string() : str(args[0]);
        } else {
            // Any argument but STOP plays (`keywait anm 0` too: meswnd.fes hides the icon with disp).
            std::string a = args.empty() ? std::string() : args[0];
            for (char& ch : a) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            o.anmPlaying = a != "STOP";
        }
        o.anmStart = frame_;
        o.anmPlayer.reset();
        if (o.anmPlaying && !o.anmName.empty()) {
            if (auto s = anmScript(o.anmName)) {
                uint32_t seed = static_cast<uint32_t>(frame_ * 2654435761u);
                for (char c : o.anmName) seed = seed * 16777619u ^ static_cast<uint8_t>(c);
                o.anmPlayer = std::make_shared<AnmPlayer>(std::move(s), seed);
                o.anmPlayer->advance(frame_);  // it runs from `anm` on
            }
        }
    } else if (m == "coordmode") {
        // `coordmode <n>` (amakanoPE.exe 0x624e80 -> image kinds' +0x280, sent with the sprite's
        // transform, message 0x113 -> sprite +0x68): with n the sprite's rectangle follows its
        // rotation and scale about its base (0x601030), else it is the unturned one. The drawing
        // turns about the base either way; Nyanroid draws transformed quads, so nothing to do.
    } else if (m == "tone") {
        // `tone <r> <g> <b>` (Amakano 2's cs2.exe 0x70a040, Fruit's 0x5e8320): an image kind's
        // colour scale in percent, 100 for each not given (0x6df270, applied by the sprite).
        if (isImageKind(o.type)) o.tone = {arg(0, 100), arg(1, 100), arg(2, 100)};
    } else if (m == "lock" || m == "noact" || m == "keypos") {
        // Input/animation details not modelled yet.
    } else {
        unhandled(o.type + "." + m);
    }
}

void FesInstance::primitive(Object& o, const std::string& p, const std::vector<std::string>& a) {
    auto at = [&](size_t i) { return i < a.size() ? a[i] : std::string("0"); };
    auto frames = [&](size_t i) { return static_cast<uint32_t>(std::max(0, num(at(i)))); };
    if (p == "pBase") {
        o.baseX = num(at(0));
        o.baseY = num(at(1));
        o.baseSet = true;
    } else if (p == "pDepth") {
        o.pri = num(at(0));
    } else if (p == "pScale") {
        o.scaleX = {o.scaleX.value(motionAt_), static_cast<int32_t>(percent(at(0), 100)), motionAt_, 0};
        o.scaleY = {o.scaleY.value(motionAt_), static_cast<int32_t>(percent(at(1), 100)), motionAt_, 0};
    } else if (p == "pRot") {
        o.rotX = static_cast<float>(num(at(0)));
        o.rotY = static_cast<float>(num(at(1)));
        o.rotation = static_cast<float>(num(at(2)));
    } else if (p == "pVram") {
        // VRAM placement is irrelevant to this renderer.
    } else if (p == "motMove") {  // frames, accel..., x, y, z
        int32_t cx = o.x.value(motionAt_), cy = o.y.value(motionAt_);
        o.x = {cx, numAt(at(4), cx), motionAt_, frames(0)};
        o.y = {cy, numAt(at(5), cy), motionAt_, frames(0)};
    } else if (p == "motScale") {
        int32_t cx = o.scaleX.value(motionAt_), cy = o.scaleY.value(motionAt_);
        o.scaleX = {cx, static_cast<int32_t>(percent(at(4), cx)), motionAt_, frames(0)};
        o.scaleY = {cy, static_cast<int32_t>(percent(at(5), cy)), motionAt_, frames(0)};
    } else if (p == "motRot") {
        // frames, from (0 0 0), by: turns by the angles over the frames, on top of where it is.
        float dx = static_cast<float>(num(at(4))), dy = static_cast<float>(num(at(5))), dz = static_cast<float>(num(at(6)));
        Motion m;
        m.kind = Motion::Kind::Settle;
        m.start = motionAt_;
        m.period = std::max(1u, frames(0));
        m.a[0] = m.a[1] = 1;
        m.a[2] = -dz, m.a[3] = -dx, m.a[4] = -dy;
        o.rotation += dz, o.rotX += dx, o.rotY += dy;
        o.motions.push_back(m);
    } else if (p == "motFade" || p == "motFade2") {  // frames, from, to
        int32_t cur = opacity(o, motionAt_);
        o.alpha = {std::clamp(num(at(1), cur), 0, 255), std::clamp(num(at(2), cur), 0, 255), motionAt_, frames(0)};
    } else if (p == "mcFade") {  // frames, from, to (the motion scenes' mcfade runs; no hold from FE scripts)
        int32_t cur = opacity(o, motionAt_);
        Motion m;
        m.kind = Motion::Kind::Fade;
        m.loop = true;
        m.start = motionAt_;
        m.period = std::max(1u, frames(0));
        m.a[0] = static_cast<float>(num(at(1), cur));
        m.a[1] = static_cast<float>(num(at(2), cur));
        m.a[2] = static_cast<float>(std::max(0, num(at(3), 0)));
        std::erase_if(o.motions, [](const Motion& x) { return x.kind == Motion::Kind::Fade; });
        o.motions.push_back(m);
    } else if (p == "mcMove") {
        Motion m;
        m.kind = Motion::Kind::Move;
        m.loop = true;  // mot.int's mcMove ends only when stopped
        m.start = motionAt_;
        m.period = std::max(1u, frames(0));
        m.a[0] = static_cast<float>(o.x.value(motionAt_));
        m.a[1] = static_cast<float>(o.y.value(motionAt_));
        m.a[2] = static_cast<float>(num(at(4)));
        m.a[3] = static_cast<float>(num(at(5)));
        o.motions.push_back(m);
    } else if (p == "mcRot") {
        Motion m;
        m.kind = Motion::Kind::Rotate;
        m.loop = true;
        m.start = motionAt_;
        m.a[0] = static_cast<float>(num(at(2)));
        m.a[1] = static_cast<float>(num(at(0)));
        m.a[2] = static_cast<float>(num(at(1)));
        o.motions.push_back(m);
    } else if (p == "mcScaleCos" || p == "mcScaleSin") {
        Motion m;
        m.kind = Motion::Kind::ScaleCos;
        m.loop = true;
        m.start = motionAt_;
        m.period = std::max(1u, frames(0));
        m.a[0] = percent(at(3), 100);
        m.a[1] = percent(at(4), 100);
        m.a[2] = percent(at(6), 100);
        m.a[3] = percent(at(7), 100);
        o.motions.push_back(m);
    } else {
        unhandled("primitive " + p);
    }
}

FesInstance::Affine FesInstance::transform(const Object& o, uint64_t frame) const {
    MotionState ms = evaluate(o.motions, frame);
    float px = (ms.hasPos ? ms.x : static_cast<float>(o.x.value(frame))) + ms.dx;
    float py = (ms.hasPos ? ms.y : static_cast<float>(o.y.value(frame))) + ms.dy;
    float sx = o.scaleX.value(frame) / 100.0f * ms.scaleX;
    float sy = o.scaleY.value(frame) / 100.0f * ms.scaleY;
    // x/y tilts show on strings; a PLANE's are not drawn (Cyanotype's choice screen tilts its
    // strings and planes by 45 and swings back only the strings, over flat bars). The image kinds
    // keep one angle: their motion apply (amakanoPE.exe 0x602d80, vtable slot 2 of kcFEScriptObj
    // Image, CImage, Button, Panel, ...) takes only z from the motion's record (+0x7c of the
    // rotation at +0x74), where planes and strings (0x6045b0) take all three. Amakano 1's title
    // decorations (`mcrot` about x and z) spin flat.
    const bool tilt = o.type != "PLANE" && o.type != "CPLANE" && !isImageKind(o.type);
    auto r = rotation2x2(tilt ? o.rotX + ms.rotX : 0, tilt ? o.rotY + ms.rotY : 0, o.rotation + ms.rotation);
    // T(pos) * R * S * T(-base): x' = a x + c y, y' = b x + d y.
    float bx = static_cast<float>(o.baseX), by = static_cast<float>(o.baseY);
    float shiftX = 0, shiftY = 0;
    // A stand's parts share one canvas; its first part (the body) gives the base.
    const std::string file = o.type == "CHTABLE3" ? [&] {
        const auto p = pictures(o);
        return p.empty() ? std::string() : p.front();
    }() : str(o.file);
    if (o.btype == 1 && !file.empty()) {
        if (const FrameInfo* f = frameInfo(file, o.frame())) bx = static_cast<float>(f->baseX), by = static_cast<float>(f->baseY);
    } else if (!o.baseSet && !file.empty() && isImageKind(o.type)) {
        // BTYPE 0, the default: the image's load (cs2.exe 0x5625e0) makes the HG base less the
        // frame's offset its base and moves the offset into the object, so the picture stays where
        // its canvas puts it and turns and scales about its HG base point (the sprite's corners,
        // amakanoPE.exe 0x601030: position, rotation and scale about the base less the offset).
        // Amakano 1's title snowflakes (frames with their base at their centre, `mcrot`) spin in
        // place; Nyanroid turned them about the canvas corner, across the screen.
        if (const FrameInfo* f = frameInfo(file, o.frame())) {
            bx = shiftX = static_cast<float>(f->baseX);
            by = shiftY = static_cast<float>(f->baseY);
        }
    }
    Affine t;
    t.a = r[0] * sx;
    t.b = r[1] * sx;
    t.c = r[2] * sy;
    t.d = r[3] * sy;
    t.tx = px + shiftX - (t.a * bx + t.c * by);
    t.ty = py + shiftY - (t.b * bx + t.d * by);
    return t;
}

FesInstance::Affine FesInstance::worldTransform(const Object& o, uint64_t frame, int depth) const {
    Affine t = transform(o, frame);
    if (!o.parent.empty() && depth < 16) {
        auto it = byName_.find(o.parent);
        if (it != byName_.end() && &objects_[it->second] != &o) {
            return worldTransform(objects_[it->second], frame, depth + 1) * t;
        }
    }
    return t;
}

float FesInstance::worldAlpha(const Object& o, uint64_t frame, int depth) const {
    if (!o.disp && depth == 0) return 0;
    // A PARENT passes on its opacity, not its DISP: Cyanotype's tips text (`ind`, PARENT pl_text)
    // shows while pl_text, declared hidden, fades from `blend 0` to 255 (the fade motion only
    // writes the opacity, mot.int motFade; DISP is a field of its own).
    float a = std::clamp(opacity(o, frame), 0, 255) / 255.0f;
    if (!o.parent.empty() && depth < 16) {
        auto it = byName_.find(o.parent);
        if (it != byName_.end() && &objects_[it->second] != &o) a *= worldAlpha(objects_[it->second], frame, depth + 1);
    }
    return a;
}

std::vector<FesQuad> FesInstance::draw(uint64_t frame) const {
    std::vector<FesQuad> out;
    // Debugging: NYAN_FES_DUMP=<frame> lists every object's state at that frame.
    if (const char* dump = std::getenv("NYAN_FES_DUMP"); dump && log && std::strtoull(dump, nullptr, 10) == frame) {
        if (const char* vs = std::getenv("NYAN_FES_DUMP_VARS")) {
            std::string line = "fes " + name_ + " vars";
            for (const char* p = vs; *p;) {
                char* end = nullptr;
                long v = std::strtol(p, &end, 10);
                if (end == p) break;
                line += " \\" + std::to_string(v) + "=" + std::to_string(locals_.get(static_cast<int32_t>(v)));
                p = *end ? end + 1 : end;
            }
            log(line);
        }
        for (const auto& o : objects_) {
            log("fes " + name_ + " obj " + o.name + " " + o.type + " disp=" + std::to_string(o.disp) + " pri=" +
                std::to_string(o.pri) + " pl=" + o.plane + " file=" + str(o.file) + " id0=" +
                (o.ids.count(0) ? std::to_string(o.ids.at(0)) : std::string("-")) + " frame=" + std::to_string(o.frame()) + " alpha=" +
                std::to_string(opacity(o, frame)) + " xy=" + std::to_string(o.x.value(frame)) + "," + std::to_string(o.y.value(frame)) + " wh=" + std::to_string(o.width) + "," + std::to_string(o.height) + " vram=" + std::to_string(o.vramX) + "," + std::to_string(o.vramY));
        }
    }
    // Containers back to front by priority; each draws its images back to front by priority.
    std::vector<const Object*> containers;
    for (const auto& o : objects_) {
        // Scroll bars outside any plane are drawn on the screen among the planes.
        if (o.container() || (o.type == "SCROLL" && byName_.find(o.plane) == byName_.end())) containers.push_back(&o);
        // Images declared without a plane are drawn on the screen itself, among the planes by priority
        // (Hatsumira's config: a black image that covers the screen while it fades in).
        // Only ones that show something of their own: a colour, or a chosen frame (user-font sources
        // such as a `userfont -1` image are images to take glyphs from, not to draw).
        // Only ones the script shows (a DISP column, a `disp` or `fade`): plane-less images it
        // never shows are templates, such as Fruit's save-panel date bar, which the
        // panels draw from and which would otherwise flash red while the menu fades in.
        else if ((o.type == "IMAGE" || o.type == "CIMAGE") && o.plane.empty() && !o.mask && !unmade(o) && (o.colorFill || o.frame() >= 0) &&
                 (o.dispColumn || o.dispCommanded || shownByScript_.count(o.base)))
            containers.push_back(&o);
    }
    std::stable_sort(containers.begin(), containers.end(),
                     [](const Object* a, const Object* b) { return a->pri < b->pri; });
    auto solid = [&](const Affine& t, float w, float h, uint32_t color, float alpha) {
        FesQuad q;
        q.color = color;
        q.alpha = alpha;
        const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
        for (int k = 0; k < 4; ++k) t.apply(xs[k] * w, ys[k] * h, q.x[k], q.y[k]);
        out.push_back(std::move(q));
    };
    // Slider / scroll bar knob: the FILE image's first frame at the value's point along the track.
    auto knob = [&](const Object* img, const Affine& world, float alpha) {
        std::string file = str(img->file);
        frameInfo(file, 0);
        auto fit = frames_.find(file);
        if (fit == frames_.end() || fit->second.empty()) return;
        const FrameInfo& k = fit->second.begin()->second;
        float span = static_cast<float>(sliderTop(*img) - img->sliderLo);
        float t = span != 0 ? (img->sliderValue - img->sliderLo) / span : 0;
        auto& kit = fit->second;
        if (img->type == "SCROLL" && kit.count(100) && kit.count(1000) && kit.count(1002)) {
            // Scroll bar kit: track (frame 0), thumb (100; 102/103 pressed/hovered) and markers for the
            // thumb's travel (1000 top, 1002 bottom; not drawn). The thumb's length shows how much of
            // the range one view covers.
            float ox = static_cast<float>(img->x.value(frame)), oy = static_cast<float>(img->y.value(frame));
            float a = alpha * std::clamp(opacity(*img, frame), 0, 255) / 255.0f;
            auto piece = [&](int32_t id, float x, float y, float w, float h) {
                FesQuad q;
                q.texture = file + "#" + std::to_string(id);
                q.alpha = a;
                const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
                for (int n = 0; n < 4; ++n) world.apply(ox + x + xs[n] * w, oy + y + ys[n] * h, q.x[n], q.y[n]);
                out.push_back(std::move(q));
            };
            if (auto tr = kit.find(0); tr != kit.end()) {
                piece(0, static_cast<float>(tr->second.x), static_cast<float>(tr->second.y),
                      static_cast<float>(tr->second.w), static_cast<float>(tr->second.h));
            }
            const FrameInfo &top = kit.at(1000), &bot = kit.at(1002);
            float y0 = static_cast<float>(top.y), y1 = static_cast<float>(bot.y + bot.h);
            float len = y1 - y0;
            float view = img->sliderPage > 0 ? static_cast<float>(img->sliderPage) : 1.0f;
            float range = static_cast<float>(std::abs(img->sliderHi - img->sliderLo));
            float thumb = std::clamp(len * view / std::max(1.0f, range), 8.0f, len);
            float ty = y0 + (len - thumb) * t;
            int32_t id = dragging_ == img && kit.count(102) ? 102 : hovered_ == img && kit.count(103) ? 103 : 100;
            const FrameInfo& th = kit.at(id);
            piece(id, static_cast<float>(th.x), ty, static_cast<float>(th.w), thumb);
            return;
        }
        float kx, ky;
        if (img->type == "SCROLL") {
            kx = img->x.value(frame) + (img->width - k.w) / 2.0f;
            ky = img->y.value(frame) + t * (img->height - k.h);
        } else {
            // kcTaskCtrlSlider (0x632550): upright when the track is narrower than tall (Cyanotype's
            // backlog bar), else across; the knob travels the track less its own length, at
            // travel * value / |max - min| (0x633180: 0 at or below 0, the end at or past the range).
            const bool upright = sliderUpright(*img);
            const int32_t travel = upright ? img->height - k.h : img->width - k.w;
            const int32_t range = std::abs(img->sliderHi - img->sliderLo);
            const int32_t v = img->sliderValue;
            const int32_t at = v < 1 ? 0 : range <= v ? travel : std::abs(travel) * v * 100 / range / 100;
            kx = img->x.value(frame) + (upright ? (img->width - k.w) / 2.0f : static_cast<float>(at));
            ky = img->y.value(frame) + (upright ? static_cast<float>(at) : (img->height - k.h) / 2.0f);
        }
        FesQuad q;
        q.texture = file + "#" + std::to_string(fit->second.begin()->first);
        q.alpha = alpha * std::clamp(opacity(*img, frame), 0, 255) / 255.0f;
        const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
        for (int n = 0; n < 4; ++n) world.apply(kx + xs[n] * k.w, ky + ys[n] * k.h, q.x[n], q.y[n]);
        out.push_back(std::move(q));
    };
    // SCROLL2's thumb (0x631390): one end, the middle stretched to the thumb's length less the ends,
    // the other end, along the track from its start (and a piece +3 centred on it, if the file has
    // one); the object's opacity is the thumb's (message 0x176).
    auto scroll2 = [&](const Object* img, const Affine& world, float alpha) {
        Scroll2Box b;
        if (!scroll2Box(*img, b)) return;
        const std::string file = str(img->file);
        const int32_t state = dragging_ == img ? 300 : scrollHover_ == img ? 200 : 0;
        const float a = alpha * std::clamp(opacity(*img, frame), 0, 255) / 255.0f;
        const int32_t pos = scroll2Thumb(*img, b);
        const float ox = static_cast<float>(img->x.value(frame)) + (b.vertical ? 0.0f : static_cast<float>(pos));
        const float oy = static_cast<float>(img->y.value(frame)) + (b.vertical ? static_cast<float>(pos) : 0.0f);
        auto quad = [&](int32_t id, float x, float y, float w, float h) {
            if (w <= 0 || h <= 0) return;
            FesQuad q;
            q.texture = file + "#" + std::to_string(id);
            q.alpha = a;
            const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
            for (int n = 0; n < 4; ++n) world.apply(x + xs[n] * w, y + ys[n] * h, q.x[n], q.y[n]);
            out.push_back(std::move(q));
        };
        float at = 0;
        for (int32_t piece : {1, 0, 2}) {
            const FrameInfo* f = frameInfo(file, state + piece);
            if (!f) continue;
            const float own = static_cast<float>(b.vertical ? f->h : f->w);
            const float len = piece == 0 ? static_cast<float>(b.thumb - b.caps) : own;
            if (b.vertical) quad(state + piece, ox, oy + at, static_cast<float>(f->w), len);
            else quad(state + piece, ox + at, oy, len, static_cast<float>(f->h));
            at += len;
        }
        if (const FrameInfo* g = frameInfo(file, state + 3)) {
            const float mid = static_cast<float>((b.thumb >> 1) - ((b.vertical ? g->h : g->w) >> 1));
            if (b.vertical) quad(state + 3, ox + g->x, oy + mid + g->y, static_cast<float>(g->w), static_cast<float>(g->h));
            else quad(state + 3, ox + mid + g->x, oy + g->y, static_cast<float>(g->w), static_cast<float>(g->h));
        }
    };
    // Text in a box (STRING objects, backlog entries).
    const Object* textObj = nullptr;  // the STRING / LOGSTRING being drawn (its font)
    auto textBox = [&](const Affine& world, float x, float y, float w, float h, const std::string& text, float size,
                       int lines, uint32_t color, float alpha, int align = 0, float pitch = 0) {
        if (text.empty() || w <= 0 || h <= 0) return;
        FesQuad q;
        q.alpha = alpha;
        const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
        for (int k = 0; k < 4; ++k) world.apply(x + xs[k] * w, y + ys[k] * h, q.x[k], q.y[k]);
        q.text = text;
        q.textSize = size;
        q.color = color ? color : 0xFFFFFFFF;
        // The layout colour's edge (`layout color r g b <edge r g b> ...`); black when unset.
        q.outlineColor = 0xFF000000;
        if (textObj) {
            if (auto e = textObj->colors.find("color#edge"); e != textObj->colors.end()) q.outlineColor = e->second;
        }
        q.maxLines = std::max(1, lines);
        q.align = align;
        q.lineHeight = pitch;
        if (textObj) {
            q.font = fontOf(*textObj);
            q.proportional = !textObj->fontFixed;
            styleOf(*textObj, q);
        }
        out.push_back(std::move(q));
    };
    // Each quad keeps its container's priority, for merging with a child script's (below).
    size_t tagged = 0;
    int32_t layer = 0;
    auto tag = [&](int32_t next) {
        for (; tagged < out.size(); ++tagged) out[tagged].layer = layer;
        layer = next;
    };
    // An IMGWND with a size (cs2.exe 0x563290 -> 0x590cc0): a window that size built from its
    // image's frames 0-8 (ids): the corners 0-3 (top left, top right, bottom left, bottom right) at
    // the window's corners, the left and right edges 4 / 5 and the top and bottom 6 / 7 repeated
    // between them, the middle 8 repeated over the rest; the last repeat of each is cut at the far
    // side. Frame offsets play no part.
    auto imgwnd = [&](const Object* img, const Affine& world, float calpha) {
        const float a = calpha * std::clamp(opacity(*img, frame), 0, 255) / 255.0f;
        if (a <= 0) return;
        const std::string file = str(img->file);
        const Affine t = world * transform(*img, frame);
        const float W = static_cast<float>(img->width), H = static_cast<float>(img->height);
        auto piece = [&](int32_t id, float x, float y, float w, float h, float fw, float fh) {
            if (w <= 0 || h <= 0) return;
            FesQuad q;
            q.texture = file + "#" + std::to_string(id);
            q.alpha = a;
            q.u1 = w / fw, q.v1 = h / fh;
            const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
            for (int k = 0; k < 4; ++k) t.apply(x + xs[k] * w, y + ys[k] * h, q.x[k], q.y[k]);
            out.push_back(std::move(q));
        };
        auto size = [&](int32_t id, float& w, float& h) {
            const FrameInfo* f = frameInfo(file, id);
            if (!f) return false;
            w = static_cast<float>(f->w), h = static_cast<float>(f->h);
            return w > 0 && h > 0;
        };
        float w, h;
        if (size(0, w, h)) piece(0, 0, 0, w, h, w, h);
        if (size(1, w, h)) piece(1, W - w, 0, w, h, w, h);
        if (size(2, w, h)) piece(2, 0, H - h, w, h, w, h);
        if (size(3, w, h)) piece(3, W - w, H - h, w, h, w, h);
        if (size(4, w, h))
            for (float y = h; y < H - h; y += h) piece(4, 0, y, w, std::min(h, H - y - h), w, h);
        if (size(5, w, h))
            for (float y = h; y < H - h; y += h) piece(5, W - w, y, w, std::min(h, H - y - h), w, h);
        if (size(6, w, h))
            for (float x = w; x < W - w; x += w) piece(6, x, 0, std::min(w, W - x - w), h, w, h);
        if (size(7, w, h))
            for (float x = w; x < W - w; x += w) piece(7, x, H - h, std::min(w, W - x - w), h, w, h);
        if (size(8, w, h))
            for (float y = h; y < H - h; y += h)
                for (float x = w; x < W - w; x += w) piece(8, x, y, std::min(w, W - x - w), std::min(h, H - y - h), w, h);
    };
    for (const Object* c : containers) {
        if (drawOnly_ && c->name != *drawOnly_) continue;
        tag(c->pri);
        // A container with a VIEWPORT shows only inside that object's rectangle (Happiness 2's
        // phone: the messages in the screen of the phone). Fitted: the rectangle taken on the screen.
        const size_t first = out.size();
        struct ClipAfter {
            std::vector<FesQuad>& out;
            size_t first;
            const Object* vp;
            uint64_t frame;
            ~ClipAfter() {
                if (!vp) return;
                for (size_t k = first; k < out.size(); ++k) {
                    out[k].clip = true;
                    out[k].clipX = static_cast<float>(vp->x.value(frame));
                    out[k].clipY = static_cast<float>(vp->y.value(frame));
                    out[k].clipW = static_cast<float>(vp->width);
                    out[k].clipH = static_cast<float>(vp->height);
                }
            }
        } clipAfter{out, first, [&]() -> const Object* {
                        if (c->viewport.empty()) return nullptr;
                        auto it = byName_.find(c->viewport);
                        return it == byName_.end() ? nullptr : &objects_[it->second];
                    }(), frame};
        if (c->type == "SCROLL") {
            if (c->disp) knob(c, Affine{}, 1.0f);
            continue;
        }
        bool loose = !c->container();  // a screen-level image: drawn as its own only item
        float calpha = loose ? 1.0f : worldAlpha(*c, frame);
        if (calpha <= 0) continue;
        Affine world = loose ? Affine{} : worldTransform(*c, frame);
        if (!loose && c->colorFill) {
            solid(world, static_cast<float>(c->width), static_cast<float>(c->height), c->color, calpha);
        }
        textObj = c;
        if (drawStrings_ && c->type == "STRING" && !c->text.empty() && !hostStrings_.count(c->name)) {
            FesQuad q = stringBox(*c, frame);
            if (q.x[1] > q.x[0] && q.y[2] > q.y[1]) {
                for (int k = 0; k < 4; ++k) {
                    const float lx = q.x[k], ly = q.y[k];
                    world.apply(lx, ly, q.x[k], q.y[k]);
                }
                q.alpha = calpha;
                out.push_back(std::move(q));
            }
        }
        if (drawStrings_ && c->type == "LOGSTRING" && c->logBound) {
            // The backlog: `COUNT` entries per page from the first shown entry. The object's rectangle
            // is one entry's block; the following blocks stack below it.
            const auto* log = host_ ? host_->backlog() : nullptr;
            float blockH = static_cast<float>(c->height);
            float size = static_cast<float>(c->textSizes[2]);
            // Lines are the font size plus the layout's `move` spacing apart.
            float pitch = size + static_cast<float>(c->moveY);
            for (int32_t i = 0; log && i < c->logCount; ++i) {
                size_t e = static_cast<size_t>(c->logPos + i);
                if (e >= log->size()) break;
                const auto& entry = (*log)[e];
                // Colours: voiced entries use the v* set; blockcolor 1 = highlighted (a*), 2 = playing (p*).
                int32_t mode = c->blockColor.count(i) ? c->blockColor.at(i) : 0;
                std::string key = std::string(entry.voice.empty() ? "" : "v") + (mode == 1 ? "a" : mode == 2 ? "p" : "") + "color";
                auto col = c->colors.find(key);
                if (col == c->colors.end()) col = c->colors.find("color");
                // Block i is frame (POS2 id + i) of the layout image when it has one, else stacked below.
                float by = i * blockH;
                if (const FrameInfo* bf = c->pos2Id >= 0 ? frameInfo(c->pos2File, c->pos2Id + i) : nullptr) {
                    by = static_cast<float>(bf->y - c->y.value(frame));
                    blockH = static_cast<float>(bf->h);
                }
                int lines = std::max(1, static_cast<int>((blockH - c->margin[1] - c->margin[3]) / pitch));
                // Entries wrap at the block's full width (the original's backlog does; `margin`'s third
                // value doesn't narrow it).
                const size_t before = out.size();
                textBox(world, static_cast<float>(c->margin[0]), by + c->margin[1],
                        static_cast<float>(c->width - c->margin[0]), lines * pitch, entry.text, size, lines,
                        col == c->colors.end() ? 0 : col->second, calpha, 0, pitch);
                // The entry's sub-language text (hist_B.fes `log remake <main> <sub>`): its own string
                // object under the main one, at shift_sub past the main text's height (0x75fe90).
                if (out.size() > before && !entry.sub.empty() && c->logSub) {
                    out.back().subText = entry.sub;
                    out.back().subFont = c->subFontName;
                    out.back().subX = static_cast<float>(c->shiftSubX);
                    out.back().subY = static_cast<float>(c->shiftSubY);
                }
            }
        }
        std::vector<const Object*> items;
        if (loose) {
            if (c->disp) items.push_back(c);
        } else {
            auto collect = [&](auto&& inPlane) {
                for (const auto& o : objects_) {
                    if ((isDrawableType(o.type) || o.type == "SLIDER" || o.type == "SCROLL" || o.type == "SCROLL2" ||
                         o.type == "SAVEPANEL" || o.type == "IMGWND" || o.type == "BGTABLE2" || o.type == "CHTABLE3") && o.disp &&
                        !o.mask && !unmade(o) && inPlane(o)) {
                        items.push_back(&o);
                    }
                }
            };
            // Objects placed there for want of a plane draw only when they show something of their own,
            // as on the screen (not user-font sources such as `uf sys_userfont`, whose glyphs the text
            // takes; a name image until `setid` picks the speaker's frame).
            collect([&](const Object& o) {
                return o.plane == c->name && (!o.originPlane || o.colorFill || o.frame() >= 0);
            });
            // Planes show an area of video memory: one with nothing of its own that shares its VRAM with
            // another shows that plane's images (Cyanotype's splash puts img_sw in pl_sp and shows pl_sw).
            bool own = std::any_of(objects_.begin(), objects_.end(), [&](const Object& o) { return o.plane == c->name; });
            if (!own && vramPlanes_ && c->type == "PLANE" && c->vramX >= 0) {
                collect([&](const Object& o) {
                    auto pit = byName_.find(o.plane);
                    if (pit == byName_.end()) return false;
                    const Object& p = objects_[pit->second];
                    return &p != c && p.type == "PLANE" && p.vramX == c->vramX && p.vramY == c->vramY;
                });
            }
        }
        std::stable_sort(items.begin(), items.end(), [](const Object* a, const Object* b) { return a->pri < b->pri; });
        for (const Object* img : items) {
            if (img->type == "IMGWND" && img->width > 0 && img->height > 0) {
                imgwnd(img, world, calpha);
                continue;
            }
            if (img->type == "SAVEPANEL") {
                drawSavePanel(*img, world, calpha * std::clamp(opacity(*img, frame), 0, 255) / 255.0f, frame, out);
                continue;
            }
            if (img->type == "SLIDER" || img->type == "SCROLL") {
                knob(img, world, calpha);
                continue;
            }
            if (img->type == "SCROLL2") {
                scroll2(img, world, calpha);
                continue;
            }
            if (img->colorFill) {
                float a = calpha * std::clamp(opacity(*img, frame), 0, 255) / 255.0f;
                solid(world * transform(*img, frame), static_cast<float>(img->width), static_cast<float>(img->height),
                      img->color, a);
                continue;
            }
            const std::array<int16_t, 3> tone{static_cast<int16_t>(std::clamp(img->tone[0], 0, 10000)),
                                              static_cast<int16_t>(std::clamp(img->tone[1], 0, 10000)),
                                              static_cast<int16_t>(std::clamp(img->tone[2], 0, 10000))};
            if (img->type == "BGTABLE2" || img->type == "CHTABLE3") {
                // A table's picture (a CHTABLE3's parts, each where the canvas puts it): its frame of
                // the object's index, else its first.
                const float alpha = calpha * std::clamp(opacity(*img, frame), 0, 255) / 255.0f;
                if (alpha <= 0) continue;
                const Affine t = world * transform(*img, frame);
                for (const std::string& part : pictures(*img)) {
                    int32_t id = img->frame();
                    const FrameInfo* f = frameInfo(part, id);  // reads the picture's frames
                    if (!f) {
                        auto fit = frames_.find(part);
                        if (fit == frames_.end() || fit->second.empty()) continue;
                        id = fit->second.begin()->first;
                        f = &fit->second.begin()->second;
                    }
                    FesQuad q;
                    q.texture = part + "#" + std::to_string(id);
                    q.alpha = alpha;
                    q.tone = tone;
                    const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
                    for (int k = 0; k < 4; ++k) {
                        t.apply(static_cast<float>(f->x + xs[k] * f->w), static_cast<float>(f->y + ys[k] * f->h), q.x[k], q.y[k]);
                    }
                    out.push_back(std::move(q));
                }
                continue;
            }
            std::string file = str(img->file);
            int32_t id = img->frame();
            if (img->anmPlaying && img->anmPlayer) {
                // The script from `anm` on; a frame the image lacks keeps the last one it has (as
                // the stage's animated images do).
                img->anmPlayer->advance(frame);
                for (int32_t fid : img->anmPlayer->history()) {
                    if (frameInfo(file, fid)) {
                        id = fid;
                        break;
                    }
                }
            }
            if (id < 0 && !img->animate) {
                // Index -1 without IDN -1: the image's first frame (cs2.exe takes the whole image
                // there; not decoded further).
                frameInfo(file, 0);
                auto fit = frames_.find(file);
                if (fit != frames_.end() && !fit->second.empty()) id = fit->second.begin()->first;
            }
            if (img->animate && id < 0) {
                // Cycle through the image's frames (click-wait icons etc.), 6 frames each.
                frameInfo(file, 0);
                auto fit = frames_.find(file);
                if (fit != frames_.end() && !fit->second.empty()) {
                    size_t n = fit->second.size();
                    auto it = fit->second.begin();
                    std::advance(it, static_cast<long>((frame / 6) % n));
                    id = it->first;
                }
            }
            const FrameInfo* f = frameInfo(file, id);
            if (!f) continue;
            float alpha = calpha * std::clamp(opacity(*img, frame), 0, 255) / 255.0f;
            if (alpha <= 0) continue;
            Affine t = world * transform(*img, frame);
            FesQuad q;
            q.texture = file + "#" + std::to_string(id);
            q.alpha = alpha;
            q.tone = tone;
            const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
            for (int k = 0; k < 4; ++k) {
                t.apply(static_cast<float>(f->x + xs[k] * f->w), static_cast<float>(f->y + ys[k] * f->h), q.x[k], q.y[k]);
            }
            out.push_back(std::move(q));
        }
    }
    tag(0);
    if (drawOnly_) return out;
    if (child_) {
        // Priorities are one space across a script and the one it runs (Amakano's config.fes: its
        // plane 9000, the page's 10000, its confirmation dialog 20000 over both).
        auto more = child_->draw(frame);
        out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        std::stable_sort(out.begin(), out.end(), [](const FesQuad& a, const FesQuad& b) { return a.layer < b.layer; });
    }
    if (wiping(frame)) {
        // The running wipe, for the renderer: "@wipe:<start>:<frames>:<mask>", over everything.
        FesQuad q;
        q.texture = "@wipe:" + std::to_string(wipe_.start) + ":" + std::to_string(wipe_.frames) + ":" + wipe_.mask;
        q.layer = std::numeric_limits<int32_t>::max();
        if (wipe_.box) {  // a plane's `draw`: only inside its box
            q.clip = true;
            q.clipX = wipe_.x0, q.clipY = wipe_.y0;
            q.clipW = wipe_.x1 - wipe_.x0, q.clipH = wipe_.y1 - wipe_.y0;
        }
        out.push_back(std::move(q));
    }
    return out;
}

std::vector<FesQuad> FesInstance::drawContainer(const std::string& name, uint64_t frame) const {
    drawOnly_ = &name;
    auto out = draw(frame);
    drawOnly_ = nullptr;
    return out;
}

void FesInstance::drawSavePanel(const Object& o, const Affine& world, float alpha, uint64_t frame,
                                std::vector<FesQuad>& out) const {
    if (alpha <= 0) return;
    SaveStore* saves = host_ ? host_->saveStore() : nullptr;
    const SaveFile* save = saves ? saves->get(o.saveSlot) : nullptr;
    std::string file = str(o.file);
    float ox = static_cast<float>(o.x.value(frame)), oy = static_cast<float>(o.y.value(frame));
    auto rectQuad = [&](float x, float y, float w, float h) {
        FesQuad q;
        q.alpha = alpha;
        const float xs[4] = {0, 1, 1, 0}, ys[4] = {0, 0, 1, 1};
        for (int k = 0; k < 4; ++k) world.apply(ox + x + xs[k] * w, oy + y + ys[k] * h, q.x[k], q.y[k]);
        return q;
    };
    std::tm tm{};
    if (save) {
        std::time_t t = static_cast<std::time_t>(save->time);
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
    }
    for (const auto& p : o.parts) {
        if (!p.visible) continue;
        const FrameInfo* f = frameInfo(file, p.id);
        const bool number = p.kind == "NUM_O" || p.kind == "NUM_O2";
        if (!f && !number) continue;
        if (p.kind == "NORM") {
            FesQuad q = rectQuad(static_cast<float>(f->x), static_cast<float>(f->y), static_cast<float>(f->w),
                                 static_cast<float>(f->h));
            q.texture = file + "#" + std::to_string(p.id);
            out.push_back(std::move(q));
            continue;
        }
        if (!save && !(number && p.field.empty())) continue;  // the rest shows the bound save's data
        if (p.kind == "SIMAGE") {
            if (save->thumbnail.empty()) continue;
            FesQuad q = rectQuad(static_cast<float>(f->x), static_cast<float>(f->y), static_cast<float>(f->w),
                                 static_cast<float>(f->h));
            q.texture = "@save:" + std::to_string(o.saveSlot) + ":" + std::to_string(saves->version(o.saveSlot));
            out.push_back(std::move(q));
        } else if (p.kind == "SSTR") {
            auto it = save->strings.find(p.mask);
            if (it == save->strings.end() || it->second.empty()) continue;
            FesQuad q = rectQuad(static_cast<float>(f->x), static_cast<float>(f->y), static_cast<float>(f->w),
                                 static_cast<float>(f->h));
            q.text = it->second;
            q.textSize = static_cast<float>(p.size);
            q.color = p.color;
            q.outlineColor = p.outline;
            q.maxLines = p.lines;
            q.lineChars = p.lineChars;
            out.push_back(std::move(q));
        } else if (number) {
            // The panel's update (cs2.exe 0x565680, Nekopara 0x660100): the digit is the value, or
            // (value % (divisor x 10)) / divisor with a divisor; NUM_O takes the font object's
            // frame for it through the object's frame table (0x654330), NUM_O2 the frame of that
            // id. Its image goes where the panel image's frame <part id> is (0x52d2d0: that frame's
            // offset, 0 0 without one), at the font frame's own offset. Only an IMAGE or BUTTON
            // font draws (object types 2 and 6).
            int32_t value = p.value;
            if (p.field == "FLAG") value = save->resume.vars.get(p.flag);
            else if (p.field == "DATE_Y") value = tm.tm_year + 1900;
            else if (p.field == "DATE_M") value = tm.tm_mon + 1;
            else if (p.field == "DATE_D") value = tm.tm_mday;
            else if (p.field == "DATE_H") value = tm.tm_hour;
            else if (p.field == "DATE_N") value = tm.tm_min;
            else if (p.field == "DATE_S") value = tm.tm_sec;
            const int32_t digit = p.div != 0 ? (value % (p.div * 10)) / p.div : value;
            // The font object: by name, `name[n]` for an array's element (0x6669a0).
            auto fit = byName_.find(p.font);
            if (fit == byName_.end() && p.font.find('[') == std::string::npos) fit = byName_.find(p.font + "[0]");
            if (fit == byName_.end()) continue;
            const Object& font = objects_[fit->second];
            if (font.type != "IMAGE" && font.type != "BUTTON") continue;
            int32_t id = digit;
            if (p.kind == "NUM_O" && font.idTable) {
                if (digit < 0 || digit >= 32) id = 0;
                else if (auto it = font.ids.find(digit); it != font.ids.end()) id = it->second;
            }
            std::string fontFile = str(font.file);
            const FrameInfo* g = frameInfo(fontFile, id);
            if (!g) continue;
            const float px = f ? static_cast<float>(f->x) : 0.0f, py = f ? static_cast<float>(f->y) : 0.0f;
            FesQuad q = rectQuad(px + static_cast<float>(g->x), py + static_cast<float>(g->y), static_cast<float>(g->w), static_cast<float>(g->h));
            q.texture = fontFile + "#" + std::to_string(id);
            out.push_back(std::move(q));
        }
    }
}

void FesInstance::layoutMethod(Object& o, const std::vector<std::string>& args) {
    // `layout size a b c d e`, `rsize n`, `margin l t r b`, `color r g b ...` (and vcolor/acolor/...).
    if (args.empty()) return;
    auto arg = [&](size_t i) { return i < args.size() ? num(args[i]) : 0; };
    const std::string& what = args[0];
    if (what == "size") {
        // `size <a> [<b> <c> <d> <e>]` (cs2.exe 0x771180, grisaia.exe 0x5e9080): the first size fills
        // all five, the ones after it replace sizes 2-5 (`s_title layout size 20` is size 20 at every
        // level, the one the text is drawn at among them).
        if (args.size() >= 2) std::fill(std::begin(o.textSizes), std::end(o.textSizes), arg(1));
        for (int i = 1; i < 5 && i + 1 < static_cast<int>(args.size()); ++i) o.textSizes[i] = arg(i + 1);
        o.sizesSet = true;
    } else if (what == "rsize") {
        o.rubySize = arg(1);
        o.rubySet = true;
    } else if (what == "margin") {
        for (int i = 0; i < 4; ++i) o.margin[i] = arg(i + 1);
        o.marginSet = true;
    } else if (what == "scrolltype") {
        o.scrollType = arg(1);
        o.scrollSet = true;
    } else if (what == "wipewidth") {
        o.wipeWidth = arg(1);
        o.wipeSet = true;
    } else if (what == "frame") {
        o.frameSpeed = arg(1);
        o.frameSet = true;
    } else if ((what == "c_prof" || what == "prof" || what == "d_prof") && args.size() >= 2) {
        // The line-break sets (FesInstance::kKinsokuDefaults): `c_prof <n>` empties set n, `prof
        // <n> <chars>` adds the characters, `d_prof <n> <chars>` takes them out.
        const int32_t n = arg(1);
        if (n < 0 || n > 2) return;
        if (!o.kinsokuSet) {
            for (int i = 0; i < 3; ++i) o.kinsoku[i] = kKinsokuDefaults[i];
            o.kinsokuSet = true;
        }
        std::string& set = o.kinsoku[n];
        if (what == "c_prof") {
            set.clear();
            return;
        }
        if (args.size() < 3) return;
        const std::string chars = isValidUtf8(args[2]) ? args[2] : sjisToUtf8(args[2]);
        // One UTF-8 character at a time.
        for (size_t i = 0; i < chars.size();) {
            size_t len = 1;
            const auto c = static_cast<unsigned char>(chars[i]);
            if (c >= 0xF0) len = 4;
            else if (c >= 0xE0) len = 3;
            else if (c >= 0xC0) len = 2;
            const std::string ch = chars.substr(i, len);
            i += len;
            bool found = false;
            for (size_t j = 0; j < set.size();) {
                size_t l = 1;
                const auto d = static_cast<unsigned char>(set[j]);
                if (d >= 0xF0) l = 4;
                else if (d >= 0xE0) l = 3;
                else if (d >= 0xC0) l = 2;
                if (set.compare(j, l, ch) == 0) {
                    found = true;
                    if (what == "d_prof") set.erase(j, l);
                    break;
                }
                j += l;
            }
            if (what == "prof" && !found) set += ch;
        }
    } else if (what == "indent") {
        o.indent = arg(1);
        o.indentSet = true;
    } else if (what == "move") {
        o.moveX = arg(1);
        o.moveY = arg(2);
        o.moveSet = true;
    } else if (what == "font") {
        // The face name may contain spaces (`layout font 源柔ゴシックX Bold`) or be a string variable.
        std::string name;
        for (size_t i = 1; i < args.size(); ++i) name += (i > 1 ? " " : "") + str(args[i]);
        o.fontName = name;
        o.fontFollows = false;
    } else if (what == "fontind_sub" && args.size() >= 3) {
        o.subFontName = str(args[2]);
    } else if (what == "shift_sub") {
        // LOGSTRING only (cs2.exe 0x771180 -> 0x7614f0): where each entry's sub-language text goes,
        // from the entry's position plus the main text's height (0x75fe90 / 0x7603a0).
        o.shiftSubX = arg(1);
        o.shiftSubY = arg(2);
    } else if (what == "fontind" && args.size() >= 3) {
        o.fontName = str(args[2]);  // a font file of the game's (`fontind 1 GenJyuuGothicX-Bold.ttf`)
        o.fontFollows = false;
    } else if (what == "fontfix") {
        o.fontFixed = arg(1) != 0;
    } else if (what == "shadow") {
        // `shadow <right-down> <around>`: the text's edge switches, no longer the config's (a
        // missing one is 0). Nekopara cs2.exe 0x771180 -> 0x74e8f0 sets +0x3c0 / +0x3c4 and clears
        // the follow flag +0x520; while it is set, 0x74f690 copies them from the config's
        // FONT/shadow rightdown / around (+0xf8fc / +0xf8f8). Happiness 2: 0x67fe00 -> 0x659630.
        o.shadow[0] = std::max(0, arg(1));
        o.shadow[1] = std::max(0, arg(2));
    } else if (what == "weight") {
        // `weight <n>`: the text's weight (400 regular, 700 bold); -1 follows the config's again
        // (0x659cc0: +0x3e4, the follow flag +0x544).
        o.weight = arg(1) < 0 ? -1 : arg(1);
    } else if (what.ends_with("color") && args.size() >= 4) {
        auto c = [&](size_t i) { return static_cast<uint32_t>(std::clamp(arg(i), 0, 255)); };
        o.colors[what] = 0xFF000000u | (c(1) << 16) | (c(2) << 8) | c(3);
        // The second colour is the text's edge (black in most games; white around Love's Sweet
        // Garnish's dark brown text), and it is blue first: `layout` keeps all three colours alike
        // (cs2.exe 0x771435), but the glyph draw (0x696f70) hands the text colour to the font image
        // byte by byte, low first (0x6911f0), and its pixel writer reverses them into the BGRA
        // texture, so the first number is red; the edge goes whole (0x691210) and lands in the
        // texture as it is, so its first number is blue (the same calls in every readable exe).
        // Measured on PC (Nekopara, the original under Wine): meswnd's `0 33 67` edge is brown
        // (67 33 0), saveload's `67 33 0` text brown too.
        if (args.size() >= 7) o.colors[what + "#edge"] = 0xFF000000u | (c(6) << 16) | (c(5) << 8) | c(4);
    }
}

void FesInstance::logMethod(Object& o, const std::string& m, const std::vector<std::string>& args) {
    auto arg = [&](size_t i, int32_t def = 0) { return i < args.size() ? num(args[i], def) : def; };
    const auto* log = host_ ? host_->backlog() : nullptr;
    int32_t entries = log ? static_cast<int32_t>(log->size()) : 0;
    int32_t maxPos = std::max(0, entries - o.logCount);  // the last page starts here
    auto clampPos = [&](int32_t p) { return std::clamp(p, 0, maxPos); };
    if (m == "setlog") {
        // Binds the backlog; the newest page is shown first.
        o.logBound = arg(0, 1) != 0;
        o.logPos = maxPos;
    } else if (m == "max" && !args.empty()) {
        // The number of entries (0x76f010 -> 0x75fe40 -> 0x72e680), not the last page's start:
        // scripts hand it to their scroll bar with the page size (`scrl page 0 \10 4`).
        globals_.set(arg(0), entries);
    } else if (m == "getpos" && !args.empty()) {
        globals_.set(arg(0), o.logPos);
    } else if (m == "next") {
        o.logPos = clampPos(o.logPos + arg(0, 1));
    } else if (m == "rev") {
        o.logPos = clampPos(o.logPos - arg(0, 1));
    } else if (m == "page") {
        o.logPos = clampPos(o.logPos + arg(0, 1) * o.logCount);
    } else if (m == "pos") {
        o.logPos = clampPos(arg(0));
    } else if (m == "blockcolor" && args.size() >= 2) {
        o.blockColor[arg(0)] = arg(1);
    } else if (m == "blockinfo" && !args.empty()) {
        // `blockinfo <entry> <a> <b> <c> <flags>` (cs2.exe 0x775d80 -> 0x75e930): four of the entry's
        // values, each into its variable unless that is -1; the fourth is its flags & 4, set when
        // it has a voice (the backlogs show the voice button on it). The scripts ask only that one.
        int32_t i = arg(0);
        bool voiced = log && i >= 0 && i < entries && !(*log)[static_cast<size_t>(i)].voice.empty();
        if (args.size() >= 5 && arg(4) != -1) globals_.set(arg(4), voiced ? 4 : 0);
    } else if (m == "pathinfo" && args.size() >= 3) {
        // `pathinfo <entry> <path var> <step var>` (0x778fa0 -> 0x75e9b0 -> 0x72e7f0): where the
        // entry's message was, for `exit -2` to go back to; the engine then reads the variables
        // adv.xml's logret_pathindex / logret_step name (sscript 0x0c8795). An entry without one
        // leaves them alone. Here the place is the entry's own snapshot, so the step is 0.
        int32_t i = arg(0);
        if (log && i >= 0 && i < entries && (*log)[static_cast<size_t>(i)].point) {
            globals_.set(arg(1), static_cast<int32_t>((*log)[static_cast<size_t>(i)].id));
            globals_.set(arg(2), 0);
        }
    } else if (m == "remake") {
        // `remake <main> <sub>` (0x779730 -> 0x7614a0): the entries rebuilt in those languages;
        // each shows its sub line only if <sub> names one (0x713bd0: none for -1 or no argument).
        // The entries keep the text of the language they were shown in.
        o.logSub = arg(1, -1) >= 0;
    } else if (m == "lineinfo" && args.size() >= 3) {
        // lineinfo <entry> <string var> <var>: the entry's voice file, and whether it has one.
        int32_t i = arg(0);
        std::string voice = log && i >= 0 && i < entries ? (*log)[static_cast<size_t>(i)].voice : std::string();
        globals_.setStr(arg(1), voice);
        globals_.set(arg(2), voice.empty() ? 0 : 1);
    } else {
        unhandled("LOGSTRING." + m);
    }
}

std::string FesInstance::stringFont(const std::string& object, bool* fixed) const {
    const Object* o = nullptr;
    if (auto it = byName_.find(object); it != byName_.end()) o = &objects_[it->second];
    if (!o && object == "str") {
        for (const auto& x : objects_) {
            if (x.type == "STRING" && x.name != "name") {
                o = &x;
                break;
            }
        }
    }
    if (fixed) *fixed = !o || o->fontFixed;
    return o ? fontOf(*o) : std::string();
}

FesInstance::StringLayout FesInstance::stringLayout(const std::string& object) const {
    StringLayout out;
    const Object* o = nullptr;
    if (auto it = byName_.find(object); it != byName_.end()) o = &objects_[it->second];
    if (!o) return out;
    if (o->sizesSet) std::copy(std::begin(o->textSizes), std::end(o->textSizes), out.sizes);
    if (o->rubySet) out.ruby = o->rubySize;
    if (o->moveSet) out.moveX = o->moveX, out.moveY = o->moveY;
    if (o->marginSet) std::copy(std::begin(o->margin), std::end(o->margin), out.margin);
    if (o->indentSet) out.indent = o->indent;
    if (o->scrollSet) out.scrollType = o->scrollType;
    if (o->wipeSet) out.wipeWidth = o->wipeWidth;
    if (o->frameSet) out.frame = o->frameSpeed;
    out.mag = o->mag;
    out.proportional = !o->fontFixed;
    if (o->kinsokuSet) {
        out.kinsokuSet = true;
        for (int i = 0; i < 3; ++i) out.kinsoku[i] = o->kinsoku[i];
    }
    return out;
}

uint32_t FesInstance::stringColor(const std::string& object, const std::string& key) const {
    const Object* o = nullptr;
    if (auto it = byName_.find(object); it != byName_.end()) o = &objects_[it->second];
    if (!o && object == "str") {
        for (const auto& x : objects_) {
            if (x.type == "STRING" && x.name != "name") {
                o = &x;
                break;
            }
        }
    }
    if (!o) return 0;
    auto c = o->colors.find(key);
    return c == o->colors.end() ? 0 : c->second;
}

void FesInstance::scroll(int kind) {
    if (child_ && !child_->exited()) {
        child_->scroll(kind);
        return;
    }
    if (kind < 0 || kind > 3) return;
    scrollFlags_[kind] = 1;
    scrollAt_ = frame_;
    clickFromHold_ = false;  // an input event (+0x5c0): the update clears a held skip's -1
}

void FesInstance::setPosition(const std::string& object, int32_t x, int32_t y) {
    if (Object* o = find(object)) {
        o->x = Tween::constant(x);
        o->y = Tween::constant(y);
    }
}

bool FesInstance::objectPosition(const std::string& object, uint64_t frame, int32_t& x, int32_t& y, std::string* parent,
                                 std::string* type) const {
    auto it = byName_.find(object);
    if (it == byName_.end()) return false;
    const Object& o = objects_[it->second];
    x = static_cast<int32_t>(std::lround(o.x.value(frame)));
    y = static_cast<int32_t>(std::lround(o.y.value(frame)));
    if (parent) *parent = o.parent;
    if (type) *type = o.type;
    return true;
}

std::string FesInstance::objectText(const std::string& object) const {
    auto it = byName_.find(object);
    if (it == byName_.end()) return {};
    // Texts keep the script's bytes (Shift-JIS, or UTF-8 from a UTF-8 script), as drawing does.
    const std::string& t = objects_[it->second].text;
    return isValidUtf8(t) ? t : sjisToUtf8(t);
}

std::string FesInstance::objectFile(const std::string& object) const {
    auto it = byName_.find(object);
    return it == byName_.end() ? std::string() : str(objects_[it->second].file);
}

int FesInstance::objectAlign(const std::string& object) const {
    auto it = byName_.find(object);
    return it == byName_.end() ? 0 : objects_[it->second].textAlign;
}

void FesInstance::clearText(const std::string& object) {
    if (Object* o = find(object)) o->text.clear();
}

std::vector<std::string> FesInstance::objectNames() const {
    std::vector<std::string> out;
    for (const auto& o : objects_) out.push_back(o.name);
    return out;
}

void FesInstance::invoke(const std::string& label, const std::vector<int32_t>& args) {
    size_t start, end;
    if (!script_.block(label, start, end)) return;
    if (log && std::getenv("NYAN_FES_TRACE") && std::string(std::getenv("NYAN_FES_TRACE")) == "3") {
        std::string a;
        for (int32_t v : args) a += " " + std::to_string(v);
        log("fes " + name_ + ": invoke " + label + a + " f" + std::to_string(frame_));
    }
    for (size_t i = 0; i < args.size(); ++i) locals_.set(static_cast<int32_t>(i), args[i]);
    // Run the callback to completion on its own, then resume whatever the script was doing.
    auto savedPc = pc_, savedEnd = end_;
    auto savedStack = std::move(callStack_);
    auto savedState = state_;
    callStack_.clear();
    pc_ = start;
    end_ = end;
    state_ = State::Running;
    uint32_t guard = 0;
    while (!exited_ && ++guard < kMaxStatementsPerTick) {
        if (pc_ >= end_) {
            if (callStack_.empty()) break;
            std::tie(pc_, end_) = callStack_.back();
            callStack_.pop_back();
            continue;
        }
        const auto& st = script_.code[pc_++];
        const auto& t = st.tokens;
        if (!t.empty() && (t[0] == "wait" || t[0] == "next")) {
            if (t[0] == "next") {
                // Callbacks that jump take over the script's main flow (e.g. into a dialog loop).
                jump(t[1], false);
                return;
            }
            continue;  // callbacks don't block the engine
        }
        execute(st);
    }
    pc_ = savedPc;
    end_ = savedEnd;
    callStack_ = std::move(savedStack);
    state_ = savedState;
}

void FesInstance::fireEvent(const Object& o, const std::string& event) {
    std::string label = o.base + "." + event;
    if (!hasLabel(label)) label = o.name + "." + event;
    if (!hasLabel(label)) return;
    Object* saved = current_;
    current_ = &objects_[byName_.at(o.name)];
    invoke(label);
    current_ = saved;
}

const FesInstance::Object* FesInstance::hitTest(float x, float y) const {
    const Object* hit = nullptr;
    int64_t bestPri = INT64_MIN;
    for (const auto& o : objects_) {
        // Hidden-but-enabled buttons are clickable: scripts use them as hit maps over visible labels.
        if (o.type != "BUTTON" || !o.enable || unmade(o)) continue;
        // Buttons outside any plane are invisible hit maps in screen coordinates.
        auto pit = byName_.find(o.plane);
        const Object* c = pit == byName_.end() ? nullptr : &objects_[pit->second];
        if (c && worldAlpha(*c, frame_) <= 0) continue;
        // A button with no image is its SIZE (touch.fes's full-screen `btn_all 1280,720`).
        const FrameInfo* f = frameInfo(str(o.file), o.frame());
        FrameInfo sized{};
        if (!f && str(o.file).empty() && o.width > 0 && o.height > 0) {
            sized.w = o.width, sized.h = o.height;
            f = &sized;
        }
        if (!f) continue;
        Affine t = c ? worldTransform(*c, frame_) * transform(o, frame_) : transform(o, frame_);
        float x0, y0, x1, y1;
        t.apply(static_cast<float>(f->x), static_cast<float>(f->y), x0, y0);
        t.apply(static_cast<float>(f->x + f->w), static_cast<float>(f->y + f->h), x1, y1);
        if (x >= std::min(x0, x1) && x < std::max(x0, x1) && y >= std::min(y0, y1) && y < std::max(y0, y1)) {
            // The button's own priority decides (hit maps sit above their visible labels);
            // the container's priority only breaks ties.
            int64_t pri = static_cast<int64_t>(o.pri) * 1000000 + (c ? c->pri : 0);
            if (pri > bestPri) bestPri = pri, hit = &o;
        }
    }
    return hit;
}

int32_t FesInstance::layerOf(const Object& o) const {
    auto pit = byName_.find(o.plane);
    return pit == byName_.end() ? o.pri : objects_[pit->second].pri;
}

std::optional<int32_t> FesInstance::hitLayerAt(float x, float y) const {
    std::optional<int32_t> best;
    if (const Object* h = hitTest(x, y)) best = layerOf(*h);
    if (auto low = blockingLayer(); low && (!best || *low > *best)) best = low;
    if (child_ && !child_->exited()) {
        if (auto c = child_->hitLayerAt(x, y); c && (!best || *c > *best)) best = c;
    }
    return best;
}

std::optional<int32_t> FesInstance::blockingLayer() const {
    // Engine priorities are an object's pri + 20 (0x749160), the low button's its own value, so
    // in script terms it sits 20 below `basebtnpri` (65000: just under the dialog's own buttons).
    if (!lowButton_ || !lowButtonEnabled_ || !lowButtonPri_ || sendBtn_ != 0) return std::nullopt;
    return *lowButtonPri_ - 20;
}

bool FesInstance::childTakes(float x, float y) const {
    if (!child_ || child_->exited()) return false;
    if (child_->dragging()) return true;  // a slider drag stays with its script
    const Object* own = hitTest(x, y);
    auto theirs = child_->hitLayerAt(x, y);
    return !own || (theirs && *theirs >= layerOf(*own));
}

bool FesInstance::childBlocks(float x, float y) const {
    // A child whose low button keeps clicks takes everything under it, buttons or not.
    if (!child_ || child_->exited()) return false;
    auto low = child_->blockingLayer();
    if (!low) return false;
    const Object* own = hitTest(x, y);
    return !own || *low >= layerOf(*own);
}

// A script started with execfes runs beside its parent, and both take input: Amakano 2's config.fes
// keeps its page tabs and Back / Title / Quit buttons while config_basic / _sound / _etc show the
// page (its #WAIT loop and tab / sys handlers only run then). Priorities are one space (see draw):
// the button on the higher plane takes the click, else the child's, then the parent's.
void FesInstance::pointerMove(float x, float y) {
    if (childTakes(x, y)) {
        child_->pointerMove(x, y);
        if (child_->hovering() || childBlocks(x, y)) {
            if (hovered_) fireEvent(*std::exchange(hovered_, nullptr), "UNFOCUS");
            return;
        }
    } else if (child_ && !child_->exited()) {
        child_->clearHover();
    }
    mouseX_ = x;
    mouseY_ = y;
    pointerSeen_ = true;
    if (dragging_ && dragging_->type == "SCROLL2") {
        // Dragging the thumb (message 6): it follows the pointer within its travel and the value is
        // the nearest step; a change is posted to the script (0x631f50: SCRL_P).
        Scroll2Box b;
        float along, across;
        if (scroll2Box(*dragging_, b) && scroll2Local(*dragging_, x, y, along, across)) {
            dragging_->scrollThumb = std::clamp(static_cast<int32_t>(std::lround(along)) - dragging_->scrollGrab, 0,
                                                std::max(0, b.travel));
            const int32_t v = dragging_->sliderLo + scroll2Value(b, dragging_->scrollThumb);
            if (v != dragging_->sliderValue) {
                dragging_->sliderValue = v;
                fireEvent(*dragging_, "SCRL_P");
            }
        }
        return;
    }
    if (dragging_) {
        dragTo(*dragging_, x);
        return;
    }
    {
        // The thumb under the pointer shows its +200 pieces (message 0x40).
        scrollHover_ = nullptr;
        float along, across;
        if (Object* sc = scroll2At(x, y, along, across)) {
            Scroll2Box b;
            if (scroll2Box(*sc, b)) {
                const int32_t pos = scroll2Thumb(*sc, b);
                if (along >= pos && along < pos + b.thumb && across < b.cross) scrollHover_ = sc;
            }
        }
    }
    const Object* hit = hitTest(x, y);
    if (hit == hovered_) return;
    if (hovered_) fireEvent(*hovered_, "UNFOCUS");
    hovered_ = hit;
    if (hit) fireEvent(*hit, "FOCUS");
}

bool FesInstance::sliderTrack(const Object& o, float& x0, float& x1, float& y0, float& y1) const {
    Affine t;  // scroll bars without a plane are placed on the screen
    auto pit = byName_.find(o.plane);
    if (pit != byName_.end()) {
        const Object& c = objects_[pit->second];
        if (worldAlpha(c, frame_) <= 0) return false;
        t = worldTransform(c, frame_);
    } else if (o.type != "SCROLL") {
        return false;
    }
    t.apply(static_cast<float>(o.x.value(frame_)), static_cast<float>(o.y.value(frame_)), x0, y0);
    t.apply(static_cast<float>(o.x.value(frame_) + o.width), static_cast<float>(o.y.value(frame_) + o.height), x1, y1);
    return true;
}

bool FesInstance::fileExists(const std::string& path) const {
    // `archive.int/entry` (0x5d59d0 mode 3): the entry in a member of the archive group of that
    // name. The update archives patch every group (as AssetIndex has them): LSG's flow script asks
    // for a scene that only its update00.int holds. Anything else: an asset.
    if (size_t slash = path.find('/'); slash != std::string::npos) {
        const std::string archive = AssetIndex::normalizeName(path.substr(0, slash));
        const std::string entry = AssetIndex::normalizeName(path.substr(slash + 1));
        for (const auto& a : assets_.archives()) {
            const std::string name = AssetIndex::normalizeName(a->name());
            const bool update = name.starts_with("update") && name.ends_with(".int");
            if (name != archive && !update) continue;
            for (const auto& e : a->entries()) {
                if (AssetIndex::normalizeName(e.name) == entry) return true;
            }
        }
        return false;
    }
    return assets_.find(path).has_value();
}

std::optional<Bytes> FesInstance::readFile(const std::string& path) const {
    if (size_t slash = path.find('/'); slash != std::string::npos) {
        const std::string archive = AssetIndex::normalizeName(path.substr(0, slash));
        const std::string entry = AssetIndex::normalizeName(path.substr(slash + 1));
        const KifArchive* found = nullptr;
        const KifEntry* foundEntry = nullptr;
        for (const auto& a : assets_.archives()) {
            const std::string name = AssetIndex::normalizeName(a->name());
            const bool update = name.starts_with("update") && name.ends_with(".int");
            if (name != archive && !update) continue;
            for (const auto& e : a->entries()) {
                if (AssetIndex::normalizeName(e.name) != entry) continue;
                if (!found || update) found = a.get(), foundEntry = &e;
            }
        }
        if (!found) return std::nullopt;
        return found->read(*foundEntry);
    }
    auto ref = assets_.find(path);
    if (!ref) return std::nullopt;
    return ref->archive->read(*ref->entry);
}

void FesInstance::loadTable(Object& o) {
    TableData& t = tables_[o.name];
    t.rows.clear();
    t.backgrounds = 0;
    t.sections.clear();
    const std::optional<Bytes> data = readFile(str(o.table));
    if (!data) {
        unhandled("missing table " + str(o.table));
        return;
    }
    const std::string text = decodeGameText(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()));
    if (o.type == "BGTABLE2") {
        // kcFEScriptObjBgTable2's list (0x6fc4e0 -> 0x727d90): the CSV, and how many rows are
        // backgrounds (first field "○").
        t.rows = parseCsv(text);
        for (const auto& row : t.rows) t.backgrounds += !row.empty() && row[0] == kBackgroundMark;
        return;
    }
    // kcFEScriptObjChTable3's list (0x6fcdf0 -> 0x729c50): a line starting with '#' names the
    // section the following lines make (the rest of the line, as written); each other line's first
    // two space-separated words are an entry's name part and the section its choice leads to (none
    // after one word). A section is kept once it has an entry, the first of a name only.
    std::string section;
    std::vector<std::pair<std::string, std::string>> entries;
    for (const std::string& line : textLines(text)) {
        if (!line.empty() && line[0] == '#') {
            if (!entries.empty()) t.sections.emplace(section, std::move(entries));
            entries.clear();
            section = line.substr(1);
            continue;
        }
        const std::vector<std::string> words = pieces(line, ' ');
        if (!words.empty()) entries.emplace_back(words[0], words.size() > 1 ? words[1] : std::string());
    }
    if (!entries.empty()) t.sections.emplace(section, std::move(entries));
}

bool FesInstance::tableMethod(Object& o, const std::string& m, const std::vector<std::string>& args) {
    auto arg = [&](size_t i, int32_t def = 0) { return i < args.size() ? num(args[i], def) : def; };
    const bool bg = o.type == "BGTABLE2";
    TableData& t = tables_[o.name];
    // The choices from a CHTABLE3's command (up to 16, as the handlers read them).
    auto choices = [&](size_t from) {
        std::vector<int32_t> v;
        for (size_t i = from; i < args.size() && v.size() < 16; ++i) v.push_back(num(args[i]));
        return v;
    };
    auto section = [&](const std::string& name) {
        auto it = t.sections.find(name);
        return it == t.sections.end() ? nullptr : &it->second;
    };
    if (m == "max") {
        // `max <var> [<choice>...]` (Amakano 2's cs2.exe 0x708c70): a BGTABLE2's backgrounds
        // (0x727ea0); a CHTABLE3's entries in the section the choices lead to from _TOP_, 0 if
        // one is out of range or leads nowhere (0x72a590).
        int32_t n = t.backgrounds;
        if (!bg) {
            n = 0;
            auto* s = section("_TOP_");
            for (int32_t c : choices(1)) {
                if (!s) break;
                if (static_cast<uint32_t>(c) >= s->size()) {
                    s = nullptr;
                    break;
                }
                s = section((*s)[static_cast<size_t>(c)].second);
            }
            if (s) n = static_cast<int32_t>(s->size());
        }
        globals_.set(arg(0), n);
        return true;
    }
    if (bg && m == "exist") {
        // `exist <background> <picture> <var>` (0x708ab0 -> 0x727eb0): 1 if the background has
        // that picture.
        globals_.set(arg(2), backgroundPicture(t.rows, t.backgrounds, arg(0), arg(1)).empty() ? 0 : 1);
        return true;
    }
    if (m == "load") {
        // BGTABLE2 `load <background> <picture>` (0x705650 -> 0x6fc540 -> 0x727f30): that
        // picture; a background or picture the list lacks leaves the image as it is.
        if (bg) {
            std::string name = backgroundPicture(t.rows, t.backgrounds, arg(0), arg(1));
            if (!name.empty()) o.file = name;
            return true;
        }
        // CHTABLE3 `load <choice>...` (0x6fce60 -> 0x72a230): from _TOP_, each choice's entry adds
        // its name part (mergeName) and leads to the next section, until the choices run out, one
        // is out of range or a section is missing. `baseopt` goes before the name's first comma
        // (when one is past the start), and every "@" left becomes "0": "Base,a,0,b", a stand's
        // parts as the image loader takes them. Without _TOP_ the image stays.
        auto* s = section("_TOP_");
        if (!s) return true;
        std::string name;
        for (int32_t c : choices(0)) {
            if (!s || static_cast<uint32_t>(c) >= s->size()) break;
            const auto& entry = (*s)[static_cast<size_t>(c)];
            name = mergeName(name, entry.first);
            s = section(entry.second);
        }
        if (const size_t comma = name.find(','); comma != std::string::npos && comma != 0) name.insert(comma, t.prefix);
        std::replace(name.begin(), name.end(), '@', '0');
        o.file = name;
        return true;
    }
    if (bg && m == "tonetbl") {
        // `tonetbl <file>` (0x70a1e0 -> 0x6fc5f0 -> 0x728fb0): the old table goes; the file's
        // comments go (0x723220), then every space and tab, then the empty lines. A line with
        // `#name(r,g,b)` starts a tone (name from the '#'; a part left out keeps the last tone's
        // value; before any, __UNKNOWN__ at 100 100 100); any other line is a picture of the tone
        // above it. The first of a name counts; names in lower case.
        if (args.empty()) return true;
        t.toneTable = false;
        t.toneOf.clear();
        t.tones.clear();
        const std::optional<Bytes> data = readFile(str(args[0]));
        if (!data) return true;
        t.toneTable = true;
        std::vector<std::string> lines =
            textLines(decodeGameText(std::string_view(reinterpret_cast<const char*>(data->data()), data->size())));
        stripComments(lines);
        std::string tone = "__UNKNOWN__";
        std::array<int32_t, 3> rgb{100, 100, 100};
        for (std::string line : lines) {
            std::erase_if(line, [](char c) { return c == ' ' || c == '\t'; });
            if (line.empty()) continue;
            // 0x728df0: a line is a tone once it has '#' and then '('.
            const size_t hash = line.find('#');
            const size_t open = hash == std::string::npos ? std::string::npos : line.find('(', hash);
            if (open != std::string::npos) {
                tone = line.substr(hash, open - hash);
                size_t at = open + 1;
                for (int k = 0; k < 3; ++k) {
                    const size_t end = line.find(k < 2 ? ',' : ')', at);
                    if (end == std::string::npos) break;
                    rgb[static_cast<size_t>(k)] = std::atoi(line.substr(at, end - at).c_str());
                    at = end + 1;
                }
                continue;
            }
            // 0x7285f0: the tone's values and the picture's tone, each unless already there.
            t.tones.emplace(toLowerAscii(tone), rgb);
            t.toneOf.emplace(toLowerAscii(line), toLowerAscii(tone));
        }
        return true;
    }
    if (bg && m == "gettonetbl") {
        // `gettonetbl <r var> <g var> <b var>` (0x70a2c0 -> 0x6fc720 -> 0x728a70): the tone of the
        // picture shown, 100 100 100 without a table or a tone for it; each variable given is set.
        std::array<int32_t, 3> rgb{100, 100, 100};
        if (t.toneTable) {
            if (auto it = t.toneOf.find(toLowerAscii(o.file)); it != t.toneOf.end()) {
                if (auto tt = t.tones.find(it->second); tt != t.tones.end()) rgb = tt->second;
            }
        }
        for (size_t k = 0; k < 3; ++k) {
            const int32_t var = arg(k, -1);
            if (var != -1) globals_.set(var, rgb[k]);
        }
        return true;
    }
    if (!bg && m == "baseopt") {
        // `baseopt <text>` (0x70f5b0 -> 0x6fcf80 -> 0x72a750): what `load` puts before the name's
        // first comma.
        t.prefix = args.empty() ? std::string() : str(args[0]);
        return true;
    }
    return false;
}

std::vector<std::string> FesInstance::pictures(const Object& o) const {
    const std::string file = str(o.file);
    if (o.type != "CHTABLE3") return {file};
    // A CHTABLE3's name is a stand's parts on one canvas (Base,a,b,...), loaded as the scene's
    // stand images are.
    auto it = parts_.find(file);
    if (it == parts_.end()) it = parts_.emplace(file, ScenePlayer::spriteParts(assets_, file)).first;
    return it->second;
}

int32_t FesInstance::cgTableGroup(const std::string& tok) {
    if (tok.empty() || tok[0] == '$' || tok[0] == '\\' || (tok[0] >= '0' && tok[0] <= '9')) return num(tok);
    return cgTableIndex(str(tok));
}

int32_t FesInstance::cgTableIndex(const std::string& name) const {
    if (!cgTable_) return -1;
    const std::string want = toLowerAscii(name);
    for (size_t i = 0; i < cgTable_->size(); ++i) {
        if (toLowerAscii((*cgTable_)[i].name) == want) return static_cast<int32_t>(i);
    }
    return -1;
}

bool FesInstance::cgTableCount(int32_t group, int32_t& seen, int32_t& total) const {
    // 0x641df0 / 0x642340 / 0x642230: each entry of the group looked up in the records.
    if (!cgTable_ || group < 0 || static_cast<size_t>(group) >= cgTable_->size()) return false;
    const auto& g = (*cgTable_)[static_cast<size_t>(group)];
    seen = 0;
    for (const auto& img : g.images) seen += host_ && host_->cgEntrySeen(img) ? 1 : 0;
    total = static_cast<int32_t>(g.images.size());
    return true;
}

bool FesInstance::sliderUpright(const Object& o) {
    // A SLIDER runs along its track's longer side (kcTaskCtrlSlider 0x632550: upright when the
    // track is narrower than tall).
    return o.type == "SLIDER" && o.width < o.height;
}

int32_t FesInstance::sliderTop(const Object& o) {
    // A scroll bar's position stops one view short of its range: kcTaskCtrlScroll clamps it to
    // 0..|max - min| - visible and moves the thumb over that (0x6305b0; the down arrow, 0x62e820,
    // stops there), so a backlog's `page 0 <entries> <per page>` ends on the last page.
    // SCROLL2 the same: `pos` clamps to min..max (0x631860, message 0x173), then the thumb's
    // placement to 0..|max - min| - visible (0x632030).
    if (o.type != "SCROLL" && o.type != "SCROLL2") return o.sliderHi;
    return o.sliderLo + std::max(0, std::abs(o.sliderHi - o.sliderLo) - o.sliderPage);
}

bool FesInstance::scroll2Box(const Object& o, Scroll2Box& b) const {
    // kcTaskCtrlScroll2 (0x630cf0, 0x630b00): the track is the POS2 frame, upright when narrower
    // than tall. The thumb is pieces 1, 0 and 2 of FILE (an end, the middle, the other end; ids
    // +200 while the pointer is over it, +300 while it is held); it is the track's length times
    // visible / range but no shorter than its ends, or the whole track when one view shows it all.
    const std::string file = str(o.file);
    const FrameInfo* c1 = frameInfo(file, 1);
    const FrameInfo* body = frameInfo(file, 0);
    const FrameInfo* c2 = frameInfo(file, 2);
    if (!c1 && !body && !c2) return false;
    b.vertical = o.width < o.height;
    auto along = [&](const FrameInfo* f) { return f ? (b.vertical ? f->h : f->w) : 0; };
    auto across = [&](const FrameInfo* f) { return f ? (b.vertical ? f->w : f->h) : 0; };
    b.len = b.vertical ? o.height : o.width;
    b.caps = along(c1) + along(c2);
    const int32_t range = std::abs(o.sliderHi - o.sliderLo);
    b.thumb = o.sliderPage < range && range > 0 ? std::max(b.len * o.sliderPage / range, b.caps) : b.len;
    b.travel = b.len - b.thumb;
    b.span = range - o.sliderPage;
    b.cross = std::max({across(c1), across(body), across(c2)});
    return true;
}

int32_t FesInstance::scroll2Thumb(const Object& o, const Scroll2Box& b) const {
    // Where the thumb sits for the value (0x632030), unless it is being dragged.
    if (dragging_ == &o) return o.scrollThumb;
    const int32_t v = o.sliderValue - o.sliderLo;
    if (v < 1 || b.span <= 0) return 0;
    if (b.span <= v) return b.travel;
    return b.travel * 100 / b.span * v / 100;
}

int32_t FesInstance::scroll2Value(const Scroll2Box& b, int32_t thumb) {
    // The value for a thumb position: the nearest step (0x630c90).
    if (b.span <= 0) return 0;
    const int32_t step = b.travel * 100 / b.span;
    int32_t k = 0;
    for (int32_t at = step / 2; k < b.span; ++k, at += step) {
        if (thumb * 100 < at) return k;
    }
    return k;
}

bool FesInstance::scroll2Local(const Object& o, float x, float y, float& along, float& across) const {
    float x0, x1, y0, y1;
    if (!sliderTrack(o, x0, x1, y0, y1) || x1 == x0 || y1 == y0) return false;
    const float u = (x - x0) / (x1 - x0) * static_cast<float>(o.width);
    const float v = (y - y0) / (y1 - y0) * static_cast<float>(o.height);
    const bool vertical = o.width < o.height;
    along = vertical ? v : u;
    across = vertical ? u : v;
    return true;
}

FesInstance::Object* FesInstance::scroll2At(float x, float y, float& along, float& across) {
    Object* best = nullptr;
    for (auto& o : objects_) {
        if (o.type != "SCROLL2" || !o.disp || !o.enable) continue;
        float a, c;
        if (!scroll2Local(o, x, y, a, c)) continue;
        const bool vertical = o.width < o.height;
        if (a < 0 || c < 0 || a >= (vertical ? o.height : o.width) || c >= (vertical ? o.width : o.height)) continue;
        if (!best || o.pri >= best->pri) best = &o, along = a, across = c;
    }
    return best;
}

const FesInstance::Object* FesInstance::sliderAt(float x, float y) const {
    for (const auto& o : objects_) {
        if ((o.type != "SLIDER" && o.type != "SCROLL") || !o.disp || !o.enable) continue;
        float x0, x1, y0, y1;
        if (!sliderTrack(o, x0, x1, y0, y1)) continue;
        if (x >= std::min(x0, x1) && x < std::max(x0, x1) && y >= std::min(y0, y1) - 6 && y < std::max(y0, y1) + 6) {
            return &o;
        }
    }
    return nullptr;
}

void FesInstance::dragTo(Object& o, float x) {
    float x0, x1, y0, y1;
    if (!sliderTrack(o, x0, x1, y0, y1)) return;
    bool vertical = o.type == "SCROLL" || sliderUpright(o);
    float a0 = vertical ? y0 : x0, a1 = vertical ? y1 : x1, p = vertical ? mouseY_ : x;
    if (a1 == a0) return;
    float t = std::clamp((p - a0) / (a1 - a0), 0.0f, 1.0f);
    int32_t v = o.sliderLo + static_cast<int32_t>(std::lround(t * (sliderTop(o) - o.sliderLo)));
    if (v == o.sliderValue) return;
    o.sliderValue = v;
    fireEvent(o, "SCRL_P");  // the script reads the new value with getpos
}

bool FesInstance::mouseDown(float x, float y, bool right) {
    if (childTakes(x, y) && (child_->mouseDown(x, y, right) || childBlocks(x, y))) return true;
    mouseX_ = x;
    mouseY_ = y;
    // A left press raises the click flag whatever it lands on (0x60e480, event 2).
    if (!right) skipClick_ = skipClickFromPress_ = true;
    if (!right) {
        float along, across;
        if (Object* sc = scroll2At(x, y, along, across)) {
            // A press on SCROLL2 (message 9): on the thumb it is held (if it can move); elsewhere on
            // the track the value moves a view towards the press, then the script hears of it.
            Scroll2Box b;
            if (!scroll2Box(*sc, b)) return true;
            const int32_t pos = scroll2Thumb(*sc, b);
            if (along >= pos && along < pos + b.thumb && across < b.cross) {
                if (b.travel > 0) {
                    dragging_ = sc;
                    sc->scrollGrab = static_cast<int32_t>(std::lround(along)) - pos;
                    sc->scrollThumb = pos;
                }
                return true;
            }
            int32_t v = sc->sliderValue;
            if (along < pos) v = std::max(0, v - sc->sliderPage);
            else v = std::min(v + sc->sliderPage, sc->sliderHi - sc->sliderPage);
            sc->sliderValue = std::clamp(v, std::min(sc->sliderLo, sliderTop(*sc)), std::max(sc->sliderLo, sliderTop(*sc)));
            fireEvent(*sc, "SCRL_P");
            return true;
        }
        if (const Object* sl = sliderAt(x, y)) {
            dragging_ = &objects_[byName_.at(sl->name)];
            dragTo(*dragging_, x);
            return true;
        }
    }
    pressed_ = hitTest(x, y);
    if (pressed_) fireEvent(*pressed_, right ? "PUSH_R" : "PUSH_L");
    return pressed_ != nullptr;
}

bool FesInstance::mouseUp(float x, float y, bool right) {
    if (childTakes(x, y) && (child_->mouseUp(x, y, right) || childBlocks(x, y))) return true;
    mouseX_ = x;
    mouseY_ = y;
    if (dragging_ && !right) {
        dragging_ = nullptr;
        return true;
    }
    const Object* hit = hitTest(x, y);
    if (log && std::getenv("NYAN_FES_TRACE")) {
        log("fes " + name_ + ": click at " + std::to_string(static_cast<int>(x)) + "," +
            std::to_string(static_cast<int>(y)) + " -> " + (hit ? hit->name : std::string("nothing")));
    }
    if (hit && hit == pressed_) fireEvent(*hit, right ? "RELEASE_R" : "RELEASE_L");
    // A click: the object under the pointer (or -1 for empty space) for one script step.
    int32_t id = hit ? hit->uid : -1;
    if (right) clickR_ = id;
    else clickL_ = id;
    clickAt_ = frame_;
    clickFromHold_ = false;
    pressed_ = nullptr;
    return hit != nullptr;
}

void FesInstance::clearHover() {
    if (child_) child_->clearHover();
    if (hovered_) fireEvent(*std::exchange(hovered_, nullptr), "UNFOCUS");
}

void FesInstance::cancelPress() {
    if (child_) child_->cancelPress();
    pressed_ = nullptr;
    dragging_ = nullptr;
    // A touch that became a gesture is no click (the engine only sees mouse presses): a flag it
    // raised that no update has used yet goes with it.
    if (skipClickFromPress_) skipClick_ = skipClickFromPress_ = false;
}

bool FesInstance::click(float x, float y, bool right) {
    mouseDown(x, y, right);
    return mouseUp(x, y, right);
}

void FesInstance::pressSkip() {
    if (child_ && !child_->exited()) {
        child_->pressSkip();
        return;
    }
    skipClick_ = true;
    skipClickFromPress_ = false;
}

void FesInstance::skipMotions() {
    // The engine's skip (Amakano 1's cs2.exe 0x601c10 on the script's root, its objects through
    // 0x601d80): every motion gets its stop flag (0x63aa90 -> 0x63b700: the motion's +0xa0, mot.int's
    // m[0x80000098]) and ends at its next update, as the scenes' `stop` (native 314, the same flag):
    // the one-shots take their end values ("time >= frames or stopped" adds the whole change),
    // mcMove one cycle on (its target), the other loops nothing (back to where they started) and
    // mcFade's opacity stays (it writes it). The screen wipes (`rwipe` / `rdraw`, a plane's `draw` /
    // `wipe`) are motion scripts too (0x631a60, 0x61f640, 0x61f900 -> 0x601b20) and end with them.
    if (log && std::getenv("NYAN_FES_TRACE")) log("fes " + name_ + ": skip at frame " + std::to_string(frame_));
    for (auto& o : objects_) {
        const int32_t held = opacity(o, frame_);
        bool fading = false;
        for (const Motion& m : o.motions) {
            if (m.kind == Motion::Kind::Fade) fading = true;
            if (m.kind == Motion::Kind::Move && m.loop) {
                o.x = Tween::constant(static_cast<int32_t>(std::lround(m.a[2])));
                o.y = Tween::constant(static_cast<int32_t>(std::lround(m.a[3])));
            }
        }
        // The one-shots at their ends, folded into the object's own values; the loops dropped.
        const std::vector<Motion> motions = std::move(o.motions);
        for (const Motion& m : motions) {
            if (m.loop) continue;
            o.motions = {m};
            foldMotions(o, std::max(frame_, m.start + m.period), false);
        }
        o.motions.clear();
        for (Tween* t : {&o.x, &o.y, &o.alpha, &o.scaleX, &o.scaleY}) {
            if (!t->done(frame_)) *t = Tween::constant(t->to);
        }
        if (fading) o.alpha = Tween::constant(held);
        o.delay = 0;
    }
    wipe_.frames = 0;
}

}  // namespace nyan::cs2
