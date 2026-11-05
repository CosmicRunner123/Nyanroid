// Files: SQFile (muramasa_en.exe registers it at 0x4e9cd0; object 0x10d8 bytes) and the file
// globals, over Mware's file manager (Mware.dll 0x10008640) as Nyanroid maps it:
//   - the save root (setting.ini [App] SAVE_PATH, "<APPDATA>/Nitroplus/Muramasa/1.00" in Muramasa;
//     the exe keeps it unexpanded and Mware expands "<APPDATA>" when opening) is the save store's root;
//   - other "<APPDATA>/", "<TEMP>/", "<TMP>/" and "file:///" paths are store folders of their own;
//   - any other path is read from the archives (Mware tries them first for reading), then from the
//     store's "folder/" (files the game wrote there), then the install's loose files; writes to it go
//     to the store's "folder/" (the PC would write into the game folder).

#include <cstring>

#include "natives.hpp"
#include "nyan/common/sjis.hpp"

namespace nyan::diesel {

namespace {

bool startsWithNoCase(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && toLowerAscii(s.substr(0, prefix.size())) == toLowerAscii(prefix);
}

}  // namespace

// Mware's path normalization (CFileManager vtable +0x18, 0x10009590), which every open runs first
// (0x10008640 -> 0x100094f0) before it searches the archives: '\' becomes '/' and a run of '/'
// becomes one, so "media/cg/sys/save//thumb_new_01.png" finds its entry. Mware also drops
// "file:///" there; Nyanroid keeps it for resolvePath, which reads what follows it.
std::string normalizeSlashes(std::string p) {
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    const size_t keep = startsWithNoCase(p, "file:///") ? 8 : 0;
    std::string out = p.substr(0, keep);
    for (size_t i = keep; i < p.size(); ++i) {
        if (p[i] == '/' && out.size() > keep && out.back() == '/') continue;
        out += p[i];
    }
    return out;
}

ScriptHost::Impl::FilePath ScriptHost::Impl::resolvePath(const std::string& scriptPath) const {
    std::string p = normalizeSlashes(scriptPath);
    // A file URL names the path after it (the save screens load thumbnails as
    // "file:///" + GetSavePathRoot() + ...): resolved as that path when it is under a known root.
    if (startsWithNoCase(p, "file:///")) {
        FilePath inner = resolvePath(p.substr(8));
        if (inner.save) return inner;
    }
    if (!initialSaveRoot.empty() && startsWithNoCase(p, initialSaveRoot)) return {true, p.substr(initialSaveRoot.size())};
    // The root without its slash names the store's root too.
    if (!initialSaveRoot.empty() && toLowerAscii(p) + "/" == toLowerAscii(initialSaveRoot)) return {true, ""};
    static const std::pair<const char*, const char*> kPrefixes[] = {
        {"<APPDATA>/", "appdata/"}, {"<TEMP>/", "temp/"}, {"<TMP>/", "temp/"}, {"file:///", "file/"}};
    for (const auto& [prefix, folder] : kPrefixes) {
        if (startsWithNoCase(p, prefix)) return {true, folder + p.substr(std::strlen(prefix))};
    }
    while (p.starts_with("./")) p = p.substr(2);
    return {false, p};
}

std::string ScriptHost::Impl::writablePath(const std::string& scriptPath) const {
    auto f = resolvePath(scriptPath);
    return f.save ? f.path : "folder/" + f.path;
}

std::optional<Bytes> ScriptHost::Impl::readAnyFile(const std::string& scriptPath) const {
    FilePath f = resolvePath(scriptPath);
    if (f.save) return saves->read(f.path);
    if (auto d = game.archives->read(f.path)) return d;
    if (auto d = saves->read("folder/" + f.path)) return d;
    if (game.folder) {
        if (auto file = game.folder->open(f.path)) return file->readAll();
    }
    return std::nullopt;
}

std::shared_ptr<RandomAccessFile> ScriptHost::Impl::openAnyFile(const std::string& scriptPath) const {
    FilePath f = resolvePath(scriptPath);
    if (f.save) {
        if (auto d = saves->read(f.path)) return std::make_shared<MemoryFile>(std::move(*d));
        return nullptr;
    }
    if (auto file = game.archives->open(f.path)) return file;
    if (auto d = saves->read("folder/" + f.path)) return std::make_shared<MemoryFile>(std::move(*d));
    if (game.folder) {
        if (auto file = game.folder->open(f.path)) return file;
    }
    return nullptr;
}

namespace {

constexpr int kCodePageSjis = 932;
constexpr int kCodePageUtf8 = 65001;

// A task object under its name (SQFile(name): CNutFile passes its nut's name), as the exe's
// SQFile is (its file at +0xd0): the save natives find it by that name.
struct FileObject final : NativeObject, FrameTask, FileStream {
    ScriptHost::Impl* host;
    std::string name;
    bool opened = false;
    int mode = 0;           // 0 read, 1 write, 2 read / write (FILE_OPEN_MODE_*)
    bool compress = false;  // Mware's compressed format (modes 0 and 1 only)
    ScriptHost::Impl::FilePath where;
    Bytes data;
    size_t pos = 0;
    bool dirty = false;
    int codePage = kCodePageUtf8;  // +0xd4, 65001 from the constructor (0x4e9e70)
    int32_t lock = 0, disused = 0;  // Totono's (vtable +0x40 .. +0x4c: +0xb8's +0x20 / +0x24)

