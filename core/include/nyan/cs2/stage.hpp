#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nyan/cs2/anm.hpp"
#include "nyan/cs2/message_text.hpp"

namespace nyan::cs2 {

// An image's parts' ANM scripts as they run (cs2.exe gives each image part with a `<part>.anm` its
// own ANM object, 0x650560): stepped by the scene player each frame.
struct StageAnm {
    std::vector<std::string> parts;                   // the parts they were made for
    std::vector<std::unique_ptr<AnmPlayer>> players;  // per part; null: the part has no script
    // Whether they run: sscript's load (0x096acd) plays an image's animation only while the config's
    // enable_sceneanm is on (native 1050), else stops it, and a stopped one keeps the part's frame.
    bool playing = true;
    const AnmPlayer* player(size_t part) const { return part < players.size() ? players[part].get() : nullptr; }
};

class EmotePlayer;
class EmoteInertia;
class ParticleEffect;

// Image layers, in back-to-front draw order. Mpl: E-mote character banks (`mpl <n> ...`).
enum class Layer : uint8_t { Bg, Cg, Eg, Fg, Fw, Mpl };
constexpr size_t kLayerCount = 6;
const char* toString(Layer layer);

// Linear interpolation of a value over a number of frames.
struct Tween {
    int32_t from = 0;
    int32_t to = 0;
    uint64_t start = 0;
    uint32_t frames = 0;
    // The curve, as mot.int's motion scripts compute it: 0 linear (motMove); 1 eases out,
    // sin(pi/2 t) (motAMove1); 2 eases in, 1 - cos(pi/2 t) (motAMove2); 3 eases in and out,
    // (1 - cos(pi t)) / 2 (motAMove3); 4 there and back, the motion engine's triangle wave peaking
    // halfway and ending at `from` again (motFade2).
    uint8_t ease = 0;

    int32_t value(uint64_t now) const;
    bool done(uint64_t now) const { return now >= start + frames; }
    static Tween constant(int32_t v) { return {v, v, 0, 0}; }
};

// A time-driven offset/scale/rotation animation (`arc`, `mcmove`, `mcarc`, `mcscalecos`, `mcrot`).
// Values are evaluated at render time from the frame counter.
struct Motion {
    enum class Kind : uint8_t { None, Move, Arc, ScaleCos, Rotate, Shake, Settle, Path, Pendulum, ScaleSaw, Fade, AddPos, AddScale, CFade, Disp };
    Kind kind = Kind::None;
    bool loop = false;     // mc*: repeats every `period` frames until stopped
    uint64_t start = 0;
    uint32_t period = 1;
    // Move: fromX, fromY, toX, toY.  Arc: angle0, angle1 (deg), radiusX, radiusY.
    // ScaleCos: fromX%, fromY%, toX%, toY%.  Rotate: frames per turn about z, x, y (a[0], a[1],
    // a[2]; negative turns the other way, 0 not at all).
    // Shake: amplitudeX, amplitudeY, oscillations per period.
    // Settle: an animated change already applied to the target: starting scale factors x, y and
    // rotation offsets (deg; a[2] z, a[3] x, a[4] y), easing to none over the period along the
    // curve a[5] (Tween::ease: 0 linear, 1-3 motAScale1-3).
    // Pendulum (motPend / mcPend): a[0] swing period in frames, a[1] z amplitude (deg); the angle is
    // sin(triangle(t) * pi/2) * amplitude. One-shot ones last `period` frames.
    // ScaleSaw (mcScale): fromX%, fromY%, toX%, toY%, from -> to over each cycle, then from again;
    // a[4], a[5] the x and y cycle lengths.
    // Fade (mcFade): from, to, hold; the opacity goes from -> to -> from over `period` frames, then
    // stays at `from` for `hold` more (newer mcFade; older ones have no hold).
    // AddPos / AddScale: a relative motion (move, amoveN / mscale, ascaleN) started while another
    // one of the object still runs. The engine runs all of an object's motions each frame and adds
    // their contributions (mot.int `addset`), so the two add up. a[0], a[1] the change in x and y
    // (pixels; scale with 1 for 100%), already counted in the object's own value: the motion adds
    // (curve - 1) x change, curve a[5] (Tween::ease), from minus the change to nothing.
    // CFade (mcCFade): the colour's bytes (blue, green, red, alpha: keys[0..3], each {from, to, 0})
    // go from -> to -> from over `period` frames, as mcFade's opacity does, without a hold.
    // Disp (mcDisp): keys[i][0] the phase lengths in frames, shown, hidden, shown, ... over `period`
    // (their sum) frames, again and again; it sets the display flag each frame.
    float a[6] = {0, 0, 0, 0, 0, 0};
    // Path: keyframes (frames to get there, x, y). a[0] = 1: positions (`spline`, the engine's
    // cubic spline from a[1], a[2]; `mcspline` round the keys), else offsets from the resting place
    // (`quake`, straight steps from 0,0). a[3] = 1: the spline sets the scale instead, in percent
    // (`spline_s`, motSpline_s: motSpline on the scale fields).
    std::vector<std::array<float, 3>> keys;

