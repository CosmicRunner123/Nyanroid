# Nyanroid

Nyanroid is a half-baked vibe coded emulator on Android for CatSystem2 games, Diesel Engine games, White Album 2 English TL, and potentially more visual novels in the future.

It plays Windows visual novels by re-implementing their engines natively, without emulating
Windows. The behaviour follows the engines' own executables, studied with Ghidra.

| Engine | Games it has been tested with |
|---|---|
| CatSystem2 | The Fruit, Labyrinth and Eden of Grisaia; Nekopara Vol. 3; Amakano and Amakano 2 (Perfect Editions); Happiness 2! Sakura Celebration; Hatsumira; Himawari; Love's Sweet Garnish; REBELLIONS; Yukikoi Melt; Cyanotype Daydream |
| White Album 2's engine | WHITE ALBUM2 Extended Edition (with the English patch) and its Special Contents |
| Diesel Engine | Full Metal Daemon Muramasa; You and Me and Her; Phantom of Inferno; Mojika; The Song of Saya |

**No games are included.** Nyanroid contains no code or data from the games it runs: play your own
copies, copied from their install folders. It is not affiliated with or endorsed by the games'
developers or publishers; their names are used only to say what Nyanroid runs.

Where another engine's code goes and what it hooks into: `docs/adding-an-engine.md`.

## Layout

