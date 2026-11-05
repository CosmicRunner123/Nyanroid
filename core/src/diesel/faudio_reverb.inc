// The reverb of Mware's voices: XAudio2's AudioReverb effect, which Mware turns on for a voice with
// one of the I3DL2 presets, as FAudio re-implements it. Ported to C++ from FAudio 26.11
// (https://github.com/FNA-XNA/FAudio): the DSP of src/FAudioFX_reverb.c (pre-delay, the early
// reflections' all-pass, per channel a delay, eight shelving comb filters, four all-pass filters
// and the room filter, then the wet / dry mix) and ReverbConvertI3DL2ToNative with the I3DL2 presets
// of include/FAudioFX.h.
//
// This is an altered version of FAudio's code, not the original: only the mono and stereo paths
// are kept, the FAPO interface is replaced by the Reverb class below, and memory comes from
// std::vector. FAudio's licence:
//
//   FAudio - XAudio Reimplementation for FNA
//
//   Copyright (c) 2011-2024 Ethan Lee, Luigi Auriemma, and the MonoGame Team
//
//   This software is provided 'as-is', without any express or implied warranty.
//   In no event will the authors be held liable for any damages arising from
//   the use of this software.
//
//   Permission is granted to anyone to use this software for any purpose,
//   including commercial applications, and to alter it and redistribute it
//   freely, subject to the following restrictions:
//
//   1. The origin of this software must not be misrepresented; you must not
//   claim that you wrote the original software. If you use this software in a
//   product, an acknowledgment in the product documentation would be
//   appreciated but is not required.
//
//   2. Altered source versions must be plainly marked as such, and must not be
//   misrepresented as being the original software.
//
//   3. This notice may not be removed or altered from any source distribution.
//
//   Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
//
// Included by sound_player.cpp (after <algorithm>, <cmath>, <cstdint>, <cstring> and <vector>) when
// the private companion's port of XAudio2's own DSP is not there (see core/CMakeLists.txt); both
// offer the same interface.

namespace xa2reverb {

namespace detail {

inline float dbGainToFactor(float gain) { return static_cast<float>(std::pow(10.0, static_cast<double>(gain / 20.0f))); }

inline uint32_t msToSamples(float msec, int32_t sampleRate) {
    return static_cast<uint32_t>((static_cast<float>(sampleRate) * msec) / 1000.0f);
}

constexpr float kMaxDelayMs = 300.0f;

// A delay line of up to 300 ms.
struct Delay {
    int32_t sampleRate = 0;
    uint32_t capacity = 0, delay = 0, readIdx = 0, writeIdx = 0;
    std::vector<float> buffer;

    void init(int32_t rate, float delayMs) {
        sampleRate = rate;
        capacity = msToSamples(kMaxDelayMs, rate);
        delay = msToSamples(delayMs, rate);
        readIdx = 0;
        writeIdx = delay;
        buffer.assign(capacity, 0.0f);
    }
    void change(float delayMs) {
        delay = msToSamples(delayMs, sampleRate);
        readIdx = (writeIdx - delay + capacity) % capacity;
    }
    float read() {
        float out = buffer[readIdx];
        readIdx = (readIdx + 1) % capacity;
        return out;
    }
    void write(float sample) {
        buffer[writeIdx] = sample;
        writeIdx = (writeIdx + 1) % capacity;
    }
    float process(float in) {
        float out = read();
        write(in);
        return out;
    }
};

inline float combFeedbackFromRT60(const Delay& d, float rt60Ms) {
    float exponent = (-3.0f * static_cast<float>(d.delay) * 1000.0f) / (static_cast<float>(d.sampleRate) * rt60Ms);
    return static_cast<float>(std::pow(10.0, static_cast<double>(exponent)));
}

enum class Shelf { Low, High };

// A first-order shelving filter (FAudio's bi-quad in its shelving form).
struct BiQuad {
    int32_t sampleRate = 0;
    float a0 = 0, a1 = 0, a2 = 0, b1 = 0, b2 = 0, c0 = 0, d0 = 0, delay0 = 0, delay1 = 0;

