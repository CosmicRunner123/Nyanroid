#include "nyan/audio/mixer.hpp"

#include <algorithm>
#include <cmath>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace nyan::audio {

struct Mixer::Track {
    Bytes data;  // must outlive `vorbis`
    stb_vorbis* vorbis = nullptr;
    std::function<int(float*, int)> pull;  // a decoded stream instead of `vorbis` (movie sound)
    int srcRate = 44100;
    int srcChannels = 2;  // 1 (decoded as mono, duplicated) or 2 (stereo; more is downmixed)
    bool loop = false;
    bool finished = false;

    // Decoded stereo frames not yet consumed, and the fractional read position into them.
    std::vector<float> buf;
    size_t bufFrames = 0;
    double pos = 0;

    float volume = 1.0f;
    float panL = 1.0f, panR = 1.0f;  // setPan: per-side gain
    float gain = 1.0f, gainTarget = 1.0f, gainStep = 0.0f;  // setGain
    float target = 1.0f;
    float step = 0.0f;  // volume change per output frame
    bool stopAtTarget = false;
    bool paused = false;  // setPaused: held where it is

    ~Track() {
        if (vorbis) stb_vorbis_close(vorbis);
    }

    // Ensures at least frame index `need` (plus one for interpolation) is buffered.
    bool fill(size_t need) {
        while (bufFrames <= need + 1 && !finished) {
            float chunk[4096 * 2];
            // stb_vorbis zero-fills extra channels instead of upmixing, so decode mono as mono.
            int got = pull ? pull(chunk, 4096)
                           : stb_vorbis_get_samples_float_interleaved(vorbis, srcChannels, chunk, 4096 * srcChannels);
            if (got <= 0) {
                if (loop && vorbis && stb_vorbis_seek_start(vorbis)) continue;
                finished = true;
                break;
            }
            if (srcChannels == 1) {
                for (int i = got - 1; i >= 0; --i) chunk[i * 2] = chunk[i * 2 + 1] = chunk[i];
            }
            buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(bufFrames * 2), chunk, chunk + got * 2);
            bufFrames += static_cast<size_t>(got);
        }
        return bufFrames > need;
    }

    void compact() {
        size_t drop = static_cast<size_t>(pos);
        if (drop == 0) return;
        drop = std::min(drop, bufFrames);
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(drop * 2));
        bufFrames -= drop;
        pos -= static_cast<double>(drop);
    }
};

Mixer::Mixer(int outputRate) : rate_(outputRate) {}
Mixer::~Mixer() = default;

void Mixer::fadeTo(Track& t, float target, uint32_t ms, bool stopAtEnd) {
    t.target = target;
    t.stopAtTarget = stopAtEnd;
    if (ms == 0) {
        t.volume = target;
        t.step = 0;
    } else {
        t.step = (target - t.volume) / (static_cast<float>(ms) * rate_ / 1000.0f);
    }
}

void Mixer::play(Bus bus, int channel, Bytes ogg, bool loop, uint32_t fadeInMs, float volume, uint32_t crossfadeMs) {
    if (channel < 0 || channel >= kChannelsPerBus) return;
    auto track = std::make_unique<Track>();
    track->data = std::move(ogg);
    int err = 0;
    track->vorbis = stb_vorbis_open_memory(track->data.data(), static_cast<int>(track->data.size()), &err, nullptr);
    if (!track->vorbis) return;
    stb_vorbis_info info = stb_vorbis_get_info(track->vorbis);
    track->srcRate = static_cast<int>(info.sample_rate);
    track->srcChannels = info.channels == 1 ? 1 : 2;
    track->loop = loop;
    track->buf.reserve(8192 * 2);
    track->volume = track->target = std::clamp(volume, 0.0f, 1.0f);
    std::lock_guard lock(mu_);
    auto& slot = tracks_[static_cast<size_t>(bus)][static_cast<size_t>(channel)];
    if (crossfadeMs && slot) {
        // The old sound leaves the channel and fades out while the new one fades in.
        fadeTo(*slot, 0.0f, crossfadeMs, true);
        leaving_[static_cast<size_t>(bus)].emplace_back(channel, std::move(slot));
        fadeInMs = std::max(fadeInMs, crossfadeMs);
    }
    if (fadeInMs) {
        track->volume = 0;
        fadeTo(*track, std::clamp(volume, 0.0f, 1.0f), fadeInMs, false);
    }
    slot = std::move(track);
}

void Mixer::playStream(Bus bus, int channel, int rate, std::function<int(float*, int)> pull, float volume) {
    if (channel < 0 || channel >= kChannelsPerBus || !pull || rate <= 0) return;
    auto track = std::make_unique<Track>();
    track->pull = std::move(pull);
    track->srcRate = rate;
    track->srcChannels = 2;
    track->buf.reserve(8192 * 2);
    track->volume = track->target = std::clamp(volume, 0.0f, 1.0f);
    std::lock_guard lock(mu_);
    tracks_[static_cast<size_t>(bus)][static_cast<size_t>(channel)] = std::move(track);
}

void Mixer::stop(Bus bus, int channel, uint32_t fadeOutMs) {
    std::lock_guard lock(mu_);
    auto& busTracks = tracks_[static_cast<size_t>(bus)];
    for (int c = 0; c < kChannelsPerBus; ++c) {
        if (channel >= 0 && c != channel) continue;
        auto& t = busTracks[static_cast<size_t>(c)];
        if (!t) continue;
        if (fadeOutMs == 0) t.reset();
        else fadeTo(*t, 0.0f, fadeOutMs, true);
    }
    // A stop at once also ends what a crossfade is still fading out there.
    if (fadeOutMs == 0) {
        std::erase_if(leaving_[static_cast<size_t>(bus)], [&](const auto& l) { return channel < 0 || l.first == channel; });
    }
}

