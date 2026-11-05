#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nyan/cs2/mersenne_twister.hpp"

namespace nyan::cs2 {

// CatSystem2's particle plugins (`particle3 <n> <plugin> <texture> <count> <depth> <plane> <f0..>`,
// ptclpi/<plugin>.ppi, plugin version 0x20002). A plugin is native code: it moves a set of records
// that the engine then draws, one textured quad each. This file holds the records, the engine's
// side of the interface (defaults, random numbers, projection, sort) and reimplementations of the
// plugins the games use, read from the DLLs.
//
// Engine side (cs2.exe, Amakano 2): message 0x140 to the particle layer (0x587280) loads the
// plugin, fills every record with defaults (0x587df0) and calls Init with {count, screen w, h,
// texture canvas w, h, the script's 32 floats}. Each frame Update (0x587040 -> plugin vtable +8)
// gets the layer clock before and after (frames; the clock ANM and motion scripts run on), and the
// layer draws the records (0x586b90).
struct ParticleVertex {
    float x = 0, y = 0, z = 0;
    uint8_t alpha = 255;  // the D3DCOLOR's top byte; most plugins write only this
    float u = 0, v = 0;
    uint8_t r = 255, g = 255, b = 255;  // its other bytes (digi and digiring tint theirs)
};

// One record (0x94 bytes in the DLLs): the quad's corners (top-left, top-right, bottom-left,
// bottom-right), and the transform the engine puts them through.
struct ParticleRecord {
    std::array<ParticleVertex, 4> v;
    std::array<float, 3> pivot{};  // subtracted first
    std::array<float, 3> pos{};
    std::array<float, 3> rot{};    // radians, about x, y, z
    std::array<float, 3> scale{1, 1, 1};
    // +0x90: drawn when non-zero. The engine copies it after each update to the layer's +0x32c
    // (Happiness 0x4fe630), whose default is 1 (0x4fe1d0, into the records by 0x4fe990), and its
    // draw skips a record holding 0 (Happiness 0x4fcf70, Amakano 2 0x586b90). Of the plugins
    // reimplemented only filmnoise writes it.
    int32_t draw = 1;
};

struct ParticleParams {
    int32_t count = 0;
    float depth = 0;  // the layer's depth (script layers' native 38 works the perspective out with it)
    float screenW = 1280, screenH = 720;
    float texW = 0, texH = 0;  // the texture's canvas
    std::array<float, 32> f{};
    // The plane fields as Init finds them (the engine fills them just before, Happiness 0x141 ->
    // 0x4fe500): ParticleEffect::setPlanePos.
    std::array<float, 2> planePos{};
    // The host's file read (kcTaskParticlePluginFunc22 +0x80: a file of the game's archives by
    // name, its bytes; empty when there is none). Only cmnptcl reads one, during Init.
    std::function<std::string(const std::string&)> readFile;
};

// The engine's random source for plugins (cs2.exe 0x739900 / 0x739910): two MT19937s (the 1999
// variant), one for integers and one for floats in [0, 1] (u32 / (2^32 - 1)), both seeded with the
// same value. The engine seeds them from the clock.
class ParticleRandom {
public:
    explicit ParticleRandom(uint32_t seed) : ints_(seed), floats_(seed) {}
    uint32_t next() { return ints_.next(); }
    float unit() { return static_cast<float>(static_cast<double>(floats_.next()) * (1.0 / 4294967295.0)); }

private:
    MersenneTwister ints_, floats_;
};

// A plugin DLL's own globals (its .data). The engine loads a layer's plugin with LoadLibraryA
// (Happiness 0x4fd660) just after freeing the layer's old one (0x4fcd50 -> PluginServer's
// destructor 0x4fcaa0: ReleaseInstance, FreeLibrary), so a DLL's globals start from the file's
// values unless another layer still runs that plugin, and layers running it at once share them.
// Of the plugins reimplemented only concent keeps state there; the others' globals are the CRT's.
struct PluginGlobals {
    virtual ~PluginGlobals() = default;
};

class ParticleEffect {
public:
    virtual ~ParticleEffect() = default;
    // The plugin for `name` (case-insensitive), or nullptr for one not reimplemented. `shared`: the
    // globals of another layer running the same plugin (its globals()), or null for a fresh load.
    static std::unique_ptr<ParticleEffect> create(const std::string& name, const ParticleParams& params, uint32_t seed,
                                                  std::shared_ptr<PluginGlobals> shared = nullptr);
    static bool known(const std::string& name);
    // The old commands' script layers (`particle` / `particle2`: ptcl/<name>.kcs run on the engine's
    // script VM), reimplemented as the plugins are; nullptr for one not reimplemented.
    static std::unique_ptr<ParticleEffect> createScript(const std::string& name, const ParticleParams& params, uint32_t seed);
    static bool knownScript(const std::string& name);

