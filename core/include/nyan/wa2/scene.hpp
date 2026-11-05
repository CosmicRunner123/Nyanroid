#pragma once

#include <array>
#include <functional>
#include <string>

#include "nyan/wa2/save.hpp"

namespace nyan::wa2 {

class Game;
class Gfx;

// The story's picture layers (WA2.exe 0x416650 show, 0x40b8d0 transition and camera step, 0x415f50 /
// 0x40d440 characters, 0x401de0 / 0x40d280 tone), on the engine's render-target path: two scene
// buffers (render targets 0x105 / 0x106) each hold a background (object 0x371+b, slot 0x71+b) and the
// characters attached to it (objects 0x379.., slots 0x75..); display objects 0x409+b put a buffer on
// the screen, and transitions blend the two.
class Scene {
public:
    explicit Scene(Game& game);

    // The scene step's first frame (0x413c10 states 0 / 1): the system pictures and the two render
    // targets. Then every frame step().
    void begin();
    // 0x413ac0's part: the black backdrop (object 1), neutral tone and colour settings.
    void reset();
    // The per-frame work after the VM, in the engine's order (0x413c10): 0x40b8d0 (transition, camera),
    // 0x40d280 (tone), then the weather, then 0x40d440 (characters).
    void transitionStep();  // 0x40b8d0
    void toneStep();        // 0x40d280
    void charStep();        // 0x40d440

    // ---- Backgrounds (B, BC, V, H and the ...2 forms) ----
    // 0x416650. kind 0 background (`B%04d%1d%1d.tga` from bak), 1 event CG (`v%06d.tga`), 2 `h%06d.tga`;
    // num -20 shows the current picture again, -10 fades out. 10000 for x, y, zx, zy, rot keeps / continues.
    void show(int num, int trans, int keepChars, int kind, int frames, int soft, int x, int y, float zx, float zy,
              float rot, int raw);
    bool transitioning() const { return transState_ != 0; }  // 0x401a30

    // ---- Camera (S, Z, R, WSZ, StopSZR) ----
    void pan(int x, int y, int frames, int ease);                                // 0x401a90
    void zoom(float zx, float zy, int px, int py, int frames, int ease);        // 0x401b80
    void rotate(float angle, int px, int py, int frames, int ease);             // 0x401c60
    void pauseCamera(int on, int frames);                                         // 0x401a70
    bool cameraMoving() const;                                                    // 0x401a40

    // ---- Characters (C, CS, CW, CR, CRS, CRW, CM) ----
    // 0x415f50: pos is an index into the position table, or (posIsFloat) the offset itself.
    void charSet(int id, int pic, int posIndex, float posFloat, bool posIsFloat, int z, int mode, int r, int g, int b,
                 int alpha, int frames);
    void charRemove(int id, int mode, int frames);  // 0x402610
    void charMove(int id, float x);                 // 0x402730
    bool charExists(int id) const;                  // 0x402560
    bool charBusy(int id) const;                    // 0x402770
    bool charsIdle() const;                         // 0x409d30: no character changing
    // C's "second branch in progress" flag (0xbc6638).
    bool cBusyFlag = false;

    // ---- Tone (F, FB) ----
    void tone(int mode, int fin, int fout, int r, int g, int b);  // 0x401de0
    bool toneActive() const { return toneOn_; }                 // 0x401e60

    // ---- Colour settings: applied when pictures load ----
    int timeMode = 0;           // 0x5218ac SetTimeMode
    int bgLevel = 0x100;        // 0x521910
    int charLevel = 0x100;      // 0x521914
    std::string bgEffect;       // 0x5218d0
    std::string charEffect;     // 0x5218f0
    void setChroma(int level, int which);                   // 0x401880
    void setEffect(const std::string& name, int which);     // 0x4018c0

    // The shake's display offset, scale and rotation (0x40c780's output: 0xa391b0, 0xa391b8, 0x4be338,
    // 0xa391c0), applied to the display objects outside transitions.
    float shakeX = 0, shakeY = 0, shakeScale = 1, shakeRotation = 0;
    // The message window is on screen (0x4d2d00).
    bool windowShown = false;