    bool active(uint64_t now) const { return kind != Kind::None && (loop || now < start + period); }
    // The engine motion's group (sscript passes it to native 535): 1 for mc* and m2* ones, 0 for the
    // classic ones. `marked`: the group is set (planes only; neither is saved).
    uint8_t group = 0;
    bool marked = false;
    // 0..1 progress through the current cycle.
    float phase(uint64_t now) const;
};

// The engine's default draw depth (base z) of an image slot or a plane (sscript.kcs); lower is
// further back.
float defaultDepth(Layer layer, int32_t index, int32_t mplFaceFrom = 10);
inline float defaultPlaneDepth(int32_t plane) { return 5000.0f + 1000.0f * static_cast<float>(plane); }

// The 2D camera (`cam`, Happiness 2), kept among the planes under this index so that it takes the
// plane subcommands. It is sscript's slot 79 (keyword 21 -> 0x0714d6 -> the image command
// 0x01b28f): its position, scale and rotation go to the camera task (kcTask2DCamera, task type
// 65; natives 84 / 101 / 344 -> messages 0x33 / 0x42 / 0x39, 0x0cc57a), and every plane vram.xml
// defines is drawn through it (0x08fbe7 -> native 1066, message 0x250), as are particles on no
// plane; E-mote banks and the window are not. The draw (cs2.exe 0x5825c0) takes each vertex v to
// C - P + (S (v - C)) R: C the screen's centre, P the camera's position, S its scale, R its
// rotation. Never drawn as a plane.
constexpr int32_t kCameraPlane = 9999;

// A random whole number in [lo, hi] (either order), for the scripts' `rand`.
int32_t randomBetween(int32_t lo, int32_t hi);

// Evaluated transform contributions of a set of motions at a given frame.
struct MotionState {
    float dx = 0, dy = 0;          // added offset
    bool hasPos = false;           // Move overrides position
    float x = 0, y = 0;
    float scaleX = 1, scaleY = 1;  // multiplied
    float addScaleX = 0, addScaleY = 0;  // AddScale: added to the object's own scale (1 = 100%) first
    float rotation = 0;            // degrees, added
    float rotX = 0, rotY = 0;      // degrees of x/y tilt, added
    // mcscale sets the scale outright (percent / 100); negative: not set.
    float setScaleX = -1, setScaleY = -1;
    int32_t setAlpha = -1;  // mcfade sets the opacity outright (0..255); negative: not set
    int32_t setVisible = -1;  // mcdisp sets the display flag (0 / 1); negative: not set
    // mccfade sets the colour outright (0xAARRGGBB; its alpha also goes to setAlpha).
    bool hasColor = false;
    uint32_t color = 0xFFFFFFFF;
};
MotionState evaluate(const std::vector<Motion>& motions, uint64_t now);
// Tween::ease's curves over progress p (0..1): 0 linear (motMove, motScale), 1 eases out (motAMove1,
// motAScale1), 2 eases in (motAMove2), 3 in and out (motAMove3).
float easeCurve(float p, int ease);
// What the running AddPos / AddScale motions (`kind`) add on `axis` (0 x, 1 y) at `now`.
float addedValue(const std::vector<Motion>& motions, Motion::Kind kind, int axis, uint64_t now);

// The engine's object rotation (D3DX RotationX * RotationY * RotationZ, row vectors, degrees) as
// drawn flat: the 2x2 that takes a local offset (x, y) to (x m00 + y m10, x m01 + y m11). Positive
// z turns clockwise on the y-down screen; a y tilt narrows by cos, 180 mirrors.
std::array<float, 4> rotation2x2(float degX, float degY, float degZ);

// One image slot, e.g. `cg 0`. Everything is in virtual-screen pixels.
struct StageImage {
    std::string spec;                // name as written in the script (for saves/debugging)
    std::vector<std::string> parts;  // asset names (without .hg3), drawn in order
    bool solid = false;              // `$AARRGGBB` solid colour rectangle
    uint32_t solidColor = 0;
    int32_t solidWidth = 0, solidHeight = 0;

