#include "nyan/wa2/sound.hpp"

#include <algorithm>
#include <cstdio>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

// The sound archives in their registration order (0x405e10).
constexpr int kBgm = 0, kSe = 1, kVoice = 2, kIcBgm = 3, kIcSe = 4, kIcVoice = 5;

// `x * typeVol * all_vol` to an 8-bit level: two arithmetic shifts that round toward zero, then
// 0..255 (the expression every channel repeats, e.g. 0x4064a0, 0x40fb80).
int level8(int cur, int typeVol, int all) {
    int x = cur * typeVol * all;
    x = ((x + ((x >> 31) & 0xff)) >> 8);
    x = ((x + ((x >> 31) & 0xff)) >> 8);
    return std::clamp(x, 0, 255);
}

// The character ids that share a switch (0x4063d0): two groups of minor characters.
int voiceGroup(int chara) {
    switch (chara) {
    case 12: case 13: case 15: case 17: case 18: case 20: case 21: case 27: case 28: case 30: case 31:
    case 32: case 36: case 37: case 99:
        return 99;
    case 14: case 16: case 19: case 22: case 23: case 24: case 25: case 26: case 29: case 33: case 34:
    case 35: case 98:
        return 98;
    default:
        return chara;
    }
}

// char_voiceK switches character kCharVoice[K] (0x4bf72c).
constexpr std::array<int, 10> kCharVoice = {0, 2, 1, 3, 4, 5, 10, 11, 99, 98};

std::string format(const char* fmt, int a, int b = 0, int c = 0) {
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, a, b, c);
    return buf;
}

}  // namespace

Sound::Sound(Game& game, SoundOut& out) : game_(game), out_(out) {
    oneShots_.fill(-1);
    systemSe_.fill(-1);
}

bool Sound::skipping() const {
    return game_.skipActive();  // 0x40ad50
}

int Sound::level(int cur, int typeVol) const {
    return level8(cur, typeVol, game_.settings.allVol);
}

int Sound::fadeFrames(int n) const {
    return game_.fr(n);  // 0x40aed0
}

int Sound::voiceFrames(int n) const {
    return game_.frT(n);  // 0x40b100
}

void Sound::freeSound(int& handle) {
    // 0x4067c0 / 0x45f8a0 + 0x460550: stop and release (the engine also sleeps 10-50 ms here).
    if (handle != -1) out_.stop(handle);
    handle = -1;
}

// ---- BGM -------------------------------------------------------------------------------------------

void Sound::playBgm(int num, int fade, int loop, int vol, int force, int variant) {
    // 0x40fa50: tracks 0..199 are marked heard (game flag n + 100, Sys.sav written at once).
    if (static_cast<unsigned>(num) < 200) {
        game_.system.setGameFlag(num + 100, 1);
        game_.writeSystem();
        bgmPlay(num, fade, loop, vol, force, 0, variant);
        return;
    }
    stopBgm(fade);
}

void Sound::bgmPlay(int num, int fade, int loop, int vol, int force, int always, int variant) {
    // 0x40f970: the same track again only changes the volume; a new one crossfades between the two
    // channels (fade -2: a cut-in with a 15-frame fade-out of the old one).
    int old = bgmCur_;
    if (num < 0) return stopBgm(fade);
    if (!force && bgmTrack_ == num) {
        bgmVolume(vol, fade);
        return;
    }
    bgmTrack_ = num;
    bgmVolume_ = vol;
    bgmLoop_ = loop;
    const auto& s = game_.settings;
    if ((s.bgm != 0 && s.allSound != 0) || always) {
        bgmCur_ = bgmCur_ == 0 ? 1 : 0;
        if (fade == -2) {
            bgmStart(bgmCur_, num, 0, loop, vol, 0, variant);
            bgmFadeOut(old, 15);
        } else {
            bgmStart(bgmCur_, num, fade, loop, vol, 0, variant);
            bgmFadeOut(old, fade);
        }
    }
}

