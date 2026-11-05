#include "nyan/media/ffmpeg_video.hpp"

#include <algorithm>
#include <cstring>

#ifdef NYAN_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}
#endif

namespace nyan::media {

#ifndef NYAN_HAVE_FFMPEG

struct FfmpegVideo::Demuxer {};

bool FfmpegVideo::available() {
    return false;
}
std::unique_ptr<FfmpegVideo> FfmpegVideo::open(std::shared_ptr<RandomAccessFile>) {
    return nullptr;
}
FfmpegVideo::~FfmpegVideo() = default;
bool FfmpegVideo::seek(double) {
    return false;
}
void FfmpegVideo::rewind() {}
bool FfmpegVideo::nextFrame() {
    return false;
}
std::function<int(float*, int)> FfmpegVideo::audioSource(bool) const {
    return [](float*, int) { return 0; };
}

#else

namespace {

constexpr int kIoBuffer = 64 * 1024;

void quietLogs() {
    static bool once = [] {
        av_log_set_level(AV_LOG_ERROR);
        return true;
    }();
    (void)once;
}

AVCodecContext* openDecoder(const AVStream* s) {
    const AVCodec* codec = avcodec_find_decoder(s->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return nullptr;
    if (avcodec_parameters_to_context(ctx, s->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

uint8_t clamp8(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

// Planar YUV 4:2:0 to RGBA (BT.601; limited or full range as the frame says).
void toRgba(const AVFrame* f, std::vector<uint8_t>& out) {
    int w = f->width, h = f->height;
    out.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    for (int y = 0; y < h; ++y) {
        const uint8_t* Y = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
        const uint8_t* U = f->data[1] + static_cast<ptrdiff_t>(y / 2) * f->linesize[1];
        const uint8_t* V = f->data[2] + static_cast<ptrdiff_t>(y / 2) * f->linesize[2];
        uint8_t* o = &out[static_cast<size_t>(y) * static_cast<size_t>(w) * 4];
        for (int x = 0; x < w; ++x, o += 4) {
            int u = U[x / 2] - 128, v = V[x / 2] - 128;
            int l;
            if (full) {
                l = Y[x] * 1024;
                o[0] = clamp8((l + 1436 * v + 512) >> 10);
                o[1] = clamp8((l - 352 * u - 731 * v + 512) >> 10);
                o[2] = clamp8((l + 1815 * u + 512) >> 10);
            } else {
                l = (Y[x] - 16) * 1192;
                o[0] = clamp8((l + 1634 * v + 512) >> 10);
                o[1] = clamp8((l - 401 * u - 833 * v + 512) >> 10);
                o[2] = clamp8((l + 2066 * u + 512) >> 10);
            }
            o[3] = 0xff;
        }
    }
}

}  // namespace

// A demuxer reading the file through FFmpeg's I/O callbacks.
struct FfmpegVideo::Demuxer {
    std::shared_ptr<RandomAccessFile> file;
    uint64_t pos = 0;
    AVFormatContext* fmt = nullptr;
    AVIOContext* io = nullptr;

    ~Demuxer() {
        if (fmt) avformat_close_input(&fmt);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }

    static int read(void* opaque, uint8_t* buf, int size) {
        auto* d = static_cast<Demuxer*>(opaque);
        uint64_t total = d->file->size();
        if (d->pos >= total) return AVERROR_EOF;
        auto n = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(size), total - d->pos));
        try {
            d->file->readAt(d->pos, buf, n);
        } catch (...) {
            return AVERROR(EIO);
        }
        d->pos += n;
        return static_cast<int>(n);
    }

    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto* d = static_cast<Demuxer*>(opaque);
        auto total = static_cast<int64_t>(d->file->size());
        if (whence & AVSEEK_SIZE) return total;
        int64_t to;
        switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: to = offset; break;
        case SEEK_CUR: to = static_cast<int64_t>(d->pos) + offset; break;
        case SEEK_END: to = total + offset; break;
        default: return -1;
        }
        if (to < 0 || to > total) return -1;
        d->pos = static_cast<uint64_t>(to);
        return to;
    }

    bool open(std::shared_ptr<RandomAccessFile> f) {
        file = std::move(f);
        auto* buffer = static_cast<uint8_t*>(av_malloc(kIoBuffer));
        if (!buffer) return false;
        io = avio_alloc_context(buffer, kIoBuffer, 0, this, &Demuxer::read, nullptr, &Demuxer::seek);
        if (!io) {
            av_free(buffer);
            return false;
        }
        fmt = avformat_alloc_context();
        if (!fmt) return false;
        fmt->pb = io;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
            fmt = nullptr;  // freed by avformat_open_input on failure
            return false;
        }
        return avformat_find_stream_info(fmt, nullptr) >= 0;
    }
};

bool FfmpegVideo::available() {
    return true;
}

std::unique_ptr<FfmpegVideo> FfmpegVideo::open(std::shared_ptr<RandomAccessFile> file) {
    quietLogs();
    if (!file) return nullptr;
    std::unique_ptr<FfmpegVideo> v(new FfmpegVideo());
    v->file_ = file;
    v->demux_ = std::make_unique<Demuxer>();
    if (!v->demux_->open(file)) return nullptr;
    AVFormatContext* fmt = v->demux_->fmt;
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) return nullptr;
    AVStream* s = fmt->streams[vs];
    AVCodecContext* ctx = openDecoder(s);
    if (!ctx) return nullptr;
    v->codec_ = ctx;
    v->videoStream_ = vs;
    v->width_ = ctx->width;
    v->height_ = ctx->height;
    v->timeBase_ = av_q2d(s->time_base);
    AVRational rate = av_guess_frame_rate(fmt, s, nullptr);
    if (rate.num > 0 && rate.den > 0) v->frameRate_ = av_q2d(rate);
    if (fmt->duration > 0) v->duration_ = static_cast<double>(fmt->duration) / AV_TIME_BASE;
    int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vs, nullptr, 0);
    if (as >= 0 && avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id)) v->sampleRate_ = fmt->streams[as]->codecpar->sample_rate;
    // Only the video stream is read here.
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (static_cast<int>(i) != vs) fmt->streams[i]->discard = AVDISCARD_ALL;
    }
    v->frame_ = av_frame_alloc();
    if (v->width_ <= 0 || v->height_ <= 0 || !v->frame_) return nullptr;
    return v;
}

