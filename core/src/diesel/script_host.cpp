#include "nyan/diesel/script_host.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <set>
#include <utility>
#include <cstring>

#include "nyan/diesel/bytecode.hpp"
#include "nyan/diesel/input.hpp"

#include <squirrel.h>

#include <sqstdaux.h>
#include <sqstdblob.h>
#include <sqstdio.h>
#include <sqstdmath.h>
#include <sqstdstring.h>
#include <sqstdsystem.h>

#include "natives.hpp"
#include "natives_table.hpp"
#include "runtime.hpp"

namespace nyan::diesel {

namespace {

// The line-break tables as the exe's start-up builds them (muramasa_en.exe 0x446160, totono_en.exe
// 0x4616b0, phantom.exe 0x140054260): the vectors its code pushes constants onto, told apart by what
// they hold: 、。 begin the head-of-line table, ぁ the small kana's, 「 the end-of-line table. You
// and Me and Her's exe builds no small-kana table (and has the simpler rule). An exe without them
// (none of the three) keeps Muramasa's.
LineBreakTables lineBreakTables(const ExeInfo& exe) {
    LineBreakTables t;
    auto has = [](const std::vector<uint32_t>& v, uint32_t c) { return std::find(v.begin(), v.end(), c) != v.end(); };
    for (const auto& [addr, values] : pushedConstants(exe)) {
        (void)addr;
        if (t.headOfLine.empty() && has(values, 0x3001) && has(values, 0x3002)) t.headOfLine = values;
        else if (t.smallKana.empty() && has(values, 0x3041)) t.smallKana = values;
        else if (t.endOfLine.empty() && has(values, 0x300c)) t.endOfLine = values;
    }
    if (t.headOfLine.empty() && t.endOfLine.empty()) {
        t.headOfLine = {0x3001, 0x3002, 0xff0c, 0xff0e, 0xff1f, 0xff01, 0x309c, 0xff40, 0xff09, 0x3015, 0xff3d, 0xff5d, 0x3009, 0x300b,
                        0x300d, 0x300f, 0x3011, 0x2026, 0x30fc, 0x2015, 0x301c, 0x002c, 0x002e, 0x003f, 0x0021, 0x0029, 0x007d};
        t.smallKana = {0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x30f5, 0x30f6, 0x3083, 0x3085, 0x3087, 0x3063,
                       0x30a1, 0x30a3, 0x30a5, 0x30a7, 0x30a9, 0x30e3, 0x30e5, 0x30e7, 0x30c3, 0x30ee};
        t.endOfLine = {0x2018, 0x201c, 0xff08, 0x3014, 0xff3b, 0xff5b, 0x3008, 0x300a, 0x300c, 0x300e, 0x3010, 0x0028, 0x007b};
    }
    t.hangingRule = !t.smallKana.empty();
    return t;
}


constexpr uint32_t kOtNull = 0x01000001, kOtInteger = 0x05000002, kOtFloat = 0x05000004, kOtString = 0x08000010;

void printFunc(HSQUIRRELVM v, const SQChar* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    auto& h = host(v);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (h.log && !s.empty()) h.log("script: " + s);
}

// The call stack of the VM as text (innermost first).
std::string callStack(HSQUIRRELVM v) {
    std::string out;
    SQStackInfos si;
    for (SQInteger level = 1; SQ_SUCCEEDED(sq_stackinfos(v, level, &si)); ++level) {
        out += "  at ";
        out += si.funcname ? si.funcname : "?";
        out += " (";
        out += si.source ? si.source : "?";
        out += ":" + std::to_string(si.line) + ")\n";
    }
    return out;
}

SQInteger errorHandler(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string msg = "script error";
    if (sq_gettop(v) >= 2) {
        const SQChar* s = nullptr;
        if (SQ_SUCCEEDED(sq_getstring(v, 2, &s))) msg = std::string("script error: ") + s;
    }
    h.lastError = msg + "\n" + callStack(v);
    if (h.log) h.log(h.lastError);
    return 0;
}

// A native the scripts reach that Nyanroid lacks: notes it and returns the default of its return
// type (0, 0.0, "" or nothing). Its name and return type are the closure's free variables.
SQInteger stubNative(HSQUIRRELVM v) {
    const SQChar* name = nullptr;
    const SQChar* ret = nullptr;
    // Free variables follow the arguments, the last created first.
    sq_getstring(v, sq_gettop(v), &name);
    sq_getstring(v, sq_gettop(v) - 1, &ret);
    host(v).note(host(v).unhandled, name ? name : "?");
    std::string r = ret ? ret : "";
    if (r == "int") {
        sq_pushinteger(v, 0);
        return 1;
    }
    if (r == "float") {
        sq_pushfloat(v, 0.0f);
        return 1;
    }
    if (r == "string") {
        sq_pushstring(v, "", 0);
        return 1;
    }
    return 0;
}

SQInteger releaseObject(SQUserPointer p, SQInteger) {
    delete static_cast<NativeObject*>(p);
    return 1;
}

// Every native class's instance holds a native object made by the class's `make`.
using NativeMaker = NativeObject* (*)(HSQUIRRELVM v);

SQInteger nativeConstructor(HSQUIRRELVM v) {
    SQUserPointer up = nullptr;
    sq_getuserpointer(v, sq_gettop(v), &up);
    auto* make = static_cast<NativeMaker*>(up);
    NativeObject* obj = make && *make ? (*make)(v) : new NativeObject();
    sq_setinstanceup(v, 1, obj);
    sq_setreleasehook(v, 1, releaseObject);
    return 0;
}

void defineStub(HSQUIRRELVM v, const std::string& fullName, const char* name, const char* signature) {
    std::string sig = signature ? signature : "";
    std::string ret = sig.substr(0, sig.find('('));
    sq_pushstring(v, name, -1);
    sq_pushstring(v, fullName.c_str(), -1);
    sq_pushstring(v, ret.c_str(), -1);
    sq_newclosure(v, stubNative, 2);
    sq_setnativeclosurename(v, -1, name);
    sq_createslot(v, -3);
}

struct ReadCursor {
    const uint8_t* p;
    size_t left;
};

SQInteger readBytes(SQUserPointer up, SQUserPointer dst, SQInteger size) {
    auto* c = static_cast<ReadCursor*>(up);
    if (size < 0 || static_cast<size_t>(size) > c->left) return -1;
    std::memcpy(dst, c->p, static_cast<size_t>(size));
    c->p += size;
    c->left -= static_cast<size_t>(size);
    return size;
}

}  // namespace

ScriptHost::Impl& host(HSQUIRRELVM v) {
    auto* h = static_cast<ScriptHost::Impl*>(sq_getforeignptr(v));
    h->current = v;
    return *h;
}

std::string scriptLocation(HSQUIRRELVM v) {
    if (!v) return "?";
    SQStackInfos si;
    for (SQInteger level = 0; SQ_SUCCEEDED(sq_stackinfos(v, level, &si)); ++level) {
        if (si.line < 0) continue;  // natives
        return std::string(si.funcname ? si.funcname : "?") + " " + (si.source ? si.source : "?") + ":" +
               std::to_string(si.line);
    }
    return "?";
}

SQInteger argCount(HSQUIRRELVM v) { return sq_gettop(v) - 1; }

SQInteger argInt(HSQUIRRELVM v, SQInteger idx, SQInteger fallback) {
    if (idx > sq_gettop(v)) return fallback;
    switch (sq_gettype(v, idx)) {
    case OT_INTEGER: {
        SQInteger i = 0;
        sq_getinteger(v, idx, &i);
        return i;
    }
    case OT_FLOAT: {
        SQFloat f = 0;
        sq_getfloat(v, idx, &f);
        return static_cast<SQInteger>(f);
    }
    case OT_BOOL: {
        SQBool b = 0;
        sq_getbool(v, idx, &b);
        return b ? 1 : 0;
    }
    default: return fallback;
    }
}

SQFloat argFloat(HSQUIRRELVM v, SQInteger idx, SQFloat fallback) {
    if (idx > sq_gettop(v)) return fallback;
    switch (sq_gettype(v, idx)) {
    case OT_INTEGER: {
        SQInteger i = 0;
        sq_getinteger(v, idx, &i);
        return static_cast<SQFloat>(i);
    }
    case OT_FLOAT: {
        SQFloat f = 0;
        sq_getfloat(v, idx, &f);
        return f;
    }
    default: return fallback;
    }
}

bool argBool(HSQUIRRELVM v, SQInteger idx, bool fallback) {
    if (idx > sq_gettop(v)) return fallback;
    switch (sq_gettype(v, idx)) {
    case OT_BOOL: {
        SQBool b = 0;
        sq_getbool(v, idx, &b);
        return b != 0;
    }
    case OT_INTEGER: return argInt(v, idx) != 0;
    case OT_NULL: return false;
    default: return fallback;
    }
}

std::string argString(HSQUIRRELVM v, SQInteger idx, const std::string& fallback) {
    if (idx > sq_gettop(v) || sq_gettype(v, idx) != OT_STRING) return fallback;
    const SQChar* s = nullptr;
    sq_getstring(v, idx, &s);
    return s ? std::string(s) : fallback;
}

void defineFunction(HSQUIRRELVM v, const char* name, SQFUNCTION fn) {
    sq_pushstring(v, name, -1);
    sq_newclosure(v, fn, 0);
    sq_setnativeclosurename(v, -1, name);
    sq_createslot(v, -3);
}

// With NYAN_DIESEL_COUNT_NATIVES set, every implemented native is reached through this, which counts
// its calls by name (the free variables: the name, then the implementation's index) before the call.
SQInteger countedNative(HSQUIRRELVM v) {
    SQInteger index = 0;
    const SQChar* name = nullptr;
    sq_getinteger(v, sq_gettop(v), &index);
    sq_getstring(v, sq_gettop(v) - 1, &name);
    sq_pop(v, 2);
    auto& h = host(v);
    ++h.nativeCalls[name ? name : "?"];
    return h.countedFns[static_cast<size_t>(index)](v);
}

// An implemented native under its name in the table on the stack; `fullName` ("SQSprite.create")
// for the catalogue and the counts.
void defineNative(ScriptHost::Impl& h, HSQUIRRELVM v, const std::string& fullName, const char* name, SQFUNCTION fn) {
    h.natives[fullName] = true;
    if (!h.countNatives) {
        defineFunction(v, name, fn);
        return;
    }
    h.countedFns.push_back(fn);
    sq_pushstring(v, name, -1);
    sq_pushinteger(v, static_cast<SQInteger>(h.countedFns.size() - 1));
    sq_pushstring(v, fullName.c_str(), -1);
    sq_newclosure(v, countedNative, 2);
    sq_setnativeclosurename(v, -1, name);
    sq_createslot(v, -3);
}

std::optional<Bytes> ScriptHost::Impl::readFile(const std::string& path) {
    auto data = game.archives->read(normalizeSlashes(path));
    if (!data) note(missing, path);
    return data;
}

namespace {

// Loads a script file as the exe's loader does (muramasa_en.exe 0x43c900): a name without "media"
// is under "media/script/"; an SCRP file's constants go into the VM's constant table, then its
// closure is read (header offset 0x10). A 64-bit exe's files (phantom.exe) hold 8-byte counts and
// integers in the constant table and a 64-bit closure stream, narrowed for the VM (bytecode.hpp).
// Leaves the closure on the stack; false (nothing pushed) if the file is missing or broken.
bool loadScript(ScriptHost::Impl& h, HSQUIRRELVM v, std::string path) {
    if (path.find("media") == std::string::npos) path = "media/script/" + path;
    auto data = h.readFile(path);
    if (!data) return false;
    const Bytes& b = *data;
    if (b.size() < 16 || std::memcmp(b.data(), "SCRP", 4) != 0) {
        // Source text: the exe would compile it (with its macro substitutions); no game ships any.
        h.note(h.unhandled, "source script " + path);
        return false;
    }
    uint32_t headerSize = readLE32(&b[4]), closureSize = readLE32(&b[8]), constOffset = readLE32(&b[12]);
    if (uint64_t(headerSize) + closureSize > b.size() || constOffset + 4u > b.size()) {
        h.lastError = "broken script " + path;
        return false;
    }
    try {
        ByteReader r(ByteSpan(b).subspan(constOffset), path);
        bool wide = h.game.exe.is64;
        auto integer = [&]() -> uint32_t {
            uint32_t lo = r.u32();
            if (wide) r.u32();  // the high half (no constant needs it)
            return lo;
        };
        uint32_t count = integer();
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t nameLength = r.u16();
            auto name = r.span(nameLength);
            std::string key(reinterpret_cast<const char*>(name.data()), nameLength);
            if (!key.empty() && key.back() == '\0') key.pop_back();
            uint32_t type = r.u32();
            sq_pushconsttable(v);
            sq_pushstring(v, key.c_str(), -1);
            if (type == kOtNull) {
                sq_pushnull(v);
            } else if (type == kOtInteger) {
                sq_pushinteger(v, static_cast<SQInteger>(integer()));
            } else if (type == kOtFloat) {
                uint32_t bits = r.u32();
                float f;
                std::memcpy(&f, &bits, 4);
                sq_pushfloat(v, f);
            } else if (type == kOtString) {
                // Stored with its terminating NUL; the exe pushes it as a C string (0x43cf89:
                // sq_pushstring with length -1), so the value ends at the first NUL.
                uint32_t n = r.u32();
                auto s = r.span(n);
                std::string value(reinterpret_cast<const char*>(s.data()), n);
                sq_pushstring(v, value.c_str(), -1);
            } else {
                sq_pop(v, 2);
                h.lastError = "unknown constant type in " + path;
                return false;
            }
            sq_createslot(v, -3);
            sq_pop(v, 1);
        }
    } catch (const Error& e) {
        h.lastError = e.what();
        return false;
    }
    ReadCursor c{b.data() + headerSize, closureSize};
    Bytes narrowed;
    if (h.game.exe.is64) {
        try {
            narrowed = narrowClosure64(ByteSpan(b).subspan(headerSize, closureSize));
        } catch (const Error& e) {
            h.lastError = path + ": " + e.what();
            return false;
        }
        c = ReadCursor{narrowed.data(), narrowed.size()};
    }
    if (SQ_FAILED(sq_readclosure(v, readBytes, &c))) {
        h.lastError = "cannot read the closure of " + path;
        return false;
    }
    return true;
}

// Import(path) / IncludeFile(path) (muramasa_en.exe 0x487ce0, 0x487ee0 -> 0x43d250): load the file
// and call it with the root table as `this`. The two differ only when compiling source text.
SQInteger nativeImport(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string path = argString(v, 2);
    if (!loadScript(h, v, path)) {
        sq_pushinteger(v, -1);
        return 1;
    }
    sq_pushroottable(v);
    if (SQ_FAILED(sq_call(v, 1, SQFalse, SQTrue))) {
        sq_pop(v, 1);
        sq_pushinteger(v, -1);
        return 1;
    }
    sq_pop(v, 1);
    sq_pushinteger(v, 0);
    return 1;
}

// PreCompile(path) (0x487d80): the file's closure, not run.
SQInteger nativePreCompile(HSQUIRRELVM v) {
    auto& h = host(v);
    if (!loadScript(h, v, argString(v, 2))) sq_pushnull(v);
    return 1;
}

// A debug hook (Squirrel's, called with root, event, source, line, function) logging calls of the
// functions NYAN_DIESEL_TRACE names (comma-separated; "*" for all).
SQInteger traceHook(HSQUIRRELVM v) {
    SQInteger event = 0, line = 0;
    const SQChar* source = nullptr;
    const SQChar* fn = nullptr;
    sq_getinteger(v, 2, &event);
    if (event != 'c') return 0;
    sq_getstring(v, 3, &source);
    sq_getinteger(v, 4, &line);
    sq_getstring(v, 5, &fn);
    auto& h = host(v);
    std::string name = fn ? fn : "?";
    for (const auto& t : h.traced) {
        if (t == "*" || t == name) {
            // With its caller (level 1 is the called function, entered; level 2 called it).
            std::string from;
            SQStackInfos si;
            for (SQInteger level = 2; level < 2 + h.traceDepth && SQ_SUCCEEDED(sq_stackinfos(v, level, &si)) && si.source; ++level) {
                from += std::string(" from ") + (si.funcname ? si.funcname : "?") + " (" + si.source + ":" + std::to_string(si.line) + ")";
            }
            if (h.log) h.log("call " + name + " (" + (source ? source : "?") + ":" + std::to_string(line) + ")" + from);
            break;
        }
    }
    return 0;
}

// Suspend() (0x487f80): suspends the VM (returns Squirrel's suspend flag) unless the app is in a
// state where it may not (two flags of the app object; not yet read).
SQInteger nativeSuspend(HSQUIRRELVM v) { return sq_suspendvm(v); }

// The SQMATH_* variables, in the order 0x43b8c0 binds them (the script host object's floats at
// +0xe4, +0xe8, ...).
const char* const kMathVarNames[ScriptHost::Impl::kMathVars] = {
    "SQMATH_V3_1_X", "SQMATH_V3_1_Y", "SQMATH_V3_1_Z", "SQMATH_V3_2_X", "SQMATH_V3_2_Y", "SQMATH_V3_2_Z",
    "SQMATH_Q_1_X",  "SQMATH_Q_1_Y",  "SQMATH_Q_1_Z",  "SQMATH_Q_1_W",  "SQMATH_Q_2_X",  "SQMATH_Q_2_Y",
    "SQMATH_Q_2_Z",  "SQMATH_Q_2_W",  "SQMATH_RET_1",  "SQMATH_RET_2",  "SQMATH_RET_3",  "SQMATH_RET_4",
    "SQMATH_RET_5",  "SQMATH_RET_6",  "SQMATH_RET_7",  "SQMATH_RET_8",  "SQMATH_RET_9",  "SQMATH_RET_10",
    "SQMATH_RET_11", "SQMATH_RET_12", "SQMATH_RET_13", "SQMATH_RET_14", "SQMATH_RET_15", "SQMATH_RET_16"};

int mathVar(HSQUIRRELVM v) {
    const SQChar* name = nullptr;
    if (sq_gettype(v, 2) != OT_STRING || SQ_FAILED(sq_getstring(v, 2, &name))) return -1;
    for (int i = 0; i < ScriptHost::Impl::kMathVars; ++i) {
        if (std::strcmp(name, kMathVarNames[i]) == 0) return i;
    }
    // phantom.exe binds SQMATH_V3_3_X/Y/Z to the floats of SQMATH_V3_2.
    if (host(v).mathV3Third && std::strncmp(name, "SQMATH_V3_3_", 12) == 0 && name[12] >= 'X' && name[12] <= 'Z' && name[13] == 0) {
        return 3 + (name[12] - 'X');
    }
    return -1;
}

// SqPlus's _get / _set on the root table's delegate (its getVarFunc / setVarFunc, "s" and
// "sn|b|s"): the bound variables, else an error, which leaves the VM's lookup to fail as for any
// missing slot.
SQInteger mathGet(HSQUIRRELVM v) {
    int i = mathVar(v);
    if (i < 0) return sq_throwerror(v, "getVarInfo: Could not retrieve UserData");
    sq_pushfloat(v, host(v).mathVars[i]);
    return 1;
}
SQInteger mathSet(HSQUIRRELVM v) {
    int i = mathVar(v);
    if (i < 0) return sq_throwerror(v, "setVarInfo: Could not retrieve UserData");
    SQFloat f = 0;
    if (SQ_FAILED(sq_getfloat(v, 3, &f))) return sq_throwerror(v, "setVar(): bad type");
    host(v).mathVars[i] = f;
    sq_pushfloat(v, f);
    return 1;
}

}  // namespace

