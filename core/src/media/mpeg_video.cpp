#include "nyan/media/mpeg_video.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pl_mpeg.h"

namespace nyan::media {

namespace {
plm_t* openPlm(const std::shared_ptr<Bytes>& data, bool video, bool audio) {
    plm_t* p = plm_create_with_memory(const_cast<uint8_t*>(data->data()), data->size(), 0);
    if (!p) return nullptr;
    plm_set_video_enabled(p, video);
    plm_set_audio_enabled(p, audio);
    if (audio) plm_set_audio_stream(p, 0);
    return p;
}
}  // namespace

std::unique_ptr<MpegVideo> MpegVideo::open(Bytes data) {
    std::unique_ptr<MpegVideo> v(new MpegVideo());
    v->data_ = std::make_shared<Bytes>(std::move(data));
    plm_t* p = openPlm(v->data_, true, false);
    if (!p) return nullptr;
    v->plm_ = p;
    if (!plm_probe(p, 5000 * 1024)) {
        plm_destroy(p);
        v->plm_ = nullptr;
        return nullptr;
    }
    v->width_ = plm_get_width(p);
    v->height_ = plm_get_height(p);
    v->frameRate_ = plm_get_framerate(p) > 0 ? plm_get_framerate(p) : 30.0;
    if (plm_get_num_audio_streams(p) > 0) v->sampleRate_ = plm_get_samplerate(p);
    if (v->width_ <= 0 || v->height_ <= 0) return nullptr;
    v->rgba_.assign(static_cast<size_t>(v->width_) * v->height_ * 4, 0);
    for (size_t i = 3; i < v->rgba_.size(); i += 4) v->rgba_[i] = 255;
    return v;
}

MpegVideo::~MpegVideo() {
    if (plm_) plm_destroy(static_cast<plm_t*>(plm_));
}

bool MpegVideo::seek(double seconds) {
    if (!plm_ || ended_) return false;
    // Frame k is shown from k / rate; decode forward to the last one due, converting only that.
    const int64_t due = static_cast<int64_t>(std::floor(seconds * frameRate_));
    plm_frame_t* last = nullptr;
    while (shown_ < due) {
        plm_frame_t* f = plm_decode_video(static_cast<plm_t*>(plm_));
        if (!f) {
            ended_ = true;
            break;
        }
        ++shown_;
        last = f;
    }
    if (!last) return false;
    plm_frame_to_rgba(last, rgba_.data(), width_ * 4);
    return true;
}

void MpegVideo::rewind() {
    if (!plm_) return;
    plm_rewind(static_cast<plm_t*>(plm_));
    shown_ = -1;
    ended_ = false;
}

std::function<int(float*, int)> MpegVideo::audioSource(bool loop) const {
    if (!hasAudio()) return {};
    struct State {
        std::shared_ptr<Bytes> data;
        plm_t* plm = nullptr;
        std::vector<float> pending;  // decoded, not yet handed out
        size_t at = 0;
        ~State() {
            if (plm) plm_destroy(plm);
        }
    };
    auto st = std::make_shared<State>();
    st->data = data_;
    st->plm = openPlm(data_, false, true);
    if (!st->plm) return {};
    plm_set_loop(st->plm, loop);
    return [st](float* out, int frames) {
        int done = 0;
        while (done < frames) {
            if (st->at >= st->pending.size()) {
                plm_samples_t* s = plm_decode_audio(st->plm);
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
