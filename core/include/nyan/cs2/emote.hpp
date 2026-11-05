#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

struct PsbValue;

// An E-mote model (`.psz`, M2's animated character format): a texture atlas of parts and a tree
// of layers per motion. Layers carry keyframes (position, angle, opacity, Bezier mesh, image);
// motions nest (a layer can show another object's motion), and variables (head_LR,
// face_eye_open, fade_a ...) pick the time of the layers and motions tied to them.
//
// What is modelled, all read from the files (no engine code was read): keyframe types (2 holds,
// 3 tweens linearly to the next key, 0 leaves the layer without content of its own while its
// children still draw), variable-driven time (value - rangeBegin, scaled by division), accumulated
// z for depth (higher is in front; the motion's priority list breaks ties), and 4x4 Bezier meshes
// that bend the layer's own image and, with meshTransform, its children.
class EmoteModel {
public:
    struct Icon {
        float left = 0, top = 0, width = 0, height = 0, originX = 0, originY = 0;
    };
    // One image to draw: a grid of (grid+1)^2 points in model space over the icon's rectangle.
    struct Draw {
        const Icon* icon = nullptr;
        int grid = 0;
        std::vector<float> xy;  // x, y per grid point, row by row
        float opacity = 1;      // 0..1
        float z = 0;
        // The layer's blend mode (`bm` & 0xF, emotedriver.dll's table at 0x1008bc98, set at
        // 0x10061770): 0 normal; 3 multiply (dest x source premultiplied by its alpha, plus dest x
        // (1 - alpha)), the only other one Amakano 2 uses.
        int blend = 0;
        std::string label;  // the layer's
        // Stencils (type-12 layers above this one, outermost first; emotedriver.dll 0x10005930 with
        // the stencil back end 0x10062cb0 / 0x10062f70 / 0x100633b0): before drawing, each one's
        // mask layers (stencilCompositeMaskLayerList, by label; all their images in the pose) are
        // drawn into the stencil where it equals the stencils so far, pixels of alpha over 64/255
        // raising it by one; the image then shows where all of them did. Mode is stencilType & 3
        // (1 adds a mask; 2, a cut-out, and masks without bit 4 are not used by Amakano 2).
        struct Stencil {
            int mode = 1;
            std::vector<std::string> masks;
        };
        std::vector<Stencil> stencil;
        std::vector<std::vector<Draw>> maskDraws;  // per stencil, the mask images (filled by evaluate)
    };

    static std::optional<EmoteModel> load(ByteSpan psz, uint32_t key);

    // The pose at `time` (frames of the base motion) with the given variables.
    std::vector<Draw> evaluate(const std::map<std::string, float>& vars, float time, int grid = 8) const;

    int textureWidth() const { return texW_; }
    int textureHeight() const { return texH_; }
    const Bytes& rgba() const { return rgba_; }  // texW x texH, straight alpha

    // metadata/variableList: every variable the model knows.
    const std::vector<std::string>& variables() const { return variables_; }

    // metadata/timelineControl: named variable animations (see EmotePlayer for how they play).
    // Keys as the DLL keeps them: time, end (a type-0 frame), value and easing; an end key keeps
    // the previous key's value and easing.
    struct Timeline {
        struct Key {
            float time = 0;
            bool end = false;
            float value = 0, easing = 0;
        };
        float loopBegin = -1, loopEnd = -1, lastTime = -1, maxTime = 0;
        std::vector<std::pair<std::string, std::vector<Key>>> tracks;
        std::vector<std::string> variables;  // every variableList label, keys or not (tracks: those with keys)
    };
    const Timeline* timeline(const std::string& label) const;
    // metadata/instantVariableList (read at emotedriver 0x10014240): variables whose tracks a
    // difference timeline still sets outright (the loader, 0x1001e6b0, marks them, track +0x18).
    bool instantVariable(const std::string& name) const { return instant_.count(name) != 0; }

    // metadata/eyeControl: blinking moves `variable` between beginFrame and endFrame over
    // `frames`, every intervalMin..intervalMax frames, when enabled.
    struct Blink {
        std::string variable;
        int beginFrame = 0, endFrame = 0;
        float frames = 16, intervalMin = 30, intervalMax = 180;
        bool enabled = false;
    };
    const std::optional<Blink>& blink() const { return blink_; }
    // metadata/mouthControl talkLabel: the variable lip sync opens (face_talk, 0..5 in Amakano 2).
    const std::string& talkVariable() const { return talk_; }