std::vector<NativeImpl> hostNatives() {
    return {{"Import", nativeImport}, {"IncludeFile", nativeImport}, {"PreCompile", nativePreCompile},
            {"Suspend", nativeSuspend}};
}

ScriptHost::ScriptHost(const GameData& game, Log log, SaveStore* saves)
    : impl_(std::make_unique<Impl>(game, std::move(log))) {
    // Files (natives_file.cpp): the save root is setting.ini [App] SAVE_PATH as the exe reads it
    // (muramasa_en.exe 0x402123, default "dev/Save"), with a '/' added (0x42f140).
    if (!saves) {
        impl_->ownSaves = makeMemorySaveStore();
        saves = impl_->ownSaves.get();
    }
    impl_->saves = saves;
    std::string root = game.settings->get("App", "SAVE_PATH").value_or("dev/Save");
    for (char& c : root) {
        if (c == '\\') c = '/';
    }
    if (root.empty() || root.back() != '/') root += '/';
    impl_->saveRoot = impl_->initialSaveRoot = root;

    // The settings: CApp's constructor makes them at the screen size (0x444c00); the app's start
    // reads system.dat and applies it (0x42d1f0 -> 0x42f490); then the launcher dialog's OK
    // (CNecroInitCallback 0x4067d0, both exes) turns the 3D options off, makes the save root's
    // "common" folder and writes system.dat back. Nyanroid has no launcher: its OK is taken as given
    // with the resolution and window mode the file had.
    {
        Impl& h = *impl_;
        h.config.screenWidth = game.screenWidth();
        h.config.screenHeight = game.screenHeight();
        h.mathClasses = game.exe.containsString("Vector3D");
        h.mathV3Third = game.exe.containsString("SQMATH_V3_3_X");
        h.lineBreakDefaults = lineBreakTables(game.exe);
        h.lineBreaks = h.lineBreakDefaults;
        h.olderText = !game.exe.containsString("/SMALL");
        h.config.launcherTag = game.launcherTag;
        h.config.setLauncherOptionCount(game.launcherOptionCount);
        std::string cfg = h.writablePath(h.systemConfigPath());
        auto data = saves->read(cfg);
        h.config.deserialize(data ? ByteSpan(*data) : ByteSpan());
        h.applyConfig();
        h.config.launcherDefaults();
        saves->makeDirectory(h.writablePath(h.saveRoot + "common"));
        saves->write(cfg, h.config.serialize());
        // WinMain then (totono_en.exe 0x402c5f..0x402d0d): IsEcoMode's value, and the window's title
        // and the app's name: WINDOW_NAME, or WINDOW_NAME_2 once the third option is set.
        h.ecoMode = h.config.launcherOptions.size() > 1 ? h.config.launcherOptions[1] : 0;
        auto name = game.settings->get("Init", "WINDOW_NAME");
        auto name2 = game.settings->get("Init", "WINDOW_NAME_2");
        bool cleared = h.config.launcherOptions.size() > 2 && h.config.launcherOptions[2] != 0;
        h.appName = cleared && name2 ? *name2 : name.value_or("DieselEngineStudio");
        h.windowTitle = h.appName;
    }
    // The fonts setting.ini's [Font] names (face name = font file), which the app registers.
    for (const auto& [face, file] : game.settings->section("Font")) {
        Impl::Font f;
        f.file = file;
        impl_->fonts[face] = f;
    }

    // A 64-bit exe's VM hashes strings as a 64-bit build does, so its tables keep the PC's order.
    HSQUIRRELVM v = sq_openex(1024, game.exe.is64 ? SQTrue : SQFalse);
    impl_->vm = v;
    sq_setforeignptr(v, impl_.get());
    sq_setprintfunc(v, printFunc);
    sq_newclosure(v, errorHandler, 0);
    sq_seterrorhandler(v);
    if (const char* trace = std::getenv("NYAN_DIESEL_TRACE")) {
        std::string list = trace, item;
        for (char c : list + ",") {
            if (c == ',') {
                if (!item.empty()) impl_->traced.push_back(item);
                item.clear();
            } else {
                item += c;
            }
        }
        // NYAN_DIESEL_TRACE_DEPTH: how many callers each traced call names (1).
        if (const char* depth = std::getenv("NYAN_DIESEL_TRACE_DEPTH")) impl_->traceDepth = std::max(0, std::atoi(depth));
        sq_newclosure(v, traceHook, 0);
        sq_setdebughook(v);
    }

    // The scripts read the constants of SCRP files (the framework's constants: DEFAULT_FONT_NAME,
    // NUT_TYPE_NULL, ...) as globals from code compiled without them (the framework's app class), so
    // they must be visible from the root table. How the exes arrange it is not found yet (their VM, the
    // loader and Get are stock): the root table is made the
    // constant table, which gives that.
    sq_pushroottable(v);
    sq_setconsttable(v);

    sq_pushroottable(v);
    sqstd_register_bloblib(v);
    sqstd_register_iolib(v);
    sqstd_register_systemlib(v);
    sqstd_register_mathlib(v);
    sqstd_register_stringlib(v);

    // The SQMATH_* variables (0x43b8c0): SqPlus binds them through a delegate of the root table
    // whose _get / _set reach the script host's floats; the scripts read them as ::SQMATH_RET_1.
    sq_newtable(v);
    defineFunction(v, "_set", mathSet);
    defineFunction(v, "_get", mathGet);
    sq_setdelegate(v, -2);

    // Globals: stubs for every name the exes register, then the implemented ones over them.
    Impl& h = *impl_;
    h.countNatives = std::getenv("NYAN_DIESEL_COUNT_NATIVES") != nullptr;
    for (const NativeName* n = kGlobals; n->name[0]; ++n) {
        if (!game.exe.holdsName(n->name)) continue;
        defineStub(v, n->name, n->name, n->signature);
        h.natives.emplace(n->name, false);
    }
    for (const auto& impl : hostNatives()) defineNative(h, v, impl.name, impl.name, impl.fn);
    for (const auto& impl : globalNatives()) {
        if (game.exe.holdsName(impl.name)) defineNative(h, v, impl.name, impl.name, impl.fn);
    }
    for (const auto& impl : exeNatives()) {
        if (game.exe.holdsName(impl.name)) defineNative(h, v, impl.name, impl.name, impl.fn);
    }

    // Classes: the methods this exe registers (the table's column for it; an exe the table does not
    // know gets every name).
    const NativeExe column = nativeExe(game.exe.fileName, game.exe.is64);
    auto addressIn = [&](const NativeName& n) -> uint64_t { return nativeAddress(n, column); };
    auto inExe = [&](const NativeName& n) { return column == NativeExe::Unknown || addressIn(n) != 0; };
    // Whether SQScene hands the renderer's settings on (runtime.hpp: sceneForwards).
    if (column != NativeExe::Unknown) {
        for (const NativeClass* c = kNativeClasses; c->name[0]; ++c) {
            if (std::strcmp(c->name, "SQScene") != 0) continue;
            uint64_t parameter = 0, emptyShared = 0;
            for (const NativeName* m = c->methods; m->name[0]; ++m) {
                if (std::strcmp(m->name, "setHDRParameter") == 0) parameter = addressIn(*m);
                if (std::strcmp(m->name, "setShadowMapCenterBone") == 0) emptyShared = addressIn(*m);
            }
            h.sceneForwards = parameter != 0 && parameter != emptyShared;
        }
    }
    for (const NativeClass* c = kNativeClasses; c->name[0]; ++c) {
        if (!c->registered) continue;
        // A class none of whose methods this exe holds is not this exe's (SQTrophy: Phantom's only).
        bool any = false;
        for (const NativeName* m = c->methods; m->name[0]; ++m) any = any || inExe(*m);
        if (!any) continue;
        ClassNatives natives = classNatives(c->name);
        sq_pushstring(v, c->name, -1);
        sq_newclass(v, SQFalse);
        HSQOBJECT obj;
        sq_getstackobj(v, -1, &obj);
        sq_addref(v, &obj);
        Impl::ClassInfo info;
        info.object = obj;
        info.tag = c->name;
        sq_settypetag(v, -1, const_cast<char*>(c->name));
        impl_->classes[c->name] = info;
        std::set<std::string> absent;  // listed, but not this exe's
        for (const NativeName* m = c->methods; m->name[0]; ++m) {
            if (std::strcmp(m->name, "constructor") == 0) continue;
            if (!inExe(*m)) {
                absent.insert(m->name);
                continue;
            }
            defineStub(v, std::string(c->name) + "." + m->name, m->name, m->signature);
            h.natives.emplace(std::string(c->name) + "." + m->name, false);
        }
        for (const auto& impl : natives.methods) {
            if (!absent.count(impl.name)) defineNative(h, v, std::string(c->name) + "." + impl.name, impl.name, impl.fn);
        }
        impl_->classMakers.push_back(std::make_unique<NativeMaker>(natives.make));
        sq_pushstring(v, "constructor", -1);
        sq_pushuserpointer(v, impl_->classMakers.back().get());
        sq_newclosure(v, nativeConstructor, 1);
        sq_createslot(v, -3);
        sq_createslot(v, -3);  // root[name] = class
    }
    sq_pop(v, 1);  // root
    registerMathClasses(v, game.exe);
}