    void change(Shelf type, float frequency, float gain) {
        const float twoPi = 6.283185307179586476925286766559005f;
        float thetaC = (twoPi * frequency) / static_cast<float>(sampleRate);
        float mu = dbGainToFactor(gain);
        float beta = type == Shelf::Low ? 4.0f / (1 + mu) : (1 + mu) / 4.0f;
        float delta = beta * static_cast<float>(std::tan(static_cast<double>(thetaC * 0.5f)));
        float gamma = (1 - delta) / (1 + delta);
        if (type == Shelf::Low) {
            a0 = (1 - gamma) * 0.5f;
            a1 = a0;
        } else {
            a0 = (1 + gamma) * 0.5f;
            a1 = -a0;
        }
        a2 = 0.0f;
        b1 = -gamma;
        b2 = 0.0f;
        c0 = mu - 1.0f;
        d0 = 1.0f;
    }
    void init(int32_t rate, Shelf type, float frequency, float gain) {
        sampleRate = rate;
        delay0 = delay1 = 0.0f;
        change(type, frequency, gain);
    }
    float process(float in) {
        // Direct form II transposed.
        float result = (a0 * in) + delay0;
        delay0 = (a1 * in) - (b1 * result) + delay1;
        delay1 = (a2 * in) - (b2 * result);
        return (result * c0) + (in * d0);
    }
};

// A comb filter whose feedback goes through a high and a low shelf.
struct CombShelving {
    Delay comb;
    float feedbackGain = 0;
    BiQuad lowShelving, highShelving;

    void init(int32_t rate, float delayMs, float rt60Ms, float lowFrequency, float lowGain, float highFrequency,
              float highGain) {
        comb.init(rate, delayMs);
        feedbackGain = combFeedbackFromRT60(comb, rt60Ms);
        lowShelving.init(rate, Shelf::Low, lowFrequency, lowGain);
        highShelving.init(rate, Shelf::High, highFrequency, highGain);
    }
    float process(float in) {
        float delayOut = comb.read();
        float feedback = highShelving.process(delayOut);
        feedback = lowShelving.process(feedback);
        comb.write(in + (feedbackGain * feedback));
        return delayOut;
    }
};

// A delaying all-pass filter.
struct AllPass {
    Delay delay;
    float feedbackGain = 0;

