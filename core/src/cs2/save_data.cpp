#include "nyan/cs2/save_data.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace nyan::cs2 {
namespace {

constexpr uint32_t kSaveMagic = 0x5653594E;    // "NYSV"
constexpr uint32_t kGlobalMagic = 0x4C47594E;  // "NYGL"
constexpr uint32_t kVersion = 38;  // 2: read-message flags; 3: looping SE; 4: plane sizes; 5: image filters;
                                   // 6: plane shapes, image blur;
                                   // 7: motion keyframes (quake/spline);
                                   // 8: delay, plane vram/bmode/raster/afterimages;
                                   // 9: six motion parameters, depth (`pri`), image tone;
                                   // 10: tween easing (amove), image base
                                   // 11: image modes as engine codes, with the applied conversions
                                   // 12: plane x/y rotation
                                   // 13: plane videos
                                   // 14: CG gallery (global data)
                                   // 15: novel-mode speakers per message
                                   // 16: fselect choice screen
                                   // 17: E-mote banks (a sixth image layer, image model and variables)
                                   // 18: particle layers (`particle3`)
                                   // 19: image mask channel and flips / turns
                                   // 20: E-mote bank touch switches, model scale and scale offset, body digits
                                   // 21: E-mote m2fspeed / m2prmscale values, m2iner pauses
                                   // 22: the speaker's own name line (name table key)
                                   // 23: E-mote attribute points (`m2att`)
                                   // 24: E-mote variables re-sent every frame
                                   // 25: E-mote bank rotation
                                   // 26: glossary state (global data)
                                   // 27: slot tones kept through clears (the slot records)
                                   // 28: text sizes in pixels (`\f<n>`, `\f+<n>`), span alignment
                                   // 29: sound channels' `vol` levels
                                   // 30: `drawdef` / `wipedef` defaults
                                   // 31: text config values (`font`, global data)
                                   // 32: message text word groups
                                   // 33: raster fades
                                   // 34: text spans' bold / italic (`\t`)
                                   // 35: an image's pending `mode` (sscript's record bit 64) in place
                                   //     of the mode last drawn
                                   // 36: `autoface`
                                   // 37: script particle layers (`particle` / `particle2`)
                                   // 38: `voiceoff`'s muted voice entries

class Writer {
public:
    explicit Writer(uint64_t frame = 0) : frame_(frame) {}
    Bytes& bytes() { return out_; }

