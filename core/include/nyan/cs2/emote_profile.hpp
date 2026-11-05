#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nyan::cs2 {

class AssetIndex;

// The engine's E-mote configuration (`m2config.int`), which turns a scene's sprite spec
// (`mpl <bank> <profile>,<body>,<digit 1>,...`) into a model file and its variables. The spec (cs2.exe 0x57e560) is `<profile>,<body>,<digit 1>,<digit 2>...[:<motion>]`,
// the motion "0" when left out:
//
//   m2view.cnf   #profile: spec base -> profile (.pfl); #position is m2view.exe's
//   <x>.pfl      #charaname: body (Amakano's costume letters, Nekopara's numbers) -> model (.psz)
//                #motname: body, motion, set name (the idle timeline), display profile (.dif),
//                [body profile]; the .dif is the body's motion "0" row's (0x59d5c0), the set the
//                spec's motion's (0x59da80)
//   <x>.dif      #include (.inc); #position, #scale, #scaleoffset: the placement
//   <x>.inc      #defval; #dif: digit n -> part name; #pattern: "%n <value>" -> variable values
//
// Argument numbering: 0 is the body, so `#dif 9 <part>` is the tenth argument.
class EmoteProfiles {
public:
    struct Resolved {
        std::string model;  // .psz name
        std::map<std::string, float> vars;
        // Placement, the .dif's (cs2.exe 0x587ba0 reads it, 0x574070 uses it every frame; Amakano's
        // 0x4d3bb0 the same): the model's coordinate (from the centre of the view) is the bank's
        // position plus `#position` plus `#scaleoffset` x (1 - zoom), its scale `#scale` (x) times
        // the zoom, the bank's own scale.
        int32_t posX = 0, posY = 0;
        float scale = 1;
        float scaleOffsetX = 0, scaleOffsetY = 0;
        // The .dif's #attribute rows, "<no> <x> <y> <ex>" (cs2.exe 0x5898d0; integers, missing
        // columns 0): points `m2att` reads (Nekopara: where the emotion marks go).
        std::map<int32_t, std::array<int32_t, 3>> attributes;
        std::string idle;  // the set's timeline (.pfl #motname's set)
        std::string bodyDif;  // .pfl #motname's fifth column (the body profile), the `m2body` digits
        // Lip sync for the spec's expression (.inc #lip_variable, #lip_table, #clipper): the
        // variable the mouth drives, its value at rest (the table row's first pattern), the
        // vowel weights (silence, a, i, u, e, o), latency and smoothing (frames).
        std::string lipVariable;
        float lipRest = 0;
        std::array<float, 6> lipWeights{0.0f, 1.0f, 0.4f, 0.2f, 0.8f, 0.6f};
        int32_t lipLatency = 0, lipSmooth = 1;
        // For a voice with no lip data (cs2.exe 0x574640 / 0x583f40): the #lip_table row's level
        // threshold ("levelmin"), its interpolation (linear 1, else 0) and pattern values, and the
        // expression's #lip_flow steps (pattern number, frames, end check).
        float lipLevelMin = 0;
        int32_t lipInterp = 0;
        std::vector<float> lipPatterns;
        struct LipStep {
            int32_t pattern = 0, frames = 1, check = 0;
        };
        std::vector<LipStep> lipFlow;
        // .inc #chain: how a change of argument n goes, "n <type> <frames>" (cs2.exe 0x4f1df0, types
        // 0x4ed9b0): fix 0, linear 1, sin_fs 2, sin_sf 3, sin_ss 4, fade 5 (a crossfade), nomove 6.
        std::map<int32_t, std::pair<int32_t, int32_t>> chains;
        // "n $name" instead names a `$name` table (cs2.exe 0x597120): `default <type>` and `frame <n>`
        // for any change, rows "<from> <to> <type> <frames>" for particular ones (Nekopara's $eye /
        // $blow: eyes and brows linear over 10 frames). Its `fix` / `fade` name lists are not used
        // by these games and not read.
        struct ChainTable {
            int32_t type = 0, frames = 0;
            std::map<std::pair<std::string, std::string>, std::pair<int32_t, int32_t>> moves;
        };
        std::map<int32_t, ChainTable> chainTables;
        // How argument n's change from one value to another goes: its table's row or default, else
        // its #chain row, else fix.
        std::pair<int32_t, int32_t> argChain(int32_t n, const std::string& from, const std::string& to) const;
        // The spec's arguments (as resolve splits them) and, for each variable an argument's
        // #pattern block sets, that argument's #dif number (its #chain row).
        std::vector<std::string> args;
        std::map<std::string, int32_t> varArg;
        // .inc #blink rows for the spec's own arguments ("<argument> <value> <start> <end> <speed>
        // <min> <max>", cs2.exe 0x4f21b0 / 0x4d7610): the variable is the first one the argument's
        // #pattern block sets, its value the rest value; a blink runs it start -> end -> start over
        // `speed` frames, the next one min + rand % (max - min) frames on (0x4e2410).
        struct Blink {
            std::string variable;
            float rest = 0, start = 0, end = 0;
            int32_t frames = 1, min = 0, range = 1;
        };
        std::vector<Blink> blinks;
        // cs2.exe's own springs (Nekopara: the bust, hair and parts sway; the models' DLL physics is
        // switched off). The .inc's sections (reader 0x5a75e0): #material per variable (mass, spring,
        // resistance, time step, maximum move, invert, scale, shift), #material_min / _max (the
        // inertia level's ends), #inertia_ud / _lr (the variables the bank's vertical / horizontal
        // movement pushes), #inertia_lv (`%group` then its variables, which the group's level
        // blends), #inertia_enable (`%group` then variables, switched together), #inertia_dif
        // (source variable, pushed variable, power, source range).
        struct Material {
            float mass = 1, spring = 0, resistance = 0, dt = 0, maxMove = 1;
            bool invert = false;
            float scale = 1, shift = 0;
        };
        struct InertiaDif {
            std::string source, target;
            float power = 0, range = 1;
        };
        std::map<std::string, Material> materials, materialMin, materialMax;
        std::vector<std::string> inertiaUD, inertiaLR;
        std::map<std::string, std::string> levelGroup;   // variable -> its #inertia_lv group
        std::map<std::string, std::string> enableGroup;  // variable -> its #inertia_enable group ("" none named)
        std::vector<InertiaDif> inertiaDifs;