void Sound::bgmStart(int ch, int num, int fade, int loop, int vol, int force, int variant) {
    // 0x4068f0
    BgmChannel& c = bgm_[static_cast<size_t>(ch)];
    if (!force && c.state != 4 && c.state != 0 && c.track == num) return;
    vol = std::clamp(vol, 0, 255);
    c.loop = loop;
    c.track = num;
    c.state = 3;
    c.c = 0;
    c.target = vol;
    if (fade < 1) {
        c.d = 1;
        c.cur = c.start = vol;
    } else {
        c.d = fade;
        c.cur = c.start = 0;
    }
    freeSound(c.handle);
    // 0x460e10: BGM_nnn.ogg, else the intro of an intro/loop pair (or its loop, variant 1); then the
    // same in IC\BGM.
    std::string plain = format("BGM_%03d.ogg", num);
    std::string part = format(variant == 1 ? "BGM_%03d_b.ogg" : "BGM_%03d_a.ogg", num);
    // Special Contents (0x460bb0): from 80 on a voice drama, one file per part, played once (Guess:
    // the exe plays them once when the stream has no loop point; the drama files have none).
    bool drama = game_.assets().build() == Build::Special && num >= 0x50;
    if (drama) {
        part = format("BGM_%03d_a.ogg", num);
        part[8] = static_cast<char>('a' + dramaPart);
    }
    int archive = -1, entry = -1;
    for (int a : {kBgm, kIcBgm}) {
        for (const std::string& name : {plain, part}) {
            if ((entry = out_.find(a, name)) != -1) break;
        }
        if (entry != -1) {
            archive = a;
            break;
        }
    }
    if (entry == -1) {
        game_.noteMissing("BGM " + plain);
        return;
    }
    c.handle = out_.play(SoundOut::Kind::Bgm, archive, entry, loop == 0 || drama ? 1 : 0, level(c.cur, game_.settings.bgmVol));
}

void Sound::bgmFadeOut(int ch, int fade) {
    // 0x406b90
    BgmChannel& c = bgm_[static_cast<size_t>(ch)];
    if (c.track == -1) return;
    c.state = 4;
    c.c = 0;
    c.d = std::max(fade, 1);
    c.start = c.cur;
    c.target = 0;
}

void Sound::stopBgm(int fade) {
    // MS, 0x406d50
    bgmTrack_ = -1;
    bgmFadeOut(bgmCur_, fade);
}

void Sound::bgmVolume(int vol, int fade) {
    // MV, 0x406de0 -> 0x406c20
    bgmVolume_ = vol;
    BgmChannel& c = bgm_[static_cast<size_t>(bgmCur_)];
    if (c.track == -1) return;
    c.state = 5;
    c.c = 0;
    c.d = std::max(fade, 1);
    c.start = c.cur;
    c.target = std::clamp(vol, 0, 255);
}

bool Sound::bgmSteady() const {
    int s = bgm_[static_cast<size_t>(bgmCur_)].state;
    return s == 0 || s == 1;
}

bool Sound::bgmStopped() const {
    return bgm_[static_cast<size_t>(bgmCur_)].state == 0;
}

// ---- Sound effects ---------------------------------------------------------------------------------

int Sound::playSe(int num, int vol, int loop) {
    // 0x405940: SE_nnnn.ogg, then SE_nnnn.WAV, in Se then IC\Se. Found only in IC\Se, the engine
    // opens the main archive's entry -1, which fails (no entry of this data gets there).
    int handle = -1;
    int entry = out_.find(kSe, format("SE_%04d.ogg", num));
    if (entry == -1) entry = out_.find(kSe, format("SE_%04d.WAV", num));
    if (entry != -1) {
        handle = out_.play(SoundOut::Kind::Se, kSe, entry, loop == 0 ? 1 : 0, vol);
    } else if (out_.find(kIcSe, format("SE_%04d.ogg", num)) == -1 && out_.find(kIcSe, format("SE_%04d.WAV", num)) == -1) {
        game_.noteMissing(format("SE %04d", num));
    }
    // The patch's hook on this function: an SE may start a subtitle.
    if (onTrigger) onTrigger(num, 2);
    return handle;
}

