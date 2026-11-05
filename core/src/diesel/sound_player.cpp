#include "nyan/diesel/sound_player.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "nyan/audio/mixer.hpp"
#include "nyan/media/ffmpeg_video.hpp"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace nyan::diesel {

// The reverb: XAudio2's own DSP when the private companion has its port (which is not published),
// else FAudio's re-implementation (core/CMakeLists.txt chooses).
#if defined(NYAN_XAUDIO2_REVERB)
#include NYAN_XAUDIO2_REVERB
#else
#include "faudio_reverb.inc"
#endif

namespace {

constexpr size_t kCachedFiles = 48;

audio::Mixer::Bus busFor(int32_t type) {
    switch (type) {
    case 0: return audio::Mixer::Bus::Bgm;
    case 1:
    case 5: return audio::Mixer::Bus::Voice;
    case 4: return audio::Mixer::Bus::Movie;
    default: return audio::Mixer::Bus::Se;
    }
}

}  // namespace

// One XAudio2 voice: the Ogg decoded on the audio thread, its plays and loop segment, the frequency
// ratio as a resampling step, Mware's pan matrix and the device amplitude (ramped over each block).
struct SoundPlayer::VoiceState {
    std::shared_ptr<const Bytes> data;
    stb_vorbis* vorbis = nullptr;
    std::function<int(float*, int)> stream;  // a movie's sound: stereo frames, looping as asked
    int64_t skip = 0;                        // its frames before the start
    int channels = 2, rate = 44100;
    int64_t total = 0;
    int64_t loopStart = 0, loopEnd = 0;  // samples
    int32_t playsLeft = 1;
    // A setLoop while playing (SoundEvent::Loop), taken up by the audio thread before its next
    // piece: the plays and the loop (milliseconds).
    std::atomic<bool> loopChanged{false};
    std::atomic<int32_t> newPlays{1}, newLoopStartMs{0}, newLoopEndMs{0};
    int64_t passStart = 0;  // where the current pass started (samples)
    int64_t decodeAt = 0;
    bool ended = false;
    std::vector<float> buf;  // decoded frames, two floats each (a mono frame twice)
    double pos = 0.0;
    float lastGain = -1.0f;
    std::atomic<float> gain{0.0f}, pan{0.0f}, frequency{1.0f};
    std::atomic<float> echoWet{0.0f}, echoFeedback{0.0f}, echoDelayMs{1.0f};
    std::vector<float> echoLine;  // the echo's delay line (two floats a frame), the audio thread's
    size_t echoAt = 0;
    // The reverb (XAudio2's, effect 2: see the include above; XAudio2 2.9's or 2.7's), made when the play
    // turns it on. It runs in XAudio2's 10 ms passes (rate / 100 frames), made ahead of what is
    // pulled; the pass the voice ends in is filled with silence and cut where the voice ends
    // (Fitted: XAudio2 would go on until the voice is stopped).
    std::unique_ptr<xa2reverb::Reverb> reverb;
    std::vector<float> passIn, passWet, pass;  // a pass in, out (the voice's channels), out as frames
    size_t passAt = 0;
    std::vector<float> pre;  // the frames before the volume and the matrix (two floats a frame)
    std::atomic<bool> paused{false}, stopped{false};

    ~VoiceState() {
        if (vorbis) stb_vorbis_close(vorbis);
    }