| Path | What |
|---|---|
| `core/` | Platform-independent C++20 engine code (`nyan_core`), shared by the desktop tools and the Android app |
| `core/include/nyan/common/` | Byte readers, file access (`RandomAccessFile`, `GameFolder`), Shift-JIS, zlib, which engine made a game folder (`detectEngine`) |
| `core/include/nyan/cs2/` | CatSystem2: Blowfish, Mersenne Twister, V_CODE extraction, KIF archives, asset index, HG-2/HG-3 images, CST scenes, expressions, message markup, `ScenePlayer` (scene interpreter) and `Stage` (what to draw) |
| `core/include/nyan/wa2/`, `core/include/nyan/diesel/` | White Album 2's engine and the Diesel Engine (Squirrel scripts) |
| `core/include/nyan/audio/` | Software mixer (Ogg Vorbis via stb_vorbis) |
| `core/include/nyan/media/` | Movie decoding: MPEG-1 video + MP2 audio via pl_mpeg (`MpegVideo`); ASF with Windows Media Video 7-9 / VC-1 and Windows Media Audio 1, 2, Pro via FFmpeg (`FfmpegVideo`, same API) |
| `render/` | `nyan_render`: `Session`, the engine-neutral interface the front ends drive (`createSession()` makes the one for a game folder's engine), each engine's session and the OpenGL ES 3.0 renderers |
| `apps/desktop/` | SDL3 desktop player (`nyan-desktop`) for development; SDL3 is fetched and built by CMake |
| `android/` | Android app (Kotlin, no AndroidX): game library + `GameActivity` (GLSurfaceView), JNI bridge, SAF folder access, AAudio output |
| `tools/nyan-cli/` | Desktop command-line tool for inspecting game data, one command file per engine |
| `tools/gen/` | Generators: the launcher icon, the Shift-JIS table, the CFF2 test font |
| `third_party/` | Vendored single-header libraries (stb_image, stb_image_write, stb_truetype, stb_vorbis, pl_mpeg) and the OpenGL ES headers |
| `cmake/ffmpeg.cmake` | FFmpeg 7.1.1 (LGPL 2.1+), built by CMake from its release tarball (pinned SHA-256; downloaded once into the git-ignored `third_party/downloads/`) for desktop and the Android NDK alike: static, only the ASF demuxer and the Windows Media decoders. `-DNYAN_WITH_FFMPEG=OFF` builds without it (no movies). |

## Building (desktop)

Requires CMake ≥ 3.22, a C++20 compiler and zlib.

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
(two/three-finger tap, swipe up/down, hold one/two/three fingers) send mouse or keyboard
input chosen under **Touch controls** in the library (defaults: two-finger tap = right
click, three-finger tap = Space, swipes = mouse wheel, hold one finger = hover the mouse
without clicking, hold two fingers = Ctrl).
The game's own key bindings decide what that input does. A hardware keyboard and mouse
work too. Back opens the app menu (pausing the game), which stands in for the original's
window menu bar: Return to Title and Settings (the game's in-game system menu) during the
story, Language for games that ship several, Virtual cursor, Press ESC, and Exit Game (back to
the library, as closing the app would). Mouse buttons act on release, so a touch that turns into a
gesture never also clicks.
With **Virtual cursor** on, the screen is a trackpad: a cursor (the game's own where it has one,
else an arrow) moves by as much as the finger does (twice as far with **Cursor speed: Fast** in the
library), a tap clicks where it points, a two-finger tap right-clicks there and two fingers dragged
up or down turn the mouse wheel; three-finger taps and the holds keep their settings (a one-finger
hold bound to a mouse button drags), the one-finger swipes are cursor moves. The switch applies to
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
the window, Ctrl skips, F1/F2 quick save/load, F6/F7 previous/next choice). F11 toggles
fullscreen. Audio goes to PulseAudio/PipeWire (loaded at runtime).

For automated checks, `--auto --capture <dir> --every <N> --frames <max>` runs in a hidden
window, auto-advancing, and saves every Nth frame as PNG. Scripted input for captures:
`--click frame:x,y`, `--rclick frame:x,y`, `--key frame:vk` (Windows virtual-key code; also
`--keydown`/`--keyup`), `--wheel frame:steps`. `--no-bundled-fonts` leaves out the `fonts/`
folder (as a player without those fonts), `--fallback-font <file>` sets the stand-in font.

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

## CatSystem2 data notes

- The V_CODE2 that keys the archives lives in the engine executable's resources. For
  Grisaia games that is the `.bin` file, not the `.exe` launcher (Fruit is the exception).
- `updateNN.int` archives are patches: loaded after the base archives, in ascending
  order, overriding same-named entries. They aren't listed in `startup.xml`.
- HG image frame IDs are not unique within a file.

## How CatSystem2 is emulated

CatSystem2's own scene commands live in `kcs.int/sscript.kcs`, a ~700 KB compiled
bytecode program, and its menus/flow in FE scripts (`.fes`). Nyanroid does not run
that bytecode; it reimplements the behaviour of the scene commands natively
(`ScenePlayer`) — high-level emulation, as FelineSystem2 does.

- Scripts run at 60 frames/second. Image changes are made to a pending stage and
  shown on the next `wait`/input, using the transition set by `rdraw N` (crossfade)
  or `wipe`/`rwipe <mask> N` (mask wipe). `movie <name> [mode] [volume]` plays movie.int's
  `<name>.mpg` (MPEG-1 + MP2, every game) full screen while the story waits; mode bit 2 lets a
  left click end it, bit 1 a right click (default 2); its sound is on the `vol_play` volume.
  `pl N video <name>` shows a clip as plane N's picture (Eden's in-scene animations), started by
  `pl N vcom play [n]` (n times, -1 looping; the last picture stays) and held by `vcom stop`. `draw [N] [plane]` (defaults from `drawdef`: 20,
  plane 0) cross-fades only that plane, over N - 2 frames; the other planes change at once.
- Character sprites: `cg 0 Abc01l,2,1,5,5 x y` stacks the parts `Abc01l_2`,
  `Abc01l_01`, `Abc01l_005`, ... (argument k zero-padded to k digits; missing parts
  are skipped). `(x, y)` is where the image's base point lands.
