#pragma once

#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::audio {

// Software mixer for Ogg Vorbis tracks. The platform audio callback pulls
// interleaved stereo float samples with render(); game logic calls the other
// methods from the main thread. All methods are thread-safe.
class Mixer {
public:
    enum class Bus : uint8_t { Bgm, Se, Voice, Movie };
    static constexpr int kChannelsPerBus = 16;  // sscript keeps 10 SE slots (`se 8 loop ...`)

    explicit Mixer(int outputRate = 48000);
    ~Mixer();

    // Starts `ogg` on a bus channel, replacing what was playing there. With `crossfadeMs` the old
    // sound fades out over that time while the new one fades in.
    void play(Bus bus, int channel, Bytes ogg, bool loop, uint32_t fadeInMs = 0, float volume = 1.0f,
              uint32_t crossfadeMs = 0);
    // Starts a decoded stream on a channel: `pull` fills up to n interleaved stereo frames at
    // `rate` and returns how many (0: finished). Called on the audio thread.
    void playStream(Bus bus, int channel, int rate, std::function<int(float*, int)> pull, float volume = 1.0f);
    // Stops a channel (channel < 0: every channel on the bus), optionally fading out.
    void stop(Bus bus, int channel, uint32_t fadeOutMs = 0);
    // Fades a channel's volume (0..1) over `ms`, starting from `from` if it is >= 0.
    // Sounds that are already fading out to stop are left alone.
    void setVolume(Bus bus, int channel, float volume, uint32_t ms = 0, float from = -1);
    // A second factor on a channel, apart from its volume: the engine's level for the sound (its
    // sound part's settings, ducking, skipping), faded over `ms`. Each new sound starts at 1.
    void setGain(Bus bus, int channel, float gain, uint32_t ms = 0);
    // Stereo pan of a channel, -100 (left) .. 100 (right), as the engine's DirectSound pan
    // (grisaia.exe 0x4a2ca0): the far side plays at (100 - |pan|)%, the near side at full volume.
    void setPan(Bus bus, int channel, int pan);
    // Holds a channel (channel < 0: every channel on the bus) where it is, silent, or lets it go on.
    void setPaused(Bus bus, int channel, bool paused);
    // Master volume per bus (user settings), 0..1.
    void setBusVolume(Bus bus, float volume);
    bool isPlaying(Bus bus, int channel) const;

    // Mixes `frames` stereo frames into `out` (overwrites).
    void render(float* out, size_t frames);
    int outputRate() const { return rate_; }

private:
    struct Track;
    void fadeTo(Track& t, float target, uint32_t ms, bool stopAtEnd);

    int rate_;
    mutable std::mutex mu_;
    std::array<std::array<std::unique_ptr<Track>, kChannelsPerBus>, 4> tracks_;
    // Sounds a crossfade took off their channel, fading out until they stop.
    std::array<std::vector<std::pair<int, std::unique_ptr<Track>>>, 4> leaving_;
    bool mixTrack(Track& t, float* out, size_t frames, float busVol);
    std::array<float, 4> busVolume_{1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<float> scratch_;
};

}  // namespace nyan::audio
