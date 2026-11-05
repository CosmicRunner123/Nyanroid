#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan {

// Random-access read-only file. Implementations must be safe to call readAt() from
// multiple threads concurrently (positional reads, no shared cursor).
class RandomAccessFile {
public:
    virtual ~RandomAccessFile() = default;
    virtual uint64_t size() const = 0;
    // Reads exactly `length` bytes or throws.
    virtual void readAt(uint64_t offset, void* dst, size_t length) const = 0;

    Bytes readAt(uint64_t offset, size_t length) const {
        Bytes out(length);
        readAt(offset, out.data(), length);
        return out;
    }
    Bytes readAll() const { return readAt(0, static_cast<size_t>(size())); }
};

// A file opened from a POSIX file descriptor. On Android, the Kotlin layer hands us
// descriptors obtained through the Storage Access Framework, so this works for both.
class FdFile final : public RandomAccessFile {
public:
    // Takes ownership of `fd` and closes it on destruction.
    FdFile(int fd, std::string name);
    ~FdFile() override;
    FdFile(const FdFile&) = delete;
    FdFile& operator=(const FdFile&) = delete;

    static std::unique_ptr<FdFile> open(const std::string& path);

    uint64_t size() const override { return size_; }
    void readAt(uint64_t offset, void* dst, size_t length) const override;

private:
    int fd_;
    uint64_t size_;
    std::string name_;
};

// Read-only view of a game's install folder. File names are matched
// case-insensitively, since the games were authored for Windows.
class GameFolder {
public:
    virtual ~GameFolder() = default;
    // Names of regular files directly in the folder.
    virtual std::vector<std::string> list() const = 0;
    // Names of regular files in a subfolder (case-insensitive name), e.g. "font".
    virtual std::vector<std::string> listDir(const std::string&) const { return {}; }
    // Opens a file by (case-insensitive) name, which may include subfolders
    // ("config/startup.xml"); returns nullptr if it does not exist.
    virtual std::unique_ptr<RandomAccessFile> open(const std::string& name) const = 0;
    virtual std::string displayName() const = 0;

    // Returns the actual on-disk name for a case-insensitive match, or "".
    std::string resolve(const std::string& name) const;
};

// GameFolder backed by a regular filesystem directory (desktop builds).
class DirectoryGameFolder final : public GameFolder {
public:
    explicit DirectoryGameFolder(std::string path);
    std::vector<std::string> list() const override;
    std::vector<std::string> listDir(const std::string& sub) const override;
    std::unique_ptr<RandomAccessFile> open(const std::string& name) const override;
    std::string displayName() const override { return path_; }

private:
    std::string path_;
};

std::string toLowerAscii(std::string s);

}  // namespace nyan