    void init(int32_t rate, float delayMs, float gain) {
        delay.init(rate, delayMs);
        feedbackGain = gain;
    }
    void change(float delayMs, float gain) {
        delay.change(delayMs);
        feedbackGain = gain;
    }
    float process(float in) {
        float delayOut = delay.read();
        float toBuf = in + (feedbackGain * delayOut);
        delay.write(toBuf);
        return delayOut - (feedbackGain * toBuf);
    }
};

constexpr int kCombCount = 8, kApfInCount = 1, kApfOutCount = 4;
constexpr float kCombDelays[kCombCount] = {25.31f, 26.94f, 28.96f, 30.75f, 32.24f, 33.80f, 35.31f, 36.67f};
constexpr float kApfInDelays[kApfInCount] = {13.28f};
constexpr float kApfOutDelays[kApfOutCount] = {5.10f, 12.61f, 10.0f, 7.73f};
constexpr float kMinDecayTime = 0.1f;

// Mono and stereo only: the one channel is the centre, of two the second is the right (delayed a
// little for the spread).
inline float stereoSpreadDelayMs(int32_t channels, int32_t channel) { return channels == 2 && channel == 1 ? 0.5216f : 0.0f; }

}  // namespace detail

// The I3DL2 presets Mware picks by number (XAudio2's XAUDIO2FX_I3DL2_PRESET_* in their usual order,
// DEFAULT to PLATE), as FAudio defines them: WetDryMix, Room, RoomHF, RoomRolloffFactor, DecayTime,
// DecayHFRatio, Reflections, ReflectionsDelay, Reverb, ReverbDelay, Diffusion, Density, HFReference.
struct I3DL2 {
    float wetDryMix;
    int32_t room, roomHF;
    float roomRolloffFactor, decayTime, decayHFRatio;
    int32_t reflections;
    float reflectionsDelay;
    int32_t reverb;
    float reverbDelay, diffusion, density, hfReference;
};
constexpr int kPresetCount = 30;
constexpr I3DL2 kI3DL2Presets[kPresetCount] = {
    {100, -10000, 0, 0.0f, 1.00f, 0.50f, -10000, 0.020f, -10000, 0.040f, 100.0f, 100.0f, 5000.0f},  // DEFAULT
    {100, -1000, -100, 0.0f, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100.0f, 100.0f, 5000.0f},    // GENERIC
    {100, -1000, -6000, 0.0f, 0.17f, 0.10f, -1204, 0.001f, 207, 0.002f, 100.0f, 100.0f, 5000.0f},   // PADDEDCELL
    {100, -1000, -454, 0.0f, 0.40f, 0.83f, -1646, 0.002f, 53, 0.003f, 100.0f, 100.0f, 5000.0f},     // ROOM
    {100, -1000, -1200, 0.0f, 1.49f, 0.54f, -370, 0.007f, 1030, 0.011f, 100.0f, 60.0f, 5000.0f},    // BATHROOM
    {100, -1000, -6000, 0.0f, 0.50f, 0.10f, -1376, 0.003f, -1104, 0.004f, 100.0f, 100.0f, 5000.0f}, // LIVINGROOM
    {100, -1000, -300, 0.0f, 2.31f, 0.64f, -711, 0.012f, 83, 0.017f, 100.0f, 100.0f, 5000.0f},      // STONEROOM
    {100, -1000, -476, 0.0f, 4.32f, 0.59f, -789, 0.020f, -289, 0.030f, 100.0f, 100.0f, 5000.0f},    // AUDITORIUM
    {100, -1000, -500, 0.0f, 3.92f, 0.70f, -1230, 0.020f, -2, 0.029f, 100.0f, 100.0f, 5000.0f},     // CONCERTHALL
    {100, -1000, 0, 0.0f, 2.91f, 1.30f, -602, 0.015f, -302, 0.022f, 100.0f, 100.0f, 5000.0f},       // CAVE
    {100, -1000, -698, 0.0f, 7.24f, 0.33f, -1166, 0.020f, 16, 0.030f, 100.0f, 100.0f, 5000.0f},     // ARENA
    {100, -1000, -1000, 0.0f, 10.05f, 0.23f, -602, 0.020f, 198, 0.030f, 100.0f, 100.0f, 5000.0f},   // HANGAR
    {100, -1000, -4000, 0.0f, 0.30f, 0.10f, -1831, 0.002f, -1630, 0.030f, 100.0f, 100.0f, 5000.0f}, // CARPETEDHALLWAY
    {100, -1000, -300, 0.0f, 1.49f, 0.59f, -1219, 0.007f, 441, 0.011f, 100.0f, 100.0f, 5000.0f},    // HALLWAY
    {100, -1000, -237, 0.0f, 2.70f, 0.79f, -1214, 0.013f, 395, 0.020f, 100.0f, 100.0f, 5000.0f},    // STONECORRIDOR
    {100, -1000, -270, 0.0f, 1.49f, 0.86f, -1204, 0.007f, -4, 0.011f, 100.0f, 100.0f, 5000.0f},     // ALLEY
    {100, -1000, -3300, 0.0f, 1.49f, 0.54f, -2560, 0.162f, -613, 0.088f, 79.0f, 100.0f, 5000.0f},   // FOREST
    {100, -1000, -800, 0.0f, 1.49f, 0.67f, -2273, 0.007f, -2217, 0.011f, 50.0f, 100.0f, 5000.0f},   // CITY
    {100, -1000, -2500, 0.0f, 1.49f, 0.21f, -2780, 0.300f, -2014, 0.100f, 27.0f, 100.0f, 5000.0f},  // MOUNTAINS
    {100, -1000, -1000, 0.0f, 1.49f, 0.83f, -10000, 0.061f, 500, 0.025f, 100.0f, 100.0f, 5000.0f},  // QUARRY
    {100, -1000, -2000, 0.0f, 1.49f, 0.50f, -2466, 0.179f, -2514, 0.100f, 21.0f, 100.0f, 5000.0f},  // PLAIN
    {100, -1000, 0, 0.0f, 1.65f, 1.50f, -1363, 0.008f, -1153, 0.012f, 100.0f, 100.0f, 5000.0f},     // PARKINGLOT
    {100, -1000, -1000, 0.0f, 2.81f, 0.14f, 429, 0.014f, 648, 0.021f, 80.0f, 60.0f, 5000.0f},       // SEWERPIPE
    {100, -1000, -4000, 0.0f, 1.49f, 0.10f, -449, 0.007f, 1700, 0.011f, 100.0f, 100.0f, 5000.0f},   // UNDERWATER
    {100, -1000, -600, 0.0f, 1.10f, 0.83f, -400, 0.005f, 500, 0.010f, 100.0f, 100.0f, 5000.0f},     // SMALLROOM
    {100, -1000, -600, 0.0f, 1.30f, 0.83f, -1000, 0.010f, -200, 0.020f, 100.0f, 100.0f, 5000.0f},   // MEDIUMROOM
    {100, -1000, -600, 0.0f, 1.50f, 0.83f, -1600, 0.020f, -1000, 0.040f, 100.0f, 100.0f, 5000.0f},  // LARGEROOM
    {100, -1000, -600, 0.0f, 1.80f, 0.70f, -1300, 0.015f, -800, 0.030f, 100.0f, 100.0f, 5000.0f},   // MEDIUMHALL
    {100, -1000, -600, 0.0f, 1.80f, 0.70f, -2000, 0.030f, -1400, 0.060f, 100.0f, 100.0f, 5000.0f},  // LARGEHALL
    {100, -1000, -200, 0.0f, 1.30f, 0.90f, 0, 0.002f, 0, 0.010f, 100.0f, 75.0f, 5000.0f},           // PLATE
};

// The effect's parameters (XAUDIO2FX_REVERB_PARAMETERS in the layout Mware passes, which has no
// SideDelay).
struct Native {
    float wetDryMix = 0.0f;
    uint32_t reflectionsDelay = 0;
    uint8_t reverbDelay = 0, rearDelay = 0, positionLeft = 0, positionRight = 0, positionMatrixLeft = 0,
            positionMatrixRight = 0, earlyDiffusion = 0, lateDiffusion = 0, lowEQGain = 0, lowEQCutoff = 0,
            highEQGain = 0, highEQCutoff = 0;
    float roomFilterFreq = 0.0f, roomFilterMain = 0.0f, roomFilterHF = 0.0f, reflectionsGain = 0.0f,
          reverbGain = 0.0f, decayTime = 0.0f, density = 0.0f, roomSize = 0.0f;
    int32_t disableLateField = 0;
};

// A preset as the effect's parameters (ReverbConvertI3DL2ToNative).
inline Native convertPreset(int preset) {
    const I3DL2& p = kI3DL2Presets[preset];
    Native n;
    n.rearDelay = 5, n.positionLeft = 6, n.positionRight = 6, n.positionMatrixLeft = 27, n.positionMatrixRight = 27;
    n.roomSize = 100.0f, n.lowEQCutoff = 4, n.highEQCutoff = 6;
    n.roomFilterMain = static_cast<float>(p.room) / 100.0f;
    n.roomFilterHF = static_cast<float>(p.roomHF) / 100.0f;
    if (p.decayHFRatio >= 1.0f) {
        int32_t index = static_cast<int32_t>(-4.0 * std::log10(static_cast<double>(p.decayHFRatio)));
        if (index < -8) index = -8;
        n.lowEQGain = static_cast<uint8_t>(index < 0 ? index + 8 : 8);
        n.highEQGain = 8;
        n.decayTime = p.decayTime * p.decayHFRatio;
    } else {
        int32_t index = static_cast<int32_t>(4.0 * std::log10(static_cast<double>(p.decayHFRatio)));
        if (index < -8) index = -8;
        n.lowEQGain = 8;
        n.highEQGain = static_cast<uint8_t>(index < 0 ? index + 8 : 8);
        n.decayTime = p.decayTime;
    }
    float reflectionsDelay = p.reflectionsDelay * 1000.0f;
    if (reflectionsDelay >= 300.0f) reflectionsDelay = 299.0f;
    else if (reflectionsDelay <= 1.0f) reflectionsDelay = 1.0f;
    n.reflectionsDelay = static_cast<uint32_t>(reflectionsDelay);
    float reverbDelay = p.reverbDelay * 1000.0f;
    if (reverbDelay >= 85.0f) reverbDelay = 84.0f;
    n.reverbDelay = static_cast<uint8_t>(reverbDelay);
    n.reflectionsGain = static_cast<float>(p.reflections) / 100.0f;
    n.reverbGain = static_cast<float>(p.reverb) / 100.0f;
    n.earlyDiffusion = static_cast<uint8_t>(15.0f * p.diffusion / 100.0f);
    n.lateDiffusion = n.earlyDiffusion;
    n.density = p.density;
    n.roomFilterFreq = p.hfReference;
    n.wetDryMix = p.wetDryMix;
    return n;
}

// The effect on one voice, mono or stereo in and out (as Mware makes it). `version` and `x64` (which
// XAudio2 build the game uses) are taken for the interface's sake: FAudio's reverb is the same for
// all.
class Reverb {
public:
    Reverb(int channels, float rate, int version = 29, bool x64 = false) : channels_(channels == 1 ? 1 : 2) {
        (void)version;
        (void)x64;
        using namespace detail;
        const int32_t r = static_cast<int32_t>(rate);
        earlyDelay_.init(r, 10);
        for (int i = 0; i < kApfInCount; ++i) apfIn_[i].init(r, kApfInDelays[i], 0.5f);
        for (int c = 0; c < channels_; ++c) {
            Channel& ch = channel_[c];
            ch.reverbDelay.init(r, 10);
            for (int i = 0; i < kCombCount; ++i) {
                ch.comb[i].init(r, kCombDelays[i] + stereoSpreadDelayMs(channels_, c), 500, 500, -6, 5000, -6);
            }
            for (int i = 0; i < kApfOutCount; ++i) {
                ch.apfOut[i].init(r, kApfOutDelays[i] + stereoSpreadDelayMs(channels_, c), 0.5f);
            }
            ch.roomHighShelf.init(r, Shelf::High, 5000, -10);
            ch.gain = 1.0f;
        }
        // The effect's defaults until the first parameters.
        Native d;
        d.wetDryMix = 100.0f, d.reflectionsDelay = 5, d.reverbDelay = 5, d.rearDelay = 5;
        d.positionLeft = d.positionRight = 6, d.positionMatrixLeft = d.positionMatrixRight = 27;
        d.earlyDiffusion = d.lateDiffusion = 8, d.lowEQGain = 8, d.lowEQCutoff = 4, d.highEQGain = 8, d.highEQCutoff = 4;
        d.roomFilterFreq = 5000.0f, d.decayTime = 1.0f, d.density = 100.0f, d.roomSize = 100.0f;
        setParameters(d);
    }

