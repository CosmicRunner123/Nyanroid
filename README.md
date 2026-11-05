# Nyanroid

Nyanroid is a half-baked vibe coded emulator on Android for CatSystem2 games, Diesel Engine games, White Album 2 English TL, and potentially more visual novels in the future.

It plays Windows visual novels by re-implementing their engines natively, without emulating
Windows. The behaviour follows the engines' own executables, studied with Ghidra.

| Engine | Games it has been tested with |
|---|---|
| CatSystem2 | The Fruit, Labyrinth, and Eden of Grisaia; Nekopara Vol. 3; Amakano and Amakano 2 (Perfect Editions); Happiness 2! Sakura Celebration; Hatsumira; Himawari; Love's Sweet Garnish; REBELLIONS; Yukikoi Melt; Cyanotype Daydream |
| White Album 2's engine | WHITE ALBUM2 Extended Edition (with the English patch) and its Special Contents |
| Diesel Engine | Full Metal Daemon Muramasa; You and Me and Her; Phantom of Inferno; Mojika; The Song of Saya |

**No games are included.** Nyanroid contains no code or data from the games it runs: play your own
copies, copied from their install folders. It is not affiliated with or endorsed by the games'
developers or publishers; their names are used only to say what Nyanroid runs.

How CatSystem2 is emulated: `docs/catsystem2.md`. Where another engine's code goes and what it hooks
into: `docs/adding-an-engine.md`.

## Linux only

This repository is meant for a Linux environment (it is developed on Ubuntu). The build steps, the
desktop player, nyan-cli, and the scripts assume Linux; other systems are untested and may need
changes. The Android app itself runs on any arm64 device with Android 8.1 or later.

## Layout