    Tween x, y;                 // anchor position (image BaseX/BaseY lands here)
    // Where the engine last settled the image (its sscript record's +124): the position, motions
    // included, from the last update in which nothing on it moved, as of this frame's first line
    // (ScenePlayer::settlePositions). `@` in a load's or `pos`'s coordinates counts from it. A new
    // or cleared slot starts at its default place; NaN (a loaded save): the position itself.
    float settledX = std::numeric_limits<float>::quiet_NaN(), settledY = std::numeric_limits<float>::quiet_NaN();
    // Where the image was placed when loaded (or by `pos`); `mcmove` targets are in these terms, so a
    // later instant `move` is an offset that travels along. INT32_MIN: unknown (use the position).
    int32_t loadX = INT32_MIN, loadY = INT32_MIN;
    // The engine motion group (1: m2* commands) of the last motion on the position, the opacity and
    // the scale tweens: skipping finishes group 0 at once (see ScenePlayer::setSkipping). Not saved.
    uint8_t posGroup = 0, alphaGroup = 0, scaleGroup = 0;
    // `base <x> <y>`: the point of the image's canvas that lands on its position, in place of the
    // HG base point. INT32_MIN: the image's own base.
    int32_t baseX = INT32_MIN, baseY = INT32_MIN;
    Tween alpha = Tween::constant(255);
    bool visible = true;

    int32_t plane = 0;                // plane this image is drawn through
    // `\mr` `\mg` `\mb` `\ma` after the image's numbers (sscript's load, Amakano 0x01cfb6: object
    // field +760 = 1..4, native 728 -> kcImage +0x4c): the image is its plane's mask, that channel
    // limiting where the plane's other images show; it is not drawn itself. 0: an ordinary image.
    uint8_t maskChannel = 0;
    // `\lr` `\ud` `\r90` `\r180` `\r270` after the image's numbers (sscript's load, Amakano
    // 0x01cd75: object +756 |= 1, 2, 0x10, 0x20, 0x30; then 0x095931 calls native 727 with 1 / 0 and
    // native 723 with the angle, cs2.exe 0x61fa50 / 0x6201f0): the image's pixels are mirrored or
    // turned about its base point (canvas, offsets, base and attribute points with them), mirrors
    // first. 0: as loaded.
    uint8_t turn = 0;
    // E-mote banks, `mpl <n> touch [on] [table]` (Nekopara's sscript 0x054482: object +936 = !on,
    // +940 = table, defaults 1 and 0, then native 965 hands them to the bank's model): whether the
    // character can be touched in touch mode, and which of the .inc #touch action tables answers.
    bool touchable = true;
    int32_t touchTable = 0;
    // `mode` (the engine's codes): 0 none, 1 mono, 2 sepia, 3 np (negative), kept in the slot's
    // record (+688). An image loads from its file with that mode's conversion (native 737). In every
    // sscript after Fruit's, `mode` on a loaded image also marks it (record bit 64), and the next
    // frame converts the pixels as they are (so conversions stack) or, for 0, loads the image again
    // from its file (Happiness 2 0x08aabd -> 737 / 0x0959c1). Fruit's applies it only at a load.
    uint8_t filter = 0;
    std::string filterApplied;        // the conversions on the shown pixels, in order ("3", "21")
    bool filterPending = false;       // record bit 64: a `mode` the next frame applies
    int16_t blurX = 0, blurY = 0;     // `blur <x> <y>`: blur radius in pixels (0: sharp)
    Tween scaleX = Tween::constant(100), scaleY = Tween::constant(100);  // percent, about the anchor
    std::vector<Motion> motions;
    // `delay <frames>`: motions and property changes given later start that much later (the
    // engine's per-object start offset, kept until `stop` or the slot is cleared).
    int32_t delay = 0;
    // `pri <depth>`: draw order (the engine's base z); lower is further back. NaN: the slot's default
    // (bg n: 10n, cg n: 60 + 30n, fg n: 70 + 30n, eg n: 80 + 30n, fw n: 990 + 10n).
    float depth = std::numeric_limits<float>::quiet_NaN();
    // `tone <r%> <g%> <b%>` or a named tone: the slot's colour scale, applied to each channel of an
    // image when it is loaded (min(255, c * pct / 100)). `toneLoaded` is what the shown image got.
    std::array<int16_t, 3> tone{100, 100, 100}, toneLoaded{100, 100, 100};