    void setParameters(const Native& p) {
        using namespace detail;
        earlyDelay_.change(static_cast<float>(p.reflectionsDelay));
        const float earlyDiffusion = 0.6f - ((static_cast<float>(p.earlyDiffusion) / 15.0f) * 0.2f);
        for (int i = 0; i < kApfInCount; ++i) apfIn_[i].change(kApfInDelays[i], earlyDiffusion);
        for (int c = 0; c < channels_; ++c) {
            Channel& ch = channel_[c];
            ch.reverbDelay.change(static_cast<float>(p.reverbDelay));  // no rear channels here
            for (int i = 0; i < kCombCount; ++i) {
                CombShelving& comb = ch.comb[i];
                comb.comb.change(kCombDelays[i] + stereoSpreadDelayMs(channels_, c));
                comb.feedbackGain = combFeedbackFromRT60(comb.comb, std::max(p.decayTime, kMinDecayTime) * 1000.0f);
                comb.lowShelving.change(Shelf::Low, 50.0f + static_cast<float>(p.lowEQCutoff) * 50.0f,
                                        static_cast<float>(p.lowEQGain) - 8.0f);
                comb.highShelving.change(Shelf::High, 1000 + static_cast<float>(p.highEQCutoff) * 500.0f,
                                         static_cast<float>(p.highEQGain) - 8.0f);
            }
        }
        earlyGain_ = dbGainToFactor(p.reflectionsGain);
        reverbGain_ = dbGainToFactor(p.reverbGain);
        roomGain_ = dbGainToFactor(p.roomFilterMain);
        const float lateDiffusion = 0.6f - ((static_cast<float>(p.lateDiffusion) / 15.0f) * 0.2f);
        for (int c = 0; c < channels_; ++c) {
            Channel& ch = channel_[c];
            for (int i = 0; i < kApfOutCount; ++i) {
                ch.apfOut[i].change(kApfOutDelays[i] + stereoSpreadDelayMs(channels_, c), lateDiffusion);
            }
            ch.roomHighShelf.change(Shelf::High, p.roomFilterFreq, p.roomFilterMain + p.roomFilterHF);
            // Mono: the centre takes the mean of left and right; stereo: left, then right.
            const bool centre = channels_ == 1, right = channels_ == 2 && c == 1;
            float gain = centre ? (static_cast<float>(p.positionMatrixLeft) + static_cast<float>(p.positionMatrixRight)) / 2.0f
                         : right ? static_cast<float>(p.positionMatrixRight)
                                 : static_cast<float>(p.positionMatrixLeft);
            ch.gain = 1.5f - (gain / 27.0f) * 0.5f;
            gain = centre ? (static_cast<float>(p.positionLeft) + static_cast<float>(p.positionRight)) / 2.0f
                   : right ? static_cast<float>(p.positionRight)
                           : static_cast<float>(p.positionLeft);
            ch.earlyGain = 1.2f - (gain / 6.0f) * 0.2f;
            ch.earlyGain = ch.earlyGain * earlyGain_;
        }
        wetRatio_ = p.wetDryMix / 100.0f;
        dryRatio_ = 1.0f - wetRatio_;
    }

