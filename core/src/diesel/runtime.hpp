#pragma once

// The script host's insides, shared by the native implementations (natives_*.cpp). Not a public
// header: it exposes Squirrel's API.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <squirrel.h>

#include "nyan/diesel/action.hpp"
#include "nyan/diesel/draw_item.hpp"
#include "nyan/diesel/font.hpp"
#include "nyan/diesel/sound_events.hpp"
#include "nyan/diesel/input.hpp"
#include "nyan/diesel/random.hpp"
#include "nyan/diesel/save_store.hpp"
#include "nyan/diesel/script_host.hpp"
#include "nyan/diesel/sprite.hpp"
#include "nyan/diesel/system_config.hpp"
#include "nyan/diesel/text_layout.hpp"

namespace nyan::diesel {

// A native object behind a script instance (SQSprite, SQText, ...): owned by the instance, deleted
// by its release hook.
struct NativeObject {
    virtual ~NativeObject() = default;
};

// The innermost script function running ("name file:line"), for reports.
std::string scriptLocation(HSQUIRRELVM v);

// An object of the task's draw list (SQSprite, SQText, ...; SQRenderingObject): drawn in order of
// priority, then of the system time it was made at (CTask 0x484a60 / 0x487730).
struct Drawable {
    virtual ~Drawable() = default;
    // The image the draw list draws as a sprite (null: the object emits its own items).
    virtual SpriteImage* image() { return nullptr; }
    // What a link() to this object hangs from (vtable +0x30): its image, or a text's node.
    virtual SpriteImage* linkNode() { return image(); }
    // Objects drawn otherwise than as one sprite (text) add their quads here.
    virtual void emit(ScriptHost::Impl& host, std::vector<DrawItem>& out) {}
    int32_t erase = 0;  // SQRenderingObject +0xbc: not drawn
    // setSurface: the render target the object draws into (its id; 0 the screen), vtable +0x4c.
    uint32_t surface = 0;
    // setMask: the mask whose texture the object's pixels are masked by (its id; 0 none).
    uint32_t mask = 0;
    // A render target or a mask this object is (its id, else 0): set*'s lookups by name.
    virtual uint32_t targetId() const { return 0; }
    virtual bool isMask() const { return false; }
};

// A native object the engine updates once per frame (the exes' CTaskObject: its vtable slot 14),
// in the task's object list, which link() searches by name (CTask +0xb4: 0x484a40, 0x484d90).
struct FrameTask {
    virtual ~FrameTask() = default;
    virtual void update(ScriptHost::Impl& host) = 0;
    virtual Drawable* drawable() { return nullptr; }
    std::string taskName;     // the constructor's name argument
    uint32_t created = 0;     // CTaskObject +0xb0: the system time it was made at
};

// An SQFile's bytes (natives_file.cpp) for the save natives (natives_vmstate.cpp), which write their
// records into the file the scripts opened, found by the name it was made with (a task object).
struct FileStream {
    virtual ~FileStream() = default;
    virtual size_t readBytes(void* dst, size_t n) = 0;  // fewer at the end
    virtual void writeBytes(const void* src, size_t n) = 0;
    virtual size_t tell() const = 0;
    virtual void seekTo(size_t pos) = 0;
};

struct ScriptHost::Impl {
    const GameData& game;
    Log log;
    HSQUIRRELVM vm = nullptr;
    HSQUIRRELVM current = nullptr;  // the VM (or thread) whose native runs now, for reports
    std::vector<std::string> traced;  // NYAN_DIESEL_TRACE: script functions whose calls are logged
    int traceDepth = 1;               // NYAN_DIESEL_TRACE_DEPTH: the callers each names
    // Every registered native ("SQSprite.create", "RandomI") and whether Nyanroid implements it (else
    // the stub); with NYAN_DIESEL_COUNT_NATIVES, the implemented ones' calls.
    std::map<std::string, bool> natives;
    bool countNatives = false;
    std::vector<SQFUNCTION> countedFns;
    std::map<std::string, uint64_t> nativeCalls;
    State state = State::Idle;
    std::string lastError;
    uint64_t frame = 0;
    std::map<std::string, int> unhandled;
    std::map<std::string, int> missing;
    // Each native class's Squirrel class object and type tag, by name.
    struct ClassInfo {
        HSQOBJECT object{};
        const void* tag = nullptr;
    };
    std::unordered_map<std::string, ClassInfo> classes;
    // Each class's maker of native objects, which its constructor closure points to (freed after the
    // VM closes; one set per host, as hosts may run on several threads).
    std::vector<std::unique_ptr<NativeObject* (*)(HSQUIRRELVM)>> classMakers;
    // The frame clock and the objects updated each frame, in creation order (null: removed).
    FrameClock clock;
    std::vector<FrameTask*> tasks;
    void addTask(FrameTask* t) { tasks.push_back(t); }
    void removeTask(FrameTask* t) {
        for (auto& p : tasks) {
            if (p == t) p = nullptr;
        }
    }
    // The first task object made with `name` (0x484d90), or null.
    FrameTask* findTask(const std::string& name) const {
        for (FrameTask* t : tasks) {
            if (t && t->taskName == name) return t;
        }
        return nullptr;
    }
    // The draw list: {made at, priority, object}, ordered by priority then time (ties: the order
    // they joined, as std::upper_bound places them).
    struct DrawEntry {
        uint32_t created;
        int32_t priority;
        Drawable* object;
    };
    std::vector<DrawEntry> drawList;
    void addDrawable(Drawable* d, int32_t priority, uint32_t created);
    void removeDrawable(Drawable* d);
    void setDrawPriority(Drawable* d, int32_t priority);
    int32_t drawPriority(const Drawable* d) const;
    // Texture sizes by file (the headless host reads headers, not pixels).
    std::map<std::string, std::pair<int, int>> textureSizes;
    bool textureSize(const std::string& path, int& w, int& h);
    // A texture's alpha (decoded once, for hit tests: SQSprite.setClickable's alpha mode); null if
    // it does not decode.
    struct AlphaPlane {
        int width = 0, height = 0;
        std::vector<uint8_t> alpha;
    };
    std::map<std::string, std::shared_ptr<const AlphaPlane>> textureAlphas;
    const AlphaPlane* textureAlpha(const std::string& path);
    // The frame as drawn (layout(), after the scripts' frame), and the screen it was laid out on.
    std::vector<DrawItem> drawItems;
    std::vector<RenderTarget> renderTargets;  // the frame's render targets and masks
    uint32_t nextTargetId = 1;
    int layoutWidth = 0, layoutHeight = 0;    // the target the object being laid out draws into
    void layout();
    void emitBlur(SpriteImage& im, const DrawItem& d, std::vector<DrawItem>& out);
    int screenWidth() const { return game.screenWidth(); }
    int screenHeight() const { return game.screenHeight(); }