ScriptHost::~ScriptHost() {
    impl_->referenceTable.reset();  // its objects go before the VM
    if (impl_->vm) {
        for (auto& [name, info] : impl_->classes) sq_release(impl_->vm, &info.object);
        sq_close(impl_->vm);
    }
}

void ScriptHost::start() {
    Impl& h = *impl_;
    HSQUIRRELVM v = h.vm;
    // The MAIN task: main.nut with the root table as `this`.
    if (!loadScript(h, v, "main.nut")) {
        h.state = State::Failed;
        if (h.lastError.empty()) h.lastError = "main.nut not found";
        return;
    }
    sq_pushroottable(v);
    SQRESULT r = sq_call(v, 1, SQFalse, SQTrue);
    if (SQ_FAILED(r)) {
        h.state = State::Failed;
        return;
    }
    h.state = sq_getvmstate(v) == SQ_VMSTATE_SUSPENDED ? State::Suspended : State::Finished;
}

void ScriptHost::frame() {
    Impl& h = *impl_;
    if (h.state != State::Suspended) return;
    // After exit() (or a close with no close callback) the window is gone: the WM_CLOSE posted in
    // the frame that asked is handled by the next pump (Mware 0x100358a0, before the frame), whose
    // quit ends the frame loop (muramasa_en.exe 0x42de00), so no frame runs again. The scripts
    // after exit() are never run on the PC (Muramasa's title, past its StartEnd, calls a scene with
    // no name in a loop that never yields a frame).
    if (h.quit) return;
    // A message box holds the frame loop (MessageBoxW inside the script's call); answered, the
    // script resumes in the call with the button and the interrupted frame goes on (no new frame).
    if (h.messageBox.pending) return;
    if (h.messageBox.answered) {
        h.messageBox.answered = false;
        if (h.messageBox.resume) {
            h.messageBox.resume = false;
            sq_pushinteger(h.vm, h.messageBox.result);
            SQRESULT r = sq_wakeupvm(h.vm, SQTrue, SQFalse, SQTrue, SQFalse);
            if (SQ_FAILED(r)) {
                h.state = State::Failed;
                return;
            }
            h.state = sq_getvmstate(h.vm) == SQ_VMSTATE_SUSPENDED ? State::Suspended : State::Finished;
        }
        if (!h.messageBox.pending) h.finishFrame();
        return;
    }
    ++h.frame;
    // The core's time stamp for the frame: the frame rate's pace from where the rate last changed
    // (the PC's runs on real time; this keeps runs reproducible).
    int32_t rate = h.frameRate > 0 ? h.frameRate : 60;
    h.clock.setTime(h.rateTime + static_cast<uint32_t>((h.frame - h.rateFrame) * 1000 / static_cast<uint64_t>(rate)));
    // muramasa_en.exe's frame loop (0x42de00): the core's pump ends with the input update (Mware
    // 0x100358a0); settings waiting to apply take the frame's place; else the frame (0x42e6f0): the
    // clock (0x42edb0), the task's key repeat (0x4860f0), the script (CTaskMain 0x47ed00), then
    // drawing, then the task objects' updates (0x486640 from 0x42e7e0).
    h.input.keyboard.update();
    h.input.mouse.update();
    if (h.config.dirty) {
        h.applyConfig();
        return;
    }
    // The task switch (0x42e400, before the frame): a name callTask left goes to 0x42e260, which
    // starts that task unless it runs already. The exe has one task, MAIN: for it nothing starts and
    // the switch clears CApp +0xcc (the cursor shape); any other name finds no task (the exe then
    // calls through a null task and crashes).
    if (!h.calledTask.empty()) {
        std::string name = std::move(h.calledTask);
        h.calledTask.clear();
        if (name == "MAIN") h.cursor = 0;
        else h.note(h.unhandled, "SQApp.callTask(" + name + "): no such task");
    }
    h.clock.beginFrame();
    h.input.repeat.update(h.input.keyboard, h.clock.milliTime);
    SQRESULT r = sq_wakeupvm(h.vm, SQFalse, SQFalse, SQTrue, SQFalse);
    if (SQ_FAILED(r)) {
        h.state = State::Failed;
        return;
    }
    h.state = sq_getvmstate(h.vm) == SQ_VMSTATE_SUSPENDED ? State::Suspended : State::Finished;
    if (h.messageBox.pending) return;  // the script opened a message box: the frame stops there
    h.finishFrame();
}

