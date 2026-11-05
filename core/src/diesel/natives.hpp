#pragma once

// Native implementations by class (natives_*.cpp); natives_globals.cpp hands them to the host.

#include <deque>

#include "runtime.hpp"
#include "nyan/diesel/camera.hpp"
#include "nyan/diesel/particle.hpp"
#include "nyan/diesel/text_layout.hpp"

namespace nyan::diesel {

ClassNatives action1DNatives();  // natives_action.cpp
ClassNatives processNatives();
ClassNatives cameraNatives();    // natives_camera.cpp
ClassNatives sceneNatives();     // natives_scene.cpp
ClassNatives particleNatives();  // natives_particle.cpp
// SQApp.getParticleParameter's table: media/particle/<name>.particle's record (natives_particle.cpp).
void pushParticleParameter(HSQUIRRELVM v, const std::string& name);
// A camera task object's view by its name (SQSprite.setCamera), or null.
std::shared_ptr<const CameraView> cameraView(ScriptHost::Impl& h, const std::string& name);
ClassNatives appNatives();       // natives_app.cpp
ClassNatives physicsNatives();
ClassNatives trophyNatives();
ClassNatives spriteNatives();    // natives_sprite.cpp
ClassNatives spriteMaskNatives();
ClassNatives spriteRenderTargetNatives();
ClassNatives displacementMapNatives();
// A task's displacement map if it is an SQDisplacementMap with one made (natives_sprite.cpp).
std::shared_ptr<const DisplacementMap> displacementMapOf(FrameTask& task);
// setSurface / setMask by name, for every drawing class (natives_sprite.cpp).
void setDrawableSurface(ScriptHost::Impl& h, Drawable& d, const std::string& name);
void setDrawableMask(ScriptHost::Impl& h, Drawable& d, const std::string& name);
// Mware's path normalization before any open ('\\' to '/', runs of '/' as one): natives_file.cpp.
std::string normalizeSlashes(std::string p);
ClassNatives soundNatives();     // natives_sound.cpp
ClassNatives textObjectNatives(); // natives_textobject.cpp
// The natives a game's exe adds to the root table beyond the engine's (natives_app.cpp); each is
// registered if the exe holds its name.
std::vector<NativeImpl> exeNatives();
std::vector<NativeImpl> n1Globals();       // natives_n1.cpp: The Song of Saya's own
ClassNatives n1ScriptReaderNatives();
ClassNatives iniFileReaderNatives();
ClassNatives fileNatives();      // natives_file.cpp
std::vector<NativeImpl> fileGlobals();
std::vector<NativeImpl> textGlobals();  // natives_text.cpp
// The save natives (natives_vmstate.cpp): CreateReferenceTable, ClearReferenceTable,
// SerializeRootTable, DeserializeRootTable.
std::vector<NativeImpl> vmStateGlobals();
std::vector<NativeImpl> mathGlobals();  // natives_math.cpp: SQMATH_V3_* / SQMATH_Q_*
// The call stacks of the VM's suspended script threads but `v` (innermost first), for reports.
std::string suspendedThreadStacks(HSQUIRRELVM v);
// The engine objects' part of a save (serialize / deserialize(file)), in the file the scripts named.
enum class EngineRecord { Sprite, Mask, Target, Text, Camera, Particle };
// The object's parts a record has: whether it holds its image (+0xc4), a sprite's blur trail, the
// wrapper's +0xcc (a render target's or screen capture's capture flag) and a screen capture's
// picture (DrawItem::screen; a frozen one is saved, and where a loaded one goes).
// A glyph as a text's trail keeps it (CBlur's 0x70-byte vertex records): the item as drawn, its
// alpha byte before the fade, and whether it is in the ruby characters' batch.
struct TextGlyphDraw {
    DrawItem item;
    float alphaByte = 0.0f;
    bool rubyFont = false;
};
using TextBlurFrames = std::array<std::deque<std::vector<TextGlyphDraw>>, 2>;
struct EngineState {
    bool hasImage = false;
    ParticleSystem* particles = nullptr;  // SQParticle's system
    std::vector<TextChar>* chars = nullptr;  // SQText's laid-out characters
    std::vector<TextChar>* rubies = nullptr; // and its ruby characters
    std::vector<PositionFixRule>* fixRules = nullptr;
    SpriteImage::Blur* textBlur = nullptr;      // a text's trail settings and its batches' frames
    TextBlurFrames* textBlurFrames = nullptr;
    std::unique_ptr<Shake>* shake = nullptr;  // CSprite +0x658
    SpriteImage::Blur* blur = nullptr;
    int32_t* capture = nullptr;
    uint32_t screen = 0;
    std::shared_ptr<const ScreenPicture>* picture = nullptr;
};
bool writeEngineRecord(ScriptHost::Impl& h, const std::string& file, EngineRecord kind, const EngineState& s);
bool readEngineRecord(ScriptHost::Impl& h, const std::string& file, EngineRecord kind, const EngineState& s);
// Mware's value types for the scripts (natives_math.cpp): Vector2D, Vector3D, ColorF, Rect2DF.
void registerMathClasses(HSQUIRRELVM v, const ExeInfo& exe);
// A native's argument that must be an instance of one of them (by class name): its members.
std::optional<std::array<float, 4>> argMath(HSQUIRRELVM v, SQInteger idx, const char* className);
// shake / randomShake's arguments (SQSprite 0x50e0e0, SQText 0x522050 and their random twins): a new
// shake in `out` with that time scale; below 0 the error the native returns.
SQInteger shakeFromArgs(HSQUIRRELVM v, bool random, float timeScale, std::unique_ptr<Shake>& out);

}  // namespace nyan::diesel
