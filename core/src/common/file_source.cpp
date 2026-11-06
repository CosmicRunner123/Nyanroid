#include "nyan/common/file_source.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

namespace nyan {

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

FdFile::FdFile(int fd, std::string name) : fd_(fd), name_(std::move(name)) {
    struct stat st {};
    if (fstat(fd_, &st) != 0) {
        int err = errno;
        ::close(fd_);
        fail("fstat failed for " + name_ + ": " + std::strerror(err));
    }
    size_ = static_cast<uint64_t>(st.st_size);
}

FdFile::~FdFile() {
    ::close(fd_);
}

std::unique_ptr<FdFile> FdFile::open(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    return std::make_unique<FdFile>(fd, path);
}

void FdFile::readAt(uint64_t offset, void* dst, size_t length) const {
    auto* out = static_cast<uint8_t*>(dst);
    while (length > 0) {
        ssize_t n = ::pread(fd_, out, length, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) continue;
            fail("Read failed for " + name_ + ": " + std::strerror(errno));
        }
        if (n == 0) fail("Unexpected end of file in " + name_ + " at offset " + std::to_string(offset));
        out += n;
        offset += static_cast<uint64_t>(n);
        length -= static_cast<size_t>(n);
    }
}

std::string GameFolder::resolve(const std::string& name) const {
    std::string want = toLowerAscii(name);
    for (const auto& entry : list()) {
        if (toLowerAscii(entry) == want) return entry;
    }
    return {};
}

DirectoryGameFolder::DirectoryGameFolder(std::string path) : path_(std::move(path)) {
    if (!std::filesystem::is_directory(path_)) fail("Not a directory: " + path_);
}

std::vector<std::string> DirectoryGameFolder::list() const {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(path_)) {
        if (entry.is_regular_file()) names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> DirectoryGameFolder::listDir(const std::string& sub) const {
    // Each part of the path ("sound/bgm") matched case-insensitively.
    std::vector<std::string> names;
    std::error_code ec;
    std::filesystem::path current(path_);
    for (size_t start = 0; start <= sub.size();) {
        size_t slash = sub.find_first_of("/\\", start);
        std::string part = sub.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? sub.size() + 1 : slash + 1;
        if (part.empty()) continue;
        std::filesystem::path match;
        for (const auto& dir : std::filesystem::directory_iterator(current, ec)) {
            if (dir.is_directory() && toLowerAscii(dir.path().filename().string()) == toLowerAscii(part)) {
                match = dir.path();
                break;
            }
        }
        if (match.empty()) return names;
        current = match;
    }
    if (current == std::filesystem::path(path_)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
        if (entry.is_regular_file()) names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::unique_ptr<RandomAccessFile> DirectoryGameFolder::open(const std::string& name) const {
    // Resolve each path component case-insensitively ("config/startup.xml").
    std::filesystem::path current(path_);
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find_first_of("/\\", start);
        std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? name.size() + 1 : slash + 1;
        if (part.empty()) continue;
        std::string want = toLowerAscii(part);
        std::filesystem::path match;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
            if (toLowerAscii(entry.path().filename().string()) == want) {
                match = entry.path();
                break;
            }
        }
        if (match.empty()) return nullptr;
        current = match;
    }
    if (!std::filesystem::is_regular_file(current)) return nullptr;
    return FdFile::open(current.string());
}

}  // namespace nyan