    // E-mote banks: the model (.psz) and its variables, whose pose is drawn in place of HG parts.
    // Placement (cs2.exe 0x574070 each frame): the model's origin is at the anchor plus (emoteX,
    // emoteY), the view's centre plus the .dif's #position, plus (emoteScaleOffX, emoteScaleOffY)
    // (#scaleoffset) x (1 - zoom); the model is drawn at emoteScale (#scale) x zoom, the zoom being
    // the bank's own scale.
    std::string emote;
    std::map<std::string, float> emoteVars;
    int32_t emoteX = 0, emoteY = 0;
    float emoteScale = 1;
    float emoteScaleOffX = 0, emoteScaleOffY = 0;
    std::map<int32_t, std::array<int32_t, 3>> emoteAttributes;  // the .dif's #attribute (`m2att`)
    // Variables cs2 sends again every frame, so timelines can't move them: a value applied without
    // its chain's move (mode 0: a load, a hidden or crossfading bank, a `fix` chain) stays in the
    // sender's records and is re-sent each frame (0x56d280; Amakano 0x4cd9d0) until a moved change
    // replaces it. Their values are emoteVars'.
    std::set<std::string> emotePinned;
    // An E-mote bank's turn (`mpl <n> rot` / `mrot`), degrees; motions add to it. kcTaskM2Plane takes
    // the plane message's z (0x37 -> 0x574480) and hands it to the player each frame (0x574070,
    // SetRot), which turns the model's root layer (emotedriver 0x1001bb90 -> node 0's angle).
    float rotation = 0;
    // Animation: the bank's E-mote player (variables, timelines, blinking), stepped by the scene
    // player each frame and shared by the stage's copies; not saved (rebuilt from the fields here,
    // its idle timeline `emoteIdle` restarting). Blinking and lip sync switches (`m2enable_blink` /
    // `m2enable_lip`), the voice the mouth follows and when it started.
    std::shared_ptr<EmotePlayer> emotePlayer;
    std::shared_ptr<EmoteInertia> emoteInertia;  // cs2.exe's springs for this model (none: empty)
    // `m2fspeed` / `m2prmscale` (Nekopara's sscript 0x05306a / 0x053388, kept at bank +804/+808 and
    // their scale twins): the speed and size of the host's variable changes, a default and per
    // variable (cs2.exe 0x56d830 / 0x56d9b0, used at 0x56d280: frames / speed, target x scale).
    float emoteSpeedAll = 1, emoteScaleAll = 1;
    std::map<std::string, float> emoteSpeedVars, emoteScaleVars;
    // `m2iner pause <var>` (0x053955, bank +884): springs held at rest; bare, none.
    std::vector<std::string> emoteInerPaused;
    // Crossfade from the previous pose (cs2.exe 0x4dae30 / 0x4f81d0 / 0x4f9430): when a shown bank
    // changes model (m2view.cnf #fade frames) or an argument whose .inc #chain is `fade`, a frozen
    // copy of the old player, drawn at its old offset, and the new pose are mixed pixel by pixel
    // (straight colour and alpha, transparent pixels taking the other's colour) over
    // emoteFadeFrames from emoteFadeStart; the bank's player does not run meanwhile. Not saved.
    std::shared_ptr<const EmotePlayer> emoteFadeFrom;
    int32_t emoteFadeX = 0, emoteFadeY = 0;
    float emoteFadeScale = 1, emoteFadeScaleOffX = 0, emoteFadeScaleOffY = 0;
    uint64_t emoteFadeStart = 0;
    int32_t emoteFadeFrames = 0;
    bool emoteFading(uint64_t now) const { return emoteFadeFrom && now < emoteFadeStart + static_cast<uint64_t>(std::max(0, emoteFadeFrames)); }
    // `mpl <n> m2body` (Nekopara): the bank's body digits (sscript object +820, merged by native 904,
    // `@` keeping a digit) and the body profile they go through (.pfl #motname's fifth column). A
    // digit that changes on a shown bank moves its variables along the profile's #chain (the
    // player's host tweens); their end values are in emoteVars.
    std::string emoteBodyDif, emoteBody;
    std::string emoteIdle, emoteVoice;
    uint64_t emoteVoiceStart = 0;
    bool emoteBlink = true, emoteLip = true;
    // The expression's lip sync settings (see EmoteProfiles::Resolved); no variable: no lip sync.
    std::string emoteLipVar;
    float emoteLipRest = 0;
    std::array<float, 6> emoteLipWeights{};
    int32_t emoteLipLatency = 0, emoteLipSmooth = 1;
    // The parts' ANM scripts (blinking, mouths, animated CGs), shared by the stage's copies; not
    // saved (after a load they start again).
    std::shared_ptr<StageAnm> anm;
};

// An image slot's `mode` as sscript runs it (see StageImage::filter).
// `mode <m>` (Happiness 2's 0x03af3b): the mode goes into the slot's record; on a loaded image the
// sscripts after Fruit's also mark it (bit 64) when `eachFrame` (Fruit's 0x02c3a0 only keeps it).
void setImageMode(StageImage& img, uint8_t mode, bool eachFrame);
// The image's load (Happiness 2's 0x0959c1): the file's pixels, converted by the mode (native 737),
// and the mark cleared (0x0969c2).
void loadImageMode(StageImage& img);
// The per-frame step (0x08aabd), once a frame for the 27 image slots: a marked image's pixels are
// converted as they are, on top of earlier conversions (native 737), or for mode 0 the image is
// loaded again from its file (0x0959c1). The mark is cleared either way.
void stepImageMode(StageImage& img);

// `pl N raster <dir> <amp> <cycle> <speed>` (Task_Eff_Raster): the plane's mesh vertices sway
// along a sine. dir 1 moves x by sin over y, otherwise y by sin over x; the wave scrolls one
// pixel per 60 fps frame and repeats every <cycle> pixels. <speed> only matters for the
// normalised form (negative dir), which no game uses.
struct Raster {
    int32_t dir = 1;
    float amplitude = 0, cycle = 1, speed = 0;
    uint64_t start = 0;
    // `raster fade <frames> <amp> <cycle> <speed>` (Happiness 2's sscript 0x03c132, the plane's
    // raster fields +328): the wave goes from these values to the ones above over the frames
    // (linearly: fitted; sscript's per-frame step behind message 0x10e was not traced).
    float fromAmplitude = 0, fromCycle = 1, fromSpeed = 0;
    uint64_t fadeStart = 0;
    uint32_t fadeFrames = 0;
    // The wave as it is at a frame.
    Raster at(uint64_t frame) const {
        Raster r = *this;
        if (fadeFrames == 0 || frame >= fadeStart + fadeFrames) return r;
        const float t = frame <= fadeStart ? 0.0f : static_cast<float>(frame - fadeStart) / static_cast<float>(fadeFrames);
        r.amplitude = fromAmplitude + (amplitude - fromAmplitude) * t;
        r.cycle = fromCycle + (cycle - fromCycle) * t;
        r.speed = fromSpeed + (speed - fromSpeed) * t;
        return r;
    }
};

// `pl N after <count> <interval> <dx> <dy> <dz> <alpha> <sx> <sy> <sz> <mode> <pri>`: afterimages.
// The engine keeps count*interval+1 snapshots of the plane, shifting them one per frame; snapshot
// i is where the plane was i frames ago, moved i*(dx, dy) further, and every interval-th one is
// drawn with its opacity and scale going linearly from the plane's to <alpha> and <s>x the
// plane's at the last. Mode 1 doubles the interval; mode 2 also turns each copy by the angle it
// travelled (between its neighbours) and stretches it (the stretch is not modelled).
struct AfterImage {
    int32_t count = 0, interval = 1, mode = 0, priority = -10;
    float dx = 0, dy = 0;
    int32_t alpha = 255;
    float scaleX = 1, scaleY = 1;
    uint64_t start = 0;
    int32_t steps() const { return count * (mode == 1 ? 2 : interval); }
    int32_t every() const { return mode == 1 ? 2 : std::max(1, interval); }
};

// A plane: a group of images transformed together (`pl N ...`). Images are placed in
// plane-local coordinates; the plane maps its base point to `pos` with rotation/scale/alpha.
struct Plane {
    Tween x, y;  // where the base point lands on screen
    float settledX = std::numeric_limits<float>::quiet_NaN(), settledY = std::numeric_limits<float>::quiet_NaN();  // see StageImage
    int32_t baseX = 0, baseY = 0;
    Tween alpha = Tween::constant(255);
    bool visible = true;
    // `color $AARRGGBB` (pColor): the colour the plane's images are drawn with, which modulates them
    // (Himawari's `pl 1 color $ff505050` darkens a flashback plane); bare `color` is white again.
    uint32_t color = 0xFFFFFFFF;
    // Degrees about z (positive turns clockwise on screen), and the x/y tilts. The engine builds
    // scale * RotX * RotY * RotZ (D3DX, row vectors) and draws the result flat: a y tilt narrows
    // the plane by cos, 90 is edge-on, 180 mirrors it.
    float rotation = 0, rotX = 0, rotY = 0;
    float scaleX = 1, scaleY = 1;
    // Solid colour fill (`pl N $AARRGGBB w h`), drawn above the image layers (flashes).
    bool fill = false;
    uint32_t fillColor = 0;
    int32_t fillWidth = 0, fillHeight = 0;
    // `pl N <w> <h>`: the plane is its own w x h surface (local 0,0 at its top-left); what is drawn
    // on it is clipped to it. 0: the whole screen, unclipped.
    int32_t width = 0, height = 0;
    // `pl N <mesh>`: the plane is drawn only inside this KX2 mesh's triangles (empty: everywhere).
    std::string shape;
    std::vector<Motion> motions;
    // The engine motions behind the plane's values: sscript starts one for every subcommand (native
    // 535), a property set (pos, scale, blend, ...) being one that applies at the next update and
    // ends, in group 0 like the classic motions; mc* and m2* ones are group 1. For each value (pos,
    // alpha, scale, rotation, colour, base, depth): the group of the last one and the frame it ends.
    // A bare `pl N` keeps what group 0 still drives (ScenePlayer::planeCommand). Not saved.
    struct ValueMotion {
        bool set = false;
        uint8_t group = 0;
        uint64_t until = 0;
        // A timed motion (`fade 20 ...`) has had its last update once its frame has come; a property
        // set (`blend`, or a motion of 0 frames) waits for the engine's update of the frame it was
        // given in (ScenePlayer::resetPlane).
        bool timed = false;
        bool running(uint64_t now) const { return set && (timed ? until > now : until >= now); }
    };
    std::array<ValueMotion, 7> valueMotions{};
    int32_t delay = 0;  // see StageImage::delay
    float depth = std::numeric_limits<float>::quiet_NaN();  // `pri`; NaN: 5000 + 1000 * plane
    // `vram <x> <y>`: where the plane's window sits in the engine's VRAM canvas. Planes whose
    // windows overlap show the same pixels (Fruit draws one leaf on plane 10 and shows it through
    // planes 10-14 at `vram 0 1500`). hasVram false: vram.xml's place, shared with nothing.
    bool hasVram = false;
    int32_t vramX = 0, vramY = 0;
    // `bmode <n>`: how the plane is blended (the engine's D3D table): 0 alpha, 1 additive,
    // 2 premultiplied, 3 multiply, 4 inverse (1 - screen) x src, 5 screen x src added.
    uint8_t blendMode = 0;
    bool raster = false;
    Raster wave;
    AfterImage after;  // count 0: none
    // `video <name>` (planes only): the plane shows an MPEG-1 clip from movie.int instead of images.
    // `vcom play [n]` plays it n times (-1: for ever) from `videoStart`; `vcom stop` (plays 0) holds
    // the picture where it is.
    std::string video;
    int32_t videoPlays = 0;
    uint64_t videoStart = 0;
};

// `particle3 <n> <plugin> <texture> <count> <depth> <plane> <floats>` (sscript 0x07f418): a
// particle plugin (see particle.hpp) drawn on a plane at a depth among its images. `alpha` and
// `fade <frames> <from> <to>` set the layer's opacity, which scales every particle's.
struct ParticleLayer {
    // A plugin (`particle3`, ptclpi/<plugin>.ppi) or, with `script`, the old commands' script
    // (`particle` / `particle2`, ptcl/<plugin>.kcs; sscript's record +368 is 0 for them).
    std::string plugin, texture;
    bool script = false;
    int32_t count = 0;
    float depth = 0;
    int32_t plane = 0;  // -1: on no plane
    std::array<float, 32> f{};
    Tween alpha = Tween::constant(255);
    uint64_t start = 0;  // frame the command ran; the effect is created on the next update
    // The running plugin, shared by the stage's copies and stepped once per frame by the player;
    // not saved (a loaded game starts it afresh).
    std::shared_ptr<ParticleEffect> effect;
};

struct Choice {
    std::string target;  // scene to jump to
    std::string prompt;  // UTF-8
};

struct MessageWindow {
    bool visible = false;
    Tween alpha = Tween::constant(0);
    std::string style = "meswnd";  // `view 1 <style>`
    bool novel = false;            // full-screen novel mode
    // Novel mode: where each message of the page starts in `text` (span index) and who speaks it.
    struct NovelLine {
        size_t at = 0;
        std::string name;
    };
    std::vector<NovelLine> novelLines;
    bool textVisible = true;       // `mesdraw`
    std::string name;              // UTF-8 speaker, empty for narration
    // The speaker's name line as the scene itself writes it (UTF-8, untranslated, markup kept):
    // the name table's key (sscript's MES_SETNAME id, 0x00135c).
    std::string speaker;
    MessageText text;
    // The same message in the sub language (`lang_text_sub`), shown under it (meswnd.fes `strsub`:
    // MES_SETSTRING appends $str1002); empty without one. Not saved: a load shows it from the next
    // message on.
    MessageText subText;
    size_t revealed = 0;           // characters of `text` shown so far (typewriter effect)
    // Both texts type out at the same speed and the message is drawn once the longer one is
    // (meswnd.fes MES_ISDRAW ors `str isdraw` with `strsub isdraw`).
    size_t revealTotal() const { return std::max(text.length(), subText.length()); }
    // The wipe reveal (`layout scrolltype 1`): the frame the new text started at and the characters
    // already shown before it (a continuation keeps what was there).
    uint64_t wipeStart = 0;
    size_t wipeFrom = 0;
};

// How newly committed stage contents replace what was on screen.
struct Stage;

struct Transition {
    enum class Kind : uint8_t { Cut, Fade, Wipe, Mosaic, PageFlip, Break, FadeMosaic, CellFlip };
    Kind kind = Kind::Cut;
    uint32_t frames = 0;
    int32_t mosaic = 1;  // Kind::Mosaic / FadeMosaic: largest block size in pixels (`rwmos` / `rfmos <frames> <size> ...`)
    std::string mask;  // wipe mask image (Kind::Wipe)
    // `wipe2` (Task_Eff2D_Wipe2): a hard edge, a pixel switching when its mask value reaches
    // 256 - 256 t, instead of the soft half-length fade of `wipe` / `rwipe`.
    bool hard = false;
    // Kind::PageFlip (`rpflip`, cs2.exe kcTaskRenderPFlip): the old screen turns away like a page.
    // angle: the direction the fold moves (radians), radius: the curl's, fold: how far the page
    // turns over (radians).
    float flipAngle = 0, flipRadius = 0, flipFold = 1.5707964f;
    // Kind::CellFlip (`rcflip`, cs2.exe kcTaskRenderCFlip): the screen in flipCellW x flipCellH
    // cells that turn over one after another, the old screen on their fronts and the new one on
    // their backs, as a line sweeps across; flipAngle and flipAxis (radians) set the sweep's and the
    // turn's direction, flipBand how far behind the line a cell is turned over, flipColor what
    // shows behind them.
    int32_t flipCellW = 256, flipCellH = 256;
    float flipBand = 200, flipAxis = 0;
    uint32_t flipColor = 0xFF000000u;
    // Kind::Break (`wbreak`, cs2.exe kcTaskRenderWBreak): the old screen breaks into triangles, two
    // to each breakCellW x breakCellH cell, that fly outwards (up to breakSpeed pixels a frame) and
    // spin (up to breakSpin / 2 degrees a frame about each axis) as they fade.
    int32_t breakCellW = 32, breakCellH = 32;
    float breakSpeed = 30.0f, breakSpin = 6.0f;
    uint64_t start = 0;
    uint64_t id = 0;  // increments on every commit; renderers snapshot on change
    // `draw ... <plane>` (Task_Eff2D_Fade): only that plane's images cross-fade, from how they were
    // in `from` (the stage before the commit); the rest of the screen changes at once. -1: the
    // whole screen (rdraw, wipes).
    int32_t plane = -1;
    // One of sscript's render transitions (`rdraw`, `rwipe` / `rwipe2`, `rwmos`, `rfmos`, `wbreak`,
    // `rpflip`, `rcflip`: its effect kinds 2-8), which the click's skip flag ends; the 2D ones
    // (`draw`, `wipe`, `wipe2`, `wipe3`) run on.
    bool render = false;
    std::shared_ptr<const Stage> from;