    // The game's files as Mware's file manager sees them (core/src/diesel/natives_file.cpp).
    SaveStore* saves = nullptr;
    std::unique_ptr<SaveStore> ownSaves;
    std::string saveRoot;         // GetSavePathRoot: setting.ini [App] SAVE_PATH plus '/'
    std::string initialSaveRoot;  // the store's root
    std::string tempRoot;         // GetTempPathRoot (never set by the exes: "")
    struct FilePath {
        bool save = false;  // in the save store (else the archives and the install)
        std::string path;   // store path, or archive / folder name
    };
    FilePath resolvePath(const std::string& scriptPath) const;
    // The store path a write to `scriptPath` goes to (paths outside the save root: "folder/...").
    std::string writablePath(const std::string& scriptPath) const;
    // A file's bytes as Mware's reader gives them (store, then archives, then loose files).
    std::optional<Bytes> readAnyFile(const std::string& scriptPath) const;
    // The same file opened for reading piece by piece (archive entries stream); null if missing.
    std::shared_ptr<RandomAccessFile> openAnyFile(const std::string& scriptPath) const;

    // The SQMATH_* variables (SqPlus-bound floats of the script host object, +0xe4..+0x158): the
    // vector and quaternion arguments of the SQMATH_* functions, then 16 results (natives that
    // return vectors write SQMATH_RET_1.. and the scripts read them back).
    static constexpr int kMathVars = 30;
    float mathVars[kMathVars] = {};
    // Whether the exe registers Mware's value types (Vector3D, ...) as script classes (Muramasa,
    // Totono). Phantom of Inferno's does not: its natives take vectors from SQMATH_V3_1 / _2 instead
    // (SQSprite.shake, phantom.exe 0x140143700), and it binds SQMATH_V3_3_* to SQMATH_V3_2's floats
    // (0x140046710: +0x19c .. +0x1a4 both).
    bool mathClasses = true;
    bool mathV3Third = false;
    static constexpr int kMathRet = 14;  // index of SQMATH_RET_1
    void setResult(float x, float y, float z = 0.0f, float w = 0.0f) {
        mathVars[kMathRet] = x;
        mathVars[kMathRet + 1] = y;
        mathVars[kMathRet + 2] = z;
        mathVars[kMathRet + 3] = w;
    }

