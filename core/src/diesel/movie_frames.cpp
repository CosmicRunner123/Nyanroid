#include "nyan/diesel/movie_frames.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include "nyan/media/ffmpeg_video.hpp"

namespace nyan::diesel {

// Each movie decodes on a thread of its own into a short queue, as Mware's does (0x1006d240): the
// thread runs ahead of the position by up to kAhead frames; the caller drops the queued frames
// before the frame its position falls in and takes that one, waiting only if the thread has not
// reached it yet. The pictures are the ones the caller's thread would make itself: each is the frame
// its position asks for, whatever the timing.
struct MovieFrames::Movie {
    static constexpr size_t kAhead = 3;

    struct Entry {
        int64_t index = 0;     // the frame step (the frame rate's period from the start)
        bool changed = false;  // the picture changed at this step (then `picture` holds it)
        Picture picture;
    };
    struct Start {
        uint32_t pass = 0;
        double at = 0.0;   // the first step's time (then each step's middle)
        int64_t index = 0;
        bool alpha = false;
        bool rewind = false;
    };

    std::unique_ptr<media::FfmpegVideo> video;
    bool failed = false;
    double rate = 30.0;
    uint64_t lastUsed = 0;

    // The caller's side.
    Picture picture;
    bool shown = false;      // a picture has been made
    bool started = false;    // the thread was given a start
    uint32_t pass = 0;
    bool alpha = false;
    int64_t index = -1;      // the step shown

    // Shared with the thread (under `mu`).
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    bool restart = false;    // `start` waits for the thread
    Start start;
    uint64_t generation = 0;
    std::deque<Entry> queue;
    bool ended = false;      // the thread has decoded the last frame
    std::vector<Picture> spare;  // buffers to reuse
    std::thread worker;

    // The thread's own state.
    bool fresh = true;  // opened or rewound: the first frame shows whatever its time
    bool decoded = false;
    std::atomic<int> phase{0};  // for the log of a long wait: 0 waiting, 1 decoding
    std::atomic<uint64_t> workerGen{0};
    std::atomic<int64_t> produced{0};