    bool active(uint64_t now) const { return kind != Kind::Cut && now < start + frames; }
};

// Snapshot of everything the renderer needs to draw a frame.
struct Stage {
    std::array<std::map<int32_t, StageImage>, kLayerCount> layers;
    std::map<int32_t, Plane> planes;  // only planes that differ from the default
    std::map<int32_t, ParticleLayer> particles;  // by slot (0-7)
    MessageWindow window;
    std::vector<Choice> choices;
    // `fselect <screen>`: the FE script that shows these choices (Labyrinth's dave_select); empty:
    // the window style's choice screen.
    std::string choiceScript;
    Transition transition;
    // The tone of each image slot as sscript keeps it in the slot's record (+276 / +280 / +284,
    // Nekopara): set by `tone` and the tone table, kept when the slot or its layer is cleared
    // (the clear 0x00ef1d leaves it), taken by each image loaded into the slot. Only slots whose
    // tone is not 100 100 100; by (layer, index).
    std::map<std::pair<int32_t, int32_t>, std::array<int16_t, 3>> slotTones;

    std::map<int32_t, StageImage>& layer(Layer l) { return layers[static_cast<size_t>(l)]; }
    const std::map<int32_t, StageImage>& layer(Layer l) const { return layers[static_cast<size_t>(l)]; }
};

}  // namespace nyan::cs2
