#pragma once

#include <aaudio/AAudio.h>

#include <atomic>

#include "nyan/audio/mixer.hpp"

// Low-latency AAudio stream that pulls from a Mixer on AAudio's callback thread.
class AAudioOutput {
public:
    explicit AAudioOutput(nyan::audio::Mixer& mixer);
    ~AAudioOutput();

    void setPaused(bool paused);

private:
    bool open();
    void close();
    static aaudio_data_callback_result_t onData(AAudioStream*, void* user, void* audio, int32_t frames);
    static void onError(AAudioStream*, void* user, aaudio_result_t error);

    nyan::audio::Mixer& mixer_;
    AAudioStream* stream_ = nullptr;
    std::atomic<bool> paused_{false};
};
