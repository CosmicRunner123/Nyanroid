#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

// The files a Diesel game writes (the PC's save folder and anything else it writes): paths are
// '/'-separated and relative to the store's root, matched in either case as on Windows.
class SaveStore {
public:
    virtual ~SaveStore() = default;
    virtual std::optional<Bytes> read(const std::string& path) const = 0;
    // Creates missing folders on the way.
    virtual bool write(const std::string& path, ByteSpan data) = 0;
    virtual bool remove(const std::string& path) = 0;
    virtual bool exists(const std::string& path) const = 0;
    virtual bool isDirectory(const std::string& path) const = 0;
    virtual bool makeDirectory(const std::string& path) = 0;
    // Seconds since 1970 (UTC) of the last write, if the file exists.
    virtual std::optional<int64_t> modifiedTime(const std::string& path) const = 0;
};

// A store in a host folder (Session::Options::saveDirectory).
std::unique_ptr<SaveStore> makeDirectorySaveStore(std::string root);
// A store in memory (headless runs; nothing kept).
std::unique_ptr<SaveStore> makeMemorySaveStore();

// Mware's compressed file format (CFileAccessCompressWriter / Reader, Mware.dll 0x10005d00,
// 0x10006310, 0x10004b00): "MWCF\0", u32 total size, then blocks of "BLCK\0", u32 compressed size,
// u32 size and a zstd frame. Written as Mware writes it: one block, its frame compressed at level 3.
Bytes encodeCompressedFile(ByteSpan data);
// nullopt if the data is not in that format or a block does not decode.
std::optional<Bytes> decodeCompressedFile(ByteSpan file);

}  // namespace nyan::diesel
