#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/common/file_source.hpp"
#include "nyan/diesel/npk_archive.hpp"

namespace nyan::diesel {

// A PE section as the loader maps it.
struct PeSection {
    std::string name;
    uint32_t virtualAddress = 0;  // RVA
    uint32_t virtualSize = 0;
    uint32_t fileOffset = 0;
    uint32_t fileSize = 0;
};

// The game's executable (muramasa_en.exe, totono_en.exe: PE32; Phantom of Inferno's
// phantom.exe.unpacked.exe: PE32+, x86-64): the exe in the folder that imports Mware.dll. Holds its
// bytes for the engine to read behaviour from.
struct ExeInfo {
    std::string fileName;
    Bytes image;  // the file as it is
    uint64_t imageBase = 0;
    bool is64 = false;       // PE32+ (x86-64)
    uint32_t timestamp = 0;  // PE header, seconds since 1970 (UTC)
    std::vector<PeSection> sections;

    size_t pointerSize() const { return is64 ? 8 : 4; }
    // The file offset of a virtual address, or nullopt outside the file's sections.
    std::optional<size_t> offsetOf(uint64_t va) const;
    // The virtual address of a file offset, or nullopt outside the sections.
    std::optional<uint64_t> addressOf(size_t offset) const;
    // The dword at a virtual address.
    std::optional<uint32_t> read32(uint64_t va) const;
    // The pointer at a virtual address (4 bytes, or 8 in a PE32+).
    std::optional<uint64_t> readPointer(uint64_t va) const;
    const PeSection* section(const std::string& name) const;
    // Whether the image holds `text` as a NUL-terminated string (with a NUL before it).
    bool containsString(const std::string& text) const;
    // Whether the exe holds `text` as a name its code can register: containsString, or (a PE32) the
    // NUL-terminated string anywhere, its address used in .text (mojika_en.exe's "IsEcoMode" follows
    // other data with no NUL before it; its registration pushes 0x5e37d8 at 0x4019ac).
    bool holdsName(const std::string& text) const;
    // The DLLs the import directory names, as written ("XAudio2_8.dll").
    std::vector<std::string> imports() const;
    // A class's vtable by its MSVC RTTI name (".?AVCApp@Diesel@@"): the type descriptor holding the
    // name, the complete object locator naming it (at offset 0), and the vtable that locator
    // precedes; nullopt if any is missing. In a PE32+ the locator (signature 1) holds image-relative
    // offsets and the type descriptor's name follows two pointers.
    std::optional<uint64_t> vtable(const std::string& rttiName) const;
};

// The constants the exe's code stores through a global vector's end pointer as it pushes them
// (std::vector<int>::push_back inlined: Diesel's line-break tables, muramasa_en.exe 0x446160):
// `mov eax, [end]; test eax, eax; je short; mov dword [eax], imm32` (a PE32+:
// `mov rax, [rip + rel]; test rax, rax; je short; mov dword [rax], imm32`), grouped by the end
// pointer's address, in the order the code holds them.
std::map<uint64_t, std::vector<uint32_t>> pushedConstants(const ExeInfo& exe);

// Finds and reads the exe; nullopt if the folder has no exe importing Mware.dll.
std::optional<ExeInfo> findExe(const GameFolder& folder);
// Reads a PE file of the folder (Mware.dll); nullopt if it is missing or not a PE32 / PE32+ image.
std::optional<ExeInfo> readPe(const GameFolder& folder, const std::string& name);

// The 32-byte key the exe passes to Mware with every archive it mounts. The exes build it on the
// stack in their startup function with eight `mov dword [ebp/rbp/esp/rsp + d], imm32` at
// consecutive d (muramasa_en.exe 0x401c38, totono_en.exe 0x401e38; phantom.exe 0x140001f40, where
// other stores come between the first and the rest); the candidates such sets give are tried on
// `probe` (system.npk) and the one that opens its index is the key. Throws nyan::Error if none does.
NpkKey findArchiveKey(const ExeInfo& exe, const RandomAccessFile& probe);

}  // namespace nyan::diesel
