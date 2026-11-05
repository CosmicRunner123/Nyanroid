package dev.nyanroid

import android.app.Activity
import android.app.AlertDialog
import android.net.Uri
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.text.InputType
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.window.OnBackInvokedDispatcher
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Runs one game full screen. The engine lives on the GLSurfaceView's render thread;
 * input is forwarded there with queueEvent.
 *
 * Input goes to the engine as mouse/keyboard events; the game's own key bindings
 * (startup.xml KEYCUSTOMIZE) decide what they do. Touch gestures are configurable (Controls.kt).
 */
class GameActivity : Activity() {
    private lateinit var view: GLSurfaceView
    private lateinit var cursorView: CursorView
    @Volatile private var handle = 0L
    private var failed = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val uri = Uri.parse(intent.getStringExtra(EXTRA_FOLDER) ?: run { finish(); return })
        val folder = SafFolder(contentResolver, uri)
        val saveDir = saveDirFor(uri).absolutePath
        // The fonts games find by name (built into this build, and those the player added), and
        // Android's own Japanese font for a face neither has.
        val fontDirs = Fonts.dirs(this)
        val fallbackFont = Fonts.systemFallback()

        // The game view also takes Android's on-screen keyboard while it is asked for (app menu:
        // Keyboard): TYPE_NULL makes the keyboard send key events, and text it commits anyway is
        // turned into key events by BaseInputConnection's fallback mode; both reach onKeyDown as a
        // hardware keyboard's keys do.
        view = object : GLSurfaceView(this) {
            override fun onCheckIsTextEditor() = keyboardWanted

            override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
                if (!keyboardWanted) return null
                outAttrs.inputType = InputType.TYPE_NULL
                outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_FLAG_NO_EXTRACT_UI
                return BaseInputConnection(this, false)
            }
        }.apply {
            isFocusable = true
            isFocusableInTouchMode = true
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                    if (handle != 0L || failed) return
                    handle = NativeBridge.create(folder, fallbackFont, saveDir, fontDirs)
                    if (handle == 0L) {
                        failed = true
                        val msg = NativeBridge.lastError()
                        runOnUiThread { showError(msg) }
                    } else {
                        val size = NativeBridge.screenSize(handle)
                        runOnUiThread {
                            cursorView.screenWidth = size[0]
                            cursorView.screenHeight = size[1]
                            cursorView.placePicture()
                            applyCursorMode(recentre = true)
                        }
                    }
                }

                override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {}

                override fun onDrawFrame(gl: GL10?) {
                    // Read the handle once: the UI thread zeroes it when finishing (destroy is
                    // queued to this thread, so the instance stays valid for this frame).
                    val h = handle
                    if (h != 0L) {
                        NativeBridge.frame(h, this@apply.width, this@apply.height)
                        if (virtualCursor) {
                            // The game's cursor picture, when it changes (a game's `xchgcursor`).
                            val c = NativeBridge.cursor(h, cursorSeen)
                            if (c != null) {
                                cursorSeen = c[0]
                                runOnUiThread { cursorView.setImage(c) }
                            }
                        }
                        if (NativeBridge.quitRequested(h)) runOnUiThread { if (!isFinishing) finish() }
                        val d = NativeBridge.dialog(h)
                        if (d != null && d[0] != shownDialog) {
                            shownDialog = d[0]
                            runOnUiThread { showGameDialog(d) }
                        }
                    }
                }
            })
        }
        cursorView = CursorView(this)
        setContentView(android.widget.FrameLayout(this).apply {
            addView(view)
            addView(cursorView, android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT, android.widget.FrameLayout.LayoutParams.MATCH_PARENT))
            // While the on-screen keyboard shows, the game (and the cursor layer) is drawn in the
            // space above it, letterboxed as usual, instead of under it.
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                setOnApplyWindowInsetsListener { v, insets ->
                    v.setPadding(0, 0, 0, insets.getInsets(WindowInsets.Type.ime()).bottom)
                    val visible = insets.isVisible(WindowInsets.Type.ime())
                    // Closed from the keyboard itself (its own key, Back): no longer asked for. The
                    // app menu's dialog takes the focus and hides it too; that one comes back.
                    if (imeVisible && !visible && !menuOpen) keyboardWanted = false
                    imeVisible = visible
                    insets
                }
            }
        })
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        virtualCursor = controls.virtualCursor
        cursorSpeed = controls.cursorSpeed
        hideSystemBars()
        // Back opens the app menu (the original's window menu bar: title, settings, language) with
        // Press ESC and Exit Game.
        // Android 13+ (API 33) delivers it through the back callback; Android 12 through
        // onBackPressed below.
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) {
                showMenu()
            }
        }
    }

    @Deprecated("Deprecated in Java")
    @Suppress("DEPRECATION")
    override fun onBackPressed() {
        // Android 12 (API 31-32): no back callback; the same menu.
        if (android.os.Build.VERSION.SDK_INT >= 33) super.onBackPressed() else showMenu()
    }

    private var menuOpen = false

    private fun showMenu() {
        val h = handle
        if (h == 0L || menuOpen) {
            if (h == 0L) finish()
            return
        }
        menuOpen = true
        // Whatever the finger was holding (a hold gesture's Ctrl = Force Skip, a pressed button)
        // never gets its release once the menu takes the touch: let go of it now.
        view.removeCallbacks(holdCheck)
        holding?.let { release(it) }
        holding = null
        leftPressed = false
        view.queueEvent {
            NativeBridge.releaseHeld(h)
            val inStory = NativeBridge.inStory(h)
            val langs = NativeBridge.languages(h)
            val lang = NativeBridge.language(h)
            NativeBridge.pauseAudio(h, true)
            runOnUiThread { showMenu(inStory, langs, lang) }
        }
    }

    private fun showMenu(inStory: Boolean, langs: Array<String>, lang: Int) {
        if (isFinishing) return
        view.onPause()
        val items = mutableListOf<Pair<String, () -> Unit>>()
        if (inStory) {
            items += "Return to Title" to { native { NativeBridge.returnToTitle(it) } }
            items += "Settings" to { native { NativeBridge.openSettings(it) } }
        }
        if (langs.size > 1) {
            val current = langs.getOrNull(lang) ?: ""
            items += "Language: $current" to { showLanguages(langs, lang) }
        }
        // The virtual cursor: a trackpad-style pointer for games played with the mouse.
        items += "Virtual cursor: " + (if (virtualCursor) "On" else "Off") to {
            virtualCursor = !virtualCursor
            controls.virtualCursor = virtualCursor
            applyCursorMode(recentre = false)
        }
        // Android's on-screen keyboard, for games typed on (Mojika's typing game) without a
        // hardware keyboard.
        items += "Keyboard: " + (if (keyboardWanted) "On" else "Off") to { keyboardWanted = !keyboardWanted }
        items += "Press ESC" to {
            native {
                NativeBridge.key(it, 0x1B, true)
                NativeBridge.key(it, 0x1B, false)
            }
        }
        // As if the app were closed: what onPause/onDestroy do (global data is written), then the list.
        items += "Exit Game" to { finish() }
        var picked: (() -> Unit)? = null
        AlertDialog.Builder(this)
            .setItems(items.map { it.first }.toTypedArray()) { _, i -> picked = items[i].second }
            .setOnDismissListener {
                menuOpen = false
                val action = picked
                if (action == null || !isFinishing) resumeGame()
                action?.invoke()
            }
            .show()
    }

    private fun showLanguages(langs: Array<String>, lang: Int) {
        menuOpen = true
        view.onPause()
        native { NativeBridge.pauseAudio(it, true) }
        var chosen = -1
        AlertDialog.Builder(this)
            .setTitle("Language")
            .setSingleChoiceItems(langs, lang) { d, i ->
                chosen = i
                d.dismiss()
            }
            .setOnDismissListener {
                menuOpen = false
                resumeGame()
                if (chosen >= 0 && chosen != lang) native { NativeBridge.setLanguage(it, chosen) }
            }
            .show()
    }

    private fun resumeGame() {
        view.onResume()
        hideSystemBars()
        native { NativeBridge.pauseAudio(it, false) }
        applyKeyboard()
    }

    // The on-screen keyboard (app menu: Keyboard): asked for, and whether it shows now.
    private var keyboardWanted = false
    private var imeVisible = false

    /** Shows or hides the on-screen keyboard as [keyboardWanted] says, once the menu has gone. */
    private fun applyKeyboard() {
        view.post {
            val imm = getSystemService(InputMethodManager::class.java) ?: return@post
            if (keyboardWanted) {
                view.requestFocus()
                imm.restartInput(view)
                imm.showSoftInput(view, 0)
            } else {
                imm.hideSoftInputFromWindow(view.windowToken, 0)
                imm.restartInput(view)
            }
        }
    }

    private fun hideSystemBars() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            // Android 10 and older: sticky immersive mode (bars come back on a swipe, then hide again).
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        }
    }

    private fun showError(message: String) {
        AlertDialog.Builder(this)
            .setTitle("Could not start game")
            .setMessage(message)
            .setPositiveButton("OK") { _, _ -> finish() }
            .setCancelable(false)
            .show()
    }

    // ---- Input -------------------------------------------------------------------------------
    // One finger is the mouse: it moves the pointer and presses/releases the left button, so
    // taps click and drags work (sliders). Other gestures send what the user configured in
    // Touch controls (Controls.kt); a gesture cancels the left press it started with.

    private val controls by lazy { Controls(this) }
    private val touchSlop by lazy { ViewConfiguration.get(this).scaledTouchSlop }
    private var startTime = 0L
    private var startX = 0f
    private var startY = 0f
    private var lastX = 0f
    private var lastY = 0f
    private var maxPointers = 0
    private var moved = false
    private var leftPressed = false
    private var holding: Binding? = null
    // A one-finger vertical swipe repeats its binding every SWIPE_STEP_DP travelled (like wheel notches).
    private var swiping = false
    private var swipeAnchorY = 0f
    // A hover (a one-finger hold) the finger then keeps still, within the touch slop, until
    // HOVER_LOCK_MS after it touched down: it lasts until the finger lifts and swipes are off.
    // Before that a vertical swipe still takes over.
    private var hoverLocked = false
    private val hoverLock = Runnable { if (holding == Binding.Hover && !moved) hoverLocked = true }

    @Volatile private var shownDialog = ""

    /** The game's own dialogs: a text box (`input_text`, e.g. the player's name) or a question
     *  (`messagebox`). The script waits until it is answered. */
    private fun showGameDialog(d: Array<String>) {
        val input = d[1] == "1"
        var answered = false
        fun answer(ok: Boolean, text: String) {
            if (answered) return
            answered = true
            native { NativeBridge.answerDialog(it, ok, text) }
            hideSystemBars()
        }
        val b = AlertDialog.Builder(this).setTitle(d[2]).setCancelable(false)
        if (input) {
            val field = android.widget.EditText(this)
            field.setText(d[4])
            field.setSelection(field.text.length)
            field.isSingleLine = true
            // The engine's edit box limit (EM_LIMITTEXT): characters, as Android counts them.
            val max = d[5].toIntOrNull() ?: 0
            if (max > 0) field.filters = arrayOf<android.text.InputFilter>(android.text.InputFilter.LengthFilter(max))
            val pad = (16 * resources.displayMetrics.density).toInt()
            val box = android.widget.FrameLayout(this)
            box.setPadding(pad, pad / 2, pad, 0)
            box.addView(field)
            b.setView(box)
                .setPositiveButton(android.R.string.ok) { _, _ -> answer(true, field.text.toString()) }
            // Cancel only where the game allows it (input_text's fifth number).
            if (d.getOrNull(7) != "0") b.setNegativeButton(android.R.string.cancel) { _, _ -> answer(false, "") }
        } else {
            b.setMessage(d[3])
            if (d[6] == "2") {
                b.setPositiveButton(android.R.string.yes) { _, _ -> answer(true, "") }
                    .setNegativeButton(android.R.string.no) { _, _ -> answer(false, "") }
            } else if (d[6] == "1") {
                b.setPositiveButton(android.R.string.ok) { _, _ -> answer(true, "") }
                    .setNegativeButton(android.R.string.cancel) { _, _ -> answer(false, "") }
            } else {
                b.setPositiveButton(android.R.string.ok) { _, _ -> answer(true, "") }
            }
        }
        b.setOnDismissListener { answer(false, "") }
        b.show()
    }

    private fun native(block: (Long) -> Unit) {
        val h = handle
        if (h != 0L) view.queueEvent { block(h) }
    }

    private fun press(b: Binding) {
        val x = lastX
        val y = lastY
        when (b) {
            is Binding.Mouse -> native { NativeBridge.mouseButton(it, b.button, true, x, y) }
            is Binding.Key -> native { NativeBridge.key(it, b.vk, true) }
            is Binding.Wheel -> native { NativeBridge.wheel(it, if (b.up) 1 else -1) }
            Binding.Hover, Binding.None -> {}  // hover: the press was dropped; moves follow the finger
        }
    }

    private fun release(b: Binding) {
        val x = lastX
        val y = lastY
        when (b) {
            is Binding.Mouse -> native { NativeBridge.mouseButton(it, b.button, false, x, y) }
            is Binding.Key -> native { NativeBridge.key(it, b.vk, false) }
            else -> {}
        }
    }

    private fun pulse(b: Binding) {
        press(b)
        release(b)
    }

    private fun cancelLeft() {
        if (leftPressed) {
            leftPressed = false
            native { NativeBridge.cancelPress(it) }
        }
    }

    private val holdCheck = Runnable {
        if (moved || holding != null) return@Runnable
        val g = when (maxPointers) {
            1 -> Gesture.HOLD_ONE
            2 -> Gesture.HOLD_TWO
            else -> Gesture.HOLD_THREE
        }
        val b = controls[g]
        if (b == Binding.None || (virtualCursor && b == Binding.Hover)) return@Runnable
        cancelLeft()
        holding = b
        press(b)
        if (b == Binding.Hover) view.postDelayed(hoverLock, HOVER_LOCK_MS - HOLD_MS)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (handle == 0L) return true
        if (event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) return mouseEvent(event)
        if (virtualCursor) return trackpadEvent(event)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                startTime = event.eventTime
                startX = event.x; startY = event.y
                lastX = startX; lastY = startY
                maxPointers = 1
                moved = false
                swiping = false
                holding = null
                hoverLocked = false
                view.removeCallbacks(hoverLock)
                leftPressed = true
                val x = lastX
                val y = lastY
                native {
                    NativeBridge.pointer(it, x, y)
                    NativeBridge.mouseButton(it, Binding.LEFT, true, x, y)
                }
                view.postDelayed(holdCheck, HOLD_MS)
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                maxPointers = maxOf(maxPointers, event.pointerCount)
                cancelLeft()
                if (holding == null) {
                    view.removeCallbacks(holdCheck)
                    view.postDelayed(holdCheck, HOLD_MS)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (!moved && (Math.abs(event.x - startX) > touchSlop || Math.abs(event.y - startY) > touchSlop)) {
                    moved = true
                    if (holding == null) view.removeCallbacks(holdCheck)
                }
                if (maxPointers == 1 && holding == Binding.Hover && !hoverLocked) {
                    // A hover not yet locked: a vertical swipe takes over (the hover pressed nothing).
                    val dx = event.x - startX
                    val dy = event.y - startY
                    if (Math.abs(dy) > swipeDistance() && Math.abs(dy) > 2 * Math.abs(dx)) {
                        view.removeCallbacks(hoverLock)
                        holding = null
                    }
                }
                if (maxPointers == 1 && holding == Binding.Hover) {
                    // Hovering: the pointer follows the finger, nothing pressed.
                    lastX = event.x; lastY = event.y
                    val x = lastX
                    val y = lastY
                    native { NativeBridge.pointer(it, x, y) }
                } else if (maxPointers == 1 && holding == null) {
                    val dx = event.x - startX
                    val dy = event.y - startY
                    if (!swiping && Math.abs(dy) > swipeDistance() && Math.abs(dy) > 2 * Math.abs(dx)) {
                        // It's a swipe, not a drag: drop the press and start counting notches.
                        swiping = true
                        cancelLeft()
                        swipeAnchorY = startY
                    }
                    if (swiping) {
                        val step = SWIPE_STEP_DP * resources.displayMetrics.density
                        while (Math.abs(event.y - swipeAnchorY) >= step) {
                            val up = event.y < swipeAnchorY
                            pulse(controls[if (up) Gesture.SWIPE_UP else Gesture.SWIPE_DOWN])
                            swipeAnchorY += if (up) -step else step
                        }
                    } else {
                        lastX = event.x; lastY = event.y
                        val x = lastX
                        val y = lastY
                        native { NativeBridge.pointer(it, x, y) }
                    }
                }
            }
            MotionEvent.ACTION_UP -> {
                view.removeCallbacks(holdCheck)
                view.removeCallbacks(hoverLock)
                val held = holding
                holding = null
                val dx = event.x - startX
                val dy = event.y - startY
                val quick = event.eventTime - startTime < TAP_MS
                val swipe = maxPointers == 1 && Math.abs(dy) > swipeDistance() && Math.abs(dy) > 2 * Math.abs(dx)
                when {
                    // A flick that ended a hover not yet locked before a move event crossed the threshold.
                    held == Binding.Hover && !hoverLocked && swipe ->
                        pulse(controls[if (dy < 0) Gesture.SWIPE_UP else Gesture.SWIPE_DOWN])
                    held != null -> release(held)
                    swiping -> {}  // its notches were sent while moving
                    maxPointers == 1 && Math.abs(dy) > swipeDistance() && Math.abs(dy) > 2 * Math.abs(dx) -> {
                        // A fast flick that ended before a move event crossed the threshold.
                        cancelLeft()
                        pulse(controls[if (dy < 0) Gesture.SWIPE_UP else Gesture.SWIPE_DOWN])
                    }
                    maxPointers == 1 -> if (leftPressed) {
                        leftPressed = false
                        lastX = event.x; lastY = event.y
                        val x = lastX
                        val y = lastY
                        native { NativeBridge.mouseButton(it, Binding.LEFT, false, x, y) }
                    }
                    quick && !moved -> pulse(controls[if (maxPointers == 2) Gesture.TWO_FINGER_TAP else Gesture.THREE_FINGER_TAP])
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                view.removeCallbacks(holdCheck)
                view.removeCallbacks(hoverLock)
                holding?.let { release(it) }
                holding = null
                cancelLeft()
            }
        }
        return true
    }

    private fun swipeDistance() = 80 * resources.displayMetrics.density

    // ---- Virtual cursor ------------------------------------------------------------------------
    // A trackpad: one finger moves the cursor by how far it moves (not to where it is), a tap
    // clicks where the cursor points, a two-finger tap right-clicks there, two fingers dragged up
    // or down turn the mouse wheel. Those replace the tap, two-finger tap and drag (swipe)
    // gestures; three-finger taps and the holds keep their settings (a one-finger hold bound to a
    // mouse button drags with it). The speed (regular or fast) is set in the library.

    @Volatile private var virtualCursor = false
    @Volatile private var cursorSeen = -1  // the cursor picture's serial (GL thread)
    private var cursorPlaced = false
    private var padPointer = -1
    private var padX = 0f
    private var padY = 0f
    private var wheelAnchorY = Float.NaN  // two fingers' mid-point where the last wheel notch was
    private var cursorSpeed = 1f

    private fun twoFingerY(event: MotionEvent) = (event.getY(0) + event.getY(1)) / 2

    private fun applyCursorMode(recentre: Boolean) {
        if (!virtualCursor) {
            cursorView.shown = false
            return
        }
        cursorSeen = -1
        if (!cursorPlaced || recentre) {
            cursorPlaced = true
            cursorView.post {
                cursorView.placePicture()
                moveCursorTo((cursorView.x0 + cursorView.x1()) / 2, (cursorView.y0 + cursorView.y1()) / 2)
            }
        } else {
            moveCursorTo(cursorView.cx, cursorView.cy)
        }
        cursorView.shown = true
    }

    /** Puts the cursor at (x, y), kept over the game's picture, and points there. */
    private fun moveCursorTo(x: Float, y: Float) {
        val cx = x.coerceIn(cursorView.x0, maxOf(cursorView.x0, cursorView.x1() - 1))
        val cy = y.coerceIn(cursorView.y0, maxOf(cursorView.y0, cursorView.y1() - 1))
        cursorView.moveTo(cx, cy)
        lastX = cx; lastY = cy
        native { NativeBridge.pointer(it, cx, cy) }
    }

    private fun clickAtCursor(button: Int) {
        val x = cursorView.cx
        val y = cursorView.cy
        native {
            NativeBridge.pointer(it, x, y)
            NativeBridge.mouseButton(it, button, true, x, y)
            NativeBridge.mouseButton(it, button, false, x, y)
        }
    }

    private fun trackpadEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                startTime = event.eventTime
                startX = event.x; startY = event.y
                padPointer = event.getPointerId(0)
                padX = event.x; padY = event.y
                maxPointers = 1
                moved = false
                holding = null
                leftPressed = false
                view.postDelayed(holdCheck, HOLD_MS)
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                maxPointers = maxOf(maxPointers, event.pointerCount)
                wheelAnchorY = if (event.pointerCount == 2) twoFingerY(event) else Float.NaN
                if (holding == null) {
                    view.removeCallbacks(holdCheck)
                    view.postDelayed(holdCheck, HOLD_MS)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (event.pointerCount == 2 && holding == null && !wheelAnchorY.isNaN()) {
                    // Two fingers dragged up or down: a wheel notch every SWIPE_STEP_DP (as the
                    // one-finger swipes give; fingers up = wheel up).
                    val y = twoFingerY(event)
                    val step = SWIPE_STEP_DP * resources.displayMetrics.density
                    while (Math.abs(y - wheelAnchorY) >= step) {
                        val up = y < wheelAnchorY
                        native { NativeBridge.wheel(it, if (up) 1 else -1) }
                        wheelAnchorY += if (up) -step else step
                    }
                }
                val i = event.findPointerIndex(padPointer)
                if (i < 0) return true
                val x = event.getX(i)
                val y = event.getY(i)
                if (!moved && (Math.abs(x - startX) > touchSlop || Math.abs(y - startY) > touchSlop)) {
                    moved = true
                    if (holding == null) view.removeCallbacks(holdCheck)
                }
                // Only a lone finger moves the cursor (two or three are a tap or a hold).
                if (event.pointerCount == 1) {
                    moveCursorTo(cursorView.cx + (x - padX) * cursorSpeed, cursorView.cy + (y - padY) * cursorSpeed)
                }
                padX = x; padY = y
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // Back to two fingers (from three): the wheel counts from where they are now.
                wheelAnchorY = Float.NaN
                if (event.pointerCount == 3) {
                    val rest = (0 until 3).filter { it != event.actionIndex }
                    wheelAnchorY = (event.getY(rest[0]) + event.getY(rest[1])) / 2
                }
                // The finger that steered lifted: go on with one still down, from where it is.
                if (event.getPointerId(event.actionIndex) == padPointer) {
                    val next = if (event.actionIndex == 0) 1 else 0
                    padPointer = event.getPointerId(next)
                    padX = event.getX(next); padY = event.getY(next)
                }
            }
            MotionEvent.ACTION_UP -> {
                view.removeCallbacks(holdCheck)
                val held = holding
                holding = null
                val quick = event.eventTime - startTime < TAP_MS
                when {
                    held != null -> release(held)
                    !quick || moved -> {}
                    maxPointers == 1 -> clickAtCursor(Binding.LEFT)
                    maxPointers == 2 -> clickAtCursor(Binding.RIGHT)
                    else -> pulse(controls[Gesture.THREE_FINGER_TAP])
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                view.removeCallbacks(holdCheck)
                holding?.let { release(it) }
                holding = null
            }
        }
        return true
    }

    /** A real mouse: buttons and pointer as they are. */
    private fun mouseEvent(event: MotionEvent): Boolean {
        lastX = event.x; lastY = event.y
        if (virtualCursor) cursorView.moveTo(event.x, event.y)
        val x = lastX
        val y = lastY
        when (event.actionMasked) {
            MotionEvent.ACTION_BUTTON_PRESS, MotionEvent.ACTION_BUTTON_RELEASE -> {
                val button = when (event.actionButton) {
                    MotionEvent.BUTTON_PRIMARY -> Binding.LEFT
                    MotionEvent.BUTTON_SECONDARY -> Binding.RIGHT
                    else -> return true
                }
                val down = event.actionMasked == MotionEvent.ACTION_BUTTON_PRESS
                native { NativeBridge.mouseButton(it, button, down, x, y) }
            }
            MotionEvent.ACTION_MOVE -> native { NativeBridge.pointer(it, x, y) }
        }
        return true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (handle == 0L) return super.onGenericMotionEvent(event)
        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_MOVE -> {
                lastX = event.x; lastY = event.y
                if (virtualCursor) cursorView.moveTo(event.x, event.y)
                val x = lastX
                val y = lastY
                native { NativeBridge.pointer(it, x, y) }
                return true
            }
            MotionEvent.ACTION_SCROLL -> {
                val v = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
                if (v != 0f) native { NativeBridge.wheel(it, if (v > 0) 1 else -1) }
                return true
            }
        }
        return super.onGenericMotionEvent(event)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val vk = virtualKey(keyCode) ?: return super.onKeyDown(keyCode, event)
        if (event.repeatCount == 0 || vk == 0x0D) native { NativeBridge.key(it, vk, true) }
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        val vk = virtualKey(keyCode) ?: return super.onKeyUp(keyCode, event)
        native { NativeBridge.key(it, vk, false) }
        return true
    }

    /** Android key code -> Windows virtual-key code (what the game's bindings use). */
    private fun virtualKey(code: Int): Int? = when (code) {
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 'A'.code + (code - KeyEvent.KEYCODE_A)
        in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 -> '0'.code + (code - KeyEvent.KEYCODE_0)
        in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> 0x70 + (code - KeyEvent.KEYCODE_F1)
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> 0x0D
        KeyEvent.KEYCODE_DEL -> 0x08  // Backspace
        KeyEvent.KEYCODE_ESCAPE -> 0x1B
        KeyEvent.KEYCODE_SPACE -> 0x20
        KeyEvent.KEYCODE_TAB -> 0x09
        KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_CTRL_RIGHT -> 0x11
        KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_SHIFT_RIGHT -> 0x10
        KeyEvent.KEYCODE_PAGE_UP -> 0x21
        KeyEvent.KEYCODE_PAGE_DOWN -> 0x22
        KeyEvent.KEYCODE_DPAD_LEFT -> 0x25
        KeyEvent.KEYCODE_DPAD_UP -> 0x26
        KeyEvent.KEYCODE_DPAD_RIGHT -> 0x27
        KeyEvent.KEYCODE_DPAD_DOWN -> 0x28
        else -> null
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
        // The keyboard asked for while the menu had the focus shows once the game has it again.
        if (hasFocus && keyboardWanted && !imeVisible) applyKeyboard()
    }

    override fun onPause() {
        super.onPause()
        val h = handle
        if (h != 0L) {
            if (isFinishing) {
                // Tear down while the GL thread is still running so GL objects are freed in-context.
                handle = 0L
                view.queueEvent {
                    NativeBridge.flush(h)  // autosave + global data, as when the app goes to the background
                    NativeBridge.destroy(h)
                }
            } else {
                view.queueEvent {
                    NativeBridge.pauseAudio(h, true)
                    NativeBridge.flush(h)
                }
            }
        }
        view.onPause()
    }

    override fun onResume() {
        super.onResume()
        view.onResume()
        val h = handle
        if (h != 0L) view.queueEvent { NativeBridge.pauseAudio(h, false) }
    }


    /** Each game's saves live in app storage, keyed by its folder (the game folder is read-only). */
    private fun saveDirFor(uri: Uri): java.io.File = Saves.dirFor(this, uri)

    companion object {
        const val EXTRA_FOLDER = "folder"
        private const val HOLD_MS = 500L
        private const val HOVER_LOCK_MS = 1000L  // from the touch: a hover kept still this long is locked
        private const val TAP_MS = 400L
        private const val SWIPE_STEP_DP = 48f
    }
}
