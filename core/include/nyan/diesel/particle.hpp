#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/diesel/random.hpp"
#include "nyan/diesel/sprite_geometry.hpp"

namespace nyan::diesel {

struct CameraView;

// A particle editor's file (media/particle/<name>.particle, muramasa_en.exe's loader 0x475be0): the
// 0x15b4-byte parameter record (the layout: core/src/diesel/particle_layout.inc, the names
// getParameterTable gives the fields) and, for files of more than one texture, the other textures'
// names after it (a 16-bit length, then the name). Files of 0x14f4 bytes are an older record without
// the texture list. Versions below 4 number the keyframes' curves differently; the loader converts
// them (0x47e8e0).
struct ParticleFile {
    static constexpr size_t kSize = 0x15b4;
    std::vector<uint8_t> raw;            // kSize bytes
    std::vector<std::string> textures;   // texture_filename, then the others

    int32_t i(size_t off) const;
    float f(size_t off) const;
    std::string s(size_t off, size_t max = 0x100) const;  // up to the first NUL
    void setI(size_t off, int32_t v);
    void setF(size_t off, float v);
    void setS(size_t off, const std::string& v);  // copied with its NUL (the caller checks the room)
};

// One field of the record as getParameterTable names it (its builder 0x4ecbc0, the reader
// 0x534300 alike): the path (dots: nested tables), the byte offset, 'i' / 'f' / 's' and a string's
// room (100 for particle names and the font's, 256 for files). In the builder's order.
struct ParticleField {
    const char* path;
    uint32_t offset;
    char type;
    uint16_t size;
};
extern const ParticleField kParticleFields[];
extern const size_t kParticleFieldCount;
// The record of a file's bytes; nothing if it is shorter than 0x14f4 bytes (the exe then uses an
// all-zero record, which makes no particles).
std::optional<ParticleFile> parseParticleFile(const uint8_t* data, size_t size);

// The frame's particles as Mware's CParticle3D batch draws them (0x1000dd10, 0x1000d1a0): four
// vertices a particle, in clip space (the world, view and projection applied: x, y, z and w as the
// D3D11 pipeline gets them, y up and 0 <= z <= w inside), the texture coordinates and the colour.
// The renderer clips and draws the triangles (0, 1, 2) and (2, 1, 3) of each four.
struct ParticleVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    float u = 0.0f, v = 0.0f;
    std::array<float, 4> color{};
};
struct ParticleBatch {
    std::string texture;          // the file
    int textureWidth = 0, textureHeight = 0;
    int32_t blendMode = 0;        // sprite.blend_mode (the batch's pipeline: 0x47ab20)
    int32_t colorMode = 0;        // sprite.color.culc_mode: 0 multiply, 1 add, 2 subtract
    std::vector<ParticleVertex> vertices;
};

// One particle system (Diesel::CParticle, 0x6444 bytes; built by 0x476420 and 0x4776b0) as SQParticle
// drives it: a fixed set of particles (occur.particle_num) made when the file is loaded, each living
// age milliseconds after its delay and starting again as often as the file says, moved, turned,
// sized and coloured by its keyframes. Positions are the particle space's, which the system's matrix
// places; the view is the default camera's (eye at z = -512, 45 degrees) or an SQCamera's.
class ParticleSystem {
public:
    struct Texture {
        std::string path;
        int width = 0, height = 0;  // 0 if it does not load
    };
    // `seed`: the generator's (the exe's: the system's address times a timer; one per system).
    ParticleSystem(ParticleFile file, std::vector<Texture> textures, int32_t seed);
    ~ParticleSystem();

    const ParticleFile& file() const { return file_; }
    size_t particleCount() const { return particles_.size(); }
    size_t aliveCount() const;

