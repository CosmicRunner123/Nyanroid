#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// The engine's glossary (Cyanotype's tips), as cs2.exe keeps it (the class at 0x82a630..0x82bf10).
// adv.xml <dictionary> names a KCDB table (`tips_%LANG_M%.qdb`); its rows are keyed by INDEX (the
// id scenes register, "1001"), each with a TITLE, the original TITLE_S and up to 24 paragraphs
// IND1..IND24. A scene's `dic_reg <n> <id>` unlocks paragraph n of that entry; the tips screen asks
// with the FE `dic_*` commands.
class Dictionary {
public:
    struct Entry {
        int32_t id = 0;  // the row's ID column
        std::string title, titleS;
        std::array<std::string, 24> ind;
        uint32_t appear = 0;   // +0x94: paragraphs unlocked
        uint32_t updated = 0;  // +0x98: paragraphs unlocked since last viewed
        bool fresh = false;    // +0x9c: the entry appeared since last viewed
    };

    // Reads a KCDB table ("KCDB", compressed size, size, zlib data: column count, columns of
    // name[16] type size (0 an integer, 2 a string of that many bytes), row count, row size, rows).
    // False if the data is not one.
    bool load(const Bytes& kcdb);
    bool loaded() const { return !entries_.empty(); }

    // adv.xml's text pieces: before each paragraph, and after it (`first`, `between`).
    void setJoiners(std::string first, std::string between) { first_ = std::move(first), between_ = std::move(between); }

    // Scene side (sscript natives 871 / 878 / 877).
    void reg(int group, const std::string& key);  // 0x82a630 with on = 1
    void resetApend() { updated_ = false; }        // 0x82a7f0
    bool apended() const { return updated_; }      // +0x4c (0x68c960)

    // FE side.
    bool find(const std::string& from, const std::string& to, int32_t n, std::string& key) const;  // 0x82b0a0
    uint32_t appear(const std::string& key) const;     // 0x82b210: unlocked paragraphs that have text
    uint32_t maxAppear(const std::string& key) const;  // 0x82b8d0: paragraphs that have text
    bool title(const std::string& key, std::string& title, std::string& titleS) const;  // 0x82ba80 / 0x82bbf0
    std::string ind(const std::string& key) const;     // 0x82b620
    bool update(const std::string& key, int32_t& fresh, int32_t& updated) const;  // 0x82bd60
    void updateClear(const std::string& key);          // 0x82a800; empty: every entry (0x82a960)
    int32_t position(const std::string& key) const;    // 0x82b410: the entry's place in key order, from 1

    // What the engine saves (dic.dat, natives 872 / 873): each entry's three states.
    struct State {
        uint32_t appear = 0, updated = 0;
        bool fresh = false;
    };
    std::map<std::string, State> state() const;
    void restore(const std::map<std::string, State>& s);

private:
    std::map<std::string, Entry> entries_;
    std::string first_, between_;
    bool updated_ = false;
};

}  // namespace nyan::cs2
