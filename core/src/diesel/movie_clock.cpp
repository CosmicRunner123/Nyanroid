#include "nyan/diesel/movie_clock.hpp"

#include <algorithm>

namespace nyan::diesel {

void MovieClock::play(int32_t loopCount) {
    loops = loopCount;
    playing = true;
    paused = false;
}

void MovieClock::stop() {
    playing = false;
    paused = false;
    position = 0;
}

bool MovieClock::pause() {
    if (!playing) return false;
    paused = true;
    return true;
}

bool MovieClock::resume() {
    if (!playing) return false;
    paused = false;
    return true;
}

void MovieClock::seek(int32_t ms) {
    bool was = playing;
    if (was) stop();
    position = static_cast<int64_t>(ms) * 10000;
    if (was) play(loops);
}

void MovieClock::setTimeScale(float s) {
    timeScale = std::clamp(s, 0.0f, 1.0f);
    if (!(timeScale >= 0.0f)) timeScale = 0.0f;  // NaN
}

bool MovieClock::step(int64_t elapsed) {
    if (!playing) return false;
    if (!paused) {
        position += static_cast<int64_t>(static_cast<double>(elapsed) * static_cast<double>(timeScale));
        position = std::min(position, duration);
    }
    if (position < duration) return false;
    if (loops > 0) --loops;
    if (loops == 0) {
        stop();
        return true;
    }
    position = 0;
    ++pass;
    return false;
}

}  // namespace nyan::diesel