    // 0x479690: visible, not begun; every particle made again (its delay too); the longest life.
    void reset();
    // 0x4797a0: begun (the start sound and the same-time system are not here).
    void start();
    // SQParticle.end (0x537710): neither visible nor begun.
    void end();
    // seek(ms) (0x4788c0): not begun, the time is kept to be skipped in one step at the first
    // update after begin; begun, the particles advance by it now.
    void seek(float ms);
    // 0x478180: the particles advance by `ms` (a kept seek replaces it once).
    void advance(float ms);
    // What SQParticle's record keeps of the system (0x479ea0 writes, 0x47a480 reads, in this order):
    // +0xc0, +0xc4, +0xc8, +0x20f4, +0x2114, +0x22a4, +0x6400.
    struct Saved {
        int32_t visible = 1, begun = 0, subActive = 0;
        float fade = 1.0f, totalTime = 0.0f, timeScale = 1.0f, seekDelay = 0.0f;
    };
    Saved saved() const;
    // The exe reads every particle back too (its generator and each CParticleObject, 0x4b7510's
    // record); Nyanroid makes the particles again and, for a begun system, runs them for the saved
    // time in one step as a seek before begin does (Fitted), then takes the saved fields.
    void restore(const Saved& s);
    // isActivle (0x537fa0): visible and begun, or a linked system is.
    bool active() const { return (visible_ && begun_) || subActive_; }
    bool visible() const { return visible_ != 0; }

    // A scene node the system or its emitter hangs from (link / linkEmitter, 0x4aa810): its world
    // matrix (+0x60) and its scale (vtable +0x24: a sprite's source scale, a text's 1, a system's
    // position scale).
    struct Node {
        Mat4 world;
        std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    };
    // link: the system's parent (none: the task's root, whose world is the identity).
    std::optional<Node> parent;
    // linkEmitter: the emitter's parent other than the system (none: the system, the default).
    std::optional<Node> emitterParent;
    // The system's world (0x478930): the local matrix, its position scaled from pixels to particle
    // space; under a parent, times the parent's world, the parent's position then rescaled by this
    // system's scale over the parent's.
    Mat4 world() const;
    // vtable +0x24 (0x475b10): the position scale (1 in a 3D scene, which is not drawn here).
    const std::array<float, 3>& nodeScale() const { return positionScale_; }

    // What SQParticle hands over before it draws (0x5371c0) and as it updates.
    Mat4 local;                                // +0x20b4: rotation x scale x translation
    float fade = 1.0f;                         // +0x20f4
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};  // +0x20f8
    float timeScale = 1.0f;                    // +0x22a4 (setTimeScale)
    // setCamera (0x4799a0): the camera (null: the default view), the depth at which its view should
    // match the default's and its field of view (degrees; <= 0: the camera's). Sets the scales.
    void setCamera(std::shared_ptr<const CameraView> camera, float depth, float fov, int screenW, int screenH);

    // The draw (0x478930, 0x478e30, then each particle's 0x4b6630 and 0x4b6d30): nothing when not
    // visible or faded out; `targetW` x `targetH` is what the system draws on (its aspect).
    std::shared_ptr<ParticleBatch> build(int targetW, int targetH);

    struct Particle;
    struct Emitter;

private:
    ParticleFile file_;
    std::vector<Texture> textures_;
    MwareRandom random_;
    std::unique_ptr<Emitter> emitter_;
    std::vector<std::unique_ptr<Particle>> particles_;
    int32_t visible_ = 1;      // +0xc0
    int32_t begun_ = 0;        // +0xc4
    int32_t subActive_ = 0;    // +0xc8
    float seekDelay_ = 0.0f;   // +0x6400
    float longestLife_ = 0.0f; // +0x6434 (-1: a particle lives for ever)
    float totalTime_ = 0.0f;   // +0x2114
    std::shared_ptr<const CameraView> camera_;  // +0x223c
    std::array<float, 3> positionScale_{1.0f, 1.0f, 1.0f};  // +0x2240
    std::array<float, 3> particleScale_{1.0f, 1.0f, 1.0f};  // +0x224c
    bool billboard_ = false;   // +0x2120

    friend struct Particle;
    friend struct Emitter;
    void initParticle(Particle& p);       // 0x4b1300
    int stepParticle(Particle& p, float dt);   // 0x4b5a80
    int integrate(Particle& p, float dt);      // 0x4b5f20
};

}  // namespace nyan::diesel