    FileObject(ScriptHost::Impl* h, std::string n) : host(h), name(std::move(n)) {
        taskName = name;
        host->addTask(this);
    }
    ~FileObject() override {
        close();
        host->removeTask(this);
    }
    void update(ScriptHost::Impl&) override {}

    size_t readBytes(void* dst, size_t n) override { return read(dst, n); }
    void writeBytes(const void* src, size_t n) override { write(src, n); }
    size_t tell() const override { return pos; }
    void seekTo(size_t p) override { pos = p; }

    // The file is written when it is closed: when its object goes (the scripts drop it; there is no
    // close() in the exes), as Mware's files flush on closing.
    void close() {
        if (opened && dirty && mode != 0) {
            std::string path = where.save ? where.path : "folder/" + where.path;
            Bytes out = (compress && mode == 1) ? encodeCompressedFile(data) : data;
            if (!host->saves->write(path, out) && host->log) host->log("SQFile: cannot write " + path);
        }
        opened = false;
        dirty = false;
        data.clear();
        pos = 0;
    }

    size_t read(void* dst, size_t n) {
        n = std::min(n, data.size() - std::min(pos, data.size()));
        if (n) std::memcpy(dst, data.data() + pos, n);
        pos += n;
        return n;
    }
    void write(const void* src, size_t n) {
        if (pos + n > data.size()) data.resize(pos + n);
        std::memcpy(data.data() + pos, src, n);
        pos += n;
        dirty = true;
    }
};

NativeObject* make(HSQUIRRELVM v) { return new FileObject(&host(v), argString(v, 2)); }

#define FILE_SELF                     \
    FileObject* f = self<FileObject>(v); \
    if (!f) return sq_throwerror(v, "SQFile: not an instance");

// open(path: string, mode: int, compress: int): int, 0 or -1 (0x4ea090).
SQInteger open(HSQUIRRELVM v) {
    FILE_SELF
    auto& h = *f->host;
    f->close();
    std::string path = argString(v, 2);
    f->mode = static_cast<int>(argInt(v, 3));
    f->compress = argInt(v, 4) != 0 && f->mode != 2;
    f->where = h.resolvePath(path);
    if (f->mode == 0 || f->mode == 2) {
        auto bytes = h.readAnyFile(path);
        if (!bytes) {
            if (f->mode == 0) {
                sq_pushinteger(v, -1);
                return 1;
            }
            bytes = Bytes();
        } else if (f->compress) {
            auto plain = decodeCompressedFile(*bytes);
            if (!plain) {  // Mware's reader closes a file without the header: the open fails
                sq_pushinteger(v, -1);
                return 1;
            }
            bytes = std::move(*plain);
        }
        f->data = std::move(*bytes);
    }
    if (f->mode == 1) f->dirty = true;  // creating a file writes it even if nothing is written
    f->opened = true;
    f->pos = 0;
    sq_pushinteger(v, 0);
    return 1;
}

SQInteger isOpened(HSQUIRRELVM v) {
    FILE_SELF
    sq_pushinteger(v, f->opened ? 1 : 0);
    return 1;
}

SQInteger setStringCodePage(HSQUIRRELVM v) {
    FILE_SELF
    f->codePage = static_cast<int>(argInt(v, 2));
    return 0;
}

// The bytes a character's lead byte says it has in the code page (0x4ea190): Shift-JIS leads
// 0x81..0x9f and 0xe0..0xfc are two; UTF-8 leads by their high bits; 1 otherwise (UTF-8 trail bytes
// and 0xfe / 0xff count as single bytes).
int leadLength(uint8_t b, int codePage) {
    if (codePage == kCodePageSjis) return ((b > 0x80 && b < 0xa0) || static_cast<uint8_t>(b + 0x20) < 0x1d) ? 2 : 1;
    if (codePage == kCodePageUtf8 && (b & 0x80)) {
        for (int n = 2, bit = 0x20; n <= 6; ++n, bit >>= 1) {
            if (!(b & 0x40)) return 1;
            if (!(b & bit)) return n;
        }
        return 1;
    }
    return 1;
}

// readString(line: int): string or null at the end (0x4ea190), as the exe scans: 1 KB at a time
// from the position, a character's trail bytes skipped unexamined (the count restarts with each
// 1 KB); without `line` the text ends at a NUL, which is skipped; with `line` each CR or LF is cut
// and the line ends at the next single-byte character that is not CR / LF (a multi-byte character
// there is passed over, as the exe does) or at a NUL, which stays. Past the end of the file the
// buffer is zeros. Shift-JIS text becomes UTF-8.
SQInteger readString(HSQUIRRELVM v) {
    FILE_SELF
    bool line = argInt(v, 2) != 0;
    size_t n = f->data.size(), start = f->pos;
    if (!f->opened || start >= n) {
        sq_pushnull(v);
        return 1;
    }
    std::vector<uint8_t> text;  // the scanned bytes, CR / LF cut to zero
    auto at = [&](size_t k) -> uint8_t { return k < n ? f->data[k] : 0; };
    size_t next = 0;
    int skip = 0;
    bool cut = false;
    for (size_t k = start;; ++k) {
        if ((k - start) % 0x400 == 0) skip = 0;
        uint8_t b = at(k);
        text.push_back(b);
        if (skip > 0) {
            --skip;
            continue;
        }
        int len = leadLength(b, f->codePage);
        if (len > 1) {
            skip = len - 1;
            continue;
        }
        if (b == 0) {
            next = line ? k : k + 1;
            break;
        }
        if (line) {
            if (b == '\r' || b == '\n') {
                text.back() = 0;
                cut = true;
            } else if (cut) {
                next = k;
                break;
            }
        }
    }
    size_t len = 0;
    while (len < text.size() && text[len] != 0) ++len;
    std::string s(reinterpret_cast<const char*>(text.data()), len);
    f->pos = next;
    if (f->codePage != kCodePageUtf8) s = sjisToUtf8(s);
    sq_pushstring(v, s.c_str(), static_cast<SQInteger>(s.size()));
    return 1;
}

// readInteger / readFloat: the next 4 bytes, or null (0x4ea6f0, 0x4ea780).
SQInteger readInteger(HSQUIRRELVM v) {
    FILE_SELF
    int32_t x = 0;
    if (!f->opened || f->read(&x, 4) != 4) {
        sq_pushnull(v);
        return 1;
    }
    sq_pushinteger(v, x);
    return 1;
}
SQInteger readFloat(HSQUIRRELVM v) {
    FILE_SELF
    float x = 0;
    if (!f->opened || f->read(&x, 4) != 4) {
        sq_pushnull(v);
        return 1;
    }
    sq_pushfloat(v, x);
    return 1;
}

// writeString(text: string, writeNull: int): int, 0 or -1 (0x4ea810): Shift-JIS unless UTF-8.
SQInteger writeString(HSQUIRRELVM v) {
    FILE_SELF
    if (!f->opened || f->mode == 0) {
        sq_pushinteger(v, -1);
        return 1;
    }
    std::string s = argString(v, 2);
    if (f->codePage != kCodePageUtf8) s = utf8ToSjis(s);
    f->write(s.data(), s.size());
    if (argInt(v, 3) != 0) f->write("", 1);
    sq_pushinteger(v, 0);
    return 1;
}
SQInteger writeInteger(HSQUIRRELVM v) {
    FILE_SELF
    if (!f->opened || f->mode == 0) {
        sq_pushinteger(v, -1);
        return 1;
    }
    int32_t x = static_cast<int32_t>(argInt(v, 2));
    f->write(&x, 4);
    sq_pushinteger(v, 0);
    return 1;
}
SQInteger writeFloat(HSQUIRRELVM v) {
    FILE_SELF
    if (!f->opened || f->mode == 0) {
        sq_pushinteger(v, -1);
        return 1;
    }
    float x = argFloat(v, 2);
    f->write(&x, 4);
    sq_pushinteger(v, 0);
    return 1;
}
SQInteger getFileSize(HSQUIRRELVM v) {
    FILE_SELF
    sq_pushinteger(v, f->opened ? static_cast<SQInteger>(f->data.size()) : 0);
    return 1;
}
SQInteger getFilePosition(HSQUIRRELVM v) {
    FILE_SELF
    sq_pushinteger(v, f->opened ? static_cast<SQInteger>(f->pos) : 0);
    return 1;
}
// seekStart / seekEnd / seek (from the start, the end, here): the new position (SetFilePointer).
SQInteger seekTo(HSQUIRRELVM v, int64_t base) {
    FILE_SELF
    if (!f->opened) {
        sq_pushinteger(v, 0);
        return 1;
    }
    int64_t p = base + argInt(v, 2);
    if (p >= 0) f->pos = static_cast<size_t>(p);
    sq_pushinteger(v, static_cast<SQInteger>(f->pos));
    return 1;
}
SQInteger seekStart(HSQUIRRELVM v) { return seekTo(v, 0); }
SQInteger seekEnd(HSQUIRRELVM v) {
    FileObject* f = self<FileObject>(v);
    return seekTo(v, f ? static_cast<int64_t>(f->data.size()) : 0);
}
SQInteger seek(HSQUIRRELVM v) {
    FileObject* f = self<FileObject>(v);
    return seekTo(v, f ? static_cast<int64_t>(f->pos) : 0);
}

// Globals.
// GetSavePathRoot(): string (0x4883a0); SetSavePathRoot(path) adds a '/' if missing (0x42f140).
SQInteger getSavePathRoot(HSQUIRRELVM v) {
    sq_pushstring(v, host(v).saveRoot.c_str(), -1);
    return 1;
}
SQInteger setSavePathRoot(HSQUIRRELVM v) {
    std::string p = argString(v, 2);
    if (p.empty() || p.back() != '/') p += '/';
    host(v).saveRoot = p;
    return 0;
}
SQInteger getTempPathRoot(HSQUIRRELVM v) {
    sq_pushstring(v, host(v).tempRoot.c_str(), -1);
    return 1;
}


// FileDelete(path): int (Mware's file manager +0x24); FileCopy(src, dst, overwrite): int;
// FileMove(src, dst): int; IsFileExist(path): int, a file the reader can open.
SQInteger fileDelete(HSQUIRRELVM v) {
    auto& h = host(v);
    bool ok = h.saves->remove(h.writablePath(argString(v, 2)));
    sq_pushinteger(v, ok ? 1 : 0);
    return 1;
}
SQInteger fileCopy(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string dst = h.writablePath(argString(v, 3));
    bool overwrite = argInt(v, 4) != 0;
    auto data = h.readAnyFile(argString(v, 2));
    bool ok = data && (overwrite || !h.saves->exists(dst)) && h.saves->write(dst, *data);
    sq_pushinteger(v, ok ? 1 : 0);
    return 1;
}
SQInteger fileMove(HSQUIRRELVM v) {
    auto& h = host(v);
    std::string src = h.writablePath(argString(v, 2)), dst = h.writablePath(argString(v, 3));
    auto data = h.saves->read(src);
    bool ok = data && h.saves->write(dst, *data) && h.saves->remove(src);
    sq_pushinteger(v, ok ? 1 : 0);
    return 1;
}
SQInteger isFileExist(HSQUIRRELVM v) {
    auto& h = host(v);
    sq_pushinteger(v, h.readAnyFile(argString(v, 2)) ? 1 : 0);
    return 1;
}

}  // namespace

namespace {
SQInteger setFileLock(HSQUIRRELVM v) {
    FILE_SELF
    f->lock = static_cast<int32_t>(argInt(v, 2));
    return 0;
}
SQInteger getFileLock(HSQUIRRELVM v) {
    FILE_SELF
    sq_pushinteger(v, f->lock);
    return 1;
}
SQInteger setFileDisused(HSQUIRRELVM v) {
    FILE_SELF
    f->disused = static_cast<int32_t>(argInt(v, 2));
    return 0;
}
SQInteger getFileDisused(HSQUIRRELVM v) {
    FILE_SELF
    sq_pushinteger(v, f->disused);
    return 1;
}
}  // namespace

ClassNatives fileNatives() {
    return {{{"setLock", setFileLock},
             {"getLock", getFileLock},
             {"setDisused", setFileDisused},
             {"getDisused", getFileDisused},
             {"open", open},
             {"isOpened", isOpened},
             {"setStringCodePage", setStringCodePage},
             {"readString", readString},
             {"readInteger", readInteger},
             {"readFloat", readFloat},
             {"writeString", writeString},
             {"writeInteger", writeInteger},
             {"writeFloat", writeFloat},
             {"getFileSize", getFileSize},
             {"getFilePosition", getFilePosition},
             {"seekStart", seekStart},
             {"seekEnd", seekEnd},
             {"seek", seek}},
            make};
}

std::vector<NativeImpl> fileGlobals() {
    return {{"GetSavePathRoot", getSavePathRoot}, {"SetSavePathRoot", setSavePathRoot},
            {"GetTempPathRoot", getTempPathRoot}, {"FileDelete", fileDelete},
            {"FileCopy", fileCopy},               {"FileMove", fileMove},
            {"IsFileExist", isFileExist}};
}

}  // namespace nyan::diesel
