#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "nyan/wa2/assets.hpp"

namespace nyan::audio {
class Mixer;
}

namespace nyan::wa2 {

// The engine's sound objects as its DirectSound layer keeps them (WA2.exe: manager 0x45f440, objects
// 0x45d9a0, Ogg streams 0x45fad0 / 0x45e630, static WAV buffers 0x45dcc0, start 0x4606f0, status
// 0x45f920, volume 0x45f990). The game-level code (BGM, SE and voice channels, fades) sets each
// object's 8-bit level once per frame; the level is a linear gain of level / 255.
//
// Sounds are mixed here into one stream per mixer bus, so any number can play at once. Without a
// mixer (headless runs, captures) they advance with the game's frames instead, so waits on them end
// as they would.
//
// The engine streams its Ogg files (a 2 s ring refilled by a thread), so starting one costs it
// little. With a mixer, an Ogg entry of 1 MB or more is streamed here too: the caller reads its first
// 256 KB and last 64 KB, which is all the decoder's start and its length need, so rate, channels,
// length and the play-once stop are known at once and are the same as from the whole file; a loader
// thread reads the rest while it plays, and the decoder never reads past what has arrived.
class SoundOut {
public:
    enum class Kind { Bgm, Se, Voice };

    SoundOut(const Assets& assets, audio::Mixer* mixer);
    ~SoundOut();
    SoundOut(const SoundOut&) = delete;
    SoundOut& operator=(const SoundOut&) = delete;

    // The entry of `name` in sound archive `archive` (the registration order: 0 BGM, 1 Se, 2 Voice,
    // 3..5 their IC copies); -1 if none. The engine's binary search (0x458620) with its comparator
    // (0x433d10: ASCII letters fold) finds every entry of these sorted tables, so a case-insensitive
    // lookup is equivalent.
    int find(int archive, const std::string& name) const;

    // Opens and starts an entry (0x460d40 + 0x4606f0): `count` 0 loops forever, 1 plays once, n plays
    // n times. An Ogg entry whose name ends in `_A.xxx` continues with its `_B` entry when it loops
    // (0x45fad0, end of data 0x45e969): played once, only `_A` sounds. Returns a handle, -1 if the
    // entry cannot be read.
    int play(Kind kind, int archive, int entry, int count, int level);
    // The two halves of play: open an entry without starting it (the SE preload, 0x405710), and
    // start a loaded object (0x4056d0 -> 0x4606f0).
    int load(Kind kind, int archive, int entry);
    void start(int handle, int count, int level);
    void setLevel(int handle, int level);
    void stop(int handle);
    // Whether the object still plays (GetStatus, 0x45f920).
    bool playing(int handle) const;

    // One game frame without a mixer: every sound moves on by 1/60 s.
    void advanceFrame();

    // Tests: false reads every entry whole, as before streaming; whether any file is still loading.
    bool streamLarge = true;
    bool loading() const;

private:
    struct Object;
    struct Stream;
    void mix(Kind kind, float* out, int frames);
    std::shared_ptr<Object> open(Kind kind, int archive, int entry);
    // The bytes of an entry: whole, or (streamed) the start and the end now and the rest queued.
    std::shared_ptr<Stream> readEntry(const LacArchive& a, const LacEntry& e, bool head);
    void queue(const std::shared_ptr<Stream>& s);  // hands a stream's missing bytes to the loader
    void loaderLoop();
    void fillStream(Stream& s);

    const Assets& assets_;
    audio::Mixer* mixer_;
    int outputRate_ = 48000;
    mutable std::mutex mu_;
    std::map<int, std::shared_ptr<Object>> objects_;
    int nextHandle_ = 0;

    // The loader thread and its queue (with a mixer only).
    std::thread loader_;
    mutable std::mutex loadMu_;
    std::condition_variable loadCv_;
    std::deque<std::shared_ptr<Stream>> jobs_;
    std::atomic<bool> quit_{false};
};

}  // namespace nyan::wa2