    // The 3D scene's camera (SQCamera.setSceneCamera; muramasa_en.exe 0x52bcb0 keeps it at the
    // render manager's +0xd3c, phantom.exe at +0xe58, totono_en.exe hands it to the scene manager).
    // Null: the camera Mware starts with.
    std::shared_ptr<const CameraView> sceneCamera;
    // totono_en.exe's Mware scene manager (app+0x30, CSceneManagerDX11): the renderer's settings its
    // SQScene hands it (vtable slots in the comments); only the world / screen transforms read
    // anything back. `sceneForwards`: this exe's SQScene does so (its setHDRParameter is not the
    // empty two-argument function setShadowMapCenterBone is in all three exes).
    struct SceneManager {
        std::array<float, 3> shadowMapOffsetRotate{};         // +0x68
        std::array<float, 3> projectionShadowOffsetRotate{};  // +0x6c
        int32_t hdrBloom = 0;                                 // +0xac
        std::array<float, 2> hdrParameter{};                  // +0xb0
        float bloomParameter = 0.0f;                          // +0xb4
        int32_t dof = 0;                                      // +0xb8
        std::array<float, 2> dofParameter{};                  // +0xbc
        int32_t toneMapping = 0;                              // +0xc0
        std::array<float, 3> toneMappingParameter{};          // +0xc4
        int32_t softFocus = 0;                                // +0xc8
        std::array<float, 2> softFocusParameter{};            // +0xcc
        std::array<int32_t, 3> sceneStateShadow{};            // +0xd0
        int32_t keepSubstanceRenderingResult = 0;             // +0xd4
        std::array<float, 2> radialBlurCenter{};              // +0xd8
        float radialBlurPower = 0.0f;
        int32_t fog = 0;                                      // +0xe4
        float softParticleLength = 0.0f;                      // +0xec
    };
    SceneManager sceneManager;
    bool sceneForwards = false;