- FE scripts (`.fes`: menus, eyecatches, the message window) run in `FesInstance`, an
  interpreter for the script language itself: `#MACRO`/`#SUBCOMMAND`/`#DEFINE`/`#OBJECT`
  tables, labelled blocks, `if`/`next`/`call`/`wait`/`exit`, object methods (motions come
  from each script's own `#SUBCOMMAND` definitions), and button events (FOCUS, PUSH_L, ...).
  Scenes start them with `fesjs` (e.g. the date/location eyecatch). The message window runs
  its theme's script (`meswnd.fes`) and is driven through the callbacks it exposes
  (`MES_SHOW`, `MES_WAITINPUT`, `MES_MODE`, `MES_SETSTATE`); its `send 0xffff0003 0 <n>`
  requests (13 skip, 14 auto, ...) go back to the engine.
- Games boot through their own `flow.fes`: logo, title screen and menus run as nested FE
  scripts (`execfes`/`endfes`, `_EXITCODE_`), and `execkcs sscript` hands over to the scene
  engine using the boot flag and first-scene string from `adv.xml` (`$512`, `str 200`).
  Blocks that end without `next` re-run every frame (polling); labels written back to back are
  separate empty blocks (`#SLEEP` above `#btn.FOCUS`, an unused `#MES_MODE` above `#MES_POS`), not
  aliases of the code below them. Click identifiers are 0 when
  nothing was clicked, and hidden-but-enabled buttons act as hit maps.
- Input follows the engine's binding table (`startup.xml` `<KEYCUSTOMIZE>`): each engine
  action (Advance Text, Hide Message Window, Force Skip, Quick Save, Options In Game, ...)
  lists Windows key codes and mouse codes in priority slots; when one input is bound to
  several story actions, the best slot wins. On FE screens the mouse buttons go to the script
  (PUSH/RELEASE events, `_CLICK_L_`/`_CLICK_R_`, sliders), and Confirm/left click hurries a
  running `wait` unless the script turned `keyskip off` (this is what skips logos).
- Engine services behind the message window's requests: skip, auto, jump to the next choice,
  return to the last choice and quick save/load (resume points: scene, line, stage, BGM and
  local flags; globals from `GAME/flag/global0` are kept), and the in-game options screen
  (`config.fes`, with `$801`/`slmode` = 1). `MES_SETSTATE` reports what is available, so
  Prev and Q.Load are greyed out until there is something to return to. `rettitle` ends the
  story and the flow script continues to its title.
- Novel (NVL) pages: `novel on` + `view 0 novel` switch to the theme's `novel` style, whose text
  object is an NVSTRING: the page's messages stack one under another,
  each speaker right-aligned in the object's name column (`DIV`), text after the layout's left
  margin; `page` clears it. `\pc` messages are centred in the box, and `nvstr endpos` gives the
  window script the text's end for its click-wait mark.
- Extras: the CG gallery reads config.int/cglist.dat (groups of pictures); a picture counts as
  seen when shown or named by `cgreg` / `cgflag`, kept with the global data, and the gallery
  screens ask through `cgreg_t` / `cgregnum` / `cgregmax`. Scene replay is sscript boot mode -2
  (the scene named in the boot string); `end_of_kaisou` ends it while adv.xml's kaisou flag is set.
- Saves: the in-game menu key opens `saveload.fes` (as sscript does), which shows the game's
  menu and captures `_SAVEOBJ_` (the resume point plus a scene thumbnail). Save screens use the
  engine's save commands (`newsave`, `saveexist`, `savedelete`, `saveexchg`, `saveapend_str`,
  `saveget_str`, `datasave`) and SAVEPANEL objects, whose `ptype` parts draw panel frames (NORM),
  the thumbnail (SIMAGE), saved strings (SSTR: message, chapter title) and date digits from a
  digit-font image (NUM_O2). Loading boots sscript with a slot number in the adv.xml boot flag.
  Quick saves use adv.xml `save/quicksave` slots. Global flags (`GAME/flag/global0`) and engine
  settings persist in `global.nsv`. Files are Nyanroid's own format (`saveNNNN.nsv`), stored per
  game: on desktop in `~/.local/share/nyanroid/<game folder>/` (or `--saves <dir>`), on Android
  in the app's private storage. Autosaves go to adv.xml `save/autosave` slots when a choice
  comes up and on the scene command `autosave` (the choice trigger is our reading of the engine,
  not something the data states).
