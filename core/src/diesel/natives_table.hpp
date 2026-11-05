#pragma once

#include <cstdint>
#include <string>

namespace nyan::diesel {

// A native as the exes register it, with its implementation's address in each exe (0: not
// registered by that exe).
struct NativeName {
    const char* name;  // "" ends a list
    uint32_t muramasa;
    uint32_t totono;
    uint64_t phantom;
    uint32_t mojika;
    uint32_t saya;
    // "ret(arg, ...)" with int, float, string, object, instance, ? (not read); "" if unknown.
    const char* signature;
};

struct NativeClass {
    const char* name;  // "" ends the list
    const NativeName* methods;
    bool registered;   // false: an exe has it but Nyanroid does not register it yet
};

extern const NativeName kGlobals[];
extern const NativeClass kNativeClasses[];

// The table's exes, by the file name (any case) and bitness of the running one; Unknown for any other.
enum class NativeExe { Muramasa, Totono, Phantom, Mojika, Saya, Unknown };
NativeExe nativeExe(const std::string& fileName, bool is64);
// The native's address in that exe (0: not registered by it; Unknown: none).
uint64_t nativeAddress(const NativeName& n, NativeExe exe);

}  // namespace nyan::diesel
