package dev.nyanroid

/** JNI entry points into the C++ engine (libnyanroid.so). All calls except the touch/skip
 *  setters and [detectEngine] must happen on the GL thread. */
object NativeBridge {
    init {
        System.loadLibrary("nyanroid")
    }

    /** Loads a game and creates its renderer (needs a current GL context). Returns 0 on failure. */
    external fun create(folder: SafFolder, fallbackFontPath: String, saveDir: String, userFontDir: String): Long
    /** Last error message from [create]. */
    external fun lastError(): String
    /** The engine of the game in [folder] ("CatSystem2"), or null if Nyanroid knows none. Any thread. */
    external fun detectEngine(folder: SafFolder): String?
    /** The fonts the game in [folder] names that neither it nor [fontDirs] (newline-separated) has,
     *  so Android's font would stand in; empty if none. Any thread (not the UI's: it reads files). */
    external fun missingFonts(folder: SafFolder, fontDirs: String): Array<String>

    /** Advances the simulation to real time and draws a frame. */
    external fun frame(handle: Long, width: Int, height: Int)
    external fun destroy(handle: Long)
    /** True once the game asked to quit (Quit Game on its title screen). */
    external fun quitRequested(handle: Long): Boolean

    /** Raw input, interpreted through the game's own key bindings. Coordinates in surface pixels. */
    external fun pointer(handle: Long, x: Float, y: Float)
    /** button: engine mouse code, 1 left / 3 right. */
    external fun mouseButton(handle: Long, button: Int, down: Boolean, x: Float, y: Float)
    /** Forget a pressed button without clicking (the touch became a gesture). */
    external fun cancelPress(handle: Long)
    /** steps > 0: wheel up. */
    external fun wheel(handle: Long, steps: Int)
    /** vk: Windows virtual-key code. */
    external fun key(handle: Long, vk: Int, down: Boolean)
    external fun setSkip(handle: Long, skip: Boolean)
    external fun setAuto(handle: Long, auto: Boolean)
    external fun pauseAudio(handle: Long, paused: Boolean)
    /** Writes global data (settings, global flags) now; saves themselves are written as they are made. */
    external fun flush(handle: Long)

    // The app menu (Back), standing in for the original's window menu bar.
    /** True while a story scene runs (Return to Title and Settings apply). */
    external fun inStory(handle: Long): Boolean
    external fun returnToTitle(handle: Long): Boolean
    /** Forgets held keys/buttons (Force Skip, a pressed button) when the app menu takes over input. */
    external fun releaseHeld(handle: Long)
    /** Opens the game's own options screen over the story. */
    external fun openSettings(handle: Long): Boolean
    /** The game's languages (startup.xml), empty for single-language games. */
    external fun languages(handle: Long): Array<String>
    external fun language(handle: Long): Int
    external fun setLanguage(handle: Long, index: Int)

    /** A dialog the game waits on (a window of its own on PC: name entry, a question):
     *  [serial, input "1"/"0", title, message, value, max Shift-JIS bytes, type], or null. */
    external fun dialog(handle: Long): Array<String>?
    external fun answerDialog(handle: Long, ok: Boolean, text: String)

    /** The game's virtual screen: [width, height]. */
    external fun screenSize(handle: Long): IntArray
    /** The game's own mouse cursor: [serial, width, height, hotX, hotY, ARGB pixels...], [0] for the
     *  system's arrow, or null while it is still the one with serial [seen]. */
    external fun cursor(handle: Long, seen: Int): IntArray?
}