// The rest of a frame after the script (0x42e6f0 / 0x42e7e0): sounds, movies, the draw, the task
// objects' updates, the app's fade.
void ScriptHost::Impl::finishFrame() {
    Impl& h = *this;
    h.updateSoundManager();
    for (auto* m : h.movies) m->stepMovie(h);
    h.layout();
    for (size_t i = 0; i < h.tasks.size(); ++i) {
        if (h.tasks[i]) h.tasks[i]->update(h);
    }
    std::erase(h.tasks, nullptr);
    // The cursor (0x42e751, totono_en.exe 0x42f67f, phantom.exe 0x1400389e7): a shape changed since
    // the last post goes to the window (PostMessage WM_SETCURSOR, the shape as wParam), whose
    // procedure shows only 1 and 7; then the shape is 1 again. (The exe sets 0 instead until its
    // window first comes to the front; Nyanroid's is in front from the start.)
    if (h.cursor != h.postedCursor && (h.cursor == 1 || h.cursor == 7)) h.shownCursor = h.cursor;
    h.postedCursor = h.cursor;
    h.cursor = 1;
    h.screenFade.fade.step(h.clock);  // the app's fade sprite, last (0x42e6f0: vtable +0x38)
}

std::optional<ScriptHost::MessageBox> ScriptHost::messageBox() const {
    const auto& m = impl_->messageBox;
    if (!m.pending) return std::nullopt;
    return MessageBox{m.serial, m.caption, m.text, m.type};
}