void Mixer::setVolume(Bus bus, int channel, float volume, uint32_t ms, float from) {
    std::lock_guard lock(mu_);
    auto& busTracks = tracks_[static_cast<size_t>(bus)];
    for (int c = 0; c < kChannelsPerBus; ++c) {
        if (channel >= 0 && c != channel) continue;
        auto& t = busTracks[static_cast<size_t>(c)];
        if (!t || t->stopAtTarget) continue;
        if (from >= 0) t->volume = std::clamp(from, 0.0f, 1.0f);
        fadeTo(*t, std::clamp(volume, 0.0f, 1.0f), ms, false);
    }
}

void Mixer::setGain(Bus bus, int channel, float gain, uint32_t ms) {
    std::lock_guard lock(mu_);
    gain = std::clamp(gain, 0.0f, 1.0f);
    auto& busTracks = tracks_[static_cast<size_t>(bus)];
    for (int c = 0; c < kChannelsPerBus; ++c) {
        if (channel >= 0 && c != channel) continue;
        auto& t = busTracks[static_cast<size_t>(c)];
        if (!t) continue;
        t->gainTarget = gain;
        if (ms == 0) {
            t->gain = gain;
            t->gainStep = 0;
        } else {
            t->gainStep = (gain - t->gain) / (static_cast<float>(ms) * rate_ / 1000.0f);
        }
    }
}

void Mixer::setPan(Bus bus, int channel, int pan) {
    std::lock_guard lock(mu_);
    pan = std::clamp(pan, -100, 100);
    auto& busTracks = tracks_[static_cast<size_t>(bus)];
    for (int c = 0; c < kChannelsPerBus; ++c) {
        if (channel >= 0 && c != channel) continue;
        auto& t = busTracks[static_cast<size_t>(c)];
        if (!t) continue;
        t->panL = pan > 0 ? static_cast<float>(100 - pan) / 100.0f : 1.0f;
        t->panR = pan < 0 ? static_cast<float>(100 + pan) / 100.0f : 1.0f;
    }
}

void Mixer::setBusVolume(Bus bus, float volume) {
    std::lock_guard lock(mu_);
    busVolume_[static_cast<size_t>(bus)] = std::clamp(volume, 0.0f, 1.0f);
}

void Mixer::setPaused(Bus bus, int channel, bool paused) {
    std::lock_guard lock(mu_);
    auto& busTracks = tracks_[static_cast<size_t>(bus)];
    for (int c = 0; c < kChannelsPerBus; ++c) {
        if (channel >= 0 && c != channel) continue;
        if (auto& t = busTracks[static_cast<size_t>(c)]) t->paused = paused;
    }
}

bool Mixer::isPlaying(Bus bus, int channel) const {
    if (channel < 0 || channel >= kChannelsPerBus) return false;
    std::lock_guard lock(mu_);
    return tracks_[static_cast<size_t>(bus)][static_cast<size_t>(channel)] != nullptr;
}

void Mixer::render(float* out, size_t frames) {
    std::fill(out, out + frames * 2, 0.0f);
    std::lock_guard lock(mu_);
    for (size_t b = 0; b < tracks_.size(); ++b) {
        float busVol = busVolume_[b];
        for (auto& slot : tracks_[b]) {
            if (slot && mixTrack(*slot, out, frames, busVol)) slot.reset();
        }
        std::erase_if(leaving_[b], [&](auto& l) { return mixTrack(*l.second, out, frames, busVol); });
    }
    for (size_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

bool Mixer::mixTrack(Track& t, float* out, size_t frames, float busVol) {
    // Mixes one sound into `out`; true once it has finished (or faded out to stop).
    if (t.paused) return false;
    double ratio = static_cast<double>(t.srcRate) / rate_;
    bool done = false;
    for (size_t i = 0; i < frames; ++i) {
        size_t idx = static_cast<size_t>(t.pos);
        if (!t.fill(idx)) {
            done = true;
            break;
        }
        float frac = static_cast<float>(t.pos - static_cast<double>(idx));
        size_t nxt = std::min(idx + 1, t.bufFrames - 1);
        float l = t.buf[idx * 2] + (t.buf[nxt * 2] - t.buf[idx * 2]) * frac;
        float r = t.buf[idx * 2 + 1] + (t.buf[nxt * 2 + 1] - t.buf[idx * 2 + 1]) * frac;
        float v = t.volume * t.gain * busVol;
        out[i * 2] += l * v * t.panL;
        out[i * 2 + 1] += r * v * t.panR;
        t.pos += ratio;
        if (t.gainStep != 0) {
            t.gain += t.gainStep;
            if ((t.gainStep > 0 && t.gain >= t.gainTarget) || (t.gainStep < 0 && t.gain <= t.gainTarget)) {
                t.gain = t.gainTarget;
                t.gainStep = 0;
            }
        }
        if (t.step != 0) {
            t.volume += t.step;
            if ((t.step > 0 && t.volume >= t.target) || (t.step < 0 && t.volume <= t.target)) {
                t.volume = t.target;
                t.step = 0;
                if (t.stopAtTarget) {
                    done = true;
                    break;
                }
            }
        }
    }
    t.compact();
    return done;
}

}  // namespace nyan::audio
