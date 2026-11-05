// SQSound: the scripts' sound object (Diesel::Squirrel::SQSound; natives muramasa_en.exe 0x526860
// ff.), a task object holding a Diesel::CSound (0x17c bytes, constructor 0x4e2080) that drives voices
// of Mware's XAudio2 device: one for create (streamed), 10 for load, which its plays use in turn.
//
// The device is simulated here, voice by voice: a voice plays for the Ogg file's length at the
// frequency ratio, repeats its loop segment (milliseconds; 0, 0 the whole file) as many times as
// asked (-1: for ever; 0 and 1 once) and then stops, so isPlaying and the times answer as the PC's would. What the
// device is told goes out as SoundEvents and SoundVoices for the host to play: volumes as CSound
// computes them (0x4e2b00: the type's volume, the groups', the ducking control and the sound's own,
// clamped) and as Mware turns them into an XAudio2 amplitude (0x10032530: v^1.661 above 0.001).

#include <algorithm>
#include <cmath>

#include "natives.hpp"
#include "nyan/diesel/ogg_info.hpp"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace nyan::diesel {

namespace {


struct SoundObject final : NativeObject, FrameTask, ScriptHost::Impl::SoundRecord {
    struct Voice {
        uint32_t key = 0;
        bool playing = false, paused = false;
        double positionMs = 0.0;
        int32_t playsLeft = 0;
    };
    ScriptHost::Impl* host;
    std::string file;
    bool loaded = false;       // a CSound (not the CNullSound a failed create leaves)
    bool streamed = true;      // create, or load
    uint32_t durationMs = 0;   // the voice's total time (device +0x58)
    float timeScale = 1.0f;    // +0x174
    Tween volume{1, 1.0f};     // +0xc8
    Tween pan{1, 0.0f};        // +0xf4
    Tween frequency{1, 1.0f};  // +0x120
    int32_t playCount = 1;     // +0x14c: plays (-1: for ever; 0 and 1: once)
    int32_t loopStart = 0;     // +0x150, ms
    int32_t loopEnd = 0;       // +0x154, ms (0: the end)
    std::vector<Voice> voiceList; // +0xc0 of them; +0xbc the next to play
    size_t nextVoice = 0;
    uint32_t seekMs = 0;       // where the next play starts (the device's +0x9c)
    uint32_t lastMilli = 0;
    // What the device has been told (set with -1: every voice of the sound).
    float deviceGain = 0.0f, devicePan = 0.0f, deviceFrequency = 1.0f;
    int32_t lock = 0, disused = 0;  // Totono's flags
    // setEcho's values (Mware's player +0x28c, the five as given) and FXEcho's parameters from them
    // (+0x2d8: the mix, the feedback, the delay); setPresetReverb's preset (+0x288).
    std::array<float, 4> echoRaw{};
    int32_t echoRawInt = 0;
    float echoWet = 0.0f, echoFeedback = 0.0f, echoDelayMs = 1.0f;
    int32_t reverbPreset = 0;

    SoundObject(ScriptHost::Impl* h, std::string name) : host(h) {
        taskName = std::move(name);
        created = h->clock.milliTime;
        h->addTask(this);
        h->soundRecords.push_back(this);
        lastMilli = h->clock.milliTime;
    }
    ~SoundObject() override {
        stopAll();
        host->removeTask(this);
        std::erase(host->soundRecords, static_cast<ScriptHost::Impl::SoundRecord*>(this));
    }

    void event(SoundEvent::Kind kind, const Voice& v) {
        SoundEvent e;
        e.kind = kind;
        e.voice = v.key;
        host->soundEvents.push_back(std::move(e));
    }
    void stopAll() {  // device stop(id, -1)
        for (auto& v : voiceList) {
            if (v.playing) event(SoundEvent::Kind::Stop, v);
            v.playing = v.paused = false;
            v.positionMs = 0.0;
        }
    }
    bool anyPlaying() const {
        return std::any_of(voiceList.begin(), voiceList.end(), [](const Voice& v) { return v.playing; });
    }
    bool anyPaused() const {
        return std::any_of(voiceList.begin(), voiceList.end(), [](const Voice& v) { return v.playing && v.paused; });
    }
    // The device volume (0x4e2b00): type × groups × control × the sound's volume (0 at or below
    // 0.001 before its power), clamped to [0, 1]; then XAudio2's amplitude (0x10032530).
    void applyVolume() {
        float own = volume.nowNoPower[0] > 0.001f ? volume.now[0] : 0.0f;
        float v = groupProduct * host->typeVolume(type) * control.now * own;
        v = std::clamp(v, 0.0f, 1.0f);
        deviceGain = v > 0.001f ? std::pow(10.0f, std::log10(v) * 33.22f * 0.05f) : 0.0f;
    }
    // The device's open (0x4e2470 / 0x4e2580): the voices made afresh.
    void open(size_t count) {
        stopAll();
        voiceList.assign(count, Voice{});
        for (auto& v : voiceList) v.key = host->nextSoundKey++;
        nextVoice = 0;
        seekMs = 0;
    }