    ~Movie() {
        {
            std::lock_guard<std::mutex> lock(mu);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void run() {
        std::unique_lock<std::mutex> lock(mu);
        Start s;
        int64_t next = 0;
        uint64_t gen = 0;
        bool first = false;
        for (;;) {
            cv.wait(lock, [&] { return stop || restart || (gen == generation && !ended && queue.size() < kAhead); });
            if (stop) return;
            if (restart) {
                s = start;
                restart = false;
                gen = generation;
                workerGen = gen;
                next = s.index;
                first = true;
            }
            Entry e;
            e.index = next;
            if (!spare.empty()) {
                e.picture = std::move(spare.back());
                spare.pop_back();
            }
            double at = first ? s.at : (static_cast<double>(next) + 0.5) / rate;
            bool rewind = first && s.rewind;
            lock.unlock();
            bool end = false;
            phase = 1;
            e.changed = work(at, rewind, s.alpha, e.picture, end);
            phase = 0;
            lock.lock();
            if (gen != generation || restart) continue;  // a new start came meanwhile: dropped
            first = false;
            ++next;
            ended = end;
            produced = e.index;
            queue.push_back(std::move(e));
            cv.notify_all();
        }
    }

    // The frame shown at `at` (FfmpegVideo shows the last frame at or before a time; the queue's front
    // after an open or a rewind is the first frame even when its time is later than the position).
    bool work(double at, bool rewind, bool withAlpha, Picture& out, bool& end) {
        media::FfmpegVideo& v = *video;
        if (rewind && decoded) {
            v.rewind();
            fresh = true;
        }
        bool c = v.seek(at);
        for (double t = at; fresh && !c && !v.ended() && t < at + 2.0;) {
            t += 1.0 / rate;
            c = v.seek(t);
        }
        fresh = fresh && !c;
        end = v.ended() && !c;
        if (!c) return false;
        decoded = true;
        const auto& src = v.rgba();
        int w = v.width(), h = v.height();
        int outH = withAlpha ? h / 2 : h;
        out.width = w;
        out.height = outH;
        size_t row = static_cast<size_t>(w) * 4;
        out.rgba.resize(row * static_cast<size_t>(outH));
        if (!withAlpha) {
            std::memcpy(out.rgba.data(), src.data(), out.rgba.size());  // opaque already
            return true;
        }
        for (int y = 0; y < outH; ++y) {
            const uint8_t* in = src.data() + static_cast<size_t>(y) * row;
            const uint8_t* below = src.data() + static_cast<size_t>(y + outH) * row;
            uint8_t* o = out.rgba.data() + static_cast<size_t>(y) * row;
            for (int x = 0; x < w; ++x) {
                o[x * 4] = in[x * 4];
                o[x * 4 + 1] = in[x * 4 + 1];
                o[x * 4 + 2] = in[x * 4 + 2];
                o[x * 4 + 3] = below[x * 4 + 2];
            }
        }
        return true;
    }
};

MovieFrames::MovieFrames(OpenFile open, Log log) : open_(std::move(open)), log_(std::move(log)) {}
MovieFrames::~MovieFrames() = default;

const MovieFrames::Picture* MovieFrames::picture(const DrawItem& d) {
    auto& slot = movies_[d.movie];
    if (!slot) {
        slot = std::make_unique<Movie>();
        auto file = open_ ? open_(d.texture) : nullptr;
        if (file) slot->video = media::FfmpegVideo::open(file);
        slot->failed = !slot->video;
        if (!slot->failed) {
            slot->rate = slot->video->frameRate() > 0.0 ? slot->video->frameRate() : 30.0;
            Movie* m = slot.get();
            slot->worker = std::thread([m] { m->run(); });
        }
    }
    Movie& m = *slot;
    m.lastUsed = frame_;
    if (m.failed) return nullptr;

    double at = std::max(static_cast<double>(d.movieTime) / 1e7 + 1.0 / m.rate - 0.0005, 0.0);
    auto want = static_cast<int64_t>(std::floor(at * m.rate + 1e-6));

    std::unique_lock<std::mutex> lock(m.mu);
    if (!m.started || d.moviePass != m.pass || want < m.index || d.movieAlpha != m.alpha) {
        // From this step on (a new pass, or the position went back: from the start).
        for (auto& e : m.queue) m.spare.push_back(std::move(e.picture));
        m.queue.clear();
        m.start = {d.moviePass, at, want, d.movieAlpha, m.started};
        m.restart = true;
        m.ended = false;
        ++m.generation;
        m.started = true;
        m.pass = d.moviePass;
        m.alpha = d.movieAlpha;
        m.index = want - 1;
        lock.unlock();
        m.cv.notify_all();
        lock.lock();
    }
    if (want > m.index) {
        // The queued steps up to this one, in order; the last picture among them is shown.
        for (;;) {
            auto ready = [&] { return !m.queue.empty() || m.ended; };
            if (!m.cv.wait_for(lock, std::chrono::seconds(2), ready)) {
                if (log_) {
                    log_("movie " + d.texture + ": no frame " + std::to_string(want) + " after 2 s (shown " +
                         std::to_string(m.index) + ", generation " + std::to_string(m.generation) + ", restart " +
                         std::to_string(m.restart) + ", thread " + (m.phase ? "decoding" : "waiting") + " at generation " +
                         std::to_string(m.workerGen) + ", last made " + std::to_string(m.produced) + ", queue " +
                         std::to_string(m.queue.size()) + ", ended " + std::to_string(m.ended) + ")");
                }
                m.cv.wait(lock, ready);
            }
            if (m.queue.empty()) break;  // the movie ended: its last picture stays
            Movie::Entry& e = m.queue.front();
            if (e.index > want) break;
            if (e.changed) {
                std::swap(m.picture, e.picture);
                m.picture.serial = ++serial_;
                m.shown = true;
            }
            m.index = e.index;
            m.spare.push_back(std::move(e.picture));
            m.queue.pop_front();
            m.cv.notify_all();  // room in the queue: the thread goes on (it may be waiting for it)
            if (m.index >= want) break;
        }
        lock.unlock();
        m.cv.notify_all();
    }
    return m.shown ? &m.picture : nullptr;
}

void MovieFrames::endFrame() {
    ++frame_;
    for (auto it = movies_.begin(); it != movies_.end();) {
        if (frame_ - it->second->lastUsed > kIdleFrames) it = movies_.erase(it);
        else ++it;
    }
}

}  // namespace nyan::diesel
