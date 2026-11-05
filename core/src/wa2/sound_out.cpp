#include "nyan/wa2/sound_out.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "nyan/audio/mixer.hpp"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace nyan::wa2 {

namespace {

constexpr int kFramesPerTick = 60;  // the engine's logic rate

bool endsWithA(const std::string& name) {
    // 0x45fad0: the six characters before the end are "_A.xxx", either case.
    if (name.size() < 6) return false;
    std::string tail = toLowerAscii(name.substr(name.size() - 6));
    return tail[0] == '_' && tail[1] == 'a' && tail[2] == '.';
}

// A RIFF/WAVE file as the engine's static-buffer loader reads it (0x45dcc0): the chunk at offset 12 is
// the format unless it is `bext`, which is skipped; the data chunk follows. PCM of 8 or 16 bits.
bool decodeWav(const Bytes& d, std::vector<float>& out, int& rate, int& channels) {
    if (d.size() < 20 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return false;
    size_t at = 12;
    auto chunkSize = [&](size_t p) { return static_cast<size_t>(readLE32(&d[p + 4])); };
    if (at + 8 <= d.size() && std::memcmp(&d[at], "bext", 4) == 0) at += 8 + chunkSize(at) + (chunkSize(at) & 1);
    if (at + 8 + 16 > d.size()) return false;
    size_t fmt = at + 8;
    int tag = readLE16(&d[fmt]);
    channels = readLE16(&d[fmt + 2]);
    rate = static_cast<int>(readLE32(&d[fmt + 4]));
    int bits = readLE16(&d[fmt + 14]);
    if (tag != 1 || (bits != 8 && bits != 16) || channels < 1 || rate <= 0) return false;
    at += 8 + chunkSize(at) + (chunkSize(at) & 1);
    while (at + 8 <= d.size() && std::memcmp(&d[at], "data", 4) != 0) at += 8 + chunkSize(at) + (chunkSize(at) & 1);
    if (at + 8 > d.size()) return false;
    size_t size = std::min(chunkSize(at), d.size() - at - 8);
    const uint8_t* p = &d[at + 8];
    size_t bytesPerFrame = static_cast<size_t>(channels * bits / 8);
    size_t frames = size / bytesPerFrame;
    out.resize(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        float s[2] = {0, 0};
        for (int c = 0; c < std::min(channels, 2); ++c) {
            const uint8_t* q = p + f * bytesPerFrame + static_cast<size_t>(c * bits / 8);
            s[c] = bits == 16 ? static_cast<int16_t>(readLE16(q)) / 32768.0f : (static_cast<int>(*q) - 128) / 128.0f;
        }
        if (channels == 1) s[1] = s[0];
        out[f * 2] = s[0];
        out[f * 2 + 1] = s[1];
    }
    return true;
}

// Without a mixer nothing is decoded: an Ogg file's rate from its identification header and its length
// from the granule position of its last page are all the clock needs.
bool oggShape(const LacArchive& a, const LacEntry& e, int& rate, int& channels, int64_t& frames) {
    Bytes head = a.readPart(e, 0, 512);
    static const uint8_t kId[] = {1, 'v', 'o', 'r', 'b', 'i', 's'};
    auto id = std::search(head.begin(), head.end(), std::begin(kId), std::end(kId));
    if (id == head.end() || head.end() - id < 16) return false;
    size_t at = static_cast<size_t>(id - head.begin());
    channels = head[at + 11];
    rate = static_cast<int>(readLE32(&head[at + 12]));
    uint32_t tailSize = std::min<uint32_t>(e.size, 65536);
    Bytes tail = a.readPart(e, e.size - tailSize, tailSize);
    for (size_t p = tail.size() >= 14 ? tail.size() - 14 : 0; p-- > 0;) {
        if (std::memcmp(&tail[p], "OggS", 4) == 0 && tail[p + 4] == 0) {
            frames = static_cast<int64_t>(readLE32(&tail[p + 6])) | static_cast<int64_t>(readLE32(&tail[p + 10])) << 32;
            return rate > 0 && channels > 0;
        }
    }
    return false;
}

// Streaming (see the header): entries from kStreamMin bytes on; the start and the end read by the
// caller; the decoder stays kMargin bytes (two of Ogg's largest pages) behind what has arrived.
constexpr size_t kStreamMin = 1u << 20;
constexpr size_t kHead = 256u << 10;
constexpr size_t kTail = 65536;  // stb_vorbis_stream_length_in_samples reads from 64 KB before the end
constexpr size_t kMargin = 128u << 10;
constexpr size_t kChunk = 1u << 20;

}  // namespace

// An entry's bytes. Whole, or arriving: [0, loaded) and [tail, size) are in, the loader fills the rest
// in order. Only the loader writes past `loaded`; readers stay below it.
struct SoundOut::Stream {
    Bytes data;
    std::atomic<size_t> loaded{0};
    size_t tail = 0;
    std::atomic<bool> failed{false};   // a read failed: the sound ends where its data does
    std::atomic<bool> dropped{false};  // the sound was released: the loader stops
    const LacArchive* archive = nullptr;
    const LacEntry* entry = nullptr;

