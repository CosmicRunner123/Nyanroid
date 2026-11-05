#include "pulse_output.hpp"

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Subset of <pulse/simple.h>; the ABI has been stable for over a decade.
struct pa_sample_spec {
    int format;  // pa_sample_format_t
    uint32_t rate;
    uint8_t channels;
};
struct pa_buffer_attr {
    uint32_t maxlength, tlength, prebuf, minreq, fragsize;
};
constexpr int kSampleFloat32LE = 5;
constexpr int kStreamPlayback = 1;

using NewFn = void* (*)(const char*, const char*, int, const char*, const char*, const pa_sample_spec*, const void*,
                         const pa_buffer_attr*, int*);
using WriteFn = int (*)(void*, const void*, size_t, int*);
using FreeFn = void (*)(void*);

NewFn pNew;
WriteFn pWrite;
FreeFn pFree;

}  // namespace

PulseOutput::PulseOutput(nyan::audio::Mixer& mixer) : mixer_(mixer) {
    lib_ = dlopen("libpulse-simple.so.0", RTLD_NOW);
    if (!lib_) {
        std::fprintf(stderr, "audio: libpulse-simple not available, running silent\n");
        return;
    }
    pNew = reinterpret_cast<NewFn>(dlsym(lib_, "pa_simple_new"));
    pWrite = reinterpret_cast<WriteFn>(dlsym(lib_, "pa_simple_write"));
    pFree = reinterpret_cast<FreeFn>(dlsym(lib_, "pa_simple_free"));
    if (!pNew || !pWrite || !pFree) return;

    pa_sample_spec spec{kSampleFloat32LE, static_cast<uint32_t>(mixer.outputRate()), 2};
    uint32_t bytesPer20ms = static_cast<uint32_t>(mixer.outputRate() / 50 * 2 * sizeof(float));
    pa_buffer_attr attr{UINT32_MAX, bytesPer20ms * 3, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    int err = 0;
    stream_ = pNew(nullptr, "Nyanroid", kStreamPlayback, nullptr, "game", &spec, nullptr, &attr, &err);
    if (!stream_) {
        std::fprintf(stderr, "audio: could not open PulseAudio stream (error %d)\n", err);
        return;
    }
    ok_ = true;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

PulseOutput::~PulseOutput() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (stream_) pFree(stream_);
    if (lib_) dlclose(lib_);
}

void PulseOutput::run() {
    constexpr size_t kFrames = 480;  // 10 ms at 48 kHz
    std::vector<float> buf(kFrames * 2);
    while (running_) {
        mixer_.render(buf.data(), kFrames);
        int err = 0;
        if (pWrite(stream_, buf.data(), buf.size() * sizeof(float), &err) < 0) break;
    }
}
