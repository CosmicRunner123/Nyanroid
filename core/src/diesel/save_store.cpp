#include "nyan/diesel/save_store.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include <zstd.h>

#include "nyan/common/file_source.hpp"

namespace fs = std::filesystem;

namespace nyan::diesel {

namespace {

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty() && cur != ".") out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur != ".") out.push_back(cur);
    return out;
}

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

class DirectoryStore final : public SaveStore {
public:
    explicit DirectoryStore(std::string root) : root_(std::move(root)) {}

    std::optional<Bytes> read(const std::string& path) const override {
        auto p = resolve(path, false);
        std::error_code ec;
        if (!p || !fs::is_regular_file(*p, ec)) return std::nullopt;
        std::ifstream in(*p, std::ios::binary);
        if (!in) return std::nullopt;
        return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    bool write(const std::string& path, ByteSpan data) override {
        auto p = resolve(path, true);
        if (!p) return false;
        std::error_code ec;
        fs::create_directories(p->parent_path(), ec);
        fs::path tmp = *p;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!out) return false;
        }
        fs::rename(tmp, *p, ec);
        return !ec;
    }
    bool remove(const std::string& path) override {
        auto p = resolve(path, false);
        std::error_code ec;
        return p && fs::remove(*p, ec);
    }
    bool exists(const std::string& path) const override {
        auto p = resolve(path, false);
        std::error_code ec;
        return p && fs::exists(*p, ec);
    }
    bool isDirectory(const std::string& path) const override {
        auto p = resolve(path, false);
        std::error_code ec;
        return p && fs::is_directory(*p, ec);
    }
    bool makeDirectory(const std::string& path) override {
        auto p = resolve(path, true);
        std::error_code ec;
        if (!p) return false;
        fs::create_directories(*p, ec);
        return fs::is_directory(*p, ec);
    }
    std::optional<int64_t> modifiedTime(const std::string& path) const override {
        auto p = resolve(path, false);
        std::error_code ec;
        if (!p) return std::nullopt;
        auto t = fs::last_write_time(*p, ec);
        if (ec) return std::nullopt;
        auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
            t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        return sys.time_since_epoch().count();
    }

private:
    // The host path of a store path: each component matched in either case against what exists;
    // components that do not exist yet keep their spelling (`create`), else nullopt.
    std::optional<fs::path> resolve(const std::string& path, bool create) const {
        fs::path cur(root_);
        std::error_code ec;
        for (const auto& part : split(path)) {
            if (part == "..") return std::nullopt;
            fs::path exact = cur / part;
            if (fs::exists(exact, ec)) {
                cur = exact;
                continue;
            }
            std::string want = toLowerAscii(part);
            bool found = false;
            if (fs::is_directory(cur, ec)) {
                for (const auto& e : fs::directory_iterator(cur, ec)) {
                    if (toLowerAscii(e.path().filename().string()) == want) {
                        cur = e.path();
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                if (!create) return std::nullopt;
                cur = exact;
            }
        }
        return cur;
    }

    std::string root_;
};

class MemoryStore final : public SaveStore {
public:
    std::optional<Bytes> read(const std::string& path) const override {
        auto it = files_.find(key(path));
        if (it == files_.end()) return std::nullopt;
        return it->second.data;
    }
    bool write(const std::string& path, ByteSpan data) override {
        std::string k = key(path);
        auto parts = split(k);
        std::string dir;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            dir += (dir.empty() ? "" : "/") + parts[i];
            dirs_.insert(dir);
        }
        files_[k] = File{Bytes(data.begin(), data.end()), nowSeconds()};
        return true;
    }
    bool remove(const std::string& path) override { return files_.erase(key(path)) > 0 || dirs_.erase(key(path)) > 0; }
    bool exists(const std::string& path) const override {
        std::string k = key(path);
        return k.empty() || files_.count(k) || dirs_.count(k);
    }
    bool isDirectory(const std::string& path) const override {
        std::string k = key(path);
        return k.empty() || dirs_.count(k);
    }
    bool makeDirectory(const std::string& path) override {
        std::string dir;
        for (const auto& part : split(key(path))) {
            dir += (dir.empty() ? "" : "/") + part;
            dirs_.insert(dir);
        }
        return true;
    }
    std::optional<int64_t> modifiedTime(const std::string& path) const override {
        auto it = files_.find(key(path));
        if (it == files_.end()) return std::nullopt;
        return it->second.time;
    }

private:
    static std::string key(const std::string& path) {
        std::string out;
        for (const auto& part : split(path)) out += (out.empty() ? "" : "/") + toLowerAscii(part);
        return out;
    }
    struct File {
        Bytes data;
        int64_t time = 0;
    };
    std::map<std::string, File> files_;
    std::set<std::string> dirs_;
};

void put32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// The data as one zstd frame at level 3 (Mware's writer: ZSTD_initCStream(ctx, 3), 0x10006310).
Bytes zstdFrame(ByteSpan data) {
    Bytes out(ZSTD_compressBound(data.size()));
    size_t n = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 3);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

}  // namespace

std::unique_ptr<SaveStore> makeDirectorySaveStore(std::string root) { return std::make_unique<DirectoryStore>(std::move(root)); }
std::unique_ptr<SaveStore> makeMemorySaveStore() { return std::make_unique<MemoryStore>(); }

Bytes encodeCompressedFile(ByteSpan data) {
    Bytes out = {'M', 'W', 'C', 'F', 0};
    put32(out, static_cast<uint32_t>(data.size()));
    Bytes frame = zstdFrame(data);
    out.insert(out.end(), {'B', 'L', 'C', 'K', 0});
    put32(out, static_cast<uint32_t>(frame.size()));
    put32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

std::optional<Bytes> decodeCompressedFile(ByteSpan file) {
    if (file.size() < 9 || std::memcmp(file.data(), "MWCF", 4) != 0) return std::nullopt;
    uint32_t total = readLE32(file.data() + 5);
    Bytes out;
    out.reserve(total);
    size_t at = 9;
    // Blocks until the next header is not "BLCK" (the reader's loop, 0x10004b00).
    while (at + 13 <= file.size() && std::memcmp(file.data() + at, "BLCK", 4) == 0) {
        uint32_t packed = readLE32(file.data() + at + 5);
        uint32_t size = readLE32(file.data() + at + 9);
        at += 13;
        if (at + packed > file.size()) return std::nullopt;
        size_t base = out.size();
        out.resize(base + size);
        size_t got = ZSTD_decompress(out.data() + base, size, file.data() + at, packed);
        if (ZSTD_isError(got) || got != size) return std::nullopt;
        at += packed;
    }
    if (out.size() != total) return std::nullopt;
    return out;
}

}  // namespace nyan::diesel