    bool complete() const { return loaded.load(std::memory_order_acquire) >= data.size(); }
    // Whether the decoder, now at byte `offset`, may decode on.
    bool readable(size_t offset) const {
        size_t l = loaded.load(std::memory_order_acquire);
        return l >= data.size() || offset + kMargin <= l;
    }
};

struct SoundOut::Object {
    Kind kind = Kind::Se;
    int count = 1;
    float gain = 0;
    bool ogg = false;
    int rate = 44100;
    int channels = 2;
    // Ogg: the entry's bytes (they must outlive the decoder) and the `_B` entry it continues with.
    std::shared_ptr<Stream> data, nextData;
    bool waitNext = false;  // the `_B` entry is due but not in yet
    stb_vorbis* vorbis = nullptr;
    int64_t entryFrames = 0, nextFrames = 0;
    bool hasNext = false;  // an `_B` entry follows
    // WAV: the whole sound, stereo.
    std::vector<float> pcm;
    int64_t wavPos = 0;
    // Decoded stereo frames not yet mixed and the fractional read position into them.
    std::vector<float> buf;
    size_t bufFrames = 0;
    double pos = 0;
    bool started = false;  // loaded objects wait for their start (SEP mode 1, 0x405710)
    bool ended = false;    // no more data
    bool stopped = false;
    int64_t clockFrames = 0;  // without a mixer: frames played in the current entry
    // A play-once Ogg stream reports its end late (3.7 of the sound notes): its thread marks the end
    // on the first refill that finds no data and stops on the second notification after that. With
    // the ring of 2 s refilled at 0.1 s and 1.1 s, that is max(floor(T), 1) + 1.1 s after the start,
    // T the length; silence fills the gap. -1: the sound stops when its data ends (WAV, loops).
    double stopAt = -1;
    double elapsed = 0;  // frames of the sound's rate since it started

    ~Object() {
        if (vorbis) stb_vorbis_close(vorbis);
        if (data) data->dropped = true;
        if (nextData) nextData->dropped = true;
    }

    // The data ran out: another round, the `_B` entry, or the end.
    bool endOfData() {
        if (count == 1) {
            ended = true;
            return false;
        }
        if (count > 0) --count;
        if (nextData) {
            waitNext = true;
            return switchToNext();
        }
        if (vorbis) {
            stb_vorbis_seek_start(vorbis);
        } else {
            wavPos = 0;
        }
        return true;
    }

    // Continues with the `_B` entry once it is in; false while it is not (or cannot be read).
    bool switchToNext() {
        if (nextData->failed) {
            ended = true;
            return false;
        }
        if (!nextData->complete()) return false;
        waitNext = false;
        int err = 0;
        stb_vorbis* next = stb_vorbis_open_memory(nextData->data.data(), static_cast<int>(nextData->data.size()), &err, nullptr);
        if (!next) {
            // As when the `_B` entry was read at the start and did not open: the first one loops.
            nextData.reset();
            if (vorbis) stb_vorbis_seek_start(vorbis);
            return vorbis != nullptr;
        }
        if (vorbis) stb_vorbis_close(vorbis);
        vorbis = next;
        data = std::move(nextData);  // the decoder reads these bytes from now on
        nextData.reset();
        entryFrames = stb_vorbis_stream_length_in_samples(vorbis);
        return true;
    }

    // Whether decoding has to wait for the loader (it never has at the speed sound plays).
    bool starving() {
        if (ended) return false;
        if (waitNext) return !switchToNext() && !ended;
        if (!vorbis || data->readable(stb_vorbis_get_file_offset(vorbis))) return false;
        if (data->failed) ended = true;
        return !ended;
    }