    // One update covering `frames` of the layer clock.
    virtual void step(int32_t frames) = 0;
    // The layer's 32 floats (+0x190), which the engine copies into each update's block
    // (Happiness 0x4fe5d0) after Init had them: `particle3 <n> set` replaces them (message 0x145).
    void setFloats(const std::array<float, 32>& f) { params_.f = f; }
    // The plane fields the engine fills in the plugin before each update (Happiness 0x4fe500,
    // Amakano 2 0x588120) from the render node of the layer's plane: +0x24 / +0x28, the plane's
    // position on screen (message 0x33: where its base point lands, 640, 360 at rest). sscript
    // gives the layer its plane's node (native 427 -> message 0x147, object 27 + plane) only for a
    // plane vram.xml defines; otherwise the layer's +0x330 stays -1 and the fields are 0. (+0x18,
    // the node's base point and depth, and +0x2c / +0x30 no reimplemented plugin reads.)
    void setPlanePos(float x, float y) { planePos_ = {x, y}; }
    const std::vector<ParticleRecord>& records() const { return records_; }
    // The DLL's globals this one uses (null for a plugin without any).
    const std::shared_ptr<PluginGlobals>& globals() const { return globals_; }
    // The layer's blend mode, the plugin object's +0x10: the engine copies it to the layer after
    // each update (Happiness 0x4fe630, Amakano 2 0x588250: +0x360) and its draw (0x4fcf70 /
    // 0x586b90) passes it to the blend switch (Happiness 0x5766e0: 0 alpha, 1 additive, 2
    // premultiplied, 3 multiply, 4 inverse, 5 (DESTCOLOR, ONE)). Set by Init where the DLL sets it.
    int32_t blend() const { return blend_; }
    // The texture sampler states the plugin asked for in its last update (host +0x88, Happiness
    // 0x560190 -> 0x4fcd10: (type, value) pairs on the layer's +0x550, emptied before each update
    // by 0x4fd420). The draw (0x4fcf70) sets them on stage 0 (SetSamplerState) after the blend
    // switch (0x5766e0), which puts ADDRESSU / ADDRESSV back to MIRROR (2) for every draw, so they
    // hold for this layer only. Types 1 / 2 are ADDRESSU / ADDRESSV; values 1 WRAP, 2 MIRROR, 3
    // CLAMP. Of the plugins reimplemented only protect asks (WRAP both ways).
    struct SamplerState {
        int32_t type, value;
    };
    const std::vector<SamplerState>& samplerStates() const { return samplerStates_; }

protected:
    explicit ParticleEffect(const ParticleParams& params, uint32_t seed);
    ParticleParams params_;
    ParticleRandom rng_;
    std::vector<ParticleRecord> records_;
    std::shared_ptr<PluginGlobals> globals_;
    int32_t blend_ = 0;
    std::vector<SamplerState> samplerStates_;
    std::array<float, 2> planePos_{};
    // The plane's move from the screen's centre, as most plugins work it out each update: +0x24 /
    // +0x28 less half the screen size, which their Init keeps (0 for a plane at rest).
    std::array<float, 2> planeMove() const {
        return {planePos_[0] - params_.screenW * 0.5f, planePos_[1] - params_.screenH * 0.5f};
    }
};

// A record as the engine draws it: the four corners on screen (same order as the record), their
// alpha after the layer's (A * layer >> 8), their colours (0xRRGGBB) and the texture rectangle.
struct ParticleQuad {
    std::array<float, 2> c[4];
    uint8_t alpha[4];
    uint32_t rgb[4];
    float u0, v0, u1, v1;
};

// The engine's draw (0x586b90) before the camera the layer is attached to (which the renderer
// applies, as for planes): each corner minus the pivot, through
// scale * RotX * RotY * RotZ * translate(pos) (D3DX, row vectors), then a perspective about the
// screen centre, scale 1000 / (1000 - z) (z' = 65000 - (z + depth) against the camera at
// 65000 - depth - 1000; the depth cancels out). Records are drawn back to front by
// pivot.z + pos.z + corner0.z (0x5871a5), in the order the engine's own quicksort (0x73d390,
// comparator 0x5888c0; Happiness 0x6b1a50) leaves them: it is not stable, so ties are reordered.
// Records whose draw flag is 0 are left out. Each corner keeps its own colour and alpha, which the
// renderer blends across the quad as D3D does (a two-triangle strip, 0x580bf0 type 5).
std::vector<ParticleQuad> projectParticles(const ParticleEffect& effect, float screenW, float screenH, int32_t layerAlpha);

}  // namespace nyan::cs2