        // Touch mode (Nekopara; the .inc readers 0x58c020 and 0x58b4c0). #hittest: the variables
        // that turn the model's hit layer on (set to 0; 1 turns it off, 0x575650). #touch: "<colour>
        // $table0 $table1 ...", the hit layer's colour (ARGB, matched exactly, 0x587a70) -> the
        // action tables a bank's `touch [on] [table]` picks from; a `$table` lists "<action> <voice>"
        // rows.
        std::vector<std::string> hitTest;
        struct TouchRow {
            std::string action, voice;
        };
        std::map<uint32_t, std::vector<std::vector<TouchRow>>> touch;
    };

    // action.def's `$name` blocks (cs2.exe 0x5a9da0), which touch mode plays on a bank. Steps: a
    // `frame` line starts one (its frames, as SetVariable's), `easing` sets its easing, `var <name>
    // <value>` adds a variable move; `wait <frames>` is a step of its own that only waits;
    // `restore` makes the step send each variable the action moves back to where it was when the
    // action first ran.
    struct Action {
        struct Step {
            float frames = 0, easing = 0;
            int32_t wait = 0;
            bool restore = false;
            std::vector<std::pair<std::string, float>> vars;
        };
        std::vector<Step> steps;
        std::vector<std::string> vars;  // every variable the steps move, in first-use order
    };
    const Action* action(const std::string& name) const;

    // Reads m2view.cnf; empty (no profiles) for games without E-mote.
    static EmoteProfiles load(const AssetIndex& assets);
    bool empty() const { return profiles_.empty(); }
    // A spec whose base names a profile; nullopt for ordinary sprite specs.
    std::optional<Resolved> resolve(const std::string& spec) const;
    // Frames of the crossfade when a bank's model changes (m2view.cnf #fade).
    int32_t fadeFrames() const { return fade_; }
    // The crossfade when a shown bank keeps its model and changes arguments (cs2.exe 0x4d3430): the
    // frames of the last changed argument whose #chain type is fade, or 0 (the change is instant).
    static int32_t chainFade(const Resolved& to, const std::string& fromSpec, const std::string& toSpec);

    // A body profile (a .dif the .pfl #motname fifth column names, as Nekopara has), which `mpl <n> m2body`
    // digits go through: the variables #pattern sets for digit `digit` (1-based) at `value`; the
    // digit's #chain, type (fix 0, linear 1, sin_fs 2, sin_sf 3, sin_ss 4, fade 5, nomove 6) and
    // frames, {0, 0} if none; and the digits it starts with (cs2.exe 0x587430): each digit's pattern
    // marked `default`, else its first, else "0".
    std::vector<std::pair<std::string, float>> bodyPattern(const std::string& dif, int32_t digit, const std::string& value) const;
    std::pair<int32_t, int32_t> bodyChain(const std::string& dif, int32_t digit) const;
    std::vector<std::string> bodyDefaults(const std::string& dif) const;
    // The body profile's #group (cs2.exe 0x58ae70): each row names models (as a spec's first field) of one
    // character; 0x5878e0 asks whether two names are both listed in the same row. #group_inertia:
    // the variables whose inputs nudge the springs on such a change (0x5785a0).
    bool sameGroup(const std::string& dif, const std::string& a, const std::string& b) const;
    std::vector<std::string> groupInertia(const std::string& dif) const;

    // Whose mouth a voice moves (config.int/sndanm.xml, read by sscript.kcs at 0x0fb2db when its
    // <mode> is MPL): a voice whose file name starts with chNN/<sound> is lip-synced on the banks
    // whose image starts with chNN/<image>, at most chNN/<limit> of them (0x09918f).
    struct LipChannel {
        std::string image, sound;
        int32_t limit = 1;
    };
    const LipChannel* lipChannel(const std::string& voice) const;
    // The same entries when <mode> isn't MPL: the voice moves images' ANM mouths (sscript 0x099953),
    // `limit` being the level that opens them.
    const std::vector<LipChannel>& imageLipChannels() const {
        static const std::vector<LipChannel> none;
        return lipMpl_ ? none : lipChannels_;
    }

private:
    using Sections = std::map<std::string, std::vector<std::vector<std::string>>>;
    const Sections* file(const std::string& name) const;

    const AssetIndex* assets_ = nullptr;
    std::map<std::string, std::string> profiles_;  // lower-case spec base -> .pfl
    int32_t fade_ = 20;
    mutable std::map<std::string, std::optional<Sections>> files_;
    std::vector<LipChannel> lipChannels_;
    bool lipMpl_ = false;
    mutable std::map<std::string, std::optional<Action>> actions_;
};

}  // namespace nyan::cs2