    // Decodes the next block of the current play; at the segment's end the next play starts at the
    // loop start (Mware 0x10030c60: -1 plays for ever, 0 and 1 once, n n passes; the last pass runs
    // to the file's end, as does one that started beyond the loop's end, which then starts again at
    // 0). False when nothing more comes.
    bool decode() {
        if (stream) {
            float tmp[1024 * 2];
            while (!ended) {
                int got = stream(tmp, 1024);
                if (got <= 0) {
                    ended = true;
                    return false;
                }
                int from = 0;
                if (skip > 0) {
                    from = static_cast<int>(std::min<int64_t>(skip, got));
                    skip -= from;
                    if (from == got) continue;
                }
                buf.insert(buf.end(), tmp + from * 2, tmp + got * 2);
                return true;
            }
            return false;
        }
        while (!ended) {
            if (loopChanged.exchange(false, std::memory_order_acquire)) {
                playsLeft = newPlays.load();
                loopStart = static_cast<int64_t>(newLoopStartMs.load()) * rate / 1000;
                loopEnd = static_cast<int64_t>(newLoopEndMs.load()) * rate / 1000;
            }
            int64_t segEnd = loopStart == 0 && loopEnd == 0 ? total : std::min(loopEnd, total);
            bool last = playsLeft != -1 && playsLeft <= 1;
            bool past = passStart > segEnd;
            int64_t end = last || past ? total : segEnd;
            int64_t left = end - decodeAt;
            if (left <= 0) {
                if (last) {
                    ended = true;
                    return false;
                }
                if (playsLeft != -1) --playsLeft;
                int64_t start = past ? 0 : (loopStart >= 0 && loopStart < end ? loopStart : 0);
                passStart = start;
                if (end - start <= 0 || !stb_vorbis_seek(vorbis, static_cast<unsigned int>(start))) {
                    ended = true;
                    return false;
                }
                decodeAt = start;
                continue;
            }
            float tmp[1024 * 8];
            int want = static_cast<int>(std::min<int64_t>(left, 1024));
            int got = stb_vorbis_get_samples_float_interleaved(vorbis, channels, tmp, want * channels);
            if (got <= 0) {
                decodeAt = end;  // the stream ran short of its length
                continue;
            }
            size_t at = buf.size();
            buf.resize(at + static_cast<size_t>(got) * 2);
            for (int i = 0; i < got; ++i) {
                buf[at + static_cast<size_t>(i) * 2] = tmp[i * channels];
                buf[at + static_cast<size_t>(i) * 2 + 1] = tmp[i * channels + (channels > 1 ? 1 : 0)];
            }
            decodeAt += got;
            return true;
        }
        return false;
    }

    // Up to n frames after the resampling and the voice's echo (two floats a frame; a mono voice's
    // twice); fewer when the voice runs out.
    int produce(float* dst, int n, float step) {
        float wet = echoWet.load(std::memory_order_relaxed), fb = echoFeedback.load(std::memory_order_relaxed);
        size_t echoFrames = 0;
        if (wet > 0.0f) {
            echoFrames = std::max<size_t>(1, static_cast<size_t>(std::lround(echoDelayMs.load(std::memory_order_relaxed) *
                                                                              static_cast<float>(rate) / 1000.0f)));
            if (echoLine.size() != echoFrames * 2) {
                echoLine.assign(echoFrames * 2, 0.0f);
                echoAt = 0;
            }
        }
        int done = 0;
        for (; done < n; ++done) {
            size_t idx = static_cast<size_t>(pos);
            while ((idx + 1) * 2 >= buf.size() && decode()) {
            }
            if (idx * 2 + 1 >= buf.size()) break;
            size_t nxt = std::min(idx + 1, buf.size() / 2 - 1);
            float frac = static_cast<float>(pos - static_cast<double>(idx));
            float sl = buf[idx * 2] + (buf[nxt * 2] - buf[idx * 2]) * frac;
            float sr = buf[idx * 2 + 1] + (buf[nxt * 2 + 1] - buf[idx * 2 + 1]) * frac;
            // The voice's effect chain runs after the resampling, before the volume and the matrix:
            // the echo (Fitted: the output is (1 - mix) x the input + mix x the line, which takes the
            // input plus feedback x what it gives back, the delay late).
            if (wet > 0.0f) {
                if (echoAt >= echoFrames) echoAt = 0;
                float dl = echoLine[echoAt * 2], dr = echoLine[echoAt * 2 + 1];
                echoLine[echoAt * 2] = sl + fb * dl;
                echoLine[echoAt * 2 + 1] = sr + fb * dr;
                sl = (1.0f - wet) * sl + wet * dl;
                sr = (1.0f - wet) * sr + wet * dr;
                ++echoAt;
            }
            dst[done * 2] = sl;
            dst[done * 2 + 1] = sr;
            pos += step;
        }
        return done;
    }

    // Then the reverb, a pass at a time.
    int produceWithReverb(float* dst, int n, float step) {
        int done = 0;
        while (done < n) {
            if (passAt * 2 >= pass.size()) {
                const size_t q = static_cast<size_t>(std::max(1, rate / 100));
                const size_t ch = channels == 1 ? 1 : 2;
                pass.resize(q * 2);
                int got = produce(pass.data(), static_cast<int>(q), step);
                if (got <= 0) break;
                passIn.assign(q * ch, 0.0f);
                passWet.resize(q * ch);
                for (int k = 0; k < got; ++k) {
                    passIn[k * ch] = pass[k * 2];
                    if (ch == 2) passIn[k * 2 + 1] = pass[k * 2 + 1];
                }
                reverb->process(passIn.data(), passWet.data(), static_cast<uint32_t>(q), true);
                pass.resize(static_cast<size_t>(got) * 2);
                for (int k = 0; k < got; ++k) {
                    pass[k * 2] = passWet[k * ch];
                    pass[k * 2 + 1] = passWet[k * ch + (ch == 2 ? 1 : 0)];
                }
                passAt = 0;
            }
            size_t take = std::min(pass.size() / 2 - passAt, static_cast<size_t>(n - done));
            std::copy(pass.begin() + static_cast<std::ptrdiff_t>(passAt * 2),
                      pass.begin() + static_cast<std::ptrdiff_t>((passAt + take) * 2), dst + done * 2);
            passAt += take;
            done += static_cast<int>(take);
        }
        return done;
    }