FfmpegVideo::~FfmpegVideo() {
    auto* frame = static_cast<AVFrame*>(frame_);
    av_frame_free(&frame);
    auto* ctx = static_cast<AVCodecContext*>(codec_);
    avcodec_free_context(&ctx);
}

bool FfmpegVideo::nextFrame() {
    auto* ctx = static_cast<AVCodecContext*>(codec_);
    auto* frame = static_cast<AVFrame*>(frame_);
    AVFormatContext* fmt = demux_->fmt;
    AVPacket* pkt = av_packet_alloc();
    bool got = false;
    while (true) {
        int r = avcodec_receive_frame(ctx, frame);
        if (r == 0) {
            int64_t ts = frame->best_effort_timestamp;
            int64_t start = fmt->streams[videoStream_]->start_time;
            if (ts == AV_NOPTS_VALUE) ts = 0;
            if (start != AV_NOPTS_VALUE) ts -= start;
            pendingTime_ = std::max(0.0, static_cast<double>(ts) * timeBase_);
            got = true;
            break;
        }
        if (r != AVERROR(EAGAIN) || drained_) break;
        r = av_read_frame(fmt, pkt);
        if (r < 0) {
            avcodec_send_packet(ctx, nullptr);
            drained_ = true;
            continue;
        }
        if (pkt->stream_index == videoStream_) avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return got;
}

bool FfmpegVideo::seek(double seconds) {
    bool changed = false;
    while (!ended_) {
        if (pendingTime_ < 0 && !nextFrame()) {
            // The last picture stays for one frame's time.
            ended_ = true;
            break;
        }
        if (pendingTime_ > seconds) break;
        auto* frame = static_cast<AVFrame*>(frame_);
        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            toRgba(frame, rgba_);
            width_ = frame->width;
            height_ = frame->height;
            changed = true;
        }
        pendingTime_ = -1;
    }
    return changed;
}