    // metadata/bustControl: a spring that follows `layer` and sets varLR / varUD (EmotePlayer runs
    // it). Settings and the editor's saved state: anchor op, particle p, velocity pv, rest offset.
    struct Bust {
        std::string layer, varLR, varUD;
        float gravity = 0, spring = 0, friction = 0, scaleX = 1, scaleY = 1, ofs = 0;
        std::array<float, 3> op{}, p{}, pv{};
    };
    // metadata/hairControl and partsControl: a two-link pendulum hanging from `layer`, setting
    // varLR (tip), varLRM (middle) and varUD. bp: the links' rest points; p / pv their saved
    // positions and velocities.
    struct Pend {
        std::string layer, varLR, varLRM, varUD;
        float gravity = 0, frictionX = 0, frictionY = 0, bRate = 0, vBound = 0;
        int udEft = 0;
        float length[2] = {0, 0}, scaleX[2] = {1, 1}, scaleY[2] = {1, 1};
        float bendSpd = 0, bendVol = 0, ofs = 0, bendR = 0, bendS = 0;
        std::array<float, 3> op{};
        std::array<float, 3> bp[2]{}, p[2]{}, pv[2]{};
    };
    // metadata/clampControl: a pair of variables limited together before the pose uses them
    // (EmotePlayer::values). Type 0 maps the square of min..max onto a disc, type 1 pulls points
    // outside the circle back onto it.
    struct Clamp {
        int type = 0;
        float min = 0, max = 0;
        std::string varLR, varUD;
    };
    const std::vector<Clamp>& clamps() const { return clamps_; }
    // metadata/selectorControl (EPSelectorControl, loader 0x1001a390): a variable that picks one of
    // its options; the chosen option's variable goes to onValue, every other one to offValue
    // (Nekopara's `arm_type` over `fade_a`.. `fade_e`, the arm poses).
    struct Selector {
        std::string label;
        struct Option {
            std::string variable;
            float offValue = 1, onValue = 0;
        };
        std::vector<Option> options;
    };
    const std::vector<Selector>& selectors() const { return selectors_; }
    const std::vector<Bust>& busts() const { return busts_; }
    const std::vector<Pend>& hairs() const { return hairs_; }
    const std::vector<Pend>& parts() const { return parts_; }

    // Where the anchor layers (type 1) named in `labels` are in the pose for `vars`, in model space;
    // labels not found are left out.
    std::map<std::string, std::array<float, 2>> layerPositions(const std::map<std::string, float>& vars,
                                                               const std::set<std::string>& labels) const;

private:
    struct Frame {
        float time = 0;
        uint8_t type = 0;
        bool content = false;
        std::string src, icon;
        bool hasCoord = false, hasAngle = false, hasOpa = false, hasMesh = false;
        float x = 0, y = 0, z = 0, angle = 0, opa = 255;
        std::array<float, 32> mesh{};
        float timeOffset = 0;
        int bm = 0;
    };
    struct Layer {
        std::string label;
        int type = 0;
        int parameterize = -1;
        bool meshTransform = false;
        // inheritMask (layer +0x20, loader 0x10035ac0): bit 0x2000000 takes the bending of the mesh
        // layers above; without it they only move the layer (its origin's displacement), as for
        // the arm groups named `..._親変形無視` ("ignore the parent's deformation"). -1: absent.
        int inheritMask = -1;
        // meshSyncChildMask: bit 8 lets this mesh layer bend its children (0x1003f530: +0x2c0).
        int meshSyncChildMask = 8;
        int index = 999;  // layerIndexMap
        int stencilType = 0;
        std::vector<std::string> stencilMasks;
        std::vector<Frame> frames;
        std::vector<int> children;
    };
    struct Param {
        std::string id;
        float begin = 0, end = 0, division = 0;
    };
    struct Motion {
        std::vector<Layer> layers;
        std::vector<int> roots;
        int parameterize = -1;
        std::vector<Param> params;
        std::vector<std::pair<float, std::vector<int>>> priority;
    };
    struct Step;
    struct Eval;

    void addLayers(Motion& m, const PsbValue& list, std::vector<int>& out);
    void evalMotion(Eval& e, const std::string& obj, const std::string& name, float t, const std::vector<Step>& xf,
                    float z0, float opa0, std::vector<Draw>& out, int depth) const;

    int texW_ = 0, texH_ = 0;
    Bytes rgba_;
    std::map<std::string, Icon> icons_;
    std::map<std::string, std::map<std::string, Motion>> objects_;
    std::string baseChara_, baseMotion_;
    std::vector<std::string> variables_;
    std::map<std::string, Timeline> timelines_;
    std::set<std::string> instant_;
    std::optional<Blink> blink_;
    std::string talk_;
    std::vector<Clamp> clamps_;
    std::vector<Selector> selectors_;
    std::vector<Bust> busts_;
    std::vector<Pend> hairs_, parts_;
};

}  // namespace nyan::cs2