    // The app (natives_app.cpp): input, the system settings, the window, fonts and sound volumes.
    Input input;
    SystemConfig config;
    int32_t frameRate = 60;     // the core's (core+0x1c): the frames a second it paces to
    uint64_t rateFrame = 0;     // the frame and time the rate last changed at (the headless clock)
    uint32_t rateTime = 0;
    std::string windowTitle;
    bool fullScreen = false;    // the window's state (isFullScreen), as the settings last applied
    bool cursorVisible = true;
    // app+0xcc (phantom.exe +0x160): the cursor shape for this frame, 1 from the constructor
    // (0x42cf17). setCursor sets it; the frame's end (0x42e751) posts it to the window if it changed
    // since the last post (0x6223e0, 0 at start) and puts it back to 1, so a script that wants the
    // hand sets 7 every frame. The window procedure (0x4017c0) shows Windows' arrow (IDC_ARROW) for 1
    // and its hand (IDC_HAND) for 7 and leaves the pointer as it was for any other shape.
    int32_t cursor = 1;
    int32_t postedCursor = 0;
    int32_t shownCursor = 1;    // what the window shows: 1 arrow (the window's own at first), 7 hand
    struct Font {
        std::string file;        // a font file (loadFont, setting.ini [Font])
        std::string systemName;  // a Windows font by name (loadSystemFont)
        int32_t size = 24, weight = 400, charset = 0, vertical = 0, noAA = 0;
        float bias = 0.0f;
    };
    std::map<std::string, Font> fonts;
    // The line-break tables the Japanese layout reads (the NG codes): as the exe builds them at
    // start-up (read from its code: script_host.cpp), changed by the AddNGCode_* globals.
    LineBreakTables lineBreaks, lineBreakDefaults;
    // The older text code (TextLayout::older): an exe whose English layout knows no SMALL.
    bool olderText = false;
    // The faces Windows would know: the game's font files (loaded on first use) and the host's
    // system fonts (ScriptHost::addSystemFont); fonts made from them, by font name.
    std::vector<std::shared_ptr<FontFace>> gameFaces, systemFaces;
    std::set<std::string> loadedFontFiles;
    std::map<std::string, std::shared_ptr<FontInstance>> fontInstances;
    std::shared_ptr<FontInstance> fontInstance(const std::string& name);
    // The face Windows finds under a family name: the game's font files (loaded on first use),
    // then the host's fonts; null if none answers.
    std::shared_ptr<FontFace> findFace(const std::string& family);
    // The fonts loadTextTexture's text code makes (Mware's factory by face name; by family, size,
    // weight and vertical here).
    std::map<std::string, std::shared_ptr<FontInstance>> typingFonts;
    std::shared_ptr<FontInstance> typingFont(const std::string& family, int size, int weight, bool vertical);
    // SQApp.loadTexture's map (CApp +0xe0: name -> the texture, held while listed; nothing draws
    // from it, so only the names matter here).
    std::set<std::string> appTextures;
    // The textures loadTextTexture made, by their names in that map: the pictures themselves
    // (SQSprite.create finds them there; a sprite keeps its texture after the name is unloaded).
    std::map<std::string, std::shared_ptr<const Image>> textTextures;
    // SQApp.callTask's name (CApp +0x78), taken by the next frame's task switch (0x42e400).
    std::string calledTask;
    // showMessageBox's dialog (MessageBoxW, which holds the whole frame loop until it is answered):
    // the script is suspended in the call and woken with the button's id once the player answers.
    struct MessageBoxState {
        bool pending = false;   // shown, waiting for the player
        bool answered = false;  // answered: the next frame() finishes the frame
        bool resume = false;    // the script waits in the call: woken with `result` first
        uint32_t serial = 0;
        std::string caption, text;
        int32_t type = 0;       // MB_* (buttons in the low four bits)
        int32_t result = 0;
    } messageBox;
    void finishFrame();
    struct Volume {
        Action1D action;
        int32_t dirty = 1;
        bool active = false, wasActive = false;       // +0x10 / +0xc (types), +0xc / +0x8 (groups)
        int32_t topPriority = 0, lastTopPriority = 0;  // groups: the audible members' highest (+4 / +0)
        Volume() { action.set(1.0f, 1.0f, 0, 0); }
    };
    std::map<int32_t, Volume> typeVolumes;         // setSoundTypeVolume (app+0x138)
    std::map<std::string, Volume> groupVolumes;    // defineSoundGroup / setSoundGroupVolume
    // A sound's entry in the sound manager (0x60 bytes, 0x44fc70): the groups it ducks in, its ducking
    // control and the flags the manager and the sound pass each other.
    struct SoundRecord {
        struct Entry {
            std::string group;
            int32_t priority = 0;
            float factor = 1.0f;
        };
        std::vector<Entry> groups;  // addSoundGroup(name, priority, factor)
        Action1D control;           // +0x0c: the ducking control, 1 at first
        int32_t fadeIn = 100, fadeInCurve = 0, fadeOut = 100, fadeOutCurve = 0;  // +0x38 .. +0x44
        bool audible = false;       // +0x48
        bool dirty = true;          // +0x4c
        bool first = true;          // +0x50
        bool silent = false;        // +0x54
        int32_t type = -1;          // +0x58: the type whose volume applies (-1: none)
        float groupProduct = 1.0f;  // +0x5c: its groups' volumes multiplied
        SoundRecord() { control.set(1.0f, 1.0f, 0, 0); }
        virtual ~SoundRecord() = default;
        // The voices playing now, as the device has them set.
        virtual void voices(std::vector<SoundVoice>& out) const = 0;
    };
    std::vector<SoundRecord*> soundRecords;
    std::vector<SoundEvent> soundEvents;  // for the host, since it last took them
    uint32_t nextSoundKey = 1;            // the device's voices (sounds' and movies')
    // Movies: Mware runs each on a thread of its own against the real-time clock; here they step
    // once a frame, after the scripts and before the draw, by the frame clock's elapsed time.
    struct MovieTask {
        virtual ~MovieTask() = default;
        virtual void stepMovie(Impl& h) = 0;
    };
    std::vector<MovieTask*> movies;
    uint32_t nextMovieId = 1;
    // The sound manager's frame (0x42cc00): groups and types step, the groups' top audible priority,
    // each record's ducking control.
    void updateSoundManager();
    // A type's volume now (1 for none).
    float typeVolume(int32_t type) const {
        if (type < 0) return 1.0f;
        auto it = typeVolumes.find(type);
        return it == typeVolumes.end() ? 1.0f : it->second.action.now;
    }
    HSQOBJECT closeCallback{};
    bool hasCloseCallback = false;
    bool quit = false;
    bool reboot = false;     // RebootRequest: the exe restarts itself
    std::string appName;     // the app object's name (getProductName): the window's first title
    int32_t ecoMode = 0;     // IsEcoMode: the launcher's second option as read at start
    // RandomI / RandomF's generator (muramasa_en.exe 0x6218e8, seeded with time(0) at 0x490aa7; 0
    // here until the session seeds it, which keeps headless runs reproducible).
    MwareRandom random;
    int32_t randomSeed = 0;  // what seedRandom gave; CRandom() objects seed from it and the time
    std::vector<std::string> systemCalls;  // SystemCall's requests (URLs to open)
    ScriptHost::ScreenReader screenReader;  // the renderer's screen captures (the session's)
    ScriptHost::ScreenReader targetReader;  // the renderer's render targets (the session's)
    uint32_t nextScreenId = 1;
    // The app's screen fade (CApp +0xf4: a screen-sized sprite of its own, made at alpha 0 by
    // 0x42d1f0, drawn over every task while its alpha is above 0 by the frame's draw 0x42e7e0).
    // fadeoutScreen / fadeoutColor (0x42ec00) put the last frame shown, or a colour, in it at alpha 1
    // and fade it to 0; its update (vtable +0x38, at the end of the frame 0x42e6f0) steps the tween
    // after the frame is drawn.
    struct ScreenFade {
        Tween fade{1, 0.0f};          // CSprite +0x13c
        bool colour = false;          // the texture holds a colour (else the last capture)
        std::array<float, 3> rgb{};   // its bytes / 255
        bool captureRequested = false;  // CApp +0xf8: the next draw copies the last frame shown in
        uint32_t screenId = 0;        // the renderer's texture for it (allocated on first use)
    } screenFade;
    // Voice files' amplitude by 10 ms block (natives_sound.cpp, getAmplitude): the mean |sample| over
    // each block's frames and channels, from the file's start; the last few files kept.
    struct AmplitudeEnvelope {
        int rate = 0, blockFrames = 0;
        std::vector<float> mean;
    };
    std::map<std::string, std::shared_ptr<const AmplitudeEnvelope>> amplitudes;
    std::vector<std::string> amplitudeOrder;
    // The named surfaces (sprite.hpp NamedSurface) by their "suf://" name, while something holds them.
    std::map<std::string, std::weak_ptr<NamedSurface>> surfaces;
    std::shared_ptr<NamedSurface> findSurface(const std::string& name) {
        auto it = surfaces.find(name);
        return it == surfaces.end() ? nullptr : it->second.lock();
    }
    // Mware's get-or-create: the surface in use under the name, else a new one with this id and size.
    std::shared_ptr<NamedSurface> namedSurface(const std::string& name, NamedSurface::Kind kind, uint32_t id, int w, int h) {
        if (auto s = findSurface(name)) return s;
        auto s = std::make_shared<NamedSurface>(NamedSurface{kind, id, w, h});
        surfaces[name] = s;
        return s;
    }
    // The save natives' reference table (natives_vmstate.cpp: CreateReferenceTable ..
    // ClearReferenceTable); released before the VM closes.
    struct ReferenceTable;
    std::unique_ptr<ReferenceTable, void (*)(ReferenceTable*)> referenceTable{nullptr, nullptr};
    // Applies the settings as 0x444ef0 does (frame rate, window mode).
    void applyConfig();
    std::string systemConfigPath() const { return saveRoot + "system.dat"; }

