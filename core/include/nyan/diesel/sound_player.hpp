#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/diesel/sound_events.hpp"

namespace nyan::audio {
class Mixer;
}

namespace nyan::diesel {

// Plays a Diesel game's sounds through the mixer as Mware's XAudio2 voices would: each voice an Ogg
// stream on a channel of the bus its sound type picks, decoded and looped here (its plays, its loop
// segment), resampled for its frequency ratio, panned through Mware's output matrix (0x100325b0) and
// at the amplitude the device was given. A movie's sound (SoundEvent::movie) is the movie file's
// Windows Media Audio, decoded by FFmpeg (nyan::media::FfmpegVideo) from the file opened with
// `open`. The mixer's buses keep the platform's volumes.
class SoundPlayer {
public:
    using ReadFile = std::function<std::optional<Bytes>(const std::string&)>;
    using OpenFile = std::function<std::shared_ptr<RandomAccessFile>(const std::string&)>;
    SoundPlayer(audio::Mixer* mixer, ReadFile read, OpenFile open = nullptr);
    ~SoundPlayer();
    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

    // The device calls of a frame, then the settings of the voices playing after it.
    void apply(const std::vector<SoundEvent>& events, const std::vector<SoundVoice>& voices);
    void stopAll();

    struct VoiceState;

private:
    struct Playing {
        std::shared_ptr<VoiceState> state;
        int bus = 0, channel = 0;
        uint64_t started = 0;
    };
    std::shared_ptr<const Bytes> file(const std::string& path);
    void start(const SoundEvent& e);
    void stop(uint32_t voice);

    audio::Mixer* mixer_;
    ReadFile read_;
    OpenFile open_;
    std::map<uint32_t, Playing> playing_;
    std::map<std::string, std::shared_ptr<const Bytes>> files_;
    std::vector<std::string> fileOrder_;  // least recently loaded first
    uint64_t starts_ = 0;
};

}  // namespace nyan::diesel
