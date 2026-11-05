#pragma once

#include <atomic>
#include <thread>

#include "nyan/audio/mixer.hpp"

// Plays a Mixer through PulseAudio (or PipeWire's Pulse server) using the
// "simple" API, loaded at runtime so no development headers are required.
class PulseOutput {
public:
    explicit PulseOutput(nyan::audio::Mixer& mixer);
    ~PulseOutput();
    bool ok() const { return ok_; }

private:
    void run();

    nyan::audio::Mixer& mixer_;
    void* lib_ = nullptr;
    void* stream_ = nullptr;
    std::atomic<bool> running_{false};
    bool ok_ = false;
    std::thread thread_;
};