    // The task update (0x4e2790): audible or not for the manager; the volume applied when it, the
    // record or the type changed (a volume of 0 stops the voices); pan and frequency as they move;
    // then the simulated voices' time.
    void update(ScriptHost::Impl& h) override {
        bool hear = !(volume.nowNoPower[0] <= 0.001f || !anyPlaying() || anyPaused() ||
                      groupProduct * h.typeVolume(type) <= 0.0f);
        if (audible != hear) dirty = true;
        audible = hear;
        bool typeMoving = false;
        if (type >= 0) {
            auto it = h.typeVolumes.find(type);
            typeMoving = it != h.typeVolumes.end() && (it->second.active || it->second.wasActive);
        }
        if (volume.step(h.clock) || dirty || typeMoving) {
            applyVolume();
            if (volume.nowNoPower[0] == 0.0f) stopAll();
        }
        if (pan.step(h.clock)) devicePan = pan.now[0];
        if (frequency.step(h.clock)) deviceFrequency = frequency.now[0];
        dirty = false;

        uint32_t now = h.clock.milliTime;
        double dt = static_cast<double>(now - lastMilli);
        lastMilli = now;
        if (durationMs == 0) return;
        double rate = frequency.now[0] > 0.0f ? frequency.now[0] : 0.0;
        // Mware's position thread (0x10030c60): a pass with plays -1 (for ever) or above 1 ends at
        // the loop's end (the whole file for 0, 0) and goes back to the loop's start, counting down
        // unless -1; with 0 or 1 left it is the last and runs to the file's end. A pass that started
        // beyond the loop's end runs to the file's end and goes back to 0.
        double fileEnd = durationMs;
        double segEnd = loopStart == 0 && loopEnd == 0 ? fileEnd : std::min<double>(loopEnd, fileEnd);
        for (auto& v : voiceList) {
            if (!v.playing || v.paused) continue;
            double before = v.positionMs;
            v.positionMs += dt * rate;
            while (true) {
                bool last = v.playsLeft != -1 && v.playsLeft <= 1;
                bool past = before > segEnd;
                double end = last || past ? fileEnd : segEnd;
                if (v.positionMs < end) break;
                if (last) {
                    v.playing = false;
                    v.positionMs = 0.0;
                    break;
                }
                if (v.playsLeft != -1) --v.playsLeft;
                double start = past ? 0.0 : (loopStart >= 0 && loopStart < end ? loopStart : 0.0);
                v.positionMs = start + (v.positionMs - end);
                before = start;
                if (end - start <= 0.0) break;
            }
        }
    }
    void voices(std::vector<SoundVoice>& out) const override {
        for (const auto& v : voiceList) {
            if (!v.playing) continue;
            SoundVoice s;
            s.paused = v.paused;
            s.voice = v.key;
            s.type = type;
            s.gain = deviceGain;
            s.pan = devicePan;
            s.frequency = deviceFrequency;
            s.echoWet = echoWet, s.echoFeedback = echoFeedback, s.echoDelayMs = echoDelayMs;
            out.push_back(s);
        }
    }
};

NativeObject* make(HSQUIRRELVM v) { return new SoundObject(&host(v), argString(v, 2)); }

#define SOUND_SELF                                                   \
    SoundObject* o = self<SoundObject>(v);                           \
    if (!o) return sq_throwerror(v, "SQSound: not an instance");

SQInteger pushInt(HSQUIRRELVM v, SQInteger i) {
    sq_pushinteger(v, i);
    return 1;
}
SQInteger pushFloat(HSQUIRRELVM v, SQFloat f) {
    sq_pushfloat(v, f);
    return 1;
}
int32_t argI(HSQUIRRELVM v, SQInteger idx) { return static_cast<int32_t>(argInt(v, idx)); }

// create(file) / load(file): int (0x526860 / 0x526a20): stops the voices and opens the file for one
// streamed voice or 10; 0, or -1 (a CNullSound) if it does not open.
SQInteger openSound(HSQUIRRELVM v, bool streamed) {
    SOUND_SELF
    auto& h = host(v);
    std::string file = argString(v, 2);
    auto data = h.readAnyFile(file);
    auto info = data ? readOggInfo(*data) : std::nullopt;
    if (!info) {
        if (!data) h.note(h.missing, file);
        o->stopAll();
        o->voiceList.clear();
        o->loaded = false;
        o->file.clear();
        o->durationMs = 0;
        return pushInt(v, -1);
    }
    o->open(streamed ? 1 : 10);
    o->loaded = true;
    o->streamed = streamed;
    o->file = file;
    o->durationMs = info->milliseconds();
    o->applyVolume();
    return pushInt(v, 0);
}
SQInteger create(HSQUIRRELVM v) { return openSound(v, true); }
SQInteger load(HSQUIRRELVM v) { return openSound(v, false); }

// play() (0x4e2920): the next voice, from where seek left it, the loop as set; the record marked
// audible and dirty so the volume goes out with it.
SQInteger play(HSQUIRRELVM v) {
    SOUND_SELF
    if (!o->loaded || o->voiceList.empty()) return 0;
    auto& voice = o->voiceList[o->nextVoice];
    o->nextVoice = (o->nextVoice + 1) % o->voiceList.size();
    if (voice.playing) o->event(SoundEvent::Kind::Stop, voice);
    voice.playing = true;
    voice.paused = false;
    voice.positionMs = o->seekMs;
    voice.playsLeft = o->playCount;
    SoundEvent e;
    e.kind = SoundEvent::Kind::Play;
    e.voice = voice.key;
    e.file = o->file;
    e.type = o->type;
    e.plays = o->playCount;
    e.loopStartMs = o->loopStart;
    e.loopEndMs = o->loopEnd;
    e.startMs = o->seekMs;
    e.reverbPreset = o->reverbPreset;
    e.xaudioVersion = host(v).game.xaudioVersion;
    e.xaudio64 = host(v).game.xaudio64;
    host(v).soundEvents.push_back(std::move(e));
    o->seekMs = 0;
    o->audible = true;
    o->dirty = true;
    o->lastMilli = host(v).clock.milliTime;
    o->applyVolume();
    return 0;
}
SQInteger stop(HSQUIRRELVM v) {
    SOUND_SELF
    o->stopAll();
    o->audible = false;
    o->dirty = true;
    return 0;
}
// pause() / onResume() (0x4e2a40 / 0x4e2aa0): every voice.
SQInteger pause(HSQUIRRELVM v) {
    SOUND_SELF
    for (auto& voice : o->voiceList) {
        if (voice.playing && !voice.paused) {
            voice.paused = true;
            o->event(SoundEvent::Kind::Pause, voice);
        }
    }
    o->dirty = true;
    return 0;
}
SQInteger onResume(HSQUIRRELVM v) {
    SOUND_SELF
    for (auto& voice : o->voiceList) {
        if (voice.playing && voice.paused) {
            voice.paused = false;
            o->event(SoundEvent::Kind::Resume, voice);
        }
    }
    o->lastMilli = host(v).clock.milliTime;
    o->dirty = true;
    return 0;
}
// isPlaying() (device +0x48 with -1): any voice, paused ones included.
SQInteger isPlaying(HSQUIRRELVM v) {
    SOUND_SELF
    return pushInt(v, o->anyPlaying() ? 1 : 0);
}
SQInteger isPaused(HSQUIRRELVM v) {
    SOUND_SELF
    return pushInt(v, o->anyPaused() ? 1 : 0);
}

// setVolume(v, ms, curve) (0x4e2b80): from where it is; at once it is applied, and a 0 stops.
SQInteger setVolume(HSQUIRRELVM v) {
    SOUND_SELF
    int32_t ms = argI(v, 3);
    o->volume.moveTo({argFloat(v, 2), 0.0f, 0.0f}, ms, argI(v, 4));
    if (ms == 0) {
        o->applyVolume();
        if (!o->volume.active && o->volume.nowNoPower[0] == 0.0f) o->stopAll();
    }
    return 0;
}
// setPan / setFrequency (0x4e2c00 / 0x4e2c60): at once they go to the device.
SQInteger setPan(HSQUIRRELVM v) {
    SOUND_SELF
    int32_t ms = argI(v, 3);
    o->pan.moveTo({argFloat(v, 2), 0.0f, 0.0f}, ms, argI(v, 4));
    if (ms == 0) o->devicePan = o->pan.now[0];
    return 0;
}
SQInteger setFrequency(HSQUIRRELVM v) {
    SOUND_SELF
    int32_t ms = argI(v, 3);
    o->frequency.moveTo({argFloat(v, 2), 0.0f, 0.0f}, ms, argI(v, 4));
    if (ms == 0) o->deviceFrequency = o->frequency.now[0];
    return 0;
}
// getVolume() (0x4e2e50, CSound +0xe0) and the others: the tween's value before its power.
SQInteger getVolume(HSQUIRRELVM v) {
    SOUND_SELF
    return pushFloat(v, o->volume.nowNoPower[0]);
}
SQInteger getPan(HSQUIRRELVM v) {
    SOUND_SELF
    return pushFloat(v, o->pan.nowNoPower[0]);
}
SQInteger getFrequency(HSQUIRRELVM v) {
    SOUND_SELF
    return pushFloat(v, o->frequency.nowNoPower[0]);
}
SQInteger setVolumeTimer(HSQUIRRELVM v) {
    SOUND_SELF
    o->volume.setTimer(argFloat(v, 2));
    return 0;
}
SQInteger setPanTimer(HSQUIRRELVM v) {
    SOUND_SELF
    o->pan.setTimer(argFloat(v, 2));
    return 0;
}
SQInteger setFrequencyTimer(HSQUIRRELVM v) {
    SOUND_SELF
    o->frequency.setTimer(argFloat(v, 2));
    return 0;
}
SQInteger getVolumeTimer(HSQUIRRELVM v) {
    SOUND_SELF
    return pushFloat(v, o->volume.timer);
}
SQInteger getPanTimer(HSQUIRRELVM v) {
    SOUND_SELF
    return pushFloat(v, o->pan.timer);
}
SQInteger getFrequencyTimer(HSQUIRRELVM v) {
    SOUND_SELF
    return pushFloat(v, o->frequency.timer);
}

// setLoop(plays, start, end) (0x4e2cc0): kept if 0 <= start <= end (else the exe logs an error), and
// handed to the device with the sound's handle at once (its vtable +0x30), so a sound playing takes
// it now: The Song of Saya's end roll turns its looping ending song to play out (its interpreter's
// SetLoop(bgm, 0)) and waits for it to stop.
SQInteger setLoop(HSQUIRRELVM v) {
    SOUND_SELF
    int32_t count = argI(v, 2), start = argI(v, 3), end = argI(v, 4);
    if (start < 0 || end < 0 || start > end) {
        auto& h = host(v);
        if (h.log) h.log("SQSound.setLoop: bad loop " + std::to_string(start) + ".." + std::to_string(end) + " for " + o->file);
        return 0;
    }
    o->playCount = count;
    o->loopStart = start;
    o->loopEnd = end;
    for (auto& voice : o->voiceList) {
        if (!voice.playing) continue;
        voice.playsLeft = count;
        SoundEvent e;
        e.kind = SoundEvent::Kind::Loop;
        e.voice = voice.key;
        e.plays = count;
        e.loopStartMs = start;
        e.loopEndMs = end;
        host(v).soundEvents.push_back(std::move(e));
    }
    return 0;
}
// setType(type) (0x4e2d50): the type whose volume the sound follows.
SQInteger setType(HSQUIRRELVM v) {
    SOUND_SELF
    int32_t t = argI(v, 2);
    o->type = t;
    host(v).typeVolumes.try_emplace(t);
    o->dirty = true;
    return 0;
}
// addSoundGroup(name, priority, factor) (0x4e30e0): in a defined group the sound is ducked by the
// factor while a member of higher priority is heard.
SQInteger addSoundGroup(HSQUIRRELVM v) {
    SOUND_SELF
    std::string name = argString(v, 2);
    if (host(v).groupVolumes.find(name) == host(v).groupVolumes.end()) return 0;
    o->groups.push_back({name, argI(v, 3), argFloat(v, 4)});
    o->dirty = true;
    return 0;
}
// setVolumeControlFadeTime(in, inCurve, out, outCurve) (0x4e3140): the ducking control's fades.
SQInteger setVolumeControlFadeTime(HSQUIRRELVM v) {
    SOUND_SELF
    o->fadeIn = argI(v, 2);
    o->fadeInCurve = argI(v, 3);
    o->fadeOut = argI(v, 4);
    o->fadeOutCurve = argI(v, 5);
    return 0;
}
// setTimeScale(s) (0x4e2d90): the tweens'.
SQInteger setTimeScale(HSQUIRRELVM v) {
    SOUND_SELF
    o->timeScale = argFloat(v, 2);
    o->volume.timeScale = o->pan.timeScale = o->frequency.timeScale = o->timeScale;
    return 0;
}
// seek(ms) (0x4e2dc0): the voices stopped, the position set, played again if they were playing.
SQInteger seek(HSQUIRRELVM v) {
    SOUND_SELF
    int32_t to = argI(v, 2);
    bool was = o->anyPlaying();
    if (was) o->stopAll();
    o->seekMs = to > 0 ? static_cast<uint32_t>(to) : 0u;
    if (was) {
        sq_settop(v, 1);
        play(v);
    }
    return 0;
}
// getTotalTime() / getRemainTime() (device +0x54 / +0x50 of voice 0, while one plays).
SQInteger getTotalTime(HSQUIRRELVM v) {
    SOUND_SELF
    return pushInt(v, static_cast<SQInteger>(o->durationMs));
}
// getAmplitude() (0x526d30 -> CSound vtable +0xa8, 0x4e2f00 -> the device's +0x54, Mware
// 0x100405c0; phantom.exe 0x1400fb9d0): 0 unless the sound's voice plays; else the player's +0x34,
// which its update (0x100319d0) sets to twice the meter Mware puts on each voice's effect chain: an
// XAPO whose Process (0x10030ab0) keeps the mean |sample| over the buffer's frames and channels (0
// for a silent buffer). XAudio2 processes 10 ms at a time; Nyanroid takes the 10 ms block of the
// decoded file the voice's position is in (Fitted: which block the PC's last processed one is, and
// the resampling to the output rate, which barely moves a mean). Phantom of Inferno's mouths follow
// it (its standing-picture script).
std::shared_ptr<const ScriptHost::Impl::AmplitudeEnvelope> amplitudeEnvelope(ScriptHost::Impl& h, const std::string& path) {
    auto it = h.amplitudes.find(path);
    if (it != h.amplitudes.end()) return it->second;
    auto env = std::make_shared<ScriptHost::Impl::AmplitudeEnvelope>();
    if (auto data = h.readAnyFile(path)) {
        int err = 0;
        stb_vorbis* vb = stb_vorbis_open_memory(data->data(), static_cast<int>(data->size()), &err, nullptr);
        if (vb) {
            stb_vorbis_info info = stb_vorbis_get_info(vb);
            int ch = std::max(1, info.channels);
            env->rate = static_cast<int>(info.sample_rate);
            env->blockFrames = std::max(1, env->rate / 100);
            std::vector<float> buf(static_cast<size_t>(env->blockFrames) * static_cast<size_t>(ch));
            while (true) {
                int got = 0;  // frames of this block
                while (got < env->blockFrames) {
                    int n = stb_vorbis_get_samples_float_interleaved(vb, ch, buf.data() + static_cast<size_t>(got) * ch,
                                                                    (env->blockFrames - got) * ch);
                    if (n <= 0) break;
                    got += n;
                }
                if (got == 0) break;
                double sum = 0.0;
                for (size_t i = 0; i < static_cast<size_t>(got) * ch; ++i) sum += std::fabs(buf[i]);
                env->mean.push_back(static_cast<float>(sum / static_cast<double>(static_cast<size_t>(got) * ch)));
                if (got < env->blockFrames) break;
            }
            stb_vorbis_close(vb);
        }
    }
    h.amplitudes[path] = env;
    h.amplitudeOrder.push_back(path);
    if (h.amplitudeOrder.size() > 16) {
        h.amplitudes.erase(h.amplitudeOrder.front());
        h.amplitudeOrder.erase(h.amplitudeOrder.begin());
    }
    return env;
}
// getPulse() (0x526d20 -> CSound vtable +0xa4, 0x4e2ec0 -> the device's +0x50, Mware 0x10040500 ->
// the player's vtable +0x14, 0x10042290): the player's +0x30, which only its reset (0x10031840)
// writes, with 0. 0 whether or not the voice plays.
SQInteger getPulse(HSQUIRRELVM v) {
    SOUND_SELF
    return pushFloat(v, 0.0f);
}
// setEcho(mix, feedback, delay, a, b) (0x526da0 -> CSound vtable +0xb0, 0x4e2fb0): the sound stops
// (vtable +0x5c, 0x4e29e0, as stop), then the device's +0x68 (Mware 0x10040940) hands the values to
// the player (0x10032ae0): it keeps the five and sets FXEcho's parameters, the mix and the feedback
// x 0.01 within 0 .. 1 and the delay within 1 .. 2000 ms (the last two values unused); a mix of 0
// disables the effect. The mixer runs the echo on the sound's voices (Fitted: FXEcho as its
// documentation describes it, a delay line fed back).
SQInteger setEcho(HSQUIRRELVM v) {
    SOUND_SELF
    o->stopAll();
    o->audible = false;
    o->dirty = true;
    for (int i = 0; i < 4; ++i) o->echoRaw[static_cast<size_t>(i)] = argFloat(v, 2 + i);
    o->echoRawInt = argI(v, 6);
    auto unit = [](float f) {
        float x = f * 0.00999999978f;
        if (0.0f <= x) return 1.0f <= x ? 1.0f : x;
        return 0.0f;
    };
    o->echoWet = unit(o->echoRaw[0]);
    o->echoFeedback = unit(o->echoRaw[1]);
    float d = o->echoRaw[2], delay = 1.0f;
    if (1.0f <= d) delay = 2000.0f <= d ? 2000.0f : d;
    o->echoDelayMs = delay;
    return 0;
}
// setPresetReverb(preset) (0x526df0 -> CSound vtable +0xb8, 0x4e3070): the sound stops (as setEcho),
// then the player (0x10032bd0) keeps the preset, which each later play hands XAudio2's reverb (its
// effect 2) as that I3DL2 preset, off for 0 (sound_player.cpp: XAudio2_9.dll's, its x86-64 build's
// for phantom.exe, XAudio2_7.dll's for totono_en.exe, whose Mware makes XAudio2 2.7:
// GameData::xaudioVersion / xaudio64; in public builds FAudio's re-implementation stands in).
SQInteger setPresetReverb(HSQUIRRELVM v) {
    SOUND_SELF
    o->stopAll();
    o->audible = false;
    o->dirty = true;
    o->reverbPreset = argI(v, 2);
    return 0;
}
SQInteger getAmplitude(HSQUIRRELVM v) {
    SOUND_SELF
    if (!o->loaded || o->voiceList.empty() || !o->anyPlaying()) return pushFloat(v, 0.0f);
    const auto& voice = o->voiceList[0];
    if (!voice.playing) return pushFloat(v, 0.0f);
    auto env = amplitudeEnvelope(host(v), o->file);
    if (env->mean.empty() || env->rate <= 0) return pushFloat(v, 0.0f);
    size_t block = static_cast<size_t>(voice.positionMs * env->rate / 1000.0) / static_cast<size_t>(env->blockFrames);
    if (block >= env->mean.size()) block = env->mean.size() - 1;
    return pushFloat(v, 2.0f * env->mean[block]);
}
SQInteger getRemainTime(HSQUIRRELVM v) {
    SOUND_SELF
    if (o->voiceList.empty()) return pushInt(v, 0);
    return pushInt(v, static_cast<SQInteger>(o->durationMs) - static_cast<SQInteger>(o->voiceList[0].positionMs));
}
SQInteger setLock(HSQUIRRELVM v) {
    SOUND_SELF
    o->lock = argI(v, 2);
    return 0;
}
SQInteger getLock(HSQUIRRELVM v) {
    SOUND_SELF
    return pushInt(v, o->lock);
}
SQInteger setDisused(HSQUIRRELVM v) {
    SOUND_SELF
    o->disused = argI(v, 2);
    return 0;
}
SQInteger getDisused(HSQUIRRELVM v) {
    SOUND_SELF
    return pushInt(v, o->disused);
}

}  // namespace

