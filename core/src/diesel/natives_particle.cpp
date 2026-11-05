// SQParticle (muramasa_en.exe natives 0x5373f0 ff.; the task object 0x5ebd38, update 0x537070, the
// hand-over before drawing 0x5371c0): a task object holding one particle system (Diesel::CParticle,
// core/src/diesel/particle.cpp) made from media/particle/<name>.particle. Its fade, move, scale,
// rotate, colour and frequency are tweens; a timed spline can drive the move. Each frame the system
// advances by the frame's time times the frequency; the draw hands it the matrix (rotation, scale,
// translation), the fade and the colour.

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

#include "natives.hpp"
#include "nyan/diesel/particle.hpp"
#include "nyan/diesel/spline.hpp"
#include "nyan/diesel/sprite_geometry.hpp"

namespace nyan::diesel {

namespace {

constexpr float kDegrees = 0.0174532924f;

// media/particle/<name>.particle's record (0x475be0); a file that is missing or too short gives an
// all-zero record (no particles, no textures).
ParticleFile particleFileNamed(ScriptHost::Impl& h, const std::string& name) {
    auto bytes = h.readFile("media/particle/" + name + ".particle");
    std::optional<ParticleFile> file;
    if (bytes) file = parseParticleFile(bytes->data(), bytes->size());
    if (!file) {
        file.emplace();
        file->raw.assign(ParticleFile::kSize, 0);
    }
    return std::move(*file);
}

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

struct ParticleObject final : NativeObject, FrameTask, Drawable {
    ScriptHost::Impl* host;
    std::unique_ptr<ParticleSystem> system;   // +0xc0
    Tween fade{1, 1.0f};                      // +0xc4 (now +0xd8)
    Tween move{3, 0.0f, 0.0f, 0.0f};          // +0xf0 (now +0x114)
    Tween scale{3, 1.0f, 1.0f, 1.0f};         // +0x13c (now +0x160)
    Tween rotate{3, 0.0f, 0.0f, 0.0f};        // +0x188 (now +0x1ac): degrees
    Tween color{3, 1.0f, 1.0f, 1.0f};         // +0x1d4 (now +0x1f8)
    TimedSpline spline;                       // +0x220
    Tween frequency{1, 1.0f};                 // +0x280 (now +0x294)
    float timeScale = 1.0f;                   // +0x2ac
    int32_t renderingScene = 0;               // +0x2b0: drawn in a 3D scene (not here)
    std::shared_ptr<const CameraView> camera;
    float cameraDepth = 512.0f, cameraFov = 45.0f;
    int32_t lock = 0, disused = 0, hidable = 0;  // Totono's task flags (as the sprites')
    uint32_t seed = 0;
    // setDither (0x537d80 -> CParticle 0x479c10): kept by the system (+0x6410 on, +0x6414 / +0x6418
    // the offset, +0x641c the file; a new system has none) and handed to its batches (CParticle3D
    // vtable +0x38). `ditherFile`: the pattern if it loaded, "" the default one.
    bool dither = false;
    std::string ditherFile;
    float ditherX = 0.0f, ditherY = 0.0f;

    // link / linkEmitter (0x4f7660 / 0x536f70 -> CNode 0x4aa810): the node the system hangs from (a
    // sprite's or a text's, joined through `linkHolder`, or another system), and the node its
    // emitter hangs from other than the system. A new system hangs from the task's root again.
    SpriteImage linkHolder, emitterHolder;
    ParticleObject* linkParticle = nullptr;
    ParticleObject* emitterParticle = nullptr;
    void unlink() {
        linkHolder.setParent(nullptr);
        emitterHolder.setParent(nullptr);
        linkParticle = emitterParticle = nullptr;
    }

    ParticleObject(ScriptHost::Impl* h, std::string name) : host(h) {
        taskName = std::move(name);
        created = h->clock.milliTime;
        h->addTask(this);
        h->addDrawable(this, 0, created);
        allParticles().push_back(this);
    }
    ~ParticleObject() override {
        host->removeTask(this);
        host->removeDrawable(this);
        // A node going leaves its children without a parent.
        std::erase(allParticles(), this);
        for (ParticleObject* p : allParticles()) {
            if (p->linkParticle == this) p->linkParticle = nullptr;
            if (p->emitterParticle == this) p->emitterParticle = nullptr;
        }
    }
    static std::vector<ParticleObject*>& allParticles() {
        static std::vector<ParticleObject*> all;
        return all;
    }