int Sound::loadSe(int num, int loop, int vol, int mode) {
    // 0x406620: mode 0 plays at once, mode 1 only loads (0x405710).
    const auto& s = game_.settings;
    if (s.se == 0 || s.allSound == 0) return -1;
    int handle = -1;
    if (mode == 0) {
        handle = playSe(num, level(vol, s.seVol), loop);
        if (handle != -1) out_.setLevel(handle, level(vol, s.seVol));
    } else if (mode == 1) {
        int entry = out_.find(kSe, format("SE_%04d.ogg", num));
        if (entry == -1) entry = out_.find(kSe, format("SE_%04d.wav", num));
        if (entry != -1) handle = out_.load(SoundOut::Kind::Se, kSe, entry);
        else game_.noteMissing(format("SE %04d", num));
    }
    return handle;
}

void Sound::se(int num, int vol) {
    // SE (0x4064a0): a free one-shot slot, or nothing.
    const auto& s = game_.settings;
    if (s.se == 0 || s.allSound == 0) return;
    auto slot = std::find(oneShots_.begin(), oneShots_.end(), -1);
    if (slot == oneShots_.end()) return;
    int v = level(vol, s.seVol);
    *slot = playSe(num, v, 0);
    if (*slot != -1) out_.setLevel(*slot, v);
}

void Sound::systemSe(int which) {
    // 0x405af0 / 0x405b90 / 0x405c30: the preloaded object restarted at the effects volume, outside the
    // one-shot slots (and outside the patch's hook, which is on the SE play core).
    static constexpr int kNum[5] = {9211, 9212, 9213, 9214, 9015};
    const auto& s = game_.settings;
    if (which < 0 || which >= 5 || s.se == 0 || s.allSound == 0) return;
    int entry = out_.find(kSe, format("SE_%04d.ogg", kNum[which]));
    if (entry == -1) entry = out_.find(kSe, format("SE_%04d.wav", kNum[which]));
    if (entry == -1) return game_.noteMissing(format("SE %04d", kNum[which]));
    int v = std::clamp(((s.allVol * s.seVol * 0xff) >> 8) / 0xff, 0, 0xff);
    int& h = systemSe_[static_cast<size_t>(which)];
    if (h != -1) out_.stop(h);
    h = out_.play(SoundOut::Kind::Se, kSe, entry, 1, v);
}

void Sound::sep(int ch, int num, int fade, int loop, int vol, int mode) {
    // SEP (0x40f2d0)
    const auto& s = game_.settings;
    if (s.allSound == 0) return;
    if (ch < 0 || ch >= static_cast<int>(sep_.size())) return game_.log("SEP: channel " + std::to_string(ch));
    SepSlot& slot = sep_[static_cast<size_t>(ch)];
    if (mode == 2) {
        if (!slot.inUse) {
            game_.log("SEP: channel " + std::to_string(ch) + " has nothing loaded");
            slot = SepSlot{};
            return;
        }
    } else {
        if (slot.inUse) freeSound(slot.handle);
        slot = SepSlot{};
        slot.inUse = true;
        slot.num = num;
    }
    if (s.se != 0) {
        if (mode == 2) {
            // The raw volume, not scaled by the settings, until the next frame's update.
            out_.start(slot.handle, loop == 0 ? 1 : 0, fade == 0 ? vol : 0);
        } else {
            slot.handle = loadSe(num, loop, fade != 0 ? 0 : vol, mode);
        }
        if (slot.handle == -1) {
            slot = SepSlot{};
            return;
        }
        slot.loop = loop;
        slot.c = 0;
        slot.d = fade;
        slot.start = 0;
        slot.preloaded = mode == 1;
        slot.target = vol;
        return;
    }
    // SE switched off: remembered, silent.
    slot.loop = loop;
    slot.c = 0;
    slot.d = 0;
    slot.start = vol;
    slot.target = vol;
    slot.handle = -1;
}