- Message log: the engine keeps a backlog of shown messages (speaker, text, voice) and opens the
  window theme's `<log>` script (images in `$str910..912`). Its LOGSTRING object pages through
  the backlog (`setlog`, `max`, `getpos`, `next`/`rev`/`page`/`pos`, `blockcolor`, `lineinfo`;
  block i at frame `POS2 id + i` of the layout image), the SCROLL object draws the theme's scroll
  bar kit, `_SCRL_U_/_SCRL_D_/_PAGE_U_/_PAGE_D_` come from the key bindings, and the
  `send 0xfffe000N` requests (play voice, speaker name, speaker voice id) are answered at once.
  Whether the message on screen is already listed depends on the engine build, read from the
  game's executable version resource: from 2.6.1.80 (Hatsumira) it is, before that (The Fruit of
  Grisaia, 2.6.1.70) it joins the log once the player moves on.
- Text is decoded as Windows code page 932 (NEC/IBM extensions included). Characters the engine
  draws from an image ("user font", hist.xml `IMAGE/ufont`, default `userfont.hg3`): CP932
  0x8740..0x8748 (①..⑨) are frames 1..9, drawn in a full-width cell.
- Fonts: the face startup.xml's `<FONT><face>` names, matched by the fonts' internal names (all
  languages) among the game's own fonts (folder, `font/`, `font.int`) and the fonts the player
  added (see Fonts above). Otherwise the engine's `default.ttf`, else MS Gothic if added, else the
  platform's Japanese font. Text sits on the engine's cell grid; a stand-in font's wider
  half-width glyphs widen their cell instead of overlapping.
- Images: `.anm` scripts animate multi-frame images (noise, speed lines); `mode sepia|mono|nega`
  filters; `pl N <w> <h>` gives a plane its own clipped surface (its base stays put); plane lists
  `pl [a,b-c] ...`; solids `$AARRGGBB <w> <h> [plane]`. Faces (`fw`) last one message.
  `pl N <mesh>` names a KX2 mesh (`kx2.int`: bounding box, then triangles of x, y, z, colour) the
  plane is drawn through, in its own coordinates (split screens, circular cut-ins; stencilled).
  The `raster_*` meshes are distortion grids for `pl N raster`, not modelled. `blur <x> <y>` is the
  engine's blur of the image as it loads (a tent of that radius, rows then columns). `mode
  mono|sepia|np` rewrites the image's pixels as the engine does (its own grey weights; sepia
  R+30/G+5/B-35; np inverts); changes stack and bare `mode` leaves them. `mcfade P from to [hold]`
  pulses the opacity from -> to -> from every P frames (then `hold` frames at `from`) until
  `stop`. `spline`/`mcspline` follow the engine's cubic spline (Forsythe-Malcolm-Moler) through
  the keys. Plane `rot rx ry rz` / `mrot frames rx ry rz` (which turns by the angles, relative) /
  `mcrot px py pz` act on all three axes: positive z turns clockwise, and x/y tilts are drawn
  flat (a 90 degree y turn is edge-on, 180 mirrors), as the engine's orthographic 2D view does. Plane `m2scale`/`m2rot`/`m2fade`/`arc` animate like the image ones.
- Choices: `fselect` runs the window theme's `<fselect>` script (images in `$str920..`), with the
  choice texts in `$str<fsstr>+i` (adv.xml), their count in `\0`; it exits with the one picked.
  `(<condition>:<text>)` prompts are offered only while the condition holds. Games without one
  get Nyanroid's own buttons. FES strings take `\f<size>` size changes.
- Read text: startup.xml colours are Windows order (0xAABBGGRR): `GAME/read/color` 0xffffe2b0 is
  light blue.
- FES: a block that ends without `next` runs again next frame, empty ones included (`#WAIT`
  right above `#EXIT` waits). A plane hidden only by its `#OBJECT` row shows once the script shows
  something on it. SOUND `play 0` loops, `play 1` plays once; STYPE 0 is BGM.