void FfmpegVideo::rewind() {
    av_seek_frame(demux_->fmt, videoStream_, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(static_cast<AVCodecContext*>(codec_));
    pendingTime_ = -1;
    drained_ = false;
    ended_ = false;
}

std::function<int(float*, int)> FfmpegVideo::audioSource(bool loop) const {
    if (sampleRate_ <= 0) return [](float*, int) { return 0; };
    // Its own demuxer and decoder, used only on the audio thread.
    struct State {
        Demuxer demux;
        AVCodecContext* ctx = nullptr;
        AVFrame* frame = nullptr;
        AVPacket* pkt = nullptr;
        int stream = -1;
        bool drained = false, done = false, loop = false;
        std::vector<float> buf;  // interleaved stereo not yet given out
        size_t at = 0;
        ~State() {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            avcodec_free_context(&ctx);
        }
        // Decodes the next frame of sound into buf; false at the end.
        bool more() {
            while (true) {
                int r = avcodec_receive_frame(ctx, frame);
                if (r == 0) {
                    int n = frame->nb_samples, ch = frame->ch_layout.nb_channels;
                    buf.assign(static_cast<size_t>(n) * 2, 0.0f);
                    at = 0;
                    auto sample = [&](int c, int i) -> float {
                        c = std::min(c, ch - 1);
                        auto fmt = static_cast<AVSampleFormat>(frame->format);
                        bool planar = av_sample_fmt_is_planar(fmt);
                        const uint8_t* p = planar ? frame->extended_data[c] : frame->extended_data[0];
                        size_t idx = planar ? static_cast<size_t>(i) : static_cast<size_t>(i) * static_cast<size_t>(ch) + static_cast<size_t>(c);
                        switch (av_get_packed_sample_fmt(fmt)) {
                        case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float*>(p)[idx];
                        case AV_SAMPLE_FMT_DBL: return static_cast<float>(reinterpret_cast<const double*>(p)[idx]);
                        case AV_SAMPLE_FMT_S16: return reinterpret_cast<const int16_t*>(p)[idx] / 32768.0f;
                        case AV_SAMPLE_FMT_S32: return static_cast<float>(reinterpret_cast<const int32_t*>(p)[idx]) / 2147483648.0f;
                        case AV_SAMPLE_FMT_U8: return (p[idx] - 128) / 128.0f;
                        default: return 0.0f;
                        }
                    };
                    for (int i = 0; i < n; ++i) {
                        buf[static_cast<size_t>(i) * 2] = sample(0, i);
                        buf[static_cast<size_t>(i) * 2 + 1] = sample(1, i);
                    }
                    return true;
                }
                if (r == AVERROR_EOF || (r != AVERROR(EAGAIN))) {
                    if (!loop) return false;
                    av_seek_frame(demux.fmt, stream, 0, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(ctx);
                    drained = false;
                    continue;
                }
                if (drained) return false;
                r = av_read_frame(demux.fmt, pkt);
                if (r < 0) {
                    avcodec_send_packet(ctx, nullptr);
                    drained = true;
                    continue;
                }
                if (pkt->stream_index == stream) avcodec_send_packet(ctx, pkt);
                av_packet_unref(pkt);
            }
        }
    };
    auto st = std::make_shared<State>();
    st->loop = loop;
    if (!st->demux.open(file_)) return [](float*, int) { return 0; };
    st->stream = av_find_best_stream(st->demux.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (st->stream < 0) return [](float*, int) { return 0; };
    for (unsigned i = 0; i < st->demux.fmt->nb_streams; ++i) {
        if (static_cast<int>(i) != st->stream) st->demux.fmt->streams[i]->discard = AVDISCARD_ALL;
    }
    st->ctx = openDecoder(st->demux.fmt->streams[st->stream]);
    st->frame = av_frame_alloc();
    st->pkt = av_packet_alloc();
    if (!st->ctx || !st->frame || !st->pkt) return [](float*, int) { return 0; };
    return [st](float* out, int frames) {
        int written = 0;
        while (written < frames && !st->done) {
            if (st->at * 2 >= st->buf.size() || st->buf.empty()) {
                if (!st->more()) {
                    st->done = true;
                    break;
                }
                continue;
            }
            size_t have = st->buf.size() / 2 - st->at;
            auto n = static_cast<int>(std::min<size_t>(have, static_cast<size_t>(frames - written)));
            std::memcpy(out + static_cast<size_t>(written) * 2, &st->buf[st->at * 2], static_cast<size_t>(n) * 2 * sizeof(float));
            st->at += static_cast<size_t>(n);
            written += n;
        }
        return written;
    };
}

#endif

}  // namespace nyan::media
