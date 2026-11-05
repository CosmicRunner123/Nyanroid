#pragma once

#include <jni.h>

#include "nyan/common/file_source.hpp"

// GameFolder backed by the Kotlin SafFolder object (Storage Access Framework).
// Files are opened as detached descriptors and read with pread(), like desktop files.
class SafGameFolder final : public nyan::GameFolder {
public:
    SafGameFolder(JNIEnv* env, jobject safFolder);
    ~SafGameFolder() override;

    std::vector<std::string> list() const override;
    std::vector<std::string> listDir(const std::string& sub) const override;
    std::unique_ptr<nyan::RandomAccessFile> open(const std::string& name) const override;
    std::string displayName() const override { return name_; }

private:
    JNIEnv* env() const;

    JavaVM* vm_ = nullptr;
    jobject folder_ = nullptr;  // global ref
    jmethodID list_ = nullptr;
    jmethodID listDir_ = nullptr;
    jmethodID openFd_ = nullptr;
    std::string name_;
    mutable std::vector<std::string> listCache_;
};