| Path | What |
|---|---|
| `core/` | Platform-independent C++20 engine code (`nyan_core`), shared by the desktop tools and the Android app |
| `core/include/nyan/common/` | Byte readers, file access (`RandomAccessFile`, `GameFolder`), Shift-JIS, zlib, and which engine made a game folder (`detectEngine`) |
| `core/include/nyan/cs2/` | CatSystem2: Blowfish, Mersenne Twister, V_CODE extraction, KIF archives, asset index, HG-2/HG-3 images, CST scenes, expressions, message markup, `ScenePlayer` (scene interpreter), and `Stage` (what to draw) |
| `core/include/nyan/wa2/`, `core/include/nyan/diesel/` | White Album 2's engine and the Diesel Engine (Squirrel scripts) |
| `core/include/nyan/audio/` | Software mixer (Ogg Vorbis via stb_vorbis) |
| `core/include/nyan/media/` | Movie decoding: MPEG-1 video + MP2 audio via pl_mpeg (`MpegVideo`); ASF with Windows Media Video 7-9 / VC-1 and Windows Media Audio 1, 2, and Pro via FFmpeg (`FfmpegVideo`, same API) |
| `render/` | `nyan_render`: `Session`, the engine-neutral interface the front ends drive (`createSession()` makes the one for a game folder's engine), each engine's session, and the OpenGL ES 3.0 renderers |
| `apps/desktop/` | SDL3 desktop player (`nyan-desktop`) for development; SDL3 is fetched and built by CMake |
| `android/` | Android app (Kotlin, no AndroidX): game library + `GameActivity` (GLSurfaceView), JNI bridge, SAF folder access, and AAudio output |
| `tools/nyan-cli/` | Desktop command-line tool for inspecting game data, one command file per engine |
| `tools/gen/` | Generators: the launcher icon, the Shift-JIS table, and the CFF2 test font |
| `third_party/` | Vendored single-header libraries (stb_image, stb_image_write, stb_truetype, stb_vorbis, and pl_mpeg) and the OpenGL ES headers |
| `cmake/ffmpeg.cmake` | FFmpeg 7.1.1 (LGPL 2.1+), built by CMake from its release tarball (pinned SHA-256; downloaded once into the git-ignored `third_party/downloads/`) for desktop and the Android NDK alike: static, only the ASF demuxer and the Windows Media decoders. `-DNYAN_WITH_FFMPEG=OFF` builds without it (no movies). |

## Building (desktop)

Requires CMake ≥ 3.22, a C++20 compiler, and zlib.

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

## Building (Android)

Requires the Android SDK (platform 35) with NDK `29.0.14206865` and CMake `3.31.6`
(`sdkmanager "ndk;29.0.14206865" "cmake;3.31.6"`). Gradle downloads a JDK 17 toolchain
itself, so a JRE is enough to run it.

```bash
cd android && ./gradlew assembleDebug     # or assembleRelease for a build to share
```

Release builds never carry the fonts in `fonts/` (see Fonts); debug builds do, for testing.
`-PappIdSuffix=.<name>` builds the app as `dev.nyanroid.<name>` ("Nyanroid <name>"), which installs
beside the main app with saves of its own (for trying a branch on the device).

The APK is written to `android/app/build/outputs/apk/debug/app-debug.apk` (arm64-v8a,
Android 8.1+, OpenGL ES 3.0). Copy a game's install folder to the device, tap **Add game
folder** and pick it. In game, one finger is the mouse (tap = left click, drag = drag); the other gestures
(two/three-finger tap, swipe up/down, and hold one/two/three fingers) send mouse or keyboard
input chosen under **Touch controls** in the library (defaults: two-finger tap = right
click, three-finger tap = Space, swipes = mouse wheel, hold one finger = hover the mouse
without clicking, and hold two fingers = Ctrl).
The game's own key bindings decide what that input does. A hardware keyboard and mouse
work too. Back opens the app menu (pausing the game), which stands in for the original's
window menu bar: Return to Title and Settings (the game's in-game system menu) during the
story, Language for games that ship several, Virtual cursor, Press ESC, and Exit Game (back to
the library, as closing the app would). Mouse buttons act on release, so a touch that turns into a
gesture never also clicks.
With **Virtual cursor** on, the screen is a trackpad: a cursor (the game's own where it has one,
else an arrow) moves by as much as the finger does (twice as far with **Cursor speed: Fast** in the
library), a tap clicks where it points, a two-finger tap right-clicks there, and two fingers dragged
up or down turn the mouse wheel; three-finger taps and the holds keep their settings (a one-finger
hold bound to a mouse button drags), and the one-finger swipes are cursor moves. The switch applies to
every game.

## Fonts

Games draw text in fonts of their own or in Windows fonts they name (MS Gothic, Meiryo, ...), which
Nyanroid cannot include. Players add font files they have with the library's **Fonts** button;
before a game whose font is missing starts, the library says so. Without them, MS Gothic (if
added) or Android's own Japanese font stands in. Details: `fonts/README.md`.

## nyan-desktop

```bash
build/apps/desktop/nyan-desktop "<game dir>" [scene]
```

Mouse and keyboard go to the engine as they are and mean what the game's key bindings say
(e.g. in the Grisaia games: click/Enter advance, right click/Esc open the menu, Space hides
the window, Ctrl skips, F1/F2 quick save/load, and F6/F7 previous/next choice). F11 toggles
fullscreen. Audio goes to PulseAudio/PipeWire (loaded at runtime).

For automated checks, `--auto --capture <dir> --every <N> --frames <max>` runs in a hidden
window, auto-advancing, and saves every Nth frame as PNG. Scripted input for captures:
`--click frame:x,y`, `--rclick frame:x,y`, `--key frame:vk` (Windows virtual-key code; also
`--keydown`/`--keyup`), and `--wheel frame:steps`. `--no-bundled-fonts` leaves out the `fonts/`
folder (as a player without those fonts); `--fallback-font <file>` sets the stand-in font.

## nyan-cli

```bash
build/tools/nyan-cli/nyan-cli info    "<game dir>"
build/tools/nyan-cli/nyan-cli list    "<game dir>" [filter]
build/tools/nyan-cli/nyan-cli extract "<game dir>" <out dir> [filter] [--png]
build/tools/nyan-cli/nyan-cli hg      "<game dir>" <image.hg3> <out dir>
build/tools/nyan-cli/nyan-cli verify  "<game dir>"
build/tools/nyan-cli/nyan-cli cst     "<game dir>" <scene>      # dump a scene script
build/tools/nyan-cli/nyan-cli cststats "<game dir>"             # command usage survey
build/tools/nyan-cli/nyan-cli play    "<game dir>" [scene]     # headless playthrough
build/tools/nyan-cli/nyan-cli playall "<game dir>"             # run every scene once
build/tools/nyan-cli/nyan-cli movie   "<game dir>" <name.mpg> [s] # decode speed and sound check
```

`verify` reads and decrypts every asset, checks file signatures, and decodes every
frame of every HG image.

## License

Nyanroid is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (`LICENSE`). It is distributed in the hope that it
will be useful, but WITHOUT ANY WARRANTY. Source: https://github.com/CosmicRunner123/Nyanroid

The third-party code it includes (FFmpeg, stb, pl_mpeg, Squirrel, zstd, libwebp, FAudio's reverb,
SDL for the desktop player, and more) keeps its own licenses: `THIRD_PARTY_NOTICES.txt`, which the
app also shows under About > Licences.

## Credits

- [FelineSystem2](https://github.com/kokseen1/FelineSystem2) (kokseen1): an open source remake of
  the CatSystem2 engine, used as a reference.
- [TriggersTools.CatSystem2](https://github.com/trigger-segfault/TriggersTools.CatSystem2) and its
  wiki (Robert Jordan / trigger-segfault): CatSystem2 file formats and tools, used as a reference.
- [Ghidra](https://ghidra-sre.org) (NSA): used to study the engines' executables.