// The sound manager's frame (0x42cc00, after the scripts, before the objects update): each group's
// volume steps; each group's top priority among its heard members; each type's volume steps; then
// each record's ducking (0x42c7f0): the product of its groups' volumes, and a control that goes to
// the product of its factors in groups where a higher priority is heard (to 0 if it is not heard),
// fading in or out by the record's times, jumping there on its first frame or after silence.
void ScriptHost::Impl::updateSoundManager() {
    for (auto& [name, g] : groupVolumes) {
        g.wasActive = g.active;
        g.active = g.action.step(clock);
    }
    for (auto& [name, g] : groupVolumes) {
        g.lastTopPriority = g.topPriority;
        g.topPriority = 0;
    }
    for (auto* r : soundRecords) {
        if (!r->audible) continue;
        for (const auto& e : r->groups) {
            auto it = groupVolumes.find(e.group);
            if (it != groupVolumes.end() && it->second.topPriority < e.priority) it->second.topPriority = e.priority;
        }
    }
    for (auto& [type, t] : typeVolumes) {
        t.wasActive = t.active;
        t.active = t.action.step(clock);
    }
    for (auto* r : soundRecords) {
        float product = 1.0f;
        for (const auto& e : r->groups) {
            auto it = groupVolumes.find(e.group);
            if (it == groupVolumes.end()) continue;
            if (it->second.active || it->second.wasActive) r->dirty = true;
            product *= it->second.action.now;
        }
        r->groupProduct = product;
        float target = 1.0f;
        bool silentNow = false;
        if (!r->audible || product <= 0.0f || (r->type >= 0 && typeVolume(r->type) == 0.0f)) {
            target = 0.0f;
            r->silent = true;
            silentNow = true;
        } else {
            for (const auto& e : r->groups) {
                auto it = groupVolumes.find(e.group);
                if (it != groupVolumes.end() && e.priority < it->second.topPriority) target *= e.factor;
            }
        }
        Action1D& c = r->control;
        if (target != c.dst) {
            bool up = target > c.dst;
            int32_t ms = up ? r->fadeIn : r->fadeOut;
            int32_t curve = up ? r->fadeInCurve : r->fadeOutCurve;
            c.nowNoPower = c.now;
            c.now = c.power * c.now;
            c.src = c.now;
            c.dst = target;
            c.timer = 0.0f;
            c.lastStamp = 0.0f;
            c.actionTime = static_cast<float>(ms);
            c.tempo = curve > 0xa0 ? 0 : curve;
            if (c.actionTime == 0.0f) {
                c.active = 0;
                c.src = target;
                c.now = c.power * target;
                c.nowNoPower = target;
            } else if (c.src != target) {
                c.active = 1;
            }
            r->dirty = true;
        }
        if (c.step(clock) || r->dirty) r->dirty = true;
        if (r->first || r->silent) {
            if (c.active) {
                c.active = 0;
                c.nowNoPower = c.dst;
                c.now = c.power * c.dst;
            }
            r->first = false;
        }
        r->silent = silentNow;
    }
}