void Sound::ses(int ch, int fade) {
    // SES (0x40f460)
    if (ch < 0 || ch >= static_cast<int>(sep_.size())) return;
    SepSlot& slot = sep_[static_cast<size_t>(ch)];
    if (!slot.inUse) return;
    if (fade > 0) {
        slot.stopAfter = true;
        sev(ch, 0, fade);
        return;
    }
    freeSound(slot.handle);
    slot = SepSlot{};
}

void Sound::sev(int ch, int vol, int fade) {
    // SEV (0x4067f0)
    if (ch < 0 || ch >= static_cast<int>(sep_.size())) return;
    SepSlot& slot = sep_[static_cast<size_t>(ch)];
    if (!slot.inUse) return;
    slot.d = fade;
    slot.c = 0;
    slot.start = slot.cur;
    slot.target = std::clamp(vol, 0, 255);
}

bool Sound::sewWaiting(int ch, int mode) {
    // SEW (0x40f4c0): mode 0 while skipping stops the channel instead of waiting.
    if (game_.settings.se == 0) return false;
    if (mode == 0 && skipping()) {
        ses(ch, 0);
        return false;
    }
    if (ch < 0 || ch >= static_cast<int>(sep_.size())) return false;
    const SepSlot& slot = sep_[static_cast<size_t>(ch)];
    return slot.inUse && slot.handle != -1;
}

bool Sound::sevwSame(int ch) const {
    if (ch < 0 || ch >= static_cast<int>(sep_.size())) return true;
    const SepSlot& slot = sep_[static_cast<size_t>(ch)];
    return slot.cur == slot.target;
}

void Sound::stopAllSe() {
    // 0x4137f0
    for (int& h : oneShots_) freeSound(h);
    for (int ch = 0; ch < static_cast<int>(sep_.size()); ++ch) ses(ch, 0);
    sep_.fill(SepSlot{});
}

// ---- Voices ----------------------------------------------------------------------------------------

void Sound::setScene(int scene) {
    if (scene == -1) return;
    viScene_ = scene;
    vaOffset_ = 0;
}

void Sound::addOffset(int n) {
    vaOffset_ = n == 0 ? 0 : vaOffset_ + n;
}

bool Sound::charVoiceOff(int chara) const {
    // 0x406460: the character's switch (by its group) is off.
    int id = voiceGroup(chara);
    for (size_t k = 0; k < kCharVoice.size(); ++k) {
        if (kCharVoice[k] == id && game_.settings.charVoice[k] == 0) return true;
    }
    return false;
}

void Sound::clearVoice(int ch) {
    VoiceChannel& v = voice_[static_cast<size_t>(ch)];
    freeSound(v.handle);
    v = VoiceChannel{};
}