    void u8(uint8_t v) { out_.push_back(v); }
    void u32(uint32_t v) {
        uint8_t b[4];
        writeLE32(b, v);
        out_.insert(out_.end(), b, b + 4);
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u64(uint64_t v) {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f32(float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        out_.insert(out_.end(), s.begin(), s.end());
    }
    void blob(const Bytes& b) {
        u32(static_cast<uint32_t>(b.size()));
        out_.insert(out_.end(), b.begin(), b.end());
    }
    // Absolute frame numbers are stored relative to the save frame.
    void time(uint64_t t) { i64(static_cast<int64_t>(t) - static_cast<int64_t>(frame_)); }

    void tween(const Tween& t) {
        i32(t.from);
        i32(t.to);
        time(t.start);
        u32(t.frames);
        u8(t.ease);
    }
    void motions(const std::vector<Motion>& ms) {
        u32(static_cast<uint32_t>(ms.size()));
        for (const auto& m : ms) {
            u8(static_cast<uint8_t>(m.kind));
            u8(m.loop);
            time(m.start);
            u32(m.period);
            for (float a : m.a) f32(a);
            u32(static_cast<uint32_t>(m.keys.size()));
            for (const auto& k : m.keys) {
                for (float v : k) f32(v);
            }
        }
    }
    void image(const StageImage& img) {
        str(img.spec);
        u32(static_cast<uint32_t>(img.parts.size()));
        for (const auto& p : img.parts) str(p);
        u8(img.solid);
        u32(img.solidColor);
        i32(img.solidWidth);
        i32(img.solidHeight);
        tween(img.x);
        tween(img.y);
        tween(img.alpha);
        u8(img.visible);
        i32(img.plane);
        tween(img.scaleX);
        tween(img.scaleY);
        motions(img.motions);
        u8(img.filter);
        str(img.filterApplied);
        u8(img.filterPending);
        i32(img.blurX);
        i32(img.blurY);
        i32(img.delay);
        f32(img.depth);
        for (int16_t v : img.tone) i32(v);
        for (int16_t v : img.toneLoaded) i32(v);
        i32(img.baseX);
        i32(img.baseY);
        str(img.emote);
        u32(static_cast<uint32_t>(img.emoteVars.size()));
        for (const auto& [k, v] : img.emoteVars) {
            str(k);
            f32(v);
        }
        i32(img.emoteX);
        i32(img.emoteY);
        u8(img.maskChannel);
        u8(img.turn);
        u8(img.touchable ? 1 : 0);
        i32(img.touchTable);
        f32(img.emoteScale);
        f32(img.emoteScaleOffX);
        f32(img.emoteScaleOffY);
        str(img.emoteBodyDif);
        str(img.emoteBody);
        f32(img.emoteSpeedAll);
        f32(img.emoteScaleAll);
        for (const auto* m : {&img.emoteSpeedVars, &img.emoteScaleVars}) {
            u32(static_cast<uint32_t>(m->size()));
            for (const auto& [k, v] : *m) {
                str(k);
                f32(v);
            }
        }
        u32(static_cast<uint32_t>(img.emoteInerPaused.size()));
        for (const auto& k : img.emoteInerPaused) str(k);
        u32(static_cast<uint32_t>(img.emoteAttributes.size()));
        for (const auto& [k, v] : img.emoteAttributes) {
            i32(k);
            for (int32_t c : v) i32(c);
        }
        u32(static_cast<uint32_t>(img.emotePinned.size()));
        for (const auto& k : img.emotePinned) str(k);
        f32(img.rotation);
    }
    void plane(const Plane& p) {
        tween(p.x);
        tween(p.y);
        i32(p.baseX);
        i32(p.baseY);
        tween(p.alpha);
        u8(p.visible);
        f32(p.rotation);
        f32(p.scaleX);
        f32(p.scaleY);
        u8(p.fill);
        u32(p.fillColor);
        i32(p.fillWidth);
        i32(p.fillHeight);
        motions(p.motions);
        i32(p.width);
        i32(p.height);
        str(p.shape);
        i32(p.delay);
        u8(p.hasVram);
        i32(p.vramX);
        i32(p.vramY);
        u8(p.blendMode);
        u8(p.raster);
        i32(p.wave.dir);
        f32(p.wave.amplitude);
        f32(p.wave.cycle);
        f32(p.wave.speed);
        time(p.wave.start);
        for (float v : {p.wave.fromAmplitude, p.wave.fromCycle, p.wave.fromSpeed}) f32(v);
        time(p.wave.fadeStart);
        u32(p.wave.fadeFrames);
        const AfterImage& a = p.after;
        for (int32_t v : {a.count, a.interval, a.mode, a.priority, a.alpha}) i32(v);
        for (float v : {a.dx, a.dy, a.scaleX, a.scaleY}) f32(v);
        time(a.start);
        f32(p.depth);
        u32(p.color);
        f32(p.rotX);
        f32(p.rotY);
        str(p.video);
        i32(p.videoPlays);
        u64(p.videoStart);
    }
    void text(const MessageText& t) {
        u32(static_cast<uint32_t>(t.spans.size()));
        for (const auto& s : t.spans) {
            str(s.text);
            u8(static_cast<uint8_t>(s.size));
            u32(s.color);
            i32(s.px);
            i32(s.dpx);
            u8(s.align);
            i32(s.group);
            u8(static_cast<uint8_t>((s.bold ? 1 : 0) | (s.italic ? 2 : 0)));
        }
        u8(t.continues);
    }
    void stage(const Stage& s) {
        for (const auto& layer : s.layers) {
            u32(static_cast<uint32_t>(layer.size()));
            for (const auto& [i, img] : layer) {
                i32(i);
                image(img);
            }
        }
        u32(static_cast<uint32_t>(s.planes.size()));
        for (const auto& [i, p] : s.planes) {
            i32(i);
            plane(p);
        }
        const auto& w = s.window;
        u8(w.visible);
        tween(w.alpha);
        str(w.style);
        u8(w.novel);
        u8(w.textVisible);
        str(w.name);
        str(w.speaker);
        text(w.text);
        u64(w.revealed);
        u32(static_cast<uint32_t>(w.novelLines.size()));
        for (const auto& l : w.novelLines) {
            u64(l.at);
            str(l.name);
        }
        u32(static_cast<uint32_t>(s.choices.size()));
        for (const auto& c : s.choices) {
            str(c.target);
            str(c.prompt);
        }
        str(s.choiceScript);
        u32(static_cast<uint32_t>(s.particles.size()));
        for (const auto& [i, p] : s.particles) {
            i32(i);
            str(p.plugin);
            str(p.texture);
            i32(p.count);
            f32(p.depth);
            i32(p.plane);
            for (float v : p.f) f32(v);
            tween(p.alpha);
            time(p.start);
            u8(p.script ? 1 : 0);
        }
        u32(static_cast<uint32_t>(s.slotTones.size()));
        for (const auto& [key, t] : s.slotTones) {
            i32(key.first);
            i32(key.second);
            for (int16_t v : t) i32(v);
        }
    }
    void vars(const VarStore& v) {
        u32(static_cast<uint32_t>(v.ints().size()));
        for (const auto& [i, x] : v.ints()) {
            i32(i);
            i32(x);
        }
        u32(static_cast<uint32_t>(v.strs().size()));
        for (const auto& [i, s] : v.strs()) {
            i32(i);
            str(s);
        }
    }
    void resume(const ScenePlayer::Resume& r) {
        str(r.scene);
        u64(r.pc);
        u8(static_cast<uint8_t>(r.state));
        str(r.title);
        u32(static_cast<uint32_t>(r.bgm.size()));
        for (const auto& [ch, name] : r.bgm) {
            i32(ch);
            str(name);
        }
        u32(static_cast<uint32_t>(r.se.size()));
        for (const auto& [ch, name] : r.se) {
            i32(ch);
            str(name);
        }
        for (const auto* m : {&r.bgmVolume, &r.seVolume}) {
            u32(static_cast<uint32_t>(m->size()));
            for (const auto& [ch, v] : *m) {
                i32(ch);
                i32(v);
            }
        }
        u32(static_cast<uint32_t>(r.channelVolume.size()));
        for (const auto& [key, v] : r.channelVolume) {
            i32(key.first);
            i32(key.second);
            i32(v);
        }
        for (int32_t v : r.transitionDefaults) i32(v);
        u8(r.autoFace ? 1 : 0);
        u32(static_cast<uint32_t>(r.voiceMuted));
        u32(static_cast<uint32_t>(r.voiceMuted >> 32));
        vars(r.vars);
        stage(r.pending);
        stage(r.shown);
    }

private:
    Bytes out_;
    uint64_t frame_;
};

class Reader {
public:
    Reader(const Bytes& data, uint64_t frame) : r_(data, "save data"), frame_(frame) {}
    uint32_t version_ = kVersion;
    ByteReader& raw() { return r_; }

    uint8_t u8() { return r_.u8(); }
    uint32_t u32() { return r_.u32(); }
    int32_t i32() { return r_.i32(); }
    uint64_t u64() {
        uint64_t lo = r_.u32();
        return lo | (static_cast<uint64_t>(r_.u32()) << 32);
    }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    float f32() {
        uint32_t u = r_.u32();
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    std::string str() {
        uint32_t n = r_.u32();
        auto s = r_.span(n);
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    }
    Bytes blob() {
        uint32_t n = r_.u32();
        auto s = r_.span(n);
        return Bytes(s.begin(), s.end());
    }
    uint64_t time() {
        int64_t t = i64() + static_cast<int64_t>(frame_);
        return t < 0 ? 0 : static_cast<uint64_t>(t);
    }
    // Element counts are bounded by what is left, so corrupt files fail cleanly.
    uint32_t count() {
        uint32_t n = r_.u32();
        if (n > r_.remaining()) fail("Corrupt save data (bad count)");
        return n;
    }

    Tween tween() {
        Tween t;
        t.from = i32();
        t.to = i32();
        t.start = time();
        t.frames = u32();
        if (version_ >= 10) t.ease = u8();
        return t;
    }
    std::vector<Motion> motions() {
        std::vector<Motion> ms(count());
        for (auto& m : ms) {
            m.kind = static_cast<Motion::Kind>(u8());
            m.loop = u8() != 0;
            m.start = time();
            m.period = u32();
            for (size_t k = 0; k < (version_ >= 9 ? 6u : 4u); ++k) m.a[k] = f32();
            if (version_ >= 7) {
                m.keys.resize(count());
                for (auto& k : m.keys) {
                    for (float& v : k) v = f32();
                }
            }
        }
        return ms;
    }
    StageImage image() {
        StageImage img;
        img.spec = str();
        img.parts.resize(count());
        for (auto& p : img.parts) p = str();
        img.solid = u8() != 0;
        img.solidColor = u32();
        img.solidWidth = i32();
        img.solidHeight = i32();
        img.x = tween();
        img.y = tween();
        img.alpha = tween();
        img.visible = u8() != 0;
        img.plane = i32();
        img.scaleX = tween();
        img.scaleY = tween();
        img.motions = motions();
        if (version_ >= 5) img.filter = u8();
        if (version_ >= 11) {
            img.filterApplied = str();
            const uint8_t pending = u8();  // before 35: the mode last drawn, nothing pending
            img.filterPending = version_ >= 35 && pending != 0;
        } else if (img.filter) {
            // Before 11: 1 sepia, 2 mono, 3 negative, applied to the image as shown.
            static constexpr uint8_t kOld[4] = {0, 2, 1, 3};
            img.filter = kOld[std::min<uint8_t>(img.filter, 3)];
            img.filterApplied = std::string(1, static_cast<char>('0' + img.filter));
        }
        if (version_ >= 6) {
            img.blurX = static_cast<int16_t>(i32());
            img.blurY = static_cast<int16_t>(i32());
        }
        if (version_ >= 8) img.delay = i32();
        if (version_ >= 9) {
            img.depth = f32();
            for (int16_t& v : img.tone) v = static_cast<int16_t>(i32());
            for (int16_t& v : img.toneLoaded) v = static_cast<int16_t>(i32());
        }
        if (version_ >= 10) {
            img.baseX = i32();
            img.baseY = i32();
        }
        if (version_ >= 17) {
            img.emote = str();
            uint32_t n = count();
            for (uint32_t k = 0; k < n; ++k) {
                std::string name = str();
                img.emoteVars[name] = f32();
            }
            img.emoteX = i32();
            img.emoteY = i32();
        }
        if (version_ >= 19) {
            img.maskChannel = u8();
            img.turn = u8();
        }
        if (version_ >= 20) {
            img.touchable = u8() != 0;
            img.touchTable = i32();
            img.emoteScale = f32();
            img.emoteScaleOffX = f32();
            img.emoteScaleOffY = f32();
            img.emoteBodyDif = str();
            img.emoteBody = str();
        }
        if (version_ >= 21) {
            img.emoteSpeedAll = f32();
            img.emoteScaleAll = f32();
            for (auto* m : {&img.emoteSpeedVars, &img.emoteScaleVars}) {
                uint32_t n = count();
                for (uint32_t k = 0; k < n; ++k) {
                    std::string key = str();
                    (*m)[key] = f32();
                }
            }
            uint32_t n = count();
            for (uint32_t k = 0; k < n; ++k) img.emoteInerPaused.push_back(str());
        }
        if (version_ >= 23) {
            uint32_t n = count();
            for (uint32_t k = 0; k < n; ++k) {
                int32_t key = i32();
                auto& v = img.emoteAttributes[key];
                for (int32_t& c : v) c = i32();
            }
        }
        if (version_ >= 24) {
            uint32_t n = count();
            for (uint32_t k = 0; k < n; ++k) img.emotePinned.insert(str());
        }
        if (version_ >= 25) img.rotation = f32();
        return img;
    }
    Plane plane() {
        Plane p;
        p.x = tween();
        p.y = tween();
        p.baseX = i32();
        p.baseY = i32();
        p.alpha = tween();
        p.visible = u8() != 0;
        p.rotation = f32();
        p.scaleX = f32();
        p.scaleY = f32();
        p.fill = u8() != 0;
        p.fillColor = u32();
        p.fillWidth = i32();
        p.fillHeight = i32();
        p.motions = motions();
        if (version_ >= 4) {
            p.width = i32();
            p.height = i32();
        }
        if (version_ >= 6) p.shape = str();
        if (version_ >= 8) {
            p.delay = i32();
            p.hasVram = u8() != 0;
            p.vramX = i32();
            p.vramY = i32();
            p.blendMode = u8();
            p.raster = u8() != 0;
            p.wave.dir = i32();
            p.wave.amplitude = f32();
            p.wave.cycle = f32();
            p.wave.speed = f32();
            p.wave.start = time();
            if (version_ >= 33) {
                for (float* v : {&p.wave.fromAmplitude, &p.wave.fromCycle, &p.wave.fromSpeed}) *v = f32();
                p.wave.fadeStart = time();
                p.wave.fadeFrames = u32();
            }
            AfterImage& a = p.after;
            for (int32_t* v : {&a.count, &a.interval, &a.mode, &a.priority, &a.alpha}) *v = i32();
            for (float* v : {&a.dx, &a.dy, &a.scaleX, &a.scaleY}) *v = f32();
            a.start = time();
        }
        if (version_ >= 9) p.depth = f32();
        if (version_ >= 10) p.color = u32();
        if (version_ >= 12) {
            p.rotX = f32();
            p.rotY = f32();
        }
        if (version_ >= 13) {
            p.video = str();
            p.videoPlays = i32();
            p.videoStart = u64();
        }
        return p;
    }
    MessageText text() {
        MessageText t;
        t.spans.resize(count());
        for (auto& s : t.spans) {
            s.text = str();
            s.size = static_cast<TextSize>(static_cast<int8_t>(u8()));
            s.color = u32();
            if (version_ >= 28) {
                s.px = static_cast<int16_t>(i32());
                s.dpx = static_cast<int16_t>(i32());
                s.align = u8();
            }
            if (version_ >= 32) {
                s.group = static_cast<int16_t>(i32());
                t.groups = std::max<int16_t>(t.groups, static_cast<int16_t>(s.group + 1));
            }
            if (version_ >= 34) {
                const uint8_t st = u8();
                s.bold = (st & 1) != 0, s.italic = (st & 2) != 0;
            }
        }
        t.continues = u8() != 0;
        return t;
    }
    Stage stage() {
        Stage s;
        for (size_t l = 0; l < s.layers.size(); ++l) {
            if (version_ < 17 && l >= 5) break;  // the E-mote layer came with version 17
            auto& layer = s.layers[l];
            uint32_t n = count();
            for (uint32_t k = 0; k < n; ++k) {
                int32_t i = i32();
                layer[i] = image();
            }
        }
        uint32_t np = count();
        for (uint32_t k = 0; k < np; ++k) {
            int32_t i = i32();
            s.planes[i] = plane();
        }
        auto& w = s.window;
        w.visible = u8() != 0;
        w.alpha = tween();
        w.style = str();
        w.novel = u8() != 0;
        w.textVisible = u8() != 0;
        w.name = str();
        w.speaker = version_ >= 22 ? str() : w.name;
        w.text = text();
        w.revealed = static_cast<size_t>(u64());
        if (version_ >= 15) {
            w.novelLines.resize(count());
            std::vector<bool> centred;
            for (auto& l : w.novelLines) {
                l.at = static_cast<size_t>(u64());
                l.name = str();
                centred.push_back(version_ < 28 && u8() != 0);
            }
            // Before version 28 a centred novel message was a flag on its line: centre its spans.
            for (size_t k = 0; k < w.novelLines.size(); ++k) {
                if (!centred[k]) continue;
                size_t to = k + 1 < w.novelLines.size() ? w.novelLines[k + 1].at : w.text.spans.size();
                for (size_t j = w.novelLines[k].at; j < std::min(to, w.text.spans.size()); ++j) w.text.spans[j].align = 1;
            }
        }
        s.choices.resize(count());
        for (auto& c : s.choices) {
            c.target = str();
            c.prompt = str();
        }
        if (version_ >= 16) s.choiceScript = str();
        if (version_ >= 18) {
            uint32_t n = count();
            for (uint32_t k = 0; k < n; ++k) {
                int32_t i = i32();
                ParticleLayer& p = s.particles[i];
                p.plugin = str();
                p.texture = str();
                p.count = i32();
                p.depth = f32();
                p.plane = i32();
                for (float& v : p.f) v = f32();
                p.alpha = tween();
                p.start = time();
                if (version_ >= 37) p.script = u8() != 0;
            }
        }
        if (version_ >= 27) {
            uint32_t n = count();
            for (uint32_t k = 0; k < n; ++k) {
                int32_t l = i32(), i = i32();
                auto& t = s.slotTones[{l, i}];
                for (int16_t& v : t) v = static_cast<int16_t>(i32());
            }
        } else {
            // Before: each image kept its slot's tone, lost with a clear.
            for (size_t l = 0; l < s.layers.size(); ++l) {
                for (const auto& [i, img] : s.layers[l]) {
                    if (img.tone != std::array<int16_t, 3>{100, 100, 100}) s.slotTones[{static_cast<int32_t>(l), i}] = img.tone;
                }
            }
        }
        return s;
    }
    VarStore vars() {
        VarStore v;
        uint32_t n = count();
        for (uint32_t k = 0; k < n; ++k) {
            int32_t i = i32();
            v.set(i, i32());
        }
        uint32_t ns = count();
        for (uint32_t k = 0; k < ns; ++k) {
            int32_t i = i32();
            v.setStr(i, str());
        }
        return v;
    }
    ScenePlayer::Resume resume(uint32_t version) {
        ScenePlayer::Resume r;
        r.scene = str();
        r.pc = static_cast<size_t>(u64());
        r.state = static_cast<ScenePlayer::State>(u8());
        r.title = str();
        uint32_t nb = count();
        for (uint32_t k = 0; k < nb; ++k) {
            int32_t ch = i32();
            r.bgm[ch] = str();
        }
        if (version >= 3) {
            uint32_t ns = count();
            for (uint32_t k = 0; k < ns; ++k) {
                int32_t ch = i32();
                r.se[ch] = str();
            }
            for (auto* m : {&r.bgmVolume, &r.seVolume}) {
                uint32_t nv = count();
                for (uint32_t k = 0; k < nv; ++k) {
                    int32_t ch = i32();
                    (*m)[ch] = i32();
                }
            }
        }
        if (version >= 29) {
            uint32_t nc = count();
            for (uint32_t k = 0; k < nc; ++k) {
                int32_t kind = i32();
                int32_t ch = i32();
                r.channelVolume[{kind, ch}] = i32();
            }
        }
        if (version >= 30) {
            for (int32_t& v : r.transitionDefaults) v = i32();
        }
        if (version >= 36) r.autoFace = u8() != 0;
        if (version >= 38) {
            const uint64_t lo = u32();
            r.voiceMuted = lo | (uint64_t{u32()} << 32);
        }
        r.vars = vars();
        r.pending = stage();
        r.shown = stage();
        return r;
    }

private:
    ByteReader r_;
    uint64_t frame_;
};

}  // namespace

Bytes encodeSave(const SaveFile& save, uint64_t frame) {
    Writer w(frame);
    w.u32(kSaveMagic);
    w.u32(kVersion);
    w.i64(save.time);
    w.u32(static_cast<uint32_t>(save.strings.size()));
    for (const auto& [mask, s] : save.strings) {
        w.u32(mask);
        w.str(s);
    }
    w.i32(save.thumbWidth);
    w.i32(save.thumbHeight);
    w.blob(save.thumbnail);
    w.resume(save.resume);
    return std::move(w.bytes());
}

SaveFile decodeSave(const Bytes& data, uint64_t frame) {
    Reader r(data, frame);
    if (r.u32() != kSaveMagic) fail("Not a Nyanroid save");
    uint32_t version = r.u32();
    if (version > kVersion) fail("Save from a newer Nyanroid");
    r.version_ = version;
    SaveFile s;
    s.time = r.i64();
    uint32_t n = r.count();
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t mask = r.u32();
        s.strings[mask] = r.str();
    }
    s.thumbWidth = r.i32();
    s.thumbHeight = r.i32();
    s.thumbnail = r.blob();
    if (s.thumbnail.size() != static_cast<size_t>(std::max(0, s.thumbWidth)) * std::max(0, s.thumbHeight) * 4) {
        s.thumbnail.clear();
        s.thumbWidth = s.thumbHeight = 0;
    }
    s.resume = r.resume(version);
    return s;
}

Bytes encodeGlobal(const GlobalData& g) {
    Writer w;
    w.u32(kGlobalMagic);
    w.u32(kVersion);
    w.u32(static_cast<uint32_t>(g.flags.size()));
    for (const auto& [i, v] : g.flags) {
        w.i32(i);
        w.i32(v);
    }
    w.u32(static_cast<uint32_t>(g.strings.size()));
    for (const auto& [i, s] : g.strings) {
        w.i32(i);
        w.str(s);
    }
    w.u32(static_cast<uint32_t>(g.config.size()));
    for (const auto& [k, v] : g.config) {
        w.str(k);
        w.i32(v);
    }
    w.u32(static_cast<uint32_t>(g.read.size()));
    for (const auto& [scene, bits] : g.read) {
        w.str(scene);
        w.blob(Bytes(bits.begin(), bits.end()));
    }
    w.u32(static_cast<uint32_t>(g.cgSeen.size()));
    for (const auto& s : g.cgSeen) w.str(s);
    w.u32(static_cast<uint32_t>(g.dictionary.size()));
    for (const auto& [k, d] : g.dictionary) {
        w.str(k);
        w.u32(d.appear);
        w.u32(d.updated);
        w.i32(d.fresh ? 1 : 0);
    }
    w.u32(static_cast<uint32_t>(g.configText.size()));
    for (const auto& [k, v] : g.configText) {
        w.str(k);
        w.str(v);
    }
    return std::move(w.bytes());
}

GlobalData decodeGlobal(const Bytes& data) {
    Reader r(data, 0);
    if (r.u32() != kGlobalMagic) fail("Not Nyanroid global data");
    uint32_t version = r.u32();
    if (version > kVersion) fail("Global data from a newer Nyanroid");
    GlobalData g;
    uint32_t n = r.count();
    for (uint32_t k = 0; k < n; ++k) {
        int32_t i = r.i32();
        g.flags[i] = r.i32();
    }
    uint32_t ns = r.count();
    for (uint32_t k = 0; k < ns; ++k) {
        int32_t i = r.i32();
        g.strings[i] = r.str();
    }
    uint32_t nc = r.count();
    for (uint32_t k = 0; k < nc; ++k) {
        std::string key = r.str();
        g.config[key] = r.i32();
    }
    if (version >= 2) {
        uint32_t nr = r.count();
        for (uint32_t k = 0; k < nr; ++k) {
            std::string scene = r.str();
            Bytes bits = r.blob();
            g.read[scene] = std::vector<uint8_t>(bits.begin(), bits.end());
        }
    }
    if (version >= 14) {
        uint32_t nc = r.count();
        for (uint32_t k = 0; k < nc; ++k) g.cgSeen.insert(r.str());
    }
    if (version >= 26) {
        uint32_t nd = r.count();
        for (uint32_t k = 0; k < nd; ++k) {
            std::string key = r.str();
            GlobalData::DictionaryState d;
            d.appear = r.u32();
            d.updated = r.u32();
            d.fresh = r.i32() != 0;
            g.dictionary[key] = d;
        }
    }
    if (version >= 31) {
        uint32_t nt = r.count();
        for (uint32_t k = 0; k < nt; ++k) {
            std::string key = r.str();
            g.configText[key] = r.str();
        }
    }
    return g;
}

namespace {
std::optional<Bytes> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

SaveStore::SaveStore(std::filesystem::path dir) : dir_(std::move(dir)) {
    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
    }
}

std::filesystem::path SaveStore::pathOf(int32_t slot) const {
    char name[32];
    std::snprintf(name, sizeof name, "save%04d.nsv", slot);
    return dir_ / name;
}

void SaveStore::writeFile(const std::filesystem::path& p, const Bytes& data) const {
    if (dir_.empty()) return;
    // Write next to the target and rename, so a crash never leaves half a save.
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
}

SaveStore::Entry& SaveStore::entry(int32_t slot) {
    Entry& e = slots_[slot];
    if (!e.loaded) {
        e.loaded = true;
        if (!dir_.empty()) {
            if (auto data = readFile(pathOf(slot))) {
                try {
                    e.file = decodeSave(*data, 0);
                    e.raw = std::move(*data);
                } catch (const Error&) {
                    e.raw.reset();  // unreadable: treated as empty
                }
            }
        }
        e.version = ++versionCounter_;
    }
    return e;
}

bool SaveStore::exists(int32_t slot) {
    return slot >= 0 && entry(slot).file.has_value();
}

const SaveFile* SaveStore::get(int32_t slot) {
    if (slot < 0) return nullptr;
    Entry& e = entry(slot);
    return e.file ? &*e.file : nullptr;
}

std::optional<ScenePlayer::Resume> SaveStore::resume(int32_t slot, uint64_t frame) {
    if (slot < 0) return std::nullopt;
    Entry& e = entry(slot);
    if (!e.raw) return std::nullopt;
    try {
        return decodeSave(*e.raw, frame).resume;
    } catch (const Error&) {
        return std::nullopt;
    }
}

void SaveStore::put(int32_t slot, const SaveFile& save, uint64_t frame) {
    if (slot < 0) return;
    Bytes data = encodeSave(save, frame);
    Entry& e = slots_[slot];
    e.loaded = true;
    e.file = decodeSave(data, 0);
    e.raw = data;
    e.version = ++versionCounter_;
    writeFile(pathOf(slot), data);
}

void SaveStore::remove(int32_t slot) {
    if (slot < 0) return;
    Entry& e = slots_[slot];
    e.loaded = true;
    e.file.reset();
    e.raw.reset();
    e.version = ++versionCounter_;
    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove(pathOf(slot), ec);
    }
}

void SaveStore::exchange(int32_t a, int32_t b) {
    if (a < 0 || b < 0 || a == b) return;
    Entry ea = entry(a), eb = entry(b);
    auto move = [&](int32_t slot, const Entry& from) {
        if (from.raw) {
            Entry& e = slots_[slot];
            e = from;
            e.version = ++versionCounter_;
            writeFile(pathOf(slot), *from.raw);
        } else {
            remove(slot);
        }
    };
    move(a, eb);
    move(b, ea);
}

void SaveStore::copy(int32_t from, int32_t to) {
    if (from < 0 || to < 0 || from == to) return;
    Entry src = entry(from);
    if (!src.raw) return;
    Entry& e = slots_[to];
    e = src;
    e.version = ++versionCounter_;
    writeFile(pathOf(to), *src.raw);
}

int32_t SaveStore::newest(int32_t from, int32_t to) {
    int32_t best = -1;
    int64_t bestTime = 0;
    for (int32_t s = from; s <= to; ++s) {
        const SaveFile* f = get(s);
        if (f && (best < 0 || f->time > bestTime)) {
            best = s;
            bestTime = f->time;
        }
    }
    return best;
}

void SaveStore::appendString(int32_t slot, uint32_t mask, const std::string& text) {
    Entry& e = entry(slot);
    if (!e.file || !e.raw) return;
    // Re-encode with the new string; the resume point inside is frame-0 based already.
    SaveFile f = decodeSave(*e.raw, 0);
    f.strings[mask] = text;
    put(slot, f, 0);
}

uint64_t SaveStore::version(int32_t slot) const {
    auto it = slots_.find(slot);
    return it == slots_.end() ? 0 : it->second.version;
}

std::optional<GlobalData> SaveStore::loadGlobal() {
    if (dir_.empty()) return std::nullopt;
    auto data = readFile(dir_ / "global.nsv");
    if (!data) return std::nullopt;
    try {
        return decodeGlobal(*data);
    } catch (const Error&) {
        return std::nullopt;
    }
}

void SaveStore::saveGlobal(const GlobalData& g) {
    writeFile(dir_ / "global.nsv", encodeGlobal(g));
}

}  // namespace nyan::cs2
