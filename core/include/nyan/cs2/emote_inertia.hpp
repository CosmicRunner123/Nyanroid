#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nyan/cs2/emote_profile.hpp"

namespace nyan::cs2 {

class EmotePlayer;

// cs2.exe's inertia: springs on model variables, driven by the bank's movement and by expression
// changes, built from the character's .inc (setup 0x591eb0). Each frame (kcTaskM2Plane update
// 0x573d60 -> 0x5930c0):
//   - the variables of #inertia_lr / _ud are pushed back by the bank's horizontal / vertical move
//     since the last frame (none on the first);
//   - each #inertia_dif row pushes its target back by (source change) / range x the target's
//     maximum move x power (0x594530);
//   - every spring steps once (0x594760): x clamped to +-maxMove, v += (-spring x - resistance v) /
//     mass x dt, x += v dt, output x / maxMove; a disabled spring rests at 0;
//   - the variable shows its own value plus output x scale, negated when inverted (0x5948f0).
// A group's inertia level (`m2_setconf iner_lv <0-200> <group>`, x 0.01, default 1) blends the
// #inertia_lv variables' time step, spring, mass, resistance and maximum move: from
// #material_min to #material at 0-1, from #material to #material_max at 1-2 (0x593c60); setting
// it puts the springs at rest.
class EmoteInertia {
public:
    explicit EmoteInertia(const EmoteProfiles::Resolved& profile);
    bool empty() const { return springs_.empty(); }

    void setLevel(const std::string& group, int32_t level);
    void setEnabled(const std::string& group, bool on);
    // `m2iner pause` (0x593800): a paused spring rests at 0 and is not stepped; a change resets it.
    void setPaused(const std::vector<std::string>& names);
    // A model change within the body profile's #group (cs2.exe 0x5785a0 -> 0x594400): the expression inputs
    // reading these variables (#group_inertia) each give their target spring one push of maxMove /
    // range on their next step (0x594530's flagged branch), in place of a change.
    void nudge(const std::vector<std::string>& sources);
    // One frame at the bank's position; reads the dif sources from the player and sets its offsets.
    // `active` false (E-mote motion or inertia off) puts every spring at rest.
    void step(float x, float y, EmotePlayer& player, bool active);

private:
    using Material = EmoteProfiles::Resolved::Material;
    struct Spring {
        Material base, min, max;
        std::string levelGroup, enableGroup;
        float dt = 0, spring = 0, mass = 1, resistance = 0, maxMove = 1;
        bool enabled = true, paused = false;
        float x = 0, v = 0, out = 0;
    };
    struct Dif {
        std::string source, target;
        float power = 0, invRange = 1;
        float cur = 0, prev = 0;
        bool started = false;
        bool nudge = false;
    };
    void blend(Spring& s, float level) const;

    std::map<std::string, Spring> springs_;
    std::vector<std::string> ud_, lr_;
    std::vector<Dif> difs_;
    std::map<std::string, int32_t> levels_, enables_;
    bool first_ = true;
    float px_ = 0, py_ = 0;
};

}  // namespace nyan::cs2