    // A linked node's world and scale as the exe's nodes keep them (+0x60, vtable +0x24).
    std::optional<ParticleSystem::Node> nodeOf(const SpriteImage& holder, ParticleObject* part, int depth) {
        if (holder.parent) {
            return ParticleSystem::Node{spriteWorldMatrix(*holder.parent, host->screenWidth(), host->screenHeight()),
                                        holder.parent->sourceScale};
        }
        if (part && part->system && depth < 8) {
            part->resolveNodes(depth + 1);
            return ParticleSystem::Node{part->system->world(), part->system->nodeScale()};
        }
        return std::nullopt;
    }
    // The hand-over, then the parents' worlds (the nodes update parent first).
    void resolveNodes(int depth = 0) {
        if (!system) return;
        handOver();
        system->parent = nodeOf(linkHolder, linkParticle, depth);
        system->emitterParent = nodeOf(emitterHolder, emitterParticle, depth);
    }
    Drawable* drawable() override { return this; }

    void placeMove(const std::array<float, 3>& p) {
        for (size_t i = 0; i < 3; ++i) move.nowNoPower[i] = p[i], move.now[i] = p[i] * move.power;
    }

    // 0x5371c0: the fade, the colour and the matrix go to the system.
    void handOver() {
        if (!system) return;
        system->fade = fade.now[0];
        system->color = color.now;
        Mat4 m = Mat4::rotationZXY(rotate.now[0] * kDegrees, rotate.now[1] * kDegrees, rotate.now[2] * kDegrees);
        m *= Mat4::scaling(scale.now[0], scale.now[1], scale.now[2]);
        m *= Mat4::translation(move.now[0], move.now[1], move.now[2]);
        system->local = m;
    }

    // The task update (0x537070): the tweens (the spline drives the move), then the system by the
    // frame's milliseconds (frame speed x 16.666 x its time scale, within the frame's step limit)
    // times the frequency.
    void update(ScriptHost::Impl& h) override {
        fade.step(h.clock);
        move.step(h.clock);
        if (spline.step(h.clock)) placeMove(spline.point);
        scale.step(h.clock);
        rotate.step(h.clock);
        frequency.step(h.clock);
        color.step(h.clock);
        if (!system) return;
        float dt = h.clock.frameSpeed * 16.666666f * system->timeScale;
        float step = 0.0f;
        if (dt >= 0.0f) {
            step = std::max(h.clock.maxFrameTime, 0.0f);
            if (dt <= step) step = dt;
        }
        system->advance(step * frequency.now[0]);
    }

