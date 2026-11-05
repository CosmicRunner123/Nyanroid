#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/diesel/crypto.hpp"

namespace nyan::diesel {

using NpkKey = std::array<uint8_t, 32>;

// One stored piece of an entry (0x14 bytes in the index).
struct NpkSegment {
    uint64_t offset = 0;        // in the archive
    uint32_t alignedSize = 0;   // bytes stored (a multiple of 16: AES blocks)
    uint32_t size = 0;          // bytes of the decrypted data the segment holds
    uint32_t unpackedSize = 0;  // bytes it gives
    // Mware (0x100b8400): packed (zstd in NPK3, raw deflate in NPK2) when size < unpackedSize; else
    // the first unpackedSize bytes as they are (stored segments may give a size above unpackedSize).
    bool compressed() const { return size < unpackedSize; }
};

struct NpkEntry {
    std::string name;                 // as stored: UTF-8, '/' separated ("media/cg/bg/x.png")
    bool flag = false;                // the index's first byte (1: one segment, never split)
    uint32_t size = 0;                // unpacked size
    std::array<uint8_t, 32> sha256{}; // of the unpacked data
    std::vector<NpkSegment> segments; // none for an empty file
};

// An NPK3 archive (Mware.dll's CArchiveNPK, Full Metal Daemon Muramasa's Mware 0x100b7090):
//   "NPK3", u32 version (must be 1), IV[16], u32 entry count, u32 index size, then the index,
//   AES-256-CBC with the game's key (the exe passes it with every archive it mounts).
//   Entry: u8 flag, u16 name length (at most 0x104), name, u32 size, SHA-256[32], u32 segment count,
//   segments of {u64 offset, u32 aligned size, u32 size, u32 unpacked size}.
// Each segment is AES-256-CBC from the archive's IV again, then zstd if compressed().
// NPK2 (The Song of Saya, its Mware of 2019; version 2) is the same layout, its packed segments raw
// deflate (CFilePackedObjectNPK's read 0x1000746d -> 0x100a3000 -> 0x100a2850, inflateInit2 with
// window bits -15; that Mware holds no zstd).
// Reading entries is thread-safe.
class NpkArchive {
public:
    // Throws nyan::Error if the file is not an NPK3 / NPK2 archive or the key does not open its index.
    NpkArchive(std::unique_ptr<RandomAccessFile> file, std::string name, const NpkKey& key);

    const std::string& name() const { return name_; }
    const std::vector<NpkEntry>& entries() const { return entries_; }
    // The entry of a name normalised as Mware does (normalizeName), if the archive has it.
    const NpkEntry* find(std::string_view normalizedName) const;
    Bytes read(const NpkEntry& entry) const;
    // The entry as a file read piece by piece: each read decrypts (and unpacks) only the segments it
    // touches, the last few kept, so a movie of hundreds of megabytes streams. The archive must
    // outlive it. Thread-safe.
    std::unique_ptr<RandomAccessFile> open(const NpkEntry& entry) const;

    // True if `key` opens this file's index: its first entry and the index's PKCS#7 padding are
    // well formed. Used to pick the key among the exe's candidates.
    static bool keyOpens(const RandomAccessFile& file, const NpkKey& key);

    // One segment decrypted and unpacked (its unpackedSize bytes).
    Bytes readSegment(const NpkEntry& entry, size_t index) const;

private:
    std::unique_ptr<RandomAccessFile> file_;
    std::string name_;
    Aes256 aes_;
    uint8_t iv_[16];
    bool deflate_ = false;  // NPK2: packed segments are raw deflate
    std::vector<NpkEntry> entries_;
    std::unordered_map<std::string, size_t> index_;  // normalised name -> entry
};

// Whether the file starts as an NPK archive Nyanroid reads: "NPK3" version 1 or "NPK2" version 2.
bool isNpkArchive(const RandomAccessFile& file);

// Mware's lookup key for a name (0x100b6e30): ASCII letters lower-cased, everything else as it is.
std::string normalizeName(std::string_view name);

}  // namespace nyan::diesel