    // `frames` frames of the voice's channels, interleaved; off, the input passes through.
    void process(const float* in, float* out, uint32_t frames, bool enabled) {
        if (!enabled) {
            std::memcpy(out, in, sizeof(float) * frames * static_cast<size_t>(channels_));
            return;
        }
        if (channels_ == 1) {
            for (uint32_t k = 0; k < frames; ++k) {
                const float s = in[k];
                const float early = processEarly(s);
                const float late = processChannel(channel_[0], early);
                out[k] = (late * wetRatio_) + (s * dryRatio_);
            }
            return;
        }
        for (uint32_t k = 0; k < frames; ++k) {
            const float l = in[k * 2], r = in[k * 2 + 1];
            const float early = processEarly((l + r) / 2.0f);
            out[k * 2] = (processChannel(channel_[0], early) * wetRatio_) + l * dryRatio_;
            out[k * 2 + 1] = (processChannel(channel_[1], early) * wetRatio_) + r * dryRatio_;
        }
    }

private:
    struct Channel {
        detail::Delay reverbDelay;
        detail::CombShelving comb[detail::kCombCount];
        detail::AllPass apfOut[detail::kApfOutCount];
        detail::BiQuad roomHighShelf;
        float earlyGain = 0.0f, gain = 1.0f;
    };

    float processEarly(float in) {
        float early = earlyDelay_.process(in);
        for (auto& a : apfIn_) early = a.process(early);
        return early;
    }
    float processChannel(Channel& ch, float in) {
        const float revdelay = ch.reverbDelay.process(in);
        float out = 0.0f;
        for (auto& comb : ch.comb) out += comb.process(revdelay);
        out /= static_cast<float>(detail::kCombCount);
        for (auto& a : ch.apfOut) out = a.process(out);
        const float earlyLate = (in * ch.earlyGain) + (out * reverbGain_);
        out = ch.roomHighShelf.process(earlyLate * roomGain_);
        return out * ch.gain;
    }

    int channels_;
    detail::Delay earlyDelay_;
    detail::AllPass apfIn_[detail::kApfInCount];
    Channel channel_[2];
    float earlyGain_ = 1.0f, reverbGain_ = 1.0f, roomGain_ = 1.0f, wetRatio_ = 1.0f, dryRatio_ = 0.0f;
};

}  // namespace xa2reverb
