#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// A value in a PSB file (M2's packed object format, used by E-mote models). PSB holds a JSON-like
// tree: null, booleans, numbers, strings, lists, objects (name -> value) and references to binary
// resources (textures).
struct PsbValue {
    enum class Type : uint8_t { Null, Bool, Int, Float, String, IntArray, List, Object, Resource };
    Type type = Type::Null;
    int64_t i = 0;       // Bool, Int, Resource (index)
    double f = 0;        // Float
    std::string s;       // String
    std::vector<int64_t> ints;                                 // IntArray
    std::vector<PsbValue> list;                                // List
    std::vector<std::pair<std::string, PsbValue>> object;      // Object, sorted by name

    bool isNull() const { return type == Type::Null; }
    bool isNumber() const { return type == Type::Int || type == Type::Float || type == Type::Bool; }
    double number(double fallback = 0) const {
        return type == Type::Float ? f : (type == Type::Int || type == Type::Bool) ? static_cast<double>(i) : fallback;
    }
    const std::string& str() const { return s; }
    // Object member, or null.
    const PsbValue* get(std::string_view key) const;
    const PsbValue& operator[](std::string_view key) const;
    double num(std::string_view key, double fallback = 0) const;
    // A list's elements or an int array's values as numbers.
    std::vector<double> numbers() const;
};

class Psb {
public:
    // Parses a PSB or PSZ (zlib-wrapped PSB) file. Encrypted headers (version 3+) are decoded with
    // `key`; returns nullopt if the file is not a PSB or the key does not fit.
    static std::optional<Psb> load(ByteSpan file, uint32_t key);
    // True if `key` decodes the file's header into a consistent one (used to pick the key).
    static bool keyFits(ByteSpan file, uint32_t key);
    // The engine library's key candidates: E-mote drivers keep it as a decimal string.
    static std::vector<uint32_t> keyCandidates(ByteSpan library);

    const PsbValue& root() const { return root_; }
    ByteSpan resource(size_t index) const;

private:
    Bytes data_;
    PsbValue root_;
    std::vector<std::pair<uint32_t, uint32_t>> resources_;  // offset, length within data_
};

}  // namespace nyan::cs2