    int pull(float* out, int frames) {
        if (stopped.load(std::memory_order_relaxed)) return 0;
        if (paused.load(std::memory_order_relaxed)) {
            std::fill(out, out + static_cast<size_t>(frames) * 2, 0.0f);
            return frames;
        }
        float step = std::clamp(frequency.load(std::memory_order_relaxed), 1.0f / 1024.0f, 4.0f);
        float target = gain.load(std::memory_order_relaxed);
        if (lastGain < 0.0f) lastGain = target;
        // Mware's output matrix for the pan (0x100325b0).
        float p = std::clamp(pan.load(std::memory_order_relaxed), -1.0f, 1.0f);
        float L = p > 0.0f ? 1.0f - p : 1.0f, R = p < 0.0f ? 1.0f + p : 1.0f;
        float a = 1.0f - R, b = 1.0f - L;
        float n1 = a + L > 1.0f ? 1.0f / (a + L) : 1.0f, n2 = b + R > 1.0f ? 1.0f / (b + R) : 1.0f;
        pre.resize(static_cast<size_t>(frames) * 2);
        int done = reverb ? produceWithReverb(pre.data(), frames, step) : produce(pre.data(), frames, step);
        for (int k = 0; k < done; ++k) {
            float sl = pre[k * 2], sr = pre[k * 2 + 1];
            float g = lastGain + (target - lastGain) * (static_cast<float>(k + 1) / static_cast<float>(frames));
            float ol, orr;
            if (channels == 1) {
                ol = sl * L, orr = sl * R;
            } else {
                ol = L * n1 * sl + a * n1 * sr;
                orr = b * n2 * sl + R * n2 * sr;
            }
            out[k * 2] = ol * g;
            out[k * 2 + 1] = orr * g;
        }
        lastGain = target;
        size_t drop = std::min(static_cast<size_t>(pos), buf.size() / 2);
        if (drop > 4096) {
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(drop * 2));
            pos -= static_cast<double>(drop);
        }
        return done;
    }
};

SoundPlayer::SoundPlayer(audio::Mixer* mixer, ReadFile read, OpenFile open)
    : mixer_(mixer), read_(std::move(read)), open_(std::move(open)) {}

SoundPlayer::~SoundPlayer() { stopAll(); }

std::shared_ptr<const Bytes> SoundPlayer::file(const std::string& path) {
    if (auto it = files_.find(path); it != files_.end()) return it->second;
    auto data = read_(path);
    if (!data) return nullptr;
    auto shared = std::make_shared<const Bytes>(std::move(*data));
    files_[path] = shared;
    fileOrder_.push_back(path);
    if (fileOrder_.size() > kCachedFiles) {
        files_.erase(fileOrder_.front());
        fileOrder_.erase(fileOrder_.begin());
    }
    return shared;
}

