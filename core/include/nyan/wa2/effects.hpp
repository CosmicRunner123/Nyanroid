#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "nyan/wa2/save.hpp"

namespace nyan::wa2 {

class Game;
class Gfx;

// Shakes (WA2.exe 0x401d10, 0x40c780, 0x40c730), weather particles (0x40fd20, 0x406e00.., 0x407900),
// free pictures (0x424500..0x4254d0, 0x424c30) and the date card (0x401320, 0x40b380).
class Effects {
public:
    explicit Effects(Game& game);

    // ---- Shake ----
    void shake(int typeFlags, int amp, int dur, int dir, int speed);  // 0x401d10
    void stopShake();                                                 // 0x40c730
    bool shaking() const { return shakeOn_ && shakeDur_ != 0; }       // 0x401d90: SetShake waits
    void endMessageShake();                                           // 0x40e6f0
    void shakeStep();                                                 // 0x40c780
    // The window's share of the shake (0xa391b4, 0xa391bc, 0x4be33c, 0xa391c4).
    float winX = 0, winY = 0, winZoom = 1, winAngle = 0;
    bool windowShakeOn() const { return shakeOn_; }

    // ---- Weather ----
    void weather(int typeFlags, int wind, int fall, int margin, int count, int layer, int mode);  // 0x40fd20
    void changeWeather(int wind, int fall, int count, int layer, int mode, int margin);           // 0x406e90..
    void resetWeather(bool full);                                                                  // 0x406e00
    bool weatherGone() const;                                                                      // 0x406f60
    bool weatherFading() const { return weatherFade_; }
    bool weatherOn() const { return weatherOn_; }  // 0x522930
    void weatherStep();                                                                            // 0x407900
    // At the end of a background transition of the B kind (0x40b8d0): weather inside the scene
    // buffers goes with the old picture.
    void transitionEnded(bool keepChars);

    // ---- Free pictures ----
    void loadPicture(int slot, const std::string& file, int layer, int colourMode, const std::string& amp, int level,
                     const std::string& group, int fixed);  // 0x424f90
    void loadAnime(int slot, const std::string& file, int layer, const std::string& amp, int level, const std::string& group,
                   int fixed);  // 0x4252d0
    void releasePicture(int slot);                                   // 0x424500
    void pictureVisible(int slot, bool on);                          // 0x424620
    void pictureLayer(int slot, int layer);                          // 0x424660
    void pictureParam(int slot, int blend, int alpha, int frames);   // 0x4246b0
    void pictureMirror(int slot, int flags);                         // 0x424760
    void pictureBright(int slot, int r, int g, int b);               // 0x4247a0
    void pictureMove(int slot, int x, int y);                        // 0x424810
    void picturePos(int slot, int x, int y, int sx, int sy, int w, int h);  // 0x4248c0
    void pictureZoom(int slot, int x, int y, int w, int h);          // 0x4249e0
    void pictureZoom2(int slot, int cx, int cy, float zoom);         // 0x424ab0
    void pictureRoll(int slot, int cx, int cy, int a4096);           // 0x424b60
    void animePlay(int slot, int seq, int play, int mode);           // 0x4245d0
    void picturesStep();                                             // 0x424c30

    // ---- The date card ----
    void calendar(int year, int month, int day, int weekday, int show);  // 0x401320
    bool calendarShown() const { return calPhase_ >= 0 && calPhase_ < 5; }
    void calendarStep();                                                 // 0x40b380

    // Back to the title or a new story (0x413ac0's part).
    void reset();

    // ---- Saves (0x4234c0 / 0x423d10) ----
    // An endless shake only ([0..5]); the weather ([0x3b5..]); the free pictures' records (0xa71b90);
    // the date card's date.
    void captureShake(SaveRecord::Shake& s) const;
    void captureWeather(SaveRecord::Weather& w) const;
    void capturePictures(std::array<SaveRecord::Picture, 32>& out) const;
    void captureCalendar(int& year, int& month, int& day, int& weekday) const;
    // 0x425640: a picture loaded again from its record and set as it was.
    void restorePicture(int slot, const SaveRecord::Picture& p);
    int calendarMonth() const { return month_; }  // 0x5216bc
    int calendarDay() const { return day_; }      // 0x5216c0

private:
    struct Particle {
        bool active = false;
        int k = 0, sprite = 0, phase = 0;
        float x = 0, y = 0, z = 0, sx = 0, sy = 0, x0 = 0, y0 = 0;
    };
    struct Picture {
        bool used = false, disp = false;
        int kind = 0;  // 0 picture, 1 anime
        int blend = 1, mirror = 0, alpha = 0xff, target = 0xff, start = 0xff, counter = 0, frames = 0;
        int seq = 0, mode = 0, layer = 0, fixed = 0;
        int x = 0, y = 0, w = 0, h = 0, sx = 0, sy = 0, fullW = 0, fullH = 0;
        int r = 0x80, g = 0x80, b = 0x80;
        float zoom = 1, angle = 0;
        int cx = 0, cy = 0;
        // What the record keeps of the load (0xa71b90: the file, the colour mode, amp, group, level).
        std::string file, amp, group;
        int colourMode = 0, level = 0x100;
    };
    void placeShake(float x, float y, float zoom, float angle, int flags);
    void weatherSpawn(int i, bool prefill);   // per type
    void snowFlake(int i, bool prefill);      // 0x4071e0
    void burstParticle(int i, bool prefill, bool inward);  // 0x4075f0
    void retire(int i);
    // A picture's two objects (main 0x11+slot, twin 0x31+slot unless fixed).
    template <typename F>
    void bothObjects(int slot, F f);

    Game& game_;
    Gfx& gfx_;
    // Shake.
    bool shakeOn_ = false;
    int shakeType_ = 0, shakeAmp_ = 0, shakeDur_ = 0, shakeDir_ = 0, shakeSpeed_ = 0, shakeCount_ = 0;
    bool joltToggle_ = false;  // 0xa39248, never reset
    // Weather.
    bool weatherOn_ = false;
    int weatherType_ = 0, weatherFlags_ = 0, weatherLayer_ = 0, weatherMode_ = 0;
    float wind_ = 0, fall_ = 0;
    int windTarget_ = 0, fallTarget_ = 0;
    int count_ = 0, live_ = 0, frame_ = 0;
    int margin_ = 0, marginTarget_ = 0;
    bool weatherFade_ = false, prefillDone_ = false;
    std::array<Particle, 400> particles_{};
    // Free pictures.
    std::array<Picture, 32> pictures_{};
    // Calendar.
    int calPhase_ = -1, calA_ = 0, calB_ = 0, calT_ = 0;
    int year_ = 0, month_ = 0, day_ = 0, weekday_ = 0;
};

}  // namespace nyan::wa2
