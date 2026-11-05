# Libraries vendored for the Diesel Engine

Built by `vendor.cmake` into the static library `nyan_diesel_vendor` (linked into `nyan_core`),
without our warning flags. Only the Diesel Engine uses them.

## Squirrel 2.2.4 stable (`squirrel/`) — ALTERED

- Source: `squirrel_2.2.4_stable.tar.gz` from the Squirrel project on SourceForge
  (`squirrel2/squirrel 2.2.4 stable`, 2009-11-15, 419,567 bytes, SHA-256
  `936ca05469ab19f16a730685ef4ad1ca5a3838195765d47935a4ba9052d8b2d4`), downloaded 2026-10-23.
- Copied: `include/`, `squirrel/`, `sqstdlib/`, `COPYRIGHT`, `README`, `HISTORY` (not the `sq`
  shell, samples or documentation).
- Licence: zlib-style (`COPYRIGHT`): altered versions must be marked as such. This copy is altered;
  every change carries a `Nyanroid` comment:
  - All files converted from CRLF to LF line endings.
  - `include/squirrel.h`: `SQInteger`, `SQUnsignedInteger` and `SQHash` are 32-bit on every host
    (stock 2.2.4 widens them on 64-bit hosts). The Diesel games' bytecode stores 4-byte integers and
    their scripts run on 32-bit integer arithmetic and 32-bit string hashes (so table order matches
    the PC). `SQRawObjectVal` is `uintptr_t` and `SQ_OBJECT_RAWINIT()` always clears it.
  - `squirrel/sqobject.h`: `SQObjectPtr::operator=(SQInteger)` and `operator=(SQFloat)` clear the
    pointer-sized raw value first (it is compared as a whole in table lookups).
  - `squirrel/sqtable.h`: `hashptr` casts through `uintptr_t`.
  - `squirrel/sqcompiler.cpp`: scalar objects clear their raw value.
  - `squirrel/squtils.h`: `sq_vm_malloc` / `sq_vm_realloc` / `sq_vm_free` declared before the
    templates that use them (needed by current compilers).
  - `squirrel/sqopcodes.h`, `squirrel/sqvm.cpp`, `squirrel/sqfuncstate.cpp`: opcode 0x3D
    (`_OP_TOSTRING`, our name), Nitroplus's addition in the Diesel exes' VM: `TARGET =
    tostring(STK(arg1))` through `SQVM::ToString` (muramasa_en.exe `SQVM::Execute` 0x4100a0, case
    0x3D, calling 0x40e570, which matches stock `SQVM::ToString`).
  - `squirrel/sqvm.cpp`: a VM made from another (`newthread`) inherits its foreign pointer (Nyanroid's
    host lookup; no effect on the scripts).
  - `squirrel/sqstring.h`, `squirrel/sqstate.h`, `squirrel/sqstate.cpp`, `squirrel/sqapi.cpp`,
    `include/squirrel.h`: `sq_openex(stack, hash64)` opens a VM whose string table hashes as a 64-bit
    build does (`_hashstr64`: the stock loop on a 64-bit value, kept to its low 32 bits, which is all
    a table's mask reads), for Phantom of Inferno's 64-bit exe; `sq_open` is `sq_openex(stack,
    false)`. Integers stay 32 bits (the 64-bit exe's scripts are narrowed on load:
    `core/src/diesel/bytecode.cpp`).
  - `sqstdlib/sqstdmath.cpp`: `rand`, `srand` and `RAND_MAX` are MSVC's (the exes' C runtime), not the
    host's: `state = state * 214013 + 2531011`, `rand = (state >> 16) & 0x7fff`, seed 1.
  - The `type()` macro is renamed `sq_type()` everywhere (as Squirrel 3 did): it collided with the C++
    standard headers (libc++'s `<limits>`, C++20's `<compare>`).
- Built as C++14.

## zstd 1.5.7 (`zstd/`)

- Source: the zstd 1.5.7 tree bundled in the Rust crate `zstd-sys 2.0.16+zstd.1.5.7` (local cargo
  registry), unchanged.
- Copied: `LICENSE` (BSD; zstd is dual BSD / GPLv2, used here under BSD), `lib/zstd.h`,
  `lib/zstd_errors.h`, the decoder's files of `lib/common/` and `lib/decompress/`, and
  `lib/compress/` without `zstdmt_compress.c` (its header only: no multithreading). The compressor
  writes the saves' MWCF files as Mware does (level 3). No dictionary builder or legacy formats.
- Built with `ZSTD_DISABLE_ASM=1` (no `huf_decompress_amd64.S`), `ZSTD_LEGACY_SUPPORT=0`.

## libwebp 1.3.1 decoder (`webp/`)

- Source: the libwebp 1.3.1 tree bundled in the Rust crate `libwebp-sys 0.9.6` (local cargo
  registry), unchanged.
- Copied: `COPYING` (BSD), `PATENTS`, `AUTHORS`, `src/webp/*.h`, `src/dec/`, the decoder's files of
  `src/dsp/` and `src/utils/` (no encoder, mux, demux; no MIPS / MSA code), and two encoder headers
  the lossless decoder includes for type definitions (`src/enc/histogram_enc.h`,
  `src/enc/backward_references_enc.h`).
- Built single-threaded (no `WEBP_USE_THREAD`); SSE2 / NEON paths by the compiler's defaults.