    // The draw (vtable +0x54 / +0x58 -> CParticle 0x478e30): the particles of the frame, on the target
    // the object draws on.
    void emit(ScriptHost::Impl& h, std::vector<DrawItem>& out) override {
        if (!system || renderingScene) return;
        resolveNodes();
        auto batch = system->build(h.layoutWidth, h.layoutHeight);
        if (!batch || batch->vertices.empty()) return;
        DrawItem d;
        d.kind = DrawItem::Kind::Particles;
        d.texture = batch->texture;
        d.textureWidth = batch->textureWidth;
        d.textureHeight = batch->textureHeight;
        d.blendMode = batch->blendMode;
        d.dither = dither;
        d.ditherTexture = ditherFile;
        d.ditherX = ditherX, d.ditherY = ditherY;
        d.particles = std::move(batch);
        out.push_back(std::move(d));
    }
};

NativeObject* make(HSQUIRRELVM v) { return new ParticleObject(&host(v), argString(v, 2)); }

#define PARTICLE_SELF                                 \
    ParticleObject* o = self<ParticleObject>(v);      \
    if (!o) return sq_throwerror(v, "SQParticle: not an instance");

SQInteger pushFloat(HSQUIRRELVM v, float f) {
    sq_pushfloat(v, f);
    return 1;
}
SQInteger pushInt(HSQUIRRELVM v, SQInteger i) {
    sq_pushinteger(v, i);
    return 1;
}
int32_t argI(HSQUIRRELVM v, SQInteger idx) { return static_cast<int32_t>(argInt(v, idx)); }

// The system of a record (0x485d70 -> CParticle 0x476420 / 0x4776b0), under the task's root (its
// emitter under it), drawn in the given scene; no dither (a new system has none).
void makeSystem(ParticleObject* o, ScriptHost::Impl& h, ParticleFile file, int32_t renderingScene) {
    o->dither = false, o->ditherFile.clear(), o->ditherX = o->ditherY = 0.0f;
    o->unlink();
    // The textures the record counts (0x4776b0 loads texture_count of the list).
    std::vector<ParticleSystem::Texture> textures;
    size_t count = static_cast<size_t>(std::max(file.i(0x14f8), 0));
    for (size_t i = 0; i < count && i < file.textures.size(); ++i) {
        const std::string& t = file.textures[i];
        ParticleSystem::Texture tex;
        tex.path = slashes(t);
        if (!tex.path.empty()) h.textureSize(tex.path, tex.width, tex.height);
        textures.push_back(tex);
    }
    // The exe seeds each system with its address times a timer; Nyanroid with the time and a count.
    o->seed = h.clock.milliTime * 0x10001u + static_cast<uint32_t>(h.frame) * 0x9e37u + 0x2545u;
    o->system = std::make_unique<ParticleSystem>(std::move(file), std::move(textures), static_cast<int32_t>(o->seed));
    o->system->setCamera(nullptr, 512.0f, 45.0f, h.screenWidth(), h.screenHeight());
    o->system->timeScale = o->timeScale;
    o->renderingScene = renderingScene;
}

// create(name, renderingScene) (0x5373f0): the system of media/particle/<name>.particle (the loader
// 0x475be0 and 0x4776b0); 0, or -1 if it could not be made. A system made before goes.
SQInteger create(HSQUIRRELVM v) {
    PARTICLE_SELF
    auto& h = host(v);
    o->system.reset();
    makeSystem(o, h, particleFileNamed(h, argString(v, 2)), argI(v, 3));
    return pushInt(v, 0);
}

// The record as a table (0x4efb10: the fields by 0x4ecbc0 and its helpers, nested by their paths:
// integers, floats, strings up to their NUL), and `texture_filename_list`: the record's textures,
// texture_filename first (0x4f3b80 appends each).
void pushParticleTable(HSQUIRRELVM v, const ParticleFile& f) {
    struct Node {
        std::vector<std::pair<std::string, Node>> kids;
        const ParticleField* field = nullptr;
    };
    Node root;
    for (size_t i = 0; i < kParticleFieldCount; ++i) {
        const ParticleField& fd = kParticleFields[i];
        Node* n = &root;
        std::string path = fd.path;
        size_t at = 0;
        while (true) {
            size_t dot = path.find('.', at);
            std::string key = path.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
            auto it = std::find_if(n->kids.begin(), n->kids.end(), [&](const auto& k) { return k.first == key; });
            if (it == n->kids.end()) {
                n->kids.emplace_back(key, Node{});
                it = n->kids.end() - 1;
            }
            n = &it->second;
            if (dot == std::string::npos) break;
            at = dot + 1;
        }
        n->field = &fd;
    }
    auto push = [&](auto&& self, const Node& n) -> void {
        sq_newtable(v);
        for (const auto& [key, kid] : n.kids) {
            sq_pushstring(v, key.c_str(), -1);
            if (kid.field) {
                const ParticleField& fd = *kid.field;
                if (fd.type == 'i') sq_pushinteger(v, f.i(fd.offset));
                else if (fd.type == 'f') sq_pushfloat(v, f.f(fd.offset));
                else sq_pushstring(v, f.s(fd.offset, fd.size).c_str(), -1);
            } else {
                self(self, kid);
            }
            sq_newslot(v, -3, SQFalse);
        }
    };
    push(push, root);
    sq_pushstring(v, "texture_filename_list", -1);
    sq_newarray(v, 0);
    for (const auto& t : f.textures) {
        sq_pushstring(v, t.c_str(), -1);
        sq_arrayappend(v, -2);
    }
    sq_newslot(v, -3, SQFalse);
}

// cvttss2si: truncated, 0x80000000 for NaN or out of range.
int32_t truncateToInt(float x) {
    if (std::isnan(x) || x >= 2147483648.0f || x < -2147483648.0f) return INT32_MIN;
    return static_cast<int32_t>(x);
}

// The table at `idx` as a record (0x536170 -> 0x534300, field by field into a zeroed record; the
// same 501 paths). A field whose key is missing, or whose table on the way is not one, stays 0.
// Integers: a bool 0 / 1, a float truncated, another type 0 (0x4f6af0); floats: an integer
// converted, another type 0 (0x42c370); strings copied when shorter than their room (0x5335e0).
// `texture_filename_list` (an array) becomes the textures. False if `idx` is not a table.
bool readParticleTable(HSQUIRRELVM v, SQInteger idx, ParticleFile& out, ScriptHost::Impl& h) {
    if (sq_gettype(v, idx) != OT_TABLE) return false;
    HSQOBJECT table;
    sq_getstackobj(v, idx, &table);
    out.raw.assign(ParticleFile::kSize, 0);
    out.textures.clear();
    const SQInteger top = sq_gettop(v);
    for (size_t i = 0; i < kParticleFieldCount; ++i) {
        const ParticleField& fd = kParticleFields[i];
        sq_settop(v, top);
        sq_pushobject(v, table);
        std::string path = fd.path;
        size_t at = 0;
        bool found = true;
        while (true) {
            size_t dot = path.find('.', at);
            std::string key = path.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
            if (sq_gettype(v, -1) != OT_TABLE) {
                found = false;
                break;
            }
            sq_pushstring(v, key.c_str(), -1);
            if (SQ_FAILED(sq_get(v, -2))) {
                found = false;
                break;
            }
            if (dot == std::string::npos) break;
            at = dot + 1;
        }
        if (!found) continue;
        const SQObjectType t = sq_gettype(v, -1);
        if (fd.type == 'i') {
            int32_t x = 0;
            if (t == OT_BOOL) {
                SQBool b = SQFalse;
                sq_getbool(v, -1, &b);
                x = b ? 1 : 0;
            } else if (t == OT_FLOAT) {
                SQFloat fl = 0;
                sq_getfloat(v, -1, &fl);
                x = truncateToInt(static_cast<float>(fl));
            } else if (t == OT_INTEGER) {
                SQInteger n = 0;
                sq_getinteger(v, -1, &n);
                x = static_cast<int32_t>(n);
            }
            out.setI(fd.offset, x);
        } else if (fd.type == 'f') {
            float x = 0.0f;
            if (t == OT_FLOAT) {
                SQFloat fl = 0;
                sq_getfloat(v, -1, &fl);
                x = static_cast<float>(fl);
            } else if (t == OT_INTEGER) {
                SQInteger n = 0;
                sq_getinteger(v, -1, &n);
                x = static_cast<float>(static_cast<int32_t>(n));
            }
            out.setF(fd.offset, x);
        } else {
            // (The exe makes a string of a non-string's null pointer: Nyanroid leaves it empty.)
            const SQChar* str = nullptr;
            if (t == OT_STRING) sq_getstring(v, -1, &str);
            else if (h.log) h.log(std::string("SQParticle table: ") + fd.path + " is not a string (the exe reads a null pointer)");
            std::string value = str ? str : "";
            if (value.size() < fd.size) out.setS(fd.offset, value);
        }
    }
    sq_settop(v, top);
    sq_pushobject(v, table);
    sq_pushstring(v, "texture_filename_list", -1);
    if (SQ_SUCCEEDED(sq_get(v, -2))) {
        if (sq_gettype(v, -1) == OT_ARRAY) {
            sq_pushnull(v);
            while (SQ_SUCCEEDED(sq_next(v, -2))) {
                const SQChar* str = nullptr;
                if (sq_gettype(v, -1) == OT_STRING && SQ_SUCCEEDED(sq_getstring(v, -1, &str)) && str) {
                    out.textures.emplace_back(str);
                } else if (h.log) {
                    h.log("SQParticle table: a texture_filename_list entry that is not a string (the exe reads a null pointer)");
                }
                sq_pop(v, 2);
            }
            sq_pop(v, 1);
        }
    }
    sq_settop(v, top);
    return true;
}

// createFromTable(table, renderingScene) (0x537570): the system goes, then the table's record makes
// a new one as create's file would (no curve renumbering: the file loader's alone); 0, or -1 for a
// non-table (no system then).
SQInteger createFromTable(HSQUIRRELVM v) {
    PARTICLE_SELF
    auto& h = host(v);
    o->system.reset();
    ParticleFile file;
    if (!readParticleTable(v, 2, file, h)) return pushInt(v, -1);
    makeSystem(o, h, std::move(file), argI(v, 3));
    return pushInt(v, 0);
}
// getParameterTable() (0x5382f0): the system's record as a table (0x4efb10), or null without one.
SQInteger getParameterTable(HSQUIRRELVM v) {
    PARTICLE_SELF
    if (!o->system) {
        sq_pushnull(v);
        return 1;
    }
    pushParticleTable(v, o->system->file());
    return 1;
}
// setParameterTable(table) (0x5383b0): createFromTable with the scene the object draws in (+0x2b0);
// 0 (whatever that answers), or -1 for a non-table (the system kept).
SQInteger setParameterTable(HSQUIRRELVM v) {
    PARTICLE_SELF
    auto& h = host(v);
    if (sq_gettype(v, 2) != OT_TABLE) return pushInt(v, -1);
    o->system.reset();
    ParticleFile file;
    if (readParticleTable(v, 2, file, h)) makeSystem(o, h, std::move(file), o->renderingScene);
    return pushInt(v, 0);
}

// link(name) (0x4f7660): the system hangs from the node of the first task object of that name (a
// sprite's or a text's picture, another system), or the task's root for "" or a name not found.
// linkEmitter(name) (0x536f70): the emitter hangs from that node, or from its system for "" or a
// name not found (the default); new particles then start where the emitter is (ParticleSystem).
// Without a system the exe reads through a null pointer; Nyanroid does nothing.
SQInteger linkTo(HSQUIRRELVM v, bool emitter) {
    PARTICLE_SELF
    auto& h = host(v);
    if (!o->system) {
        if (h.log) h.log(std::string("SQParticle.") + (emitter ? "linkEmitter" : "link") + " without a system (the exe reads a null pointer)");
        return 0;
    }
    SpriteImage& holder = emitter ? o->emitterHolder : o->linkHolder;
    ParticleObject*& part = emitter ? o->emitterParticle : o->linkParticle;
    holder.setParent(nullptr);
    part = nullptr;
    std::string name = argString(v, 2);
    if (name.empty()) return 0;
    FrameTask* t = h.findTask(name);
    if (!t) return 0;
    if (auto* p = dynamic_cast<ParticleObject*>(t)) {
        // Its own node: the emitter's default; the system under itself would make the exe's node
        // update recurse for ever (Nyanroid leaves it under the root).
        if (p == o) {
            if (!emitter && h.log) h.log("SQParticle.link to itself (the exe's node update would recurse for ever)");
            return 0;
        }
        part = p;
    } else if (Drawable* d = t->drawable()) {
        holder.setParent(d->linkNode());
    }
    return 0;
}
SQInteger link(HSQUIRRELVM v) { return linkTo(v, false); }
SQInteger linkEmitter(HSQUIRRELVM v) { return linkTo(v, true); }
SQInteger begin(HSQUIRRELVM v) {  // 0x5376e0
    PARTICLE_SELF
    if (!o->system) return 0;
    o->handOver();
    o->system->reset();
    o->system->start();
    return 0;
}
SQInteger end(HSQUIRRELVM v) {  // 0x537710
    PARTICLE_SELF
    if (o->system) o->system->end();
    return 0;
}
SQInteger reset(HSQUIRRELVM v) {  // 0x537720
    PARTICLE_SELF
    if (!o->system) return 0;
    o->handOver();
    o->system->reset();
    return 0;
}
// fade(dst, time, curve) (0x537740); move / scale / rotate / setColor(x, y, z, time, curve)
// (0x537770, 0x537800, 0x537890, 0x537af0): from where they are; setFrequency(dst, time, curve)
// (0x537ac0).
SQInteger fade(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->fade.moveTo({argFloat(v, 2), 0.0f, 0.0f}, argI(v, 3), argI(v, 4));
    return 0;
}
SQInteger tween3(HSQUIRRELVM v, Tween ParticleObject::*member) {
    PARTICLE_SELF
    (o->*member).moveTo({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, argI(v, 5), argI(v, 6));
    return 0;
}
SQInteger moveSet(HSQUIRRELVM v) { return tween3(v, &ParticleObject::move); }
SQInteger scaleSet(HSQUIRRELVM v) { return tween3(v, &ParticleObject::scale); }
SQInteger rotateSet(HSQUIRRELVM v) { return tween3(v, &ParticleObject::rotate); }
SQInteger colorSet(HSQUIRRELVM v) { return tween3(v, &ParticleObject::color); }
SQInteger setFrequency(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->frequency.moveTo({argFloat(v, 2), 0.0f, 0.0f}, argI(v, 3), argI(v, 4));
    return 0;
}
// The spline (0x537920 / 0x5379a0 / 0x537a40): as the sprites', driving the move.
SQInteger resetSpline(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->spline.reset();
    return 0;
}
SQInteger setSpline(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->spline.add({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, argI(v, 5), argFloat(v, 6));
    return 0;
}
SQInteger beginSpline(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->spline.begin(argI(v, 2), argI(v, 3));
    o->spline.t.timeScale = o->timeScale;
    o->placeMove(o->spline.point);
    return 0;
}
// setTimeScale(s) (0x537b80): the system's, and the fade, move, scale, rotate and spline tweens'
// (not the colour's or the frequency's).
SQInteger setTimeScale(HSQUIRRELVM v) {
    PARTICLE_SELF
    float s = argFloat(v, 2);
    o->timeScale = s;
    if (o->system) o->system->timeScale = s;
    for (Tween* t : {&o->fade, &o->move, &o->scale, &o->rotate, &o->spline.t}) t->timeScale = s;
    return 0;
}
// setDither(on, x, y, file) (0x537d80 -> CParticle 0x479c10, the system at +0xc0 without a check):
// kept by the system, and the pattern (the file if it is named and loads, else the graphics' default
// one; none when off) handed with the offset to its batches, which draw it as the sprites do.
SQInteger setDither(HSQUIRRELVM v) {
    PARTICLE_SELF
    if (!o->system) return 0;
    o->dither = argI(v, 2) != 0;
    o->ditherX = static_cast<float>(argI(v, 3));
    o->ditherY = static_cast<float>(argI(v, 4));
    std::string file = argString(v, 5);
    int w = 0, h = 0;
    o->ditherFile = o->dither && !file.empty() && host(v).textureSize(file, w, h) ? file : std::string();
    return 0;
}
// setSurface(name) (0x537be0): as the sprites' (not for a scene's system).
SQInteger setSurface(HSQUIRRELVM v) {
    PARTICLE_SELF
    if (!o->renderingScene) setDrawableSurface(host(v), *o, argString(v, 2));
    return 0;
}
// setCamera(name, depth, fov) (0x537cb0): the default view first, then the named camera's.
SQInteger setCamera(HSQUIRRELVM v) {
    PARTICLE_SELF
    if (o->renderingScene || !o->system) return 0;
    auto& h = host(v);
    o->system->setCamera(nullptr, -1.0f, 40.0f, h.screenWidth(), h.screenHeight());
    std::string name = argString(v, 2);
    if (!name.empty()) {
        if (auto c = cameraView(h, name)) {
            o->camera = c;
            o->system->setCamera(c, argFloat(v, 3), argFloat(v, 4), h.screenWidth(), h.screenHeight());
        }
    }
    return 0;
}
SQInteger seek(HSQUIRRELVM v) {  // 0x537e20
    PARTICLE_SELF
    if (o->system) o->system->seek(argFloat(v, 2));
    return 0;
}
SQInteger isFading(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, o->fade.active);
}
SQInteger isMoving(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, o->move.active);
}
SQInteger isScaling(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, o->scale.active);
}
SQInteger isRotating(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, o->rotate.active);
}
SQInteger getFade(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushFloat(v, o->fade.now[0]);
}
SQInteger getVector(HSQUIRRELVM v, Tween ParticleObject::*member) {
    PARTICLE_SELF
    const Tween& t = o->*member;
    host(v).setResult(t.now[0], t.now[1], t.now[2], 0.0f);
    return 0;
}
SQInteger getMove(HSQUIRRELVM v) { return getVector(v, &ParticleObject::move); }
SQInteger getScale(HSQUIRRELVM v) { return getVector(v, &ParticleObject::scale); }
SQInteger getRotate(HSQUIRRELVM v) { return getVector(v, &ParticleObject::rotate); }
SQInteger getColor(HSQUIRRELVM v) { return getVector(v, &ParticleObject::color); }
SQInteger getFrequency(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushFloat(v, o->frequency.now[0]);
}
SQInteger isActivle(HSQUIRRELVM v) {  // 0x537fa0
    PARTICLE_SELF
    return pushInt(v, o->system && o->system->active() ? 1 : 0);
}
SQInteger timerOf(HSQUIRRELVM v, Tween ParticleObject::*member) {
    PARTICLE_SELF
    return pushFloat(v, (o->*member).timer);
}
SQInteger getFadeTimer(HSQUIRRELVM v) { return timerOf(v, &ParticleObject::fade); }
SQInteger getMoveTimer(HSQUIRRELVM v) { return timerOf(v, &ParticleObject::move); }
SQInteger getScaleTimer(HSQUIRRELVM v) { return timerOf(v, &ParticleObject::scale); }
SQInteger getRotateTimer(HSQUIRRELVM v) { return timerOf(v, &ParticleObject::rotate); }
SQInteger getColorTimer(HSQUIRRELVM v) { return timerOf(v, &ParticleObject::color); }
// getSplineTimer and getFrequencyTimer share one function (0x538010): the spline's timer.
SQInteger getSplineTimer(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushFloat(v, o->spline.t.timer);
}
SQInteger setTimerOf(HSQUIRRELVM v, Tween ParticleObject::*member) {
    PARTICLE_SELF
    (o->*member).setTimer(argFloat(v, 2));
    return 0;
}
SQInteger setFadeTimer(HSQUIRRELVM v) { return setTimerOf(v, &ParticleObject::fade); }
SQInteger setMoveTimer(HSQUIRRELVM v) { return setTimerOf(v, &ParticleObject::move); }
SQInteger setScaleTimer(HSQUIRRELVM v) { return setTimerOf(v, &ParticleObject::scale); }
SQInteger setRotateTimer(HSQUIRRELVM v) { return setTimerOf(v, &ParticleObject::rotate); }
SQInteger setFrequencyTimer(HSQUIRRELVM v) { return setTimerOf(v, &ParticleObject::frequency); }
SQInteger setColorTimer(HSQUIRRELVM v) { return setTimerOf(v, &ParticleObject::color); }
SQInteger setSplineTimer(HSQUIRRELVM v) {  // 0x5381a0
    PARTICLE_SELF
    if (!o->spline.built()) return 0;
    o->spline.setTimer(argFloat(v, 2));
    if (o->spline.t.active) o->placeMove(o->spline.point);
    return 0;
}
// The task object's flags (vtable +0x80 .. +0x94).
SQInteger setPriority(HSQUIRRELVM v) {
    PARTICLE_SELF
    host(v).setDrawPriority(o, argI(v, 2));
    return 0;
}
SQInteger getPriority(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, host(v).drawPriority(o));
}
SQInteger setErase(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->erase = argI(v, 2);
    return 0;
}
SQInteger getErase(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, o->erase);
}
SQInteger setLock(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->lock = argI(v, 2);
    return 0;
}
SQInteger getLock(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, o->lock);
}
SQInteger setDisused(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->disused = argI(v, 2);
    return 0;
}
SQInteger getDisused(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, o->disused);
}
// setHidable / getHidable (Totono: 0x4d1dcd / 0x4d1ded, the task object's flag as the sprites').
SQInteger setHidable(HSQUIRRELVM v) {
    PARTICLE_SELF
    o->hidable = argI(v, 2);
    return 0;
}
SQInteger getHidable(HSQUIRRELVM v) {
    PARTICLE_SELF
    return pushInt(v, o->hidable);
}
// serialize / deserialize(file) (vtable +0x88 / +0x8c: 0x538440 / 0x538610): the record of the
// system's state. The framework reads it last, after making the system again (create) and replaying
// its last calls, so a running system goes on running (ParticleSystem::restore).
SQInteger serialize(HSQUIRRELVM v) {
    PARTICLE_SELF
    EngineState st;
    st.particles = o->system.get();
    bool ok = writeEngineRecord(*o->host, argString(v, 2), EngineRecord::Particle, st);
    return pushInt(v, ok && o->system ? 1 : 0);
}
SQInteger deserialize(HSQUIRRELVM v) {
    PARTICLE_SELF
    EngineState st;
    st.particles = o->system.get();
    bool ok = readEngineRecord(*o->host, argString(v, 2), EngineRecord::Particle, st);
    return pushInt(v, ok && o->system ? 1 : 0);
}

}  // namespace

