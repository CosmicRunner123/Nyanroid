# Adding an engine

Nyanroid emulates each engine it supports: the game's own data, scripts and executable decide
what happens, never code written for one game. CatSystem2 is the first engine and is still being
worked on. This page is for building a second engine alongside that work: where the new engine's
files go, which shared files it hooks into, and what any change to shared code has to keep.

## Where an engine's files go

`<e>` is the engine's short name, as `cs2` is CatSystem2's.

| What | Where |
|---|---|
| The engine core: archives, images, scripts, scene state, saves | `core/include/nyan/<e>/`, `core/src/<e>/`, namespace `nyan::<e>` |
| Its `Session` (what the front ends drive) and its drawing | `render/include/nyan/render/<e>_session.hpp`, `render/src/<e>_session.cpp`, more in `render/src/<e>/` |
| Its nyan-cli commands | `tools/nyan-cli/<e>_commands.cpp` |
| Its unit tests | a test file of its own in `tests/` |
| Its notes and status | `docs/<e>-*.md` |

CatSystem2's own files predate these names: its session is `GameSession`
(`render/include/nyan/render/game_session.hpp`) and its renderer `StageRenderer`. They stay as
they are.

## Shared hook points

A new engine adds one case to each of these files and changes nothing else in them:

1. `core/include/nyan/common/engine.hpp`: a value in `enum class Engine`.
2. `core/src/common/engine.cpp`: its name in `engineName()` and its check in `detectEngine()`,
   judged by the folder's files, the most specific check first. Every front end finds a folder's
   engine only through this function: the Android library before it adds a folder, `createSession()`,
   and nyan-cli.
3. `render/src/session.cpp`: the case in `createSession()` that makes its session.
4. `tools/nyan-cli/main.cpp`: its command set in `kCommandSets`, declared in
   `tools/nyan-cli/commands.hpp`.
5. The build lists: `core/CMakeLists.txt` (its core sources as a block of their own),
   `render/CMakeLists.txt`, `tools/nyan-cli/CMakeLists.txt`, and the top-level `CMakeLists.txt`
   for its test program.

`engineName()` and `createSession()` switch over `Engine`, so the compiler warns (`-Wswitch`)
where a new value has no case yet.

## The session

`render/include/nyan/render/session.hpp` is everything the desktop player
(`apps/desktop/main.cpp`) and the Android app (`android/app/src/main/cpp/jni_bridge.cpp`) use:

- the game's title and virtual screen size, and where its log messages go;
- `frame()` (step to real time and draw), `step()` (one frame, for captures), `renderFrame()`, and
  `frameNumber()`, which timed captures and scripted input go by;
- raw input as the PC games get it: Windows virtual-key codes, mouse buttons 1 (left) and 3
  (right), wheel steps, pointer moves in framebuffer pixels, and `cancelPress()` for a touch that
  became a gesture. The engine maps them through the game's own bindings, so Android's touch
  controls (`Controls.kt`) work for every engine unchanged;
- dialogs the game waits on (text entry, a question), which the platform shows;
- skip, auto and force skip;
- the app menu: `inStory()`, `returnToTitle()`, `openSettings()`, `suspend()` (the app goes to the
  background or closes: keep the story's place and the global data), `releaseHeld()`,
  `quitRequested()`;
- the game's languages, the one in use, and choosing one;
- `cursor()`: the game's own mouse cursor picture and hotspot, which Android's virtual cursor
  draws (optional: an engine without one leaves the default, and the app draws an arrow).

If the new engine needs something the interface lacks, adding it is a change to shared code
(below): `GameSession` implements the addition without changing what CatSystem2 does.

## Shared code and the CatSystem2 baselines

Shared code is everything outside an engine's own folders: `common`, `audio` and `media` in
`core/include/nyan/` and `core/src/`, `session.hpp` and `session.cpp`, `render/src/font.*`, the
desktop player, the JNI bridge and the Kotlin app, nyan-cli's `main.cpp` and `commands.hpp`, and
the build files. Any change there has to keep the CatSystem2 regression baselines:

- `ctest` passes (in `build/`);
- for each CatSystem2 game at hand, `build/tools/nyan-cli/nyan-cli playall "<game dir>"` gives
  the same first line (scenes / stuck / errors) and the same unhandled and missing lists before
  and after the change;
- nyan-desktop and the debug APK (`cd android && ./gradlew assembleDebug`) build.

## Code in core/cs2 another engine may want

Some code in `core/cs2/` is not CatSystem2's alone: PSB and E-mote (`psb`, `emote`,
`emote_player`, `emote_profile`), Blowfish, the Mersenne Twister and the PE resource reader
(`pe_resources`). Don't move any of it without asking the user first: what moves, and where to,
depends on the next engine. Until then, use it from `core/cs2/` where it is.

## Front ends

- **Desktop**: `nyan-desktop <game dir>` finds the engine itself. Development options that reach
  into one engine (CatSystem2's scene argument, `--conf`, `--lang`, `--fes`, `--skip-to`) sit behind
  a check of `session->engine()`; an engine's own options go the same way.
- **Android**: the library adds a folder if `NativeBridge.detectEngine` (that is,
  `nyan::detectEngine`) finds an engine for it, and `NativeBridge.create` makes the session with
  `createSession()`. A new engine needs no Kotlin change. The library's title for a game
  (`SafFolder.guessTitle`) comes from CatSystem2's `config/startup.xml`; other engines' games show
  their folder name until they get a title source there.
- **nyan-cli**: a command line goes first to the command set of the engine whose game folder it
  names (its first argument), then to the others in `kCommandSets` order, so engines can share
  command names such as `playall`. The usage text is every set's lines in that order.

## Trying a branch build on the tablet

```bash
cd android && ./gradlew assembleDebug -PappIdSuffix=.<e>
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

This installs "Nyanroid `<e>`" as `dev.nyanroid.<e>` next to the main app, with its own library,
saves, settings and `Android/data/dev.nyanroid.<e>/files/fonts` folder; game folders have to be
added to its library separately. Without the property the build is `dev.nyanroid` again.