    // Decodes up to `frames` stereo frames into `dst`; 0 once the sound has ended (or while it
    // waits for its data).
    int decode(float* dst, int frames) {
        int got = 0;
        while (got < frames && !ended) {
            if (starving()) break;
            int n = 0;
            if (vorbis) {
                n = stb_vorbis_get_samples_float_interleaved(vorbis, channels, dst + got * 2, (frames - got) * channels);
                if (channels == 1) {
                    float* p = dst + got * 2;
                    for (int i = n - 1; i >= 0; --i) p[i * 2] = p[i * 2 + 1] = p[i];
                }
            } else {
                int64_t left = static_cast<int64_t>(pcm.size() / 2) - wavPos;
                n = static_cast<int>(std::min<int64_t>(left, frames - got));
                std::memcpy(dst + got * 2, pcm.data() + wavPos * 2, static_cast<size_t>(n) * 2 * sizeof(float));
                wavPos += n;
            }
            if (n <= 0) {
                if (!endOfData()) break;
                continue;
            }
            got += n;
        }
        return got;
    }

    bool fill(size_t need) {
        while (bufFrames <= need + 1 && !ended) {
            float chunk[2048 * 2];
            int got = decode(chunk, 2048);
            if (got <= 0) break;
            buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(bufFrames * 2), chunk, chunk + got * 2);
            bufFrames += static_cast<size_t>(got);
        }
        return bufFrames > need;
    }
};

SoundOut::SoundOut(const Assets& assets, audio::Mixer* mixer) : assets_(assets), mixer_(mixer) {
    if (!mixer_) return;
    outputRate_ = mixer_->outputRate();
    // One stream per bus, never finishing: silence when nothing of that kind plays.
    const std::pair<Kind, audio::Mixer::Bus> buses[] = {
        {Kind::Bgm, audio::Mixer::Bus::Bgm}, {Kind::Se, audio::Mixer::Bus::Se}, {Kind::Voice, audio::Mixer::Bus::Voice}};
    for (auto [kind, bus] : buses) {
        mixer_->playStream(bus, 0, outputRate_, [this, kind = kind](float* out, int frames) {
            mix(kind, out, frames);
            return frames;
        });
    }
}

SoundOut::~SoundOut() {
    if (!mixer_) return;
    for (auto bus : {audio::Mixer::Bus::Bgm, audio::Mixer::Bus::Se, audio::Mixer::Bus::Voice}) mixer_->stop(bus, 0);
    if (loader_.joinable()) {
        {
            std::lock_guard lock(loadMu_);
            quit_ = true;
        }
        loadCv_.notify_all();
        loader_.join();
    }
}