void pushParticleParameter(HSQUIRRELVM v, const std::string& name) {
    pushParticleTable(v, particleFileNamed(host(v), name));
}

ClassNatives particleNatives() {
    ClassNatives c;
    c.make = make;
    c.methods = {{"create", create},
                 {"createFromTable", createFromTable},
                 {"link", link},
                 {"linkEmitter", linkEmitter},
                 {"getParameterTable", getParameterTable},
                 {"setParameterTable", setParameterTable},
                 {"begin", begin},
                 {"end", end},
                 {"reset", reset},
                 {"fade", fade},
                 {"move", moveSet},
                 {"scale", scaleSet},
                 {"rotate", rotateSet},
                 {"setColor", colorSet},
                 {"setFrequency", setFrequency},
                 {"resetSpline", resetSpline},
                 {"setSpline", setSpline},
                 {"beginSpline", beginSpline},
                 {"setTimeScale", setTimeScale},
                 {"setSurface", setSurface},
                 {"setDither", setDither},
                 {"setCamera", setCamera},
                 {"seek", seek},
                 {"isFading", isFading},
                 {"isMoving", isMoving},
                 {"isScaling", isScaling},
                 {"isRotating", isRotating},
                 {"getFade", getFade},
                 {"getMove", getMove},
                 {"getScale", getScale},
                 {"getRotate", getRotate},
                 {"getFrequency", getFrequency},
                 {"getColor", getColor},
                 {"isActivle", isActivle},
                 {"getFadeTimer", getFadeTimer},
                 {"getMoveTimer", getMoveTimer},
                 {"getScaleTimer", getScaleTimer},
                 {"getRotateTimer", getRotateTimer},
                 {"getSplineTimer", getSplineTimer},
                 {"getFrequencyTimer", getSplineTimer},
                 {"getColorTimer", getColorTimer},
                 {"setFadeTimer", setFadeTimer},
                 {"setMoveTimer", setMoveTimer},
                 {"setScaleTimer", setScaleTimer},
                 {"setRotateTimer", setRotateTimer},
                 {"setSplineTimer", setSplineTimer},
                 {"setFrequencyTimer", setFrequencyTimer},
                 {"setColorTimer", setColorTimer},
                 {"setPriority", setPriority},
                 {"getPriority", getPriority},
                 {"setErase", setErase},
                 {"getErase", getErase},
                 {"setLock", setLock},
                 {"getLock", getLock},
                 {"setHidable", setHidable},
                 {"getHidable", getHidable},
                 {"setDisused", setDisused},
                 {"getDisused", getDisused},
                 {"serialize", serialize},
                 {"deserialize", deserialize}};
    return c;
}

}  // namespace nyan::diesel
