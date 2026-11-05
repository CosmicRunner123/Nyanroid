#pragma once

#include <cstdint>
#include <string>

namespace nyan::diesel {

// What the scripts' sounds ask of the audio device, as Mware's device calls carry it (Mware.dll
// CSound: play 0x1003fac0, stop, pause, resume), for a host that plays the audio. Each voice has its
// own key: a sound made by SQSound.create has one voice, one made by load has 10 that its plays use
// in turn (0x485b40 / 0x4e2920).
struct SoundEvent {
    // Loop: a playing voice's plays and loop changed (SQSound.setLoop: the exe hands the device the
    // sound's handle with them at once, 0x4e2cc0).
    enum class Kind { Play, Stop, Pause, Resume, Loop };
    Kind kind = Kind::Play;
    uint32_t voice = 0;
    // Play: the Ogg file (a script path), from `startMs`, played `plays` times (-1: for ever; 0 and 1
    // once), the
    // later ones over [loopStartMs, loopEndMs) (0, 0: the whole file; the end beyond the file: its
    // end). The sound type (SOUND_TYPE_*: 0 BGM, 1 voice, 2 SE, 3 system, 4 movie, 5 BGV; -1 none)
    // only picks the host's channel group.
    std::string file;
    int32_t type = -1;
    int32_t plays = 1, loopStartMs = 0, loopEndMs = 0;
    uint32_t startMs = 0;
    // The player's reverb preset as the play applies it (Mware 0x10032bd0 on each start: an I3DL2
    // preset, 1 .. 29, for XAudio2's reverb, its effect 2; 0 leaves it off).
    int32_t reverbPreset = 0;
    // Whose reverb runs it: XAudio2 2.9's or 2.7's (GameData::xaudioVersion), 2.9's x86-64
    // build for a 64-bit Mware (GameData::xaudio64).
    int32_t xaudioVersion = 29;
    bool xaudio64 = false;
    // A movie's sound (Mware's CMovieMediaFoundation plays it on an XAudio2 voice of its own): the
    // file is the movie (ASF, Windows Media Audio), played from startMs, `plays` times (0: until
    // stopped) over the whole of it.
    bool movie = false;
};

// A playing voice's device settings now: XAudio2's volume (an amplitude), the pan (-1 .. 1, through
// Mware's output matrix 0x100325b0) and the frequency ratio.
struct SoundVoice {
    uint32_t voice = 0;
    int32_t type = -1;
    float gain = 1.0f;
    float pan = 0.0f;
    float frequency = 1.0f;
    bool paused = false;
    // The player's echo (XAudio2's FXEcho, its effect 1: Mware 0x10032ae0): wet / dry mix and
    // feedback 0 .. 1, the delay in ms (1 .. 2000); a mix of 0 turns it off.
    float echoWet = 0.0f, echoFeedback = 0.0f, echoDelayMs = 1.0f;
};

}  // namespace nyan::diesel