void ScriptHost::answerMessageBox(int32_t button) {
    auto& m = impl_->messageBox;
    if (!m.pending) return;
    m.pending = false;
    m.answered = true;
    m.result = button;
}

std::string ScriptHost::evaluate(const std::string& source) {
    auto& h = *impl_;
    if (!h.vm) return "error: no VM";
    std::string savedError = h.lastError;
    HSQUIRRELVM t = sq_newthread(h.vm, 1024);
    HSQOBJECT thread;
    sq_resetobject(&thread);
    sq_getstackobj(h.vm, -1, &thread);
    sq_addref(h.vm, &thread);
    sq_pop(h.vm, 1);
    sq_setforeignptr(t, &h);
    HSQUIRRELVM current = h.current;
    std::string out;
    if (SQ_FAILED(sq_compilebuffer(t, source.c_str(), static_cast<SQInteger>(source.size()), "evaluate", SQTrue))) {
        out = "error: does not compile";
    } else {
        sq_pushroottable(t);
        if (SQ_FAILED(sq_call(t, 1, SQTrue, SQTrue))) {
            out = "error: " + h.lastError;
        } else if (sq_gettype(t, -1) == OT_NULL) {
            out = "null";
        } else {
            sq_tostring(t, -1);
            const SQChar* str = nullptr;
            if (SQ_SUCCEEDED(sq_getstring(t, -1, &str)) && str) out = str;
        }
    }
    sq_settop(t, 0);
    h.current = current;
    h.lastError = savedError;
    sq_release(h.vm, &thread);
    return out;
}

