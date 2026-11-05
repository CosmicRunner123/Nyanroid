#include "nyan/media/mpeg_video.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "media/plm_stream.hpp"

namespace nyan::media {

namespace {

// One pl_mpeg decoder reading the file. Neither copied nor moved: its buffer points at `stream`.
struct Plm {
    PlmStream stream;
    plm_t* plm = nullptr;
    bool probed = false;
    int sampleRate = 0;

    // Probed with both streams on: the probe finds streams the system header leaves out (Amakano+'s
    // opening lists no audio) and creates their decoders, and a new decoder reads its stream's
    // first header at once. Created for a stream that is off, it would read the whole file looking
    // for it. The stream not wanted is turned off after.
    Plm(std::shared_ptr<RandomAccessFile> file, bool video, bool audio) {
        stream.file = std::move(file);
        plm = plm_create_with_buffer(createPlmBuffer(stream), 1);
        probed = plm_probe(plm, 5000 * 1024) != 0;
        sampleRate = plm_get_samplerate(plm);
        plm_set_video_enabled(plm, video);
        plm_set_audio_enabled(plm, audio);
        if (audio) plm_set_audio_stream(plm, 0);
    }
    ~Plm() { plm_destroy(plm); }
    Plm(const Plm&) = delete;
    Plm& operator=(const Plm&) = delete;
};

// How many pictures the worker keeps decoded ahead of the one shown.
constexpr size_t kAhead = 3;

}  // namespace

// The worker: decodes the pictures in order into `ready`, at most kAhead ahead.
struct MpegVideo::Decoder {
    struct Picture {
        int64_t index;
        std::vector<uint8_t> rgba;
    };

    std::unique_ptr<Plm> plm;  // the worker's alone once it runs
    int stride = 0;
    std::vector<uint8_t> blank;  // a black, opaque picture (pl_mpeg writes only R, G and B)
    std::mutex m;
    std::condition_variable cv;
    std::deque<Picture> ready;
    std::vector<std::vector<uint8_t>> spare;  // buffers to reuse
    bool end = false;      // the worker found no picture after the last in `ready`
    bool restart = false;  // rewind() asked for the first picture again
    bool stop = false;
    std::thread thread;

    void run() {
        int64_t next = 0;
        for (;;) {
            std::vector<uint8_t> buffer;
            {
                std::unique_lock lock(m);
                cv.wait(lock, [&] { return stop || restart || (!end && ready.size() < kAhead); });
                if (stop) return;
                if (restart) {
                    plm_rewind(plm->plm);
                    next = 0;
                    restart = false;
                    continue;
                }
                if (!spare.empty()) {
                    buffer = std::move(spare.back());
                    spare.pop_back();
                }
            }
            plm_frame_t* frame = plm_decode_video(plm->plm);
            if (frame) {
                if (buffer.size() != blank.size()) buffer = blank;
                plm_frame_to_rgba(frame, buffer.data(), stride);
            }
            std::lock_guard lock(m);
            if (restart) {
                if (!buffer.empty()) spare.push_back(std::move(buffer));
                continue;
            }
            if (frame) ready.push_back({next++, std::move(buffer)});
            else end = true;
            cv.notify_all();
        }
    }
};

std::unique_ptr<MpegVideo> MpegVideo::open(std::shared_ptr<RandomAccessFile> file) {
    if (!file) return nullptr;
    auto plm = std::make_unique<Plm>(file, true, false);
    plm_t* p = plm->plm;
    if (!plm->probed) return nullptr;
    std::unique_ptr<MpegVideo> v(new MpegVideo());
    v->file_ = file;
    v->width_ = plm_get_width(p);
    v->height_ = plm_get_height(p);
    v->frameRate_ = plm_get_framerate(p) > 0 ? plm_get_framerate(p) : 30.0;
    if (v->width_ <= 0 || v->height_ <= 0) return nullptr;
    v->sampleRate_ = plm->sampleRate;

    auto d = std::make_unique<Decoder>();
    d->plm = std::move(plm);
    d->stride = v->width_ * 4;
    d->blank.assign(static_cast<size_t>(v->width_) * v->height_ * 4, 0);
    for (size_t i = 3; i < d->blank.size(); i += 4) d->blank[i] = 255;
    v->rgba_ = d->blank;
    d->thread = std::thread([d = d.get()] { d->run(); });
    v->decoder_ = std::move(d);
    return v;
}

MpegVideo::~MpegVideo() {
    if (!decoder_) return;
    {
        std::lock_guard lock(decoder_->m);
        decoder_->stop = true;
    }
    decoder_->cv.notify_all();
    decoder_->thread.join();
}

bool MpegVideo::seek(double seconds) {
    if (!decoder_ || ended_) return false;
    // Frame k is shown from k / rate: take the last one due, dropping the ones before it.
    const int64_t due = static_cast<int64_t>(std::floor(seconds * frameRate_));
    if (due <= shown_) return false;
    Decoder& d = *decoder_;
    std::unique_lock lock(d.m);
    std::vector<uint8_t> got;
    int64_t at = shown_;
    for (;;) {
        while (!d.ready.empty() && d.ready.front().index <= due) {
            if (!got.empty()) d.spare.push_back(std::move(got));
            got = std::move(d.ready.front().rgba);
            at = d.ready.front().index;
            d.ready.pop_front();
            d.cv.notify_all();
        }
        if (at == due) break;
        if (d.end && d.ready.empty()) {
            ended_ = true;
            break;
        }
        d.cv.wait(lock);
    }
    if (got.empty()) return false;
    rgba_.swap(got);
    d.spare.push_back(std::move(got));
    shown_ = at;
    return true;
}

void MpegVideo::rewind() {
    if (!decoder_) return;
    Decoder& d = *decoder_;
    {
        std::lock_guard lock(d.m);
        for (auto& picture : d.ready) d.spare.push_back(std::move(picture.rgba));
        d.ready.clear();
        d.end = false;
        d.restart = true;
    }
    d.cv.notify_all();
    shown_ = -1;
    ended_ = false;
}

std::function<int(float*, int)> MpegVideo::audioSource(bool loop) const {
    if (!hasAudio()) return {};
    struct State {
        std::unique_ptr<Plm> plm;
        std::vector<float> pending;  // decoded, not yet handed out
        size_t at = 0;
    };
    auto st = std::make_shared<State>();
    st->plm = std::make_unique<Plm>(file_, false, true);
    plm_set_loop(st->plm->plm, loop);
    return [st](float* out, int frames) {
        int done = 0;
        while (done < frames) {
            if (st->at >= st->pending.size()) {
                plm_samples_t* s = plm_decode_audio(st->plm->plm);
                if (!s) break;
                st->pending.assign(s->interleaved, s->interleaved + s->count * 2);
                st->at = 0;
            }
            size_t take = std::min((st->pending.size() - st->at) / 2, static_cast<size_t>(frames - done));
            std::memcpy(out + done * 2, st->pending.data() + st->at, take * 2 * sizeof(float));
            st->at += take * 2;
            done += static_cast<int>(take);
        }
        return done;
    };
}

}  // namespace nyan::media
