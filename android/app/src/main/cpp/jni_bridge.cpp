// JNI entry points for dev.nyanroid.NativeBridge.

#include <android/log.h>
#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "aaudio_output.hpp"
#include "nyan/common/engine.hpp"
#include "nyan/render/session.hpp"
#include "saf_folder.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "nyanroid", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "nyanroid", __VA_ARGS__)

namespace {

struct Instance {
    nyan::audio::Mixer mixer{48000};
    std::unique_ptr<AAudioOutput> audio;
    std::unique_ptr<nyan::render::Session> session;
};

std::string g_lastError;

Instance* from(jlong h) {
    return reinterpret_cast<Instance*>(h);
}

std::string toStd(JNIEnv* env, jstring s) {
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

// Newline-separated folders as a list.
std::vector<std::string> splitDirs(const std::string& dirs) {
    std::vector<std::string> out;
    for (size_t start = 0; start <= dirs.size();) {
        size_t nl = dirs.find('\n', start);
        std::string d = dirs.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!d.empty()) out.push_back(d);
        start = nl == std::string::npos ? dirs.size() + 1 : nl + 1;
    }
    return out;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_dev_nyanroid_NativeBridge_create(JNIEnv* env, jobject, jobject folder, jstring fontPath,
                                                            jstring saveDir, jstring userFontDir) {
    auto inst = std::make_unique<Instance>();
    try {
        nyan::render::Session::Options opt;
        // As on desktop: an animated event CG alone holds up to 61 full-screen frames (214 MB).
        opt.textureBudgetBytes = 384u * 1024 * 1024;
        opt.saveDirectory = toStd(env, saveDir);
        // Bundled fonts and the user's extra fonts, newline-separated.
        opt.fontDirectories = splitDirs(toStd(env, userFontDir));
        if (auto f = nyan::FdFile::open(toStd(env, fontPath))) opt.fallbackFont = f->readAll();
        inst->session = nyan::render::createSession(std::make_unique<SafGameFolder>(env, folder), &inst->mixer,
                                                    std::move(opt));
        inst->session->setLog([](const std::string& m) { LOGI("%s", m.c_str()); });
        LOGI("loaded %s (%dx%d)", inst->session->title().c_str(), inst->session->screenWidth(),
             inst->session->screenHeight());
    } catch (const std::exception& e) {
        g_lastError = e.what();
        LOGE("create failed: %s", e.what());
        return 0;
    }
    inst->audio = std::make_unique<AAudioOutput>(inst->mixer);
    return reinterpret_cast<jlong>(inst.release());
}

JNIEXPORT jstring JNICALL Java_dev_nyanroid_NativeBridge_lastError(JNIEnv* env, jobject) {
    return env->NewStringUTF(g_lastError.c_str());
}

// The engine of the game in a folder ("CatSystem2"), or null if Nyanroid knows none: the library
// checks folders with this before adding them.
JNIEXPORT jstring JNICALL Java_dev_nyanroid_NativeBridge_detectEngine(JNIEnv* env, jobject, jobject folder) {
    try {
        SafGameFolder f(env, folder);
        if (auto engine = nyan::detectEngine(f)) return env->NewStringUTF(nyan::engineName(*engine));
    } catch (const std::exception& e) {
        LOGE("detectEngine: %s", e.what());
    }
    return nullptr;
}

// The fonts the game names that neither it nor the font folders (newline-separated) have, which
// Android's font would stand in for; empty if none (or on any error, so the game still starts).
JNIEXPORT jobjectArray JNICALL Java_dev_nyanroid_NativeBridge_missingFonts(JNIEnv* env, jobject, jobject folder, jstring fontDirs) {
    std::vector<std::string> missing;
    try {
        SafGameFolder f(env, folder);
        missing = nyan::render::missingFonts(f, splitDirs(toStd(env, fontDirs)));
    } catch (const std::exception& e) {
        LOGE("missingFonts: %s", e.what());
    }
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(missing.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < missing.size(); ++i) {
        env->SetObjectArrayElement(out, static_cast<jsize>(i), env->NewStringUTF(missing[i].c_str()));
    }
    return out;
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_frame(JNIEnv*, jobject, jlong h, jint w, jint hgt) {
    try {
        from(h)->session->frame(w, hgt);
    } catch (const std::exception& e) {
        LOGE("frame: %s", e.what());
    }
}

JNIEXPORT jboolean JNICALL Java_dev_nyanroid_NativeBridge_quitRequested(JNIEnv*, jobject, jlong h) {
    return from(h)->session->quitRequested() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_destroy(JNIEnv*, jobject, jlong h) {
    std::unique_ptr<Instance> inst(from(h));
    inst->audio.reset();  // stop callbacks before the mixer goes away
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_pointer(JNIEnv*, jobject, jlong h, jfloat x, jfloat y) {
    from(h)->session->pointerMove(x, y);
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_mouseButton(JNIEnv*, jobject, jlong h, jint button,
                                                                 jboolean down, jfloat x, jfloat y) {
    from(h)->session->mouseButton(button, down, x, y);
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_cancelPress(JNIEnv*, jobject, jlong h) {
    from(h)->session->cancelPress();
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_wheel(JNIEnv*, jobject, jlong h, jint steps) {
    from(h)->session->wheel(steps);
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_key(JNIEnv*, jobject, jlong h, jint vk, jboolean down) {
    from(h)->session->key(vk, down);
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_setSkip(JNIEnv*, jobject, jlong h, jboolean on) {
    from(h)->session->setSkip(on);
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_setAuto(JNIEnv*, jobject, jlong h, jboolean on) {
    from(h)->session->setAuto(on);
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_flush(JNIEnv*, jobject, jlong h) {
    from(h)->session->suspend();
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_pauseAudio(JNIEnv*, jobject, jlong h, jboolean paused) {
    if (auto& a = from(h)->audio) a->setPaused(paused);
}

// ---- The app's menu (Back) ----

JNIEXPORT jboolean JNICALL Java_dev_nyanroid_NativeBridge_inStory(JNIEnv*, jobject, jlong h) {
    return from(h)->session->inStory();
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_releaseHeld(JNIEnv*, jobject, jlong h) {
    from(h)->session->releaseHeld();
    from(h)->session->cancelPress();
}

JNIEXPORT jboolean JNICALL Java_dev_nyanroid_NativeBridge_returnToTitle(JNIEnv*, jobject, jlong h) {
    bool ok = from(h)->session->returnToTitle();
    LOGI("menu: return to title %s", ok ? "done" : "unavailable");
    return ok;
}

JNIEXPORT jboolean JNICALL Java_dev_nyanroid_NativeBridge_openSettings(JNIEnv*, jobject, jlong h) {
    bool ok = from(h)->session->openSettings();
    LOGI("menu: settings %s", ok ? "opened" : "unavailable");
    return ok;
}

JNIEXPORT jobjectArray JNICALL Java_dev_nyanroid_NativeBridge_languages(JNIEnv* env, jobject, jlong h) {
    const std::vector<std::string> langs = from(h)->session->languages();
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(langs.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < langs.size(); ++i) {
        env->SetObjectArrayElement(out, static_cast<jsize>(i), env->NewStringUTF(langs[i].c_str()));
    }
    return out;
}

JNIEXPORT jint JNICALL Java_dev_nyanroid_NativeBridge_language(JNIEnv*, jobject, jlong h) {
    return from(h)->session->language();
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_setLanguage(JNIEnv*, jobject, jlong h, jint index) {
    from(h)->session->setLanguage(index);
}

// A dialog the game waits on (PC: a window of its own): [serial, input 1/0, title, message, value,
// max characters, type, cancel 1/0], or null.
JNIEXPORT jobjectArray JNICALL Java_dev_nyanroid_NativeBridge_dialog(JNIEnv* env, jobject, jlong h) {
    const auto d = from(h)->session->dialog();
    if (!d) return nullptr;
    std::string fields[] = {std::to_string(d->serial), d->input ? "1" : "0", d->title, d->message, d->value,
                            std::to_string(d->maxChars), std::to_string(d->type), d->cancel ? "1" : "0"};
    jobjectArray out = env->NewObjectArray(8, env->FindClass("java/lang/String"), nullptr);
    for (jsize i = 0; i < 8; ++i) env->SetObjectArrayElement(out, i, env->NewStringUTF(fields[i].c_str()));
    return out;
}

JNIEXPORT void JNICALL Java_dev_nyanroid_NativeBridge_answerDialog(JNIEnv* env, jobject, jlong h, jboolean ok, jstring text) {
    from(h)->session->answerDialog(ok, toStd(env, text));
}

// The game's virtual screen size: [width, height].
JNIEXPORT jintArray JNICALL Java_dev_nyanroid_NativeBridge_screenSize(JNIEnv* env, jobject, jlong h) {
    const jint size[2] = {from(h)->session->screenWidth(), from(h)->session->screenHeight()};
    jintArray out = env->NewIntArray(2);
    env->SetIntArrayRegion(out, 0, 2, size);
    return out;
}

// The game's mouse cursor for the virtual cursor: [serial, width, height, hotX, hotY, ARGB pixels...],
// [0] for the system's arrow, or null while it is still the one with serial `seen`.
JNIEXPORT jintArray JNICALL Java_dev_nyanroid_NativeBridge_cursor(JNIEnv* env, jobject, jlong h, jint seen) {
    const nyan::render::Session::Cursor* c = from(h)->session->cursor();
    const jint serial = c ? static_cast<jint>(c->serial) : 0;
    if (serial == seen) return nullptr;
    if (!c) {
        jintArray out = env->NewIntArray(1);
        env->SetIntArrayRegion(out, 0, 1, &serial);
        return out;
    }
    const jint head[5] = {serial, c->width, c->height, c->hotX, c->hotY};
    jintArray out = env->NewIntArray(static_cast<jsize>(5 + c->argb.size()));
    env->SetIntArrayRegion(out, 0, 5, head);
    env->SetIntArrayRegion(out, 5, static_cast<jsize>(c->argb.size()), reinterpret_cast<const jint*>(c->argb.data()));
    return out;
}

}  // extern "C"