void ScriptHost::keyEvent(int vk, bool down) {
    int code = keyCodeForVk(vk);
    if (down) {
        impl_->input.keyboard.keyDown(code);
    } else {
        impl_->input.keyboard.keyUp(code);
    }
}

void ScriptHost::mouseButtonEvent(int button, bool down) {
    if (button < 0 || button > 2) return;
    impl_->input.mouse.button(static_cast<Mouse::Button>(button), down);
}

void ScriptHost::pointerEvent(int x, int y) { impl_->input.mouse.moveTo(x, y); }
void ScriptHost::wheelEvent(int delta) { impl_->input.mouse.wheel(delta); }

void ScriptHost::releaseInput() {
    impl_->input.keyboard.releaseAll();
    impl_->input.mouse.releaseAll();
}

void ScriptHost::requestClose() {
    Impl& h = *impl_;
    // WM_CLOSE (CTaskMain 0x47f960): the script's close callback if it set one, else quit.
    if (!h.hasCloseCallback || h.state == State::Failed || h.state == State::Finished) {
        h.quit = true;
        return;
    }
    HSQUIRRELVM v = h.vm;
    SQInteger top = sq_gettop(v);
    sq_pushobject(v, h.closeCallback);
    sq_pushroottable(v);
    if (SQ_FAILED(sq_call(v, 1, SQFalse, SQTrue))) h.log("close callback failed: " + h.lastError);
    sq_settop(v, top);
}

