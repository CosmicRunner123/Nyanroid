#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace nyan::cs2 {

// CatSystem2 script variables: integer flags (#N in scenes, $N in FE scripts)
// and string variables (str N). Unset values read as 0 / "".
class VarStore {
public:
    int32_t get(int32_t index) const {
        auto it = ints_.find(index);
        return it == ints_.end() ? 0 : it->second;
    }
    void set(int32_t index, int32_t value) { ints_[index] = value; }

    const std::string& str(int32_t index) const {
        static const std::string empty;
        auto it = strs_.find(index);
        return it == strs_.end() ? empty : it->second;
    }
    void setStr(int32_t index, std::string value) { strs_[index] = std::move(value); }

    const std::map<int32_t, int32_t>& ints() const { return ints_; }
    const std::map<int32_t, std::string>& strs() const { return strs_; }
    void clear() {
        ints_.clear();
        strs_.clear();
    }

private:
    std::map<int32_t, int32_t> ints_;
    std::map<int32_t, std::string> strs_;
};

}  // namespace nyan::cs2