void Sound::playVoice(int ch, int chara, int scene, int num, int vol, int loop, int noRecord) {
    // 0x40ecc0
    num += vaOffset_;
    if (scene == -1) scene = viScene_ != -1 ? viScene_ : game_.scriptNumber;
    if (chara == -1) chara = currentSpeaker;
    if (noRecord == 0) {
        lastVoice = {scene, num, chara, vol};
        voiceSpeaker = chara;
    }
    if (ch == 0 && noRecord == 0) voiceStartTime = game_.milliseconds();
    if (ch < 0 || ch >= static_cast<int>(voice_.size())) {
        game_.log("voice channel " + std::to_string(ch));
        if (onTrigger) onTrigger(num, 1);
        return;
    }
    const auto& s = game_.settings;
    std::string name = format("%04d_%04d_%02d.ogg", scene, num, chara);
    auto store = [&] {
        VoiceChannel& v = voice_[static_cast<size_t>(ch)];
        v.cur = v.target = v.start = v.base = vol;
        v.chara = chara;
        v.scene = scene;
        v.num = num;
        v.loop = loop;
    };
    if (!charVoiceOff(chara) && (ch != 0 || !skipping() || loop != 0) && s.voice != 0 && s.allSound != 0) {
        if (s.eroVoice != 0 && eroMode && chara == 0) {
            if (onTrigger) onTrigger(num, 1);
            return;
        }
        stopVoice(ch, 0);
        store();
        VoiceChannel& v = voice_[static_cast<size_t>(ch)];
        int lv = level(v.cur, s.voiceVol);
        int entry = out_.find(kVoice, name);
        if (entry != -1) {
            v.handle = out_.play(SoundOut::Kind::Voice, kVoice, entry, loop == 0 ? 1 : 0, lv);
            // The next channel: a breath loop of the same character is ducked to 1 while this one
            // speaks; another character's ducked loop comes back.
            if (ch + 1 < static_cast<int>(voice_.size())) {
                VoiceChannel& n = voice_[static_cast<size_t>(ch + 1)];
                if (n.handle != -1) {
                    if (n.chara == v.chara) {
                        if (n.base == 0) n.base = n.cur;
                        fadeVoice(ch + 1, 0, 1);
                    } else if (n.base != 0) {
                        fadeVoice(ch + 1, 0, n.base);
                        n.base = 0;
                    }
                }
            }
            if (skipping() && ch == 1) {
                VoiceChannel& one = voice_[1];
                if (one.base == 0) one.base = one.cur;
                fadeVoice(1, 0, 1);
            }
        } else if ((entry = out_.find(kIcVoice, name)) != -1) {
            v.handle = out_.play(SoundOut::Kind::Voice, kIcVoice, entry, loop == 0 ? 1 : 0, lv);
        } else {
            game_.noteMissing("voice " + name);
        }
    } else {
        if (skipping()) stopVoice(ch, 0);
        if (out_.find(kVoice, name) == -1 && out_.find(kIcVoice, name) == -1) game_.noteMissing("voice " + name);
        store();
    }
    // The patch's hook on this function: a voice may start a subtitle.
    if (onTrigger) onTrigger(num, 1);
}

bool Sound::voiceDone(int ch, int mode, int force) const {
    // VW (0x40ec40)
    bool free = ch < 0 || ch >= static_cast<int>(voice_.size()) || voice_[static_cast<size_t>(ch)].handle == -1;
    if (force) return free;
    const auto& s = game_.settings;
    if (mode == 0) return s.voice == 1 ? free : true;
    if (game_.modes.demo == 0 && s.voice == 1 && !game_.keys.advance && !skipping()) return free;
    return true;
}

void Sound::stopVoice(int ch, int fade) {
    // VS (0x40eb80)
    if (ch < 0 || ch >= static_cast<int>(voice_.size())) return;
    if (fade != 0) {
        fadeVoice(ch, fade, 0);
        voice_[static_cast<size_t>(ch)].stopAfter = true;
        return;
    }
    // Channel 0 ending brings a ducked breath loop of the same character back over 30 frames.
    const VoiceChannel& one = voice_[1];
    if (ch == 0 && one.handle != -1 && voice_[0].chara == one.chara && !skipping() && one.base > 1) {
        fadeVoice(1, 30, one.base);
    }
    clearVoice(ch);
}

void Sound::fadeVoice(int ch, int frames, int target) {
    // 0x4061b0: a channel that is fading out to stop takes no new fade.
    if (ch < 0 || ch >= static_cast<int>(voice_.size())) return;
    VoiceChannel& v = voice_[static_cast<size_t>(ch)];
    if (v.stopAfter) return;
    if (target > 1) v.base = target;
    v.target = target;
    v.start = v.cur;
    v.c = 0;
    v.d = frames;
}

void Sound::playVoiceMessage(int chara, int vol) {
    // 9500_000{c - 1}_{c}, once, at all_vol * voice_vol * vol; the channel's levels set to vol. Not the
    // play function the patch hooks: no subtitle.
    const auto& s = game_.settings;
    int lv = std::clamp(((s.allVol * s.voiceVol * vol) >> 8) >> 8, 0, 0xff);
    VoiceChannel& v = voice_[0];
    freeSound(v.handle);
    v.cur = v.target = v.start = vol;
    std::string name = format("9500_%04d_%02d.ogg", chara - 1, chara);
    int entry = out_.find(kVoice, name);
    if (entry == -1) return game_.noteMissing("voice " + name);
    v.handle = out_.play(SoundOut::Kind::Voice, kVoice, entry, 1, lv);
}

