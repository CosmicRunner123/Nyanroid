package dev.nyanroid

import android.content.Context

/**
 * Touch gestures and what they send to the engine. Gestures map to plain mouse/keyboard input
 * (Windows virtual-key codes); the game's own key bindings decide what that input does.
 */
enum class Gesture(val key: String, val label: String, val default: Binding) {
    TWO_FINGER_TAP("tap2", "Two-finger tap", Binding.Mouse(Binding.RIGHT)),
    THREE_FINGER_TAP("tap3", "Three-finger tap", Binding.Key(0x20)),
    SWIPE_UP("swipe_up", "Swipe up", Binding.Wheel(up = true)),
    SWIPE_DOWN("swipe_down", "Swipe down", Binding.Wheel(up = false)),
    HOLD_ONE("hold1", "Hold one finger", Binding.Hover),
    HOLD_TWO("hold2", "Hold two fingers", Binding.Key(0x11)),
    HOLD_THREE("hold3", "Hold three fingers", Binding.None);

    /** Only a hold can hover the mouse: a tap or swipe has no time to. */
    val isHold: Boolean get() = this == HOLD_ONE || this == HOLD_TWO || this == HOLD_THREE

    /** What the settings menu offers for this gesture. */
    val choices: List<Binding> get() = if (isHold) Binding.CHOICES else Binding.CHOICES.filter { it != Binding.Hover }
}

sealed class Binding {
    abstract val id: String
    abstract val label: String

    object None : Binding() {
        override val id = "none"
        override val label = "Nothing"
    }

    /** The pointer follows the finger with no button pressed (menus' hover highlights and captions);
     *  lifting the finger doesn't click. */
    object Hover : Binding() {
        override val id = "hover"
        override val label = "Hover the mouse"
    }

    /** Engine mouse button code: 1 left, 3 right. */
    data class Mouse(val button: Int) : Binding() {
        override val id get() = "mouse:$button"
        override val label get() = if (button == LEFT) "Left click" else "Right click"
    }

    data class Wheel(val up: Boolean) : Binding() {
        override val id get() = if (up) "wheel:up" else "wheel:down"
        override val label get() = if (up) "Mouse wheel up" else "Mouse wheel down"
    }

    /** Windows virtual-key code. */
    data class Key(val vk: Int) : Binding() {
        override val id get() = "key:$vk"
        override val label get() = "Key: " + (KEY_NAMES[vk] ?: "0x%02X".format(vk))
    }

    companion object {
        const val LEFT = 1
        const val RIGHT = 3

        val KEY_NAMES: Map<Int, String> = linkedMapOf(
            0x0D to "Enter", 0x1B to "Esc", 0x20 to "Space", 0x11 to "Ctrl", 0x10 to "Shift",
            0x09 to "Tab", 0x21 to "Page Up", 0x22 to "Page Down",
            0x26 to "Up", 0x28 to "Down", 0x25 to "Left", 0x27 to "Right",
        ) + (1..12).associate { (0x6F + it) to "F$it" }

        /** Everything offered in the settings menu. Built when asked for: a list made with the
         *  class (a stored val) could be made while Binding was still being set up, when a game
         *  touches Binding.Hover first (GameActivity), and then held null in place of Hover. */
        val CHOICES: List<Binding>
            get() = listOf(None, Hover, Mouse(LEFT), Mouse(RIGHT), Wheel(true), Wheel(false)) +
                KEY_NAMES.keys.map { Key(it) }

        fun parse(id: String?): Binding? = when {
            id == null -> null
            id == "none" -> None
            id == "hover" -> Hover
            id == "wheel:up" -> Wheel(true)
            id == "wheel:down" -> Wheel(false)
            id.startsWith("mouse:") -> id.substringAfter(':').toIntOrNull()?.let { Mouse(it) }
            id.startsWith("key:") -> id.substringAfter(':').toIntOrNull()?.let { Key(it) }
            else -> null
        }
    }
}

/** Gesture settings, stored per app (they apply to every game). */
class Controls(context: Context) {
    private val prefs = context.getSharedPreferences("controls", Context.MODE_PRIVATE)

    operator fun get(g: Gesture): Binding {
        val b = Binding.parse(prefs.getString(g.key, null)) ?: return g.default
        return if (b == Binding.Hover && !g.isHold) g.default else b  // hover is for holds only
    }

    operator fun set(g: Gesture, b: Binding) {
        prefs.edit().putString(g.key, b.id).apply()
    }

    /** The virtual cursor (the app menu's switch): one finger moves a cursor like a trackpad, a tap
     *  clicks where it points, a two-finger tap right-clicks, a two-finger drag turns the wheel; the
     *  other gestures keep their settings. */
    var virtualCursor: Boolean
        get() = prefs.getBoolean("virtual_cursor", false)
        set(on) = prefs.edit().putBoolean("virtual_cursor", on).apply()

    /** How far the virtual cursor moves for the finger's travel: regular 1:1, fast twice as far. */
    var fastCursor: Boolean
        get() = prefs.getBoolean("fast_cursor", false)
        set(on) = prefs.edit().putBoolean("fast_cursor", on).apply()

    val cursorSpeed: Float get() = if (fastCursor) 2f else 1f

    /** The gestures back to their defaults (the cursor settings stay). */
    fun reset() {
        prefs.edit().apply { Gesture.values().forEach { remove(it.key) } }.apply()
    }
}


