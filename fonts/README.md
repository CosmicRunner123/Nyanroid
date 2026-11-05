# Fonts

Games draw their text in fonts of their own or in Windows fonts their configuration names (MS Gothic,
Meiryo, ...). Nyanroid looks a face up by name among:

1. the game's own fonts (in its archives, its `font` folder or its own folder);
2. the fonts the player added: on Android with the library's **Fonts** button (they are copied to
   `Android/data/dev.nyanroid/files/fonts`), on the desktop in `~/.local/share/nyanroid/fonts`;
3. the fonts in this folder, in your own builds (below).

A face found in none of them is drawn in MS Gothic when one of those places has it (Windows would
also substitute a Japanese face), else in the system's Japanese font: on Android its Noto Sans CJK
(a variable font on recent versions, drawn at its regular weight), on the desktop Noto Sans CJK or
the file given with `--fallback-font`. Before starting a game whose font is missing, the Android
library says so and offers to add fonts.

## This folder

Font files copied here (`.ttf`, `.ttc`, `.otf`) are found by the desktop player and go into
**debug** APKs. They are not in git, and **release** APKs leave them out: most fonts' licences,
Windows' included, do not allow passing them on, so publish only release builds.

`nyan-desktop --no-bundled-fonts` runs as a player without them.