void ScriptHost::seedRandom(int32_t seed) {
    impl_->random.seed(seed);
    impl_->randomSeed = seed;
}
bool ScriptHost::quitRequested() const { return impl_->quit; }
bool ScriptHost::rebootRequested() const { return impl_->reboot; }
const std::string& ScriptHost::windowTitle() const { return impl_->windowTitle; }
int ScriptHost::cursor() const { return impl_->shownCursor; }
const std::vector<DrawItem>& ScriptHost::drawItems() const { return impl_->drawItems; }
const std::vector<RenderTarget>& ScriptHost::renderTargets() const { return impl_->renderTargets; }
std::vector<SoundEvent> ScriptHost::takeSoundEvents() { return std::exchange(impl_->soundEvents, {}); }
std::vector<SoundVoice> ScriptHost::soundVoices() const {
    std::vector<SoundVoice> out;
    for (const auto* r : impl_->soundRecords) r->voices(out);
    return out;
}
std::optional<Bytes> ScriptHost::readGameFile(const std::string& path) const { return impl_->readAnyFile(path); }
std::shared_ptr<RandomAccessFile> ScriptHost::openGameFile(const std::string& path) const { return impl_->openAnyFile(path); }
bool ScriptHost::cursorVisible() const { return impl_->cursorVisible; }
int ScriptHost::frameRate() const { return impl_->frameRate; }

ScriptHost::State ScriptHost::state() const { return impl_->state; }
const std::string& ScriptHost::lastError() const { return impl_->lastError; }

std::string ScriptHost::threadStacks() const { return impl_->vm ? suspendedThreadStacks(impl_->vm) : ""; }
uint64_t ScriptHost::frameNumber() const { return impl_->frame; }
const std::map<std::string, int>& ScriptHost::unhandled() const { return impl_->unhandled; }
const std::map<std::string, bool>& ScriptHost::natives() const { return impl_->natives; }
const std::map<std::string, uint64_t>& ScriptHost::nativeCalls() const { return impl_->nativeCalls; }
const std::map<std::string, int>& ScriptHost::missing() const { return impl_->missing; }

}  // namespace nyan::diesel