- Planes are composited in plane order (layers bg, cg, eg, fg inside each); a plane argument given
  explicitly, 0 included, overrides the slot's default plane. Each plane is a surface of its
  vram.xml size: shaking or moving it shows black past its edges.
- Transitions: `wipe` changes dark mask areas first, `rwipe` bright ones; `rwmos <frames> <size>`
  pixelates the old screen up to <size> then resolves the new one (kcTaskRenderWMosaic). Images of
  several parts drawn see-through are merged first, then blended once.
- Plane effects, read from sscript.kcs (bg/cg/eg/fg/fw slots and `pl` planes share one object
  table; planes start at object 27) and the engine:
  - `delay <frames>` (any object): every later motion or property change on it (`pos`, `move`,
    `fade`, `mcmove`, `mrot`, ...) starts that many frames later; `disp`, `mode` and effect
    commands act at once. `stop` and loading a new image/size reset it.
  - `pl N vram <x> <y>`: moves the plane's window in VRAM (vram.xml `plNN/x,y`); planes whose
    windows overlap show the same pixels (one picture drawn once can show through several
    planes). Planes only, like `bmode`, `raster` and `after`.
  - `pl N bmode <n>`: 0 alpha, 1 additive, 2 premultiplied, 3 multiply, 4 (1 - screen) x plane,
    5 screen x plane added (the engine's D3D blend table).
  - `pl N raster <dir> <amp> <cycle> <speed>` on a `raster_*` mesh (Task_Eff_Raster): dir 1 sways x
    by `amp * sin` of y, otherwise y by x, repeating every <cycle> pixels and scrolling one pixel
    per 60 fps frame; `raster` / `raster stop` end it. `raster fade` and negative dirs (the
    normalised form, where <speed> is used) are not modelled; no game uses them.
  - `pl N after <count> <interval> <dx> <dy> <dz> <alpha> <sx%> <sy%> <sz%> <mode> <pri>`:
    afterimages. Copy i is the plane i frames ago moved i*(dx,dy), every <interval>-th drawn
    under the plane, opacity and scale going linearly to <alpha> and <s> at the last copy; mode 1
    doubles the interval, mode 2 also turns each copy by the angle it travelled (its stretch is
    not modelled); `after` alone ends them. Without a plane number (`pl after`, `pl delay`) a
    subcommand goes to every plane.
  - `pl attr_def`: the engine keeps attribute defaults for image slots only; ignored for planes.
  - Draw order is each object's depth (its base z), back to front: bg n 10n, cg n 60+30n,
    fg n 70+30n, eg n 80+30n, fw n 990+10n, plane n 5000+1000n. `pri <depth>` (`@+n` relative)
    changes it; `base` puts it back to the default.
  - `pend`/`m2pend <frames> <half swings> <x> <y> <z>` (motPend): the angle is
    sin(triangle(t) * 90°) * z over a period of 2 * frames / half swings, for <frames> frames;
    `mcpend <frames> <x> <y> <z>` repeats with a period of 2 * frames. Only z (on screen) is drawn.
  - `mcscale <cx> <cy> <cz> <fromX> <fromY> <fromZ> <toX> <toY> <toZ>` (mcScale): each axis goes
    from -> to over its own cycle, then starts again. A plane's missing from/to are its scale.
  - `pl N clone pl M`: plane N becomes a copy of plane M (position, size, motions, VRAM window, so
    M's images show through it), keeping its own depth and effects.
  - `tone <r%> <g%> <b%>` / `tone <name>` (config.int/bgtone.tbl `#name(r,g,b)`) / bare `tone`:
    the slot scales each colour channel, min(255, c * pct / 100), of the images loaded into it
    afterwards; `cg tone` sets every slot of the layer. Planes load no image, so `pl tone` shows
    nothing.
- `.anm` steps naming a frame the image lacks keep the last frame shown. `quake`/`spline` (and their
  looping mc* forms) take keyframe lists `<frames>,<x>,<y>` (offsets for quake, positions with `@`
  for spline, a smooth closed curve). `rand <var> <min> <max>` in scenes and FE scripts.
- Continue / Prev: leaving the story (the app menu's Return to Title, the game's Game End, the app
  closing or going to the background) keeps an autosave, which the title's Continue uses; before any
  choice, Prev returns to the start of the story.
- Layout from vram.xml: each plane's surface base lands at its `pos` (planes larger than the
  screen leave a margin around it); each image slot's `shift` places image
  coordinate 0,0 on its plane, and its `pos` is where an image loaded without coordinates goes.
- Languages (startup.xml `<language>`: `L00`.. with an asset `prefix`, and a `<default>`): the
  engine config holds the chosen ones as indices, `lang_ui` and `lang_text_main`, which the
  game's options screen sets. `%LANG%` (UI) and `%LANG_M%` (story text) in asset names expand to
  the prefix (`$str900_%LANG%` -> `<name>_en`). Each scene's `.cstl` table holds every
  message's speaker and text per language; a scene's messages map to its entries in order (a
  message starts at a name or first text after an input wait, or at a name after a `\@` line;
  empty text starts none; then one entry per distinct prompt of a choice block). A scene already
  written in the chosen language keeps its own lines. The title's `$8000-0001$` id is looked up
  in the game folder's `language_<prefix>.txt`. Desktop: `--lang <prefix>`.
- Engine settings (`getconf` keys) can be set for a desktop run with `--conf <key>=<value>`, e.g.
  `--conf effspd=4` (the 2D-effect wipes at four times speed).
- Read text: messages are marked read per scene line (kept in global data). A read message gets
  startup.xml `GAME/read/color` through the window's `MES_SETNAMECOLOR` when `enable_readcol` is
  on, skip mode stops at unread text unless `fullskip` is on, and `getexvar` reports
  STILL_READ / SKIP / AUTO / SKIP_FORCE / NAME to window scripts.
- Reading settings come from the engine config the options screens write, read through
  startup.xml's timing tables: `messpd` 0-10 × `GAME/strframe_frame` tenths of a frame per
  character; auto mode goes on at the deadline sscript sets as the message goes up,
  `autospd_b` × 6 frames plus `autospd_c` × 0.6 frames per character (a scene's `auto on b c`
  in their place), once the text is out and, with `waitpcm`, the voice has ended; `skip_speed`
  spaces skipped messages; `contskip`/`contauto` decide whether skip/auto survive a choice.
  When the config changes the window script gets `MES_CHANGECONFIG` (opacity, frame).

## License

Nyanroid is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (`LICENSE`). It is distributed in the hope that it
will be useful, but WITHOUT ANY WARRANTY. Source: https://github.com/CosmicRunner123/Nyanroid

The third-party code it includes (FFmpeg, stb, pl_mpeg, Squirrel, zstd, libwebp, FAudio's reverb,
SDL for the desktop player, ...) keeps its own licenses: `THIRD_PARTY_NOTICES.txt`, which the app also shows
under About > Licences.

## Credits

- [FelineSystem2](https://github.com/kokseen1/FelineSystem2) (kokseen1): an open source remake of
  the CatSystem2 engine, used as a reference.
- [TriggersTools.CatSystem2](https://github.com/trigger-segfault/TriggersTools.CatSystem2) and its
  wiki (Robert Jordan / trigger-segfault): CatSystem2 file formats and tools, used as a reference.
- [Ghidra](https://ghidra-sre.org) (NSA): used to study the engines' executables.