void SoundPlayer::start(const SoundEvent& e) {
    stop(e.voice);
    auto st = std::make_shared<VoiceState>();
    if (e.movie) {
        // The movie's own reader: its sound from the start, the frames before startMs dropped,
        // repeating while the movie has passes left (the last pass ends with a stop).
        auto f = open_ ? open_(e.file) : nullptr;
        auto movie = f ? media::FfmpegVideo::open(f) : nullptr;
        if (!movie || !movie->hasAudio()) return;
        st->stream = movie->audioSource(e.plays != 1);
        st->rate = movie->sampleRate();
        st->skip = static_cast<int64_t>(e.startMs) * st->rate / 1000;
    } else {
        auto data = file(e.file);
        if (!data) return;
        st->data = data;
        int err = 0;
        st->vorbis = stb_vorbis_open_memory(data->data(), static_cast<int>(data->size()), &err, nullptr);
        if (!st->vorbis) return;
        stb_vorbis_info info = stb_vorbis_get_info(st->vorbis);
        st->channels = std::clamp(info.channels, 1, 8);
        st->rate = static_cast<int>(info.sample_rate);
        st->total = stb_vorbis_stream_length_in_samples(st->vorbis);
        st->loopStart = static_cast<int64_t>(e.loopStartMs) * st->rate / 1000;
        st->loopEnd = static_cast<int64_t>(e.loopEndMs) * st->rate / 1000;
        st->playsLeft = e.plays;
        int64_t from = static_cast<int64_t>(e.startMs) * st->rate / 1000;
        if (from > 0 && from < st->total && stb_vorbis_seek(st->vorbis, static_cast<unsigned int>(from))) st->decodeAt = st->passStart = from;
        // The reverb: Mware makes one for a voice at 48 kHz or below (0x10031413), which XAudio2
        // takes from 20 kHz (LockForProcess: 2.9 0x10070500, 2.7's format check 0x100477f3), mono
        // or stereo in and out; each play sets it from the preset and turns it on, or leaves it
        // off for 0 (off, it passes the sound through, so Nyanroid makes none). The game's
        // XAudio2 picks whose reverb. Guess: a preset beyond the table's 30 leaves it off (Mware
        // would read past the table).
        if (e.reverbPreset >= 1 && e.reverbPreset < xa2reverb::kPresetCount && st->rate >= 20000 && st->rate <= 48000 &&
            (st->channels == 1 || st->channels == 2)) {
            st->reverb = std::make_unique<xa2reverb::Reverb>(st->channels, static_cast<float>(st->rate), e.xaudioVersion,
                                                             e.xaudio64);
            st->reverb->setParameters(xa2reverb::convertPreset(e.reverbPreset));
        }
    }

    // A free channel of the bus, else the one that started longest ago.
    auto bus = busFor(e.type);
    int channel = -1;
    for (int c = 0; c < audio::Mixer::kChannelsPerBus && channel < 0; ++c) {
        bool used = false;
        for (const auto& [k, pl] : playing_) used = used || (pl.bus == static_cast<int>(bus) && pl.channel == c);
        if (!used) channel = c;
    }
    if (channel < 0) {
        auto oldest = playing_.end();
        for (auto it = playing_.begin(); it != playing_.end(); ++it) {
            if (it->second.bus == static_cast<int>(bus) && (oldest == playing_.end() || it->second.started < oldest->second.started)) oldest = it;
        }
        channel = oldest->second.channel;
        stop(oldest->first);
    }
    Playing pl{st, static_cast<int>(bus), channel, ++starts_};
    playing_[e.voice] = pl;
    mixer_->playStream(bus, channel, st->rate, [st](float* out, int n) { return st->pull(out, n); }, 1.0f);
}

void SoundPlayer::stop(uint32_t voice) {
    auto it = playing_.find(voice);
    if (it == playing_.end()) return;
    it->second.state->stopped = true;
    mixer_->stop(static_cast<audio::Mixer::Bus>(it->second.bus), it->second.channel);
    playing_.erase(it);
}

void SoundPlayer::stopAll() {
    while (!playing_.empty()) stop(playing_.begin()->first);
}

void SoundPlayer::apply(const std::vector<SoundEvent>& events, const std::vector<SoundVoice>& voices) {
    if (!mixer_) return;
    for (const auto& e : events) {
        switch (e.kind) {
        case SoundEvent::Kind::Play: start(e); break;
        case SoundEvent::Kind::Stop: stop(e.voice); break;
        case SoundEvent::Kind::Pause:
        case SoundEvent::Kind::Resume:
            if (auto it = playing_.find(e.voice); it != playing_.end()) {
                it->second.state->paused = e.kind == SoundEvent::Kind::Pause;
            }
            break;
        case SoundEvent::Kind::Loop:
            if (auto it = playing_.find(e.voice); it != playing_.end()) {
                auto& st = *it->second.state;
                st.newPlays = e.plays;
                st.newLoopStartMs = e.loopStartMs;
                st.newLoopEndMs = e.loopEndMs;
                st.loopChanged.store(true, std::memory_order_release);
            }
            break;
        }
    }
    // The settings of the voices still playing; one the device let end is let go.
    std::vector<uint32_t> gone;
    for (auto& [key, pl] : playing_) {
        auto v = std::find_if(voices.begin(), voices.end(), [&](const SoundVoice& s) { return s.voice == key; });
        if (v == voices.end()) {
            gone.push_back(key);
            continue;
        }
        pl.state->gain = v->gain;
        pl.state->pan = v->pan;
        pl.state->frequency = v->frequency;
        pl.state->paused = v->paused;
        pl.state->echoWet = v->echoWet;
        pl.state->echoFeedback = v->echoFeedback;
        pl.state->echoDelayMs = v->echoDelayMs;
    }
    for (uint32_t key : gone) stop(key);
}

}  // namespace nyan::diesel