void Sound::restoreDucked() {
    // 0x40eb50
    if (voice_[0].handle == -1 && voice_[1].handle != -1 && voice_[1].base != 0) fadeVoice(1, 30, voice_[1].base);
}

// ---- Frames ----------------------------------------------------------------------------------------

void Sound::stopAll() {
    // 0x413ac0: effects, the BGM, the voices.
    stopAllSe();
    stopBgm(0);
    for (int ch = 0; ch < static_cast<int>(voice_.size()); ++ch) stopVoice(ch, 0);
}

void Sound::update() {
    // 0x40fb80: 0x40f630 (voices, one-shot effects, SEP channels), then the BGM channels.
    const auto& s = game_.settings;
    for (int ch = 0; ch < static_cast<int>(voice_.size()); ++ch) {
        VoiceChannel& v = voice_[static_cast<size_t>(ch)];
        if (v.handle == -1) continue;
        if (!out_.playing(v.handle)) {
            stopVoice(ch, 0);  // a finished voice frees its channel
            continue;
        }
        int frames = voiceFrames(v.d);
        if (frames == 0 || frames <= v.c) {
            v.cur = v.target;
            if (v.target == 0 && v.stopAfter) {
                stopVoice(ch, 0);
                continue;
            }
        } else {
            v.cur = ((frames - v.c) * v.start + v.target * v.c) / frames;
            ++v.c;
        }
        out_.setLevel(v.handle, level(v.cur, s.voiceVol));
    }
    for (int& h : oneShots_) {
        if (h != -1 && !out_.playing(h)) freeSound(h);
    }
    for (SepSlot& slot : sep_) {
        if (!slot.inUse || slot.preloaded || s.se == 0 || s.allSound == 0) continue;
        if (!out_.playing(slot.handle)) {
            freeSound(slot.handle);
            slot = SepSlot{};
            continue;
        }
        int frames = fadeFrames(slot.d);
        if (frames == 0 || frames <= slot.c) {
            slot.cur = slot.target;
            if (slot.stopAfter) {
                // The engine returns from 0x40f630 here: the later channels wait a frame.
                freeSound(slot.handle);
                slot = SepSlot{};
                break;
            }
        } else {
            slot.cur = ((frames - slot.c) * slot.start + slot.target * slot.c) / frames;
            ++slot.c;
        }
        if (slot.handle != -1) out_.setLevel(slot.handle, level(slot.cur, s.seVol));
    }
    for (BgmChannel& c : bgm_) {
        int frames = fadeFrames(c.d);
        auto step = [&] {
            if (frames == 0 || frames <= c.c) {
                c.cur = c.target;
                c.state = 1;
            } else {
                c.cur = (c.start * (frames - c.c) + c.target * c.c) / frames;
                ++c.c;
            }
        };
        if (c.state == 3 || c.state == 5) {
            step();
        } else if (c.state == 4) {
            if (c.c < frames) {
                step();
            } else {
                c.track = -1;
                c.state = 0;
                freeSound(c.handle);
            }
        }
        if (c.state != 0 && c.handle != -1) out_.setLevel(c.handle, level(c.cur, s.bgmVol));
    }
}

// ---- The options' switches -------------------------------------------------------------------------

void Sound::switchBgm(bool on, bool all) {
    // 0x40faa0: on, the current track again over 60 frames (forced when its channel has no sound); off,
    // a 60-frame fade-out.
    auto& s = game_.settings;
    if (!all) s.bgm = on ? 1 : 0;
    if (on && s.bgm != 0) {
        playBgm(bgmTrack_, 0x3c, bgmLoop_, bgmVolume_, bgm_[static_cast<size_t>(bgmCur_)].handle == -1 ? 1 : 0, 0);
        return;
    }
    bgmFadeOut(bgmCur_, 0x3c);
}

