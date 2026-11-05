#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/cs2/scene_player.hpp"
#include "nyan/cs2/vars.hpp"

namespace nyan::cs2 {

// One save slot: when it was made, the strings scripts attach to it (`saveapend_str`, keyed by
// mask: 0x200 the message on screen, 0x80000000 a chapter title, ...), a thumbnail, and the
// resume point itself.
struct SaveFile {
    int64_t time = 0;  // seconds since the epoch, local time is derived when displayed
    std::map<uint32_t, std::string> strings;  // UTF-8
    int32_t thumbWidth = 0, thumbHeight = 0;
    Bytes thumbnail;  // RGBA, top row first
    ScenePlayer::Resume resume;
};

// Serialised resume points keep their timing relative to the frame they were saved on, so they
// can be restored into a player whose frame counter is somewhere else.
Bytes encodeSave(const SaveFile& save, uint64_t frame);
// Decodes a save; its tweens and motions are re-based on `frame`. Throws Error on bad data.
SaveFile decodeSave(const Bytes& data, uint64_t frame);

// Data that survives between plays: global flags and the engine configuration.
struct GlobalData {
    std::map<int32_t, int32_t> flags;
    std::map<int32_t, std::string> strings;
    std::map<std::string, int32_t> config;
    std::map<std::string, std::vector<uint8_t>> read;  // read messages: scene -> bitset by line
    std::set<std::string> cgSeen;                       // CG gallery: image names seen
    // The glossary (adv.xml <dictionary>): per entry key, its unlocked / new paragraphs and
    // whether it is new (the engine's dic.dat, sscript natives 872 / 873).
    struct DictionaryState {
        uint32_t appear = 0, updated = 0;
        bool fresh = false;
        bool operator==(const DictionaryState&) const = default;
    };
    std::map<std::string, DictionaryState> dictionary;
    // Text config values (`setconf <str> font`: the face strings start with).
    std::map<std::string, std::string> configText;
};
Bytes encodeGlobal(const GlobalData& g);
GlobalData decodeGlobal(const Bytes& data);

// Save slots and global data of one game, kept in a directory (or only in memory when the
// directory is empty). Slot files are read lazily and cached.
class SaveStore {
public:
    explicit SaveStore(std::filesystem::path dir = {});

    bool exists(int32_t slot);
    // Header data of a slot (time, strings, thumbnail); null if empty. The resume point inside is
    // based on frame 0; use resume() to restore.
    const SaveFile* get(int32_t slot);
    std::optional<ScenePlayer::Resume> resume(int32_t slot, uint64_t frame);
    void put(int32_t slot, const SaveFile& save, uint64_t frame);
    void remove(int32_t slot);
    void exchange(int32_t a, int32_t b);
    // Slot `to` becomes a copy of slot `from`; nothing happens if `from` is empty.
    void copy(int32_t from, int32_t to);
    // Most recently written existing slot in [from, to], or -1.
    int32_t newest(int32_t from, int32_t to);
    void appendString(int32_t slot, uint32_t mask, const std::string& text);
    // Bumps whenever a slot changes (for caching thumbnails).
    uint64_t version(int32_t slot) const;

    std::optional<GlobalData> loadGlobal();
    void saveGlobal(const GlobalData& g);

private:
    struct Entry {
        bool loaded = false;
        std::optional<Bytes> raw;      // encoded (frame 0 based)
        std::optional<SaveFile> file;  // decoded at frame 0
        uint64_t version = 0;
    };
    Entry& entry(int32_t slot);
    std::filesystem::path pathOf(int32_t slot) const;
    void writeFile(const std::filesystem::path& p, const Bytes& data) const;

    std::filesystem::path dir_;
    std::map<int32_t, Entry> slots_;
    uint64_t versionCounter_ = 0;
};

}  // namespace nyan::cs2