bool SoundOut::loading() const {
    std::lock_guard lock(loadMu_);
    if (!jobs_.empty()) return true;
    std::lock_guard objects(mu_);
    for (const auto& [h, o] : objects_) {
        if ((o->data && !o->data->complete() && !o->data->failed) ||
            (o->nextData && !o->nextData->complete() && !o->nextData->failed)) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<SoundOut::Stream> SoundOut::readEntry(const LacArchive& a, const LacEntry& e, bool head) {
    auto s = std::make_shared<Stream>();
    size_t size = e.size;
    if (!mixer_ || !streamLarge || size < kStreamMin) {
        s->data = a.read(e);
        s->loaded = s->data.size();
        return s;
    }
    s->data.resize(size);
    s->archive = &a;
    s->entry = &e;
    s->tail = size - kTail;
    if (head) {
        // What the decoder's start (headers, first frame) and its length need: the start and the end.
        Bytes start = a.readPart(e, 0, static_cast<uint32_t>(kHead));
        Bytes end = a.readPart(e, static_cast<uint32_t>(s->tail), static_cast<uint32_t>(kTail));
        std::memcpy(s->data.data(), start.data(), start.size());
        std::memcpy(s->data.data() + s->tail, end.data(), end.size());
        s->loaded.store(kHead, std::memory_order_release);
    } else {
        s->tail = size;  // an `_B` entry: wanted whole, later
    }
    return s;
}

void SoundOut::queue(const std::shared_ptr<Stream>& s) {
    if (s->complete()) return;
    {
        std::lock_guard lock(loadMu_);
        jobs_.push_back(s);
        if (!loader_.joinable()) loader_ = std::thread([this] { loaderLoop(); });
    }
    loadCv_.notify_one();
}

void SoundOut::loaderLoop() {
    std::unique_lock lock(loadMu_);
    for (;;) {
        loadCv_.wait(lock, [this] { return quit_.load() || !jobs_.empty(); });
        if (quit_) return;
        std::shared_ptr<Stream> s = jobs_.front();
        lock.unlock();
        fillStream(*s);
        lock.lock();
        // Popped only now, so `loading()` sees the job until its bytes are in.
        if (!jobs_.empty() && jobs_.front() == s) jobs_.pop_front();
    }
}

void SoundOut::fillStream(Stream& s) {
    try {
        size_t at = s.loaded.load(std::memory_order_relaxed);
        while (at < s.tail) {
            if (quit_ || s.dropped) return;
            size_t n = std::min(kChunk, s.tail - at);
            Bytes part = s.archive->readPart(*s.entry, static_cast<uint32_t>(at), static_cast<uint32_t>(n));
            std::memcpy(s.data.data() + at, part.data(), n);
            at += n;
            s.loaded.store(at >= s.tail ? s.data.size() : at, std::memory_order_release);
        }
        s.loaded.store(s.data.size(), std::memory_order_release);
    } catch (...) {
        s.failed = true;
    }
}

int SoundOut::find(int archive, const std::string& name) const {
    const auto& archives = assets_.soundArchives();
    if (archive < 0 || archive >= static_cast<int>(archives.size())) return -1;
    return archives[static_cast<size_t>(archive)]->indexOf(name);
}

std::shared_ptr<SoundOut::Object> SoundOut::open(Kind kind, int archive, int entry) {
    const auto& archives = assets_.soundArchives();
    if (archive < 0 || archive >= static_cast<int>(archives.size())) return nullptr;
    const LacArchive& a = *archives[static_cast<size_t>(archive)];
    if (entry < 0 || entry >= static_cast<int>(a.entries().size())) return nullptr;
    const LacEntry& e = a.entries()[static_cast<size_t>(entry)];
    auto o = std::make_shared<Object>();
    o->kind = kind;
    if (!mixer_ && a.readPart(e, 0, 4) == Bytes{'O', 'g', 'g', 'S'}) {
        o->ogg = true;
        if (!oggShape(a, e, o->rate, o->channels, o->entryFrames)) return nullptr;
        if (endsWithA(e.name)) {
            std::string b = e.name;
            b[b.size() - 5] = static_cast<char>(b[b.size() - 5] == 'a' ? 'b' : 'B');
            int r = 0, c = 0;
            if (const LacEntry* next = a.find(b)) o->hasNext = oggShape(a, *next, r, c, o->nextFrames);
        }
        if (o->entryFrames <= 0) o->ended = true;
        return o;
    }
    // An Ogg entry is told by its first bytes; a large one streams (WAV and small entries are read
    // whole, as before).
    bool isOgg = a.readPart(e, 0, std::min<uint32_t>(e.size, 4)) == Bytes{'O', 'g', 'g', 'S'};
    std::shared_ptr<Stream> data = isOgg ? readEntry(a, e, true) : nullptr;
    if (isOgg) {
        o->ogg = true;
        o->data = data;
        int err = 0;
        o->vorbis = stb_vorbis_open_memory(data->data.data(), static_cast<int>(data->data.size()), &err, nullptr);
        if (!o->vorbis) return nullptr;
        stb_vorbis_info info = stb_vorbis_get_info(o->vorbis);
        o->rate = static_cast<int>(info.sample_rate);
        o->channels = std::min(info.channels, 2);
        o->entryFrames = stb_vorbis_stream_length_in_samples(o->vorbis);
        // The start must leave the decoder inside the bytes read so far (an Ogg setup header is a
        // few kilobytes); otherwise the rest is read here. Then the loader takes the rest.
        if (!data->readable(stb_vorbis_get_file_offset(o->vorbis))) fillStream(*data);
        queue(data);
        if (endsWithA(e.name)) {
            std::string b = e.name;
            b[b.size() - 5] = static_cast<char>(b[b.size() - 5] == 'a' ? 'b' : 'B');
            if (const LacEntry* next = a.find(b)) {
                if (next->size >= kStreamMin && mixer_ && streamLarge) {
                    // Wanted only when the first entry ends; its length counts only without a mixer.
                    o->nextData = readEntry(a, *next, false);
                    o->hasNext = true;
                    queue(o->nextData);
                } else {
                    o->nextData = readEntry(a, *next, false);
                    int err2 = 0;
                    if (stb_vorbis* v = stb_vorbis_open_memory(o->nextData->data.data(), static_cast<int>(o->nextData->data.size()), &err2, nullptr)) {
                        o->nextFrames = stb_vorbis_stream_length_in_samples(v);
                        o->hasNext = true;
                        stb_vorbis_close(v);
                    } else {
                        o->nextData.reset();
                    }
                }
            }
        }
    } else {
        Bytes whole = a.read(e);
        if (!decodeWav(whole, o->pcm, o->rate, o->channels)) return nullptr;
        o->entryFrames = static_cast<int64_t>(o->pcm.size() / 2);
    }
    if (o->entryFrames <= 0) o->ended = true;
    return o;
}

int SoundOut::load(Kind kind, int archive, int entry) {
    auto o = open(kind, archive, entry);
    if (!o) return -1;
    std::lock_guard lock(mu_);
    int h = nextHandle_++;
    objects_[h] = std::move(o);
    return h;
}

void SoundOut::start(int handle, int count, int level) {
    std::lock_guard lock(mu_);
    auto it = objects_.find(handle);
    if (it == objects_.end() || it->second->started) return;
    Object& o = *it->second;
    o.count = std::max(count, 0);
    o.gain = std::clamp(level, 0, 255) / 255.0f;
    if (o.ogg && o.count == 1) {
        int64_t seconds = std::max<int64_t>(o.entryFrames / o.rate, 1);
        o.stopAt = static_cast<double>((seconds + 1) * o.rate + o.rate / 10);
    }
    o.started = true;
}

int SoundOut::play(Kind kind, int archive, int entry, int count, int level) {
    int h = load(kind, archive, entry);
    if (h != -1) start(h, count, level);
    return h;
}

void SoundOut::setLevel(int handle, int level) {
    std::lock_guard lock(mu_);
    if (auto it = objects_.find(handle); it != objects_.end()) it->second->gain = std::clamp(level, 0, 255) / 255.0f;
}

void SoundOut::stop(int handle) {
    std::lock_guard lock(mu_);
    objects_.erase(handle);
}

bool SoundOut::playing(int handle) const {
    std::lock_guard lock(mu_);
    auto it = objects_.find(handle);
    return it != objects_.end() && it->second->started && !it->second->stopped;
}

void SoundOut::advanceFrame() {
    if (mixer_) return;
    std::lock_guard lock(mu_);
    for (auto& [h, o] : objects_) {
        if (!o->started || o->stopped) continue;
        if (o->stopAt >= 0) {
            o->elapsed += static_cast<double>(o->rate) / kFramesPerTick;
            if (o->elapsed >= o->stopAt) o->stopped = true;
            continue;
        }
        o->clockFrames += o->rate / kFramesPerTick;
        while (!o->stopped && o->clockFrames >= o->entryFrames) {
            o->clockFrames -= o->entryFrames;
            if (o->count == 1 || o->entryFrames <= 0) {
                o->stopped = true;
                break;
            }
            if (o->count > 0) --o->count;
            if (o->hasNext) {
                o->entryFrames = o->nextFrames;
                o->hasNext = false;
            }
        }
    }
}

void SoundOut::mix(Kind kind, float* out, int frames) {
    std::fill(out, out + frames * 2, 0.0f);
    std::lock_guard lock(mu_);
    for (auto& [h, o] : objects_) {
        if (o->kind != kind || !o->started || o->stopped) continue;
        double ratio = static_cast<double>(o->rate) / outputRate_;
        for (int i = 0; i < frames; ++i) {
            auto idx = static_cast<size_t>(o->pos);
            if (o->fill(idx)) {
                float frac = static_cast<float>(o->pos - static_cast<double>(idx));
                size_t nxt = std::min(idx + 1, o->bufFrames - 1);
                float l = o->buf[idx * 2] + (o->buf[nxt * 2] - o->buf[idx * 2]) * frac;
                float r = o->buf[idx * 2 + 1] + (o->buf[nxt * 2 + 1] - o->buf[idx * 2 + 1]) * frac;
                out[i * 2] += l * o->gain;
                out[i * 2 + 1] += r * o->gain;
                o->pos += ratio;
            } else if (o->stopAt < 0 && o->ended) {
                o->stopped = true;
                break;
            }
            // Otherwise silence: a play-once stream's gap before its end is reported, or (never at
            // the speed sound plays) a streamed entry whose next bytes have not arrived.
            o->elapsed += ratio;
            if (o->stopAt >= 0 && o->elapsed >= o->stopAt) {
                o->stopped = true;
                break;
            }
        }
        auto drop = std::min(static_cast<size_t>(o->pos), o->bufFrames);
        if (drop) {
            o->buf.erase(o->buf.begin(), o->buf.begin() + static_cast<std::ptrdiff_t>(drop * 2));
            o->bufFrames -= drop;
            o->pos -= static_cast<double>(drop);
        }
    }
}

}  // namespace nyan::wa2