ClassNatives soundNatives() {
    ClassNatives c;
    c.make = make;
    c.methods = {
        {"create", create},
        {"load", load},
        {"play", play},
        {"stop", stop},
        {"pause", pause},
        {"onResume", onResume},
        {"isPlaying", isPlaying},
        {"isPaused", isPaused},
        {"getAmplitude", getAmplitude},
        {"getPulse", getPulse},
        {"setEcho", setEcho},
        {"setPresetReverb", setPresetReverb},
        {"setVolume", setVolume},
        {"setPan", setPan},
        {"setFrequency", setFrequency},
        {"getVolume", getVolume},
        {"getPan", getPan},
        {"getFrequency", getFrequency},
        {"setVolumeTimer", setVolumeTimer},
        {"setPanTimer", setPanTimer},
        {"setFrequencyTimer", setFrequencyTimer},
        {"getVolumeTimer", getVolumeTimer},
        {"getPanTimer", getPanTimer},
        {"getFrequencyTimer", getFrequencyTimer},
        {"setLoop", setLoop},
        {"setType", setType},
        {"addSoundGroup", addSoundGroup},
        {"setVolumeControlFadeTime", setVolumeControlFadeTime},
        {"setTimeScale", setTimeScale},
        {"seek", seek},
        {"getTotalTime", getTotalTime},
        {"getDurationTime", getTotalTime},
        {"getRemainTime", getRemainTime},
        {"setLock", setLock},
        {"getLock", getLock},
        {"setDisused", setDisused},
        {"getDisused", getDisused},
    };
    return c;
}

}  // namespace nyan::diesel
