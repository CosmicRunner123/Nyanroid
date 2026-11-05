#include "aaudio_output.hpp"

#include <android/log.h>

#include <algorithm>
#include <thread>

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "nyanroid", __VA_ARGS__)

AAudioOutput::AAudioOutput(nyan::audio::Mixer& mixer) : mixer_(mixer) {
    open();
}

AAudioOutput::~AAudioOutput() {
    close();
}

bool AAudioOutput::open() {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, mixer_.outputRate());
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    // Usage and content type are Android 9+ (API 28); Android 8.1 plays as plain media.
    if (__builtin_available(android 28, *)) {
        AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    }
    AAudioStreamBuilder_setDataCallback(b, &AAudioOutput::onData, this);
    AAudioStreamBuilder_setErrorCallback(b, &AAudioOutput::onError, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGW("AAudio open failed: %s", AAudio_convertResultToText(r));
        stream_ = nullptr;
        return false;
    }
    if (!paused_) AAudioStream_requestStart(stream_);
    return true;
}

void AAudioOutput::close() {
    if (stream_) {
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
}

void AAudioOutput::setPaused(bool paused) {
    paused_ = paused;
    if (!stream_) return;
    if (paused) AAudioStream_requestPause(stream_);
    else AAudioStream_requestStart(stream_);
}

aaudio_data_callback_result_t AAudioOutput::onData(AAudioStream*, void* user, void* audio, int32_t frames) {
    auto* self = static_cast<AAudioOutput*>(user);
    self->mixer_.render(static_cast<float*>(audio), static_cast<size_t>(frames));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioOutput::onError(AAudioStream*, void* user, aaudio_result_t error) {
    // Typically the output device changed (headphones plugged in): reopen on another thread,
    // as the stream must not be closed from its own callback.
    if (error != AAUDIO_ERROR_DISCONNECTED) return;
    auto* self = static_cast<AAudioOutput*>(user);
    std::thread([self] {
        self->close();
        self->open();
    }).detach();
}