void Sound::switchSe(bool on, bool all) {
    // 0x40f510: on, the looping SEP channels start again; off, the others stop and the looping ones
    // fall silent (kept for later).
    auto& s = game_.settings;
    if (!all) {
        if (s.se == (on ? 1 : 0)) return;
        s.se = on ? 1 : 0;
    }
    if (on && s.se != 0) {
        for (SepSlot& slot : sep_) {
            if (slot.inUse && slot.loop != 0) slot.handle = loadSe(slot.num, slot.loop, slot.cur, 0);
        }
        return;
    }
    for (int i = 0; i < static_cast<int>(sep_.size()); ++i) {
        SepSlot& slot = sep_[static_cast<size_t>(i)];
        if (!slot.inUse) continue;
        if (slot.loop == 0) ses(i, 0);
        else freeSound(slot.handle);
    }
}

void Sound::switchVoice(bool on, bool all) {
    // 0x40eb10 -> 0x406210: every playing voice fades to silence, over 0 frames (voices off), 1 (all
    // sound off) or 255 (all sound on).
    int frames = 0;
    if (!all) {
        game_.settings.voice = on ? 1 : 0;
        if (on) return;
    } else {
        frames = on ? 0xff : 1;
    }
    for (int ch = 0; ch < static_cast<int>(voice_.size()); ++ch) {
        if (voice_[static_cast<size_t>(ch)].handle != -1) fadeVoice(ch, frames, 0);
    }
}

// ---- Saves ------------------------------------------------------------------------------------------

void Sound::capture(SaveRecord& r) const {
    // 0x4234c0 [0x3bf..0x436]
    if (bgmTrack_ == -1) r.bgm[0] = {-1, 0, 0, 0};
    else r.bgm[0] = {bgmTrack_, 1, bgmLoop_, bgmVolume_};
    r.bgm[1] = {-1, 0, 0, 0};
    for (size_t ch = 2; ch < 4; ++ch) r.bgm[ch] = {bgm_[ch].track, bgm_[ch].state, bgm_[ch].loop, bgm_[ch].target};
    for (size_t i = 0; i < sep_.size(); ++i) r.sep[i] = {sep_[i].inUse, sep_[i].num, sep_[i].loop, sep_[i].target};
    for (size_t ch = 0; ch < voice_.size(); ++ch) {
        const VoiceChannel& v = voice_[ch];
        if (v.loop == 0) r.voices[ch] = {};
        else r.voices[ch] = {true, v.chara, v.scene, v.num, v.start, v.base};
    }
    r.eroMode = eroMode;
    r.viScene = viScene_;
    r.vaOffset = vaOffset_;
    r.voiceSpeaker = voiceSpeaker;
}

void Sound::restore(const SaveRecord& r) {
    // 0x423d10: channels 0 and 1 until one plays (a missing one stops the BGM, fade -1), then 2 and 3.
    for (size_t ch = 0; ch < 2; ++ch) {
        const SaveRecord::Bgm& b = r.bgm[ch];
        if (b.track == -1) {
            stopBgm(-1);
        } else if (b.state != 0 && b.state != 4) {
            playBgm(b.track, -2, b.loop, b.vol, 1, 0);
            break;
        }
    }
    for (size_t ch = 2; ch < 4; ++ch) {
        const SaveRecord::Bgm& b = r.bgm[ch];
        if (b.track != -1 && b.state != 0 && b.state != 4) bgmStart(static_cast<int>(ch), b.track, -2, b.loop, b.vol, 1, 0);
    }
    stopAllSe();
    for (int mode = 1; mode <= 2; ++mode) {
        for (size_t i = 0; i < r.sep.size(); ++i) {
            const SaveRecord::Sep& s = r.sep[i];
            if (s.inUse && s.loop != 0) sep(static_cast<int>(i), s.num, -2, 1, s.vol, mode);
        }
    }
    for (int ch = 0; ch < 4; ++ch) stopVoice(ch, 0);
    for (size_t ch = 0; ch < r.voices.size(); ++ch) {
        const SaveRecord::Voice& v = r.voices[ch];
        if (v.on) playVoice(static_cast<int>(ch), v.chara, v.scene, v.num, v.vol, 1, 1);
    }
    eroMode = r.eroMode;
    viScene_ = r.viScene;
    vaOffset_ = r.vaOffset;
}

}  // namespace nyan::wa2