    int lastKind() const { return lastKind_; }        // 0x521740: 0 background, 1 CG, 2 H
    int currentBuffer() const { return cur_; }        // 0x5216d8
    bool keepsCharacters() const { return keepChars_ != 0; }  // 0x521748
    const std::array<int, 3>& tone() const { return toneCur_; }  // 0x52187c..
    // The characters' first colour table (0x415920): the background's own .amp if it exists, else the
    // time of day's.
    std::string characterAmp() const;
    // Called when a transition ends (the weather goes with a B-kind change).
    std::function<void(bool keepChars)> onTransitionEnd;
    int windowFade() const { return windowFade_; }    // 0x521744
    // Saves: the background, the characters, the colour settings and the tone as 0x4234c0 / 0x401e70
    // keep them; the restore (0x423d10's first part) sets the characters again (mode 3) and shows the
    // background again without a transition.
    void capture(SaveRecord::Scene& s) const;
    void restore(const SaveRecord::Scene& s);
    int lastNumber() const { return lastNumber_; }    // 0x5216d4

private:
    struct Char {
        bool used = false;
        int mode = 0, state = 0, id = 0, pic = 0;
        bool samePic = false;
        int z = 0;
        bool posFloat = false, prevPosFloat = false;
        int posIndex = 0, prevPosIndex = 0;
        float pos = 0, prevPos = 0;
        int r = 0, g = 0, b = 0, prevR = 0, prevG = 0, prevB = 0;
        int alpha = 0, prevAlpha = 0;
        int w = 0, h = 0, prevW = 0, prevH = 0;
        int k = 0;            // which of the two pictures is current
        int counter = 0, duration = 0;
        bool releaseOld = false;  // +0x38
    };
    struct Buffer {
        // Source offset (pan): resting/start and target, ints; zoom, rotation floats.
        int panX = 0, panY = 0, panTargetX = 0, panTargetY = 0;
        bool panOn = false;
        int panEase = 0, panCounter = 0, panDuration = 0;
        float zoomX = 1, zoomY = 1, zoomTargetX = 1, zoomTargetY = 1;
        bool zoomOn = false;
        int zoomEase = 0, zoomCounter = 0, zoomDuration = 0;
        int pivotX = 640, pivotY = 360;
        float rot = 0, rotTarget = 0;
        bool rotOn = false;
        int rotEase = 0, rotCounter = 0, rotDuration = 0;
    };

    int charIndex(int id) const;  // 0x402220: 8 if none
    void charLoad(int i, int prioExtra, int& w, int& h, bool toggle);  // 0x415920
    void charRelease(int i, bool oldOnly);                            // 0x402580
    void charsAtEnd();                                                // 0x4026c0
    void charsToNewBuffer();                                          // 0x4163c0
    float charX(int w, int i, bool previous) const;                   // 0x402270
    void cameraStep();
    void fadeOut(int trans, int frames);                              // 0x40b820

    Game& game_;
    Gfx& gfx_;
    std::array<Char, 8> chars_;
    std::array<Buffer, 2> buf_;
    int cur_ = 0;                // 0x5216d8
    bool shown_ = false;         // 0x5216d0
    int lastNumber_ = 0;         // 0x5216d4
    int lastKind_ = 0;           // 0x521740
    int windowFade_ = 0;         // 0x521744
    int keepChars_ = 0;          // 0x521748
    int transState_ = 0;         // 0x521784
    int transType_ = 0;          // 0x521788
    int transCounter_ = 0;       // 0x52178c
    int transDuration_ = 0;      // 0x521790
    int transSoft_ = 0;          // 0x521860
    int picW_ = 0, picH_ = 0;    // 0x52170c / 0x521710
    int srcW_ = 1280, srcH_ = 720;  // 0x5217e0 / 0x5217e4
    std::string bgAmp_;          // 0x5218b0
    bool cameraPaused_ = false;  // 0x5217ac
    int pauseLength_ = 0, pauseCounter_ = 0;
    // Tone: mode (1 = F's first half), durations, counter, start / target / current R, G, B.
    bool toneOn_ = false;
    int toneMode_ = 0, toneIn_ = 0, toneOut_ = 0, toneCounter_ = 0;
    std::array<int, 3> toneStart_{128, 128, 128}, toneTarget_{128, 128, 128}, toneCur_{128, 128, 128};
};

}  // namespace nyan::wa2
