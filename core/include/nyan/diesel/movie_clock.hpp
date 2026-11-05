#pragma once

#include <cstdint>

namespace nyan::diesel {

// A movie's clock as Mware's CMovieMediaFoundation keeps it (Mware.dll: play 0x1006d970, stop
// 0x1006dbb0, pause 0x1006dcb0, resume 0x1006dcf0, seek 0x1006dd30, setLoop 0x1006d8b0,
// setTimeScale 0x1006d8d0, and its thread's loop 0x1006d240). Times are 100 ns units, as Media
// Foundation's. The thread advances the position by the elapsed time times the time scale while the
// movie plays unpaused, up to the duration; at the duration a pass ends: the plays left count down
// (a count of -1 never does), and at 0 the movie stops (so 0 and 1 both play once), else the
// position goes back to 0 for the next pass. Stopping puts the position back to 0; playing goes on
// from where the position is.
struct MovieClock {
    int64_t duration = 0;    // +0x3c: MF_PD_DURATION
    int64_t position = 0;    // +0x4c
    bool playing = false;    // +0x24
    bool paused = false;     // +0x28
    int32_t loops = 0;       // +0x34: plays left (-1: for ever)
    float timeScale = 1.0f;  // +0x30: within [0, 1]
    uint32_t pass = 0;       // which pass the position is in (counted from the open)

    void play(int32_t loopCount);
    void stop();
    // False (the exe's -1) when not playing; paused (or resumed) otherwise.
    bool pause();
    bool resume();
    // The position set to `ms`; a playing movie is stopped and played again with the plays it had
    // left. Media Foundation then moves the position to the first sample it decodes there (Fitted:
    // the position is taken as asked).
    void seek(int32_t ms);
    void setLoop(int32_t n) { loops = n; }
    void setTimeScale(float s);
    // The thread's work for `elapsed` (100 ns of real time); true if the movie stopped at its end.
    bool step(int64_t elapsed);

    int32_t durationMs() const { return static_cast<int32_t>(duration / 10000); }
    int32_t positionMs() const { return static_cast<int32_t>(position / 10000); }
};

}  // namespace nyan::diesel