    Impl(const GameData& g, Log l) : game(g), log(std::move(l)) { sq_resetobject(&closeCallback); }

    // The first time a native or file is noted, it is logged; a missing file with the script line
    // that asked for it.
    void note(std::map<std::string, int>& what, const std::string& name) {
        if (what[name]++ != 0 || !log) return;
        if (&what == &unhandled) {
            log("unhandled: " + name);
        } else {
            log("missing: " + name + " (" + scriptLocation(current ? current : vm) + ")");
        }
    }
    // The file a script names, as the engine resolves it; nullopt (and noted missing) if absent.
    std::optional<Bytes> readFile(const std::string& path);
};

// The host of a VM (set as its foreign pointer).
ScriptHost::Impl& host(HSQUIRRELVM v);

// Argument helpers: the value at `idx`, converted as SqPlus converts (numbers either way).
SQInteger argInt(HSQUIRRELVM v, SQInteger idx, SQInteger fallback = 0);
SQFloat argFloat(HSQUIRRELVM v, SQInteger idx, SQFloat fallback = 0);
bool argBool(HSQUIRRELVM v, SQInteger idx, bool fallback = false);
std::string argString(HSQUIRRELVM v, SQInteger idx, const std::string& fallback = "");
SQInteger argCount(HSQUIRRELVM v);  // parameters after `this`

// Defining natives in the table on top of the stack.
void defineFunction(HSQUIRRELVM v, const char* name, SQFUNCTION fn);

// The native object of `this` (stack index 1) if it is an instance of a native class.
template <typename T>
T* self(HSQUIRRELVM v) {
    SQUserPointer p = nullptr;
    if (SQ_FAILED(sq_getinstanceup(v, 1, &p, nullptr)) || !p) return nullptr;
    return dynamic_cast<T*>(static_cast<NativeObject*>(p));
}

// Native implementations of one class or group: name -> function. Registered over the stubs that
// every name of natives_table.cpp gets.
struct NativeImpl {
    const char* name;
    SQFUNCTION fn;
};

// The groups (natives_*.cpp): globals, and each class's constructor object and methods.
std::vector<NativeImpl> globalNatives();
struct ClassNatives {
    std::vector<NativeImpl> methods;
    // Makes the native object a new instance holds (nullptr: a plain NativeObject).
    NativeObject* (*make)(HSQUIRRELVM v) = nullptr;
};
ClassNatives classNatives(const std::string& className);

}  // namespace nyan::diesel
