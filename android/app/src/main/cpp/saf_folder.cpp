#include "saf_folder.hpp"

namespace {

std::string toStd(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

}  // namespace

SafGameFolder::SafGameFolder(JNIEnv* env, jobject safFolder) {
    env->GetJavaVM(&vm_);
    folder_ = env->NewGlobalRef(safFolder);
    jclass cls = env->GetObjectClass(safFolder);
    list_ = env->GetMethodID(cls, "list", "()[Ljava/lang/String;");
    listDir_ = env->GetMethodID(cls, "listDir", "(Ljava/lang/String;)[Ljava/lang/String;");
    openFd_ = env->GetMethodID(cls, "openFd", "(Ljava/lang/String;)I");
    jmethodID nameId = env->GetMethodID(cls, "displayName", "()Ljava/lang/String;");
    auto name = static_cast<jstring>(env->CallObjectMethod(folder_, nameId));
    name_ = toStd(env, name);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(cls);
}

SafGameFolder::~SafGameFolder() {
    if (JNIEnv* e = env()) e->DeleteGlobalRef(folder_);
}

JNIEnv* SafGameFolder::env() const {
    JNIEnv* e = nullptr;
    if (vm_->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) vm_->AttachCurrentThread(&e, nullptr);
    return e;
}

std::vector<std::string> SafGameFolder::list() const {
    if (!listCache_.empty()) return listCache_;
    JNIEnv* e = env();
    auto arr = static_cast<jobjectArray>(e->CallObjectMethod(folder_, list_));
    if (e->ExceptionCheck()) {
        e->ExceptionClear();
        return {};
    }
    jsize n = e->GetArrayLength(arr);
    for (jsize i = 0; i < n; ++i) {
        auto s = static_cast<jstring>(e->GetObjectArrayElement(arr, i));
        listCache_.push_back(toStd(e, s));
        e->DeleteLocalRef(s);
    }
    e->DeleteLocalRef(arr);
    return listCache_;
}

std::vector<std::string> SafGameFolder::listDir(const std::string& sub) const {
    std::vector<std::string> out;
    JNIEnv* e = env();
    jstring jsub = e->NewStringUTF(sub.c_str());
    auto arr = static_cast<jobjectArray>(e->CallObjectMethod(folder_, listDir_, jsub));
    e->DeleteLocalRef(jsub);
    if (e->ExceptionCheck() || !arr) {
        e->ExceptionClear();
        return out;
    }
    jsize n = e->GetArrayLength(arr);
    for (jsize i = 0; i < n; ++i) {
        auto s = static_cast<jstring>(e->GetObjectArrayElement(arr, i));
        out.push_back(toStd(e, s));
        e->DeleteLocalRef(s);
    }
    e->DeleteLocalRef(arr);
    return out;
}

std::unique_ptr<nyan::RandomAccessFile> SafGameFolder::open(const std::string& name) const {
    JNIEnv* e = env();
    jstring jname = e->NewStringUTF(name.c_str());
    jint fd = e->CallIntMethod(folder_, openFd_, jname);
    e->DeleteLocalRef(jname);
    if (e->ExceptionCheck()) {
        e->ExceptionClear();
        return nullptr;
    }
    if (fd < 0) return nullptr;
    return std::make_unique<nyan::FdFile>(fd, name);
}
