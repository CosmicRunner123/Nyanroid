package dev.nyanroid

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast

/** Lists the game folders the user has added and launches them. */
private const val TITLE_VERSION = 2

/**
 * Lays its children out left to right at their own size, `gapX` apart, and starts a new line,
 * `gapY` below, when the next one does not fit. A child wider than the row gets the row's width.
 */
private class FlowRow(context: Context, private val gapX: Int, private val gapY: Int) : ViewGroup(context) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val mode = MeasureSpec.getMode(widthMeasureSpec)
        val avail = if (mode == MeasureSpec.UNSPECIFIED) Int.MAX_VALUE / 2
        else MeasureSpec.getSize(widthMeasureSpec) - paddingLeft - paddingRight
        val childWidth = MeasureSpec.makeMeasureSpec(maxOf(0, avail), MeasureSpec.AT_MOST)
        val childHeight = MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED)
        var x = 0
        var y = 0
        var line = 0
        var widest = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == View.GONE) continue
            c.measure(childWidth, childHeight)
            if (x > 0 && x + c.measuredWidth > avail) {
                x = 0
                y += line + gapY
                line = 0
            }
            x += c.measuredWidth
            widest = maxOf(widest, x)
            x += gapX
            line = maxOf(line, c.measuredHeight)
        }
        setMeasuredDimension(
            resolveSize(widest + paddingLeft + paddingRight, widthMeasureSpec),
            resolveSize(y + line + paddingTop + paddingBottom, heightMeasureSpec))
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val avail = r - l - paddingLeft - paddingRight
        var x = 0
        var y = 0
        var line = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == View.GONE) continue
            if (x > 0 && x + c.measuredWidth > avail) {
                x = 0
                y += line + gapY
                line = 0
            }
            c.layout(paddingLeft + x, paddingTop + y, paddingLeft + x + c.measuredWidth, paddingTop + y + c.measuredHeight)
            x += c.measuredWidth + gapX
            line = maxOf(line, c.measuredHeight)
        }
    }
}

class LibraryActivity : Activity() {
    private lateinit var list: LinearLayout

    private val prefs by lazy { getSharedPreferences("library", Context.MODE_PRIVATE) }
    private var checkingFonts = false  // a game's font check is running (a second tap waits for it)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        NativeBridge.setDataDirectory(filesDir.absolutePath)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(24), dp(24), dp(24))
            setBackgroundColor(Color.rgb(18, 24, 32))
        }
        // Title row: the app's name, and an (i) at the right for the projects Nyanroid builds on.
        val titleRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        titleRow.addView(TextView(this).apply {
            text = getString(R.string.app_name)
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f)
            typeface = Typeface.DEFAULT_BOLD
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        titleRow.addView(TextView(this).apply {
            text = "i"
            contentDescription = "About and credits"
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            typeface = Typeface.create(Typeface.SERIF, Typeface.BOLD_ITALIC)
            gravity = android.view.Gravity.CENTER
            background = android.graphics.drawable.GradientDrawable().apply {
                shape = android.graphics.drawable.GradientDrawable.OVAL
                setStroke(dp(2), Color.rgb(170, 180, 190))
            }
            isClickable = true
            isFocusable = true
            setOnClickListener { showCredits() }
        }, LinearLayout.LayoutParams(dp(36), dp(36)))
        root.addView(titleRow)
        root.addView(TextView(this).apply {
            text = "Windows visual novels. Add a game's install folder (the one its .exe is in)."
            setTextColor(Color.rgb(170, 180, 190))
            setPadding(0, dp(4), 0, dp(16))
        })
        // The buttons run on to a new line when the screen is too narrow for them (portrait phones).
        val buttons = FlowRow(this, dp(12), dp(8))
        buttons.addView(Button(this).apply {
            text = "Add game folder"
            setOnClickListener { pickFolder() }
        })
        buttons.addView(Button(this).apply {
            text = "Fonts"
            setOnClickListener { showFonts() }
        })
        buttons.addView(Button(this).apply {
            text = "Touch controls"
            setOnClickListener { showControls() }
        })
        // The virtual cursor's speed (the switch itself is in the game's Back menu): regular moves
        // it as far as the finger, fast twice as far.
        buttons.addView(Button(this).apply {
            val controls = Controls(this@LibraryActivity)
            fun label() = "Cursor speed: " + if (controls.fastCursor) "Fast" else "Regular"
            text = label()
            setOnClickListener {
                controls.fastCursor = !controls.fastCursor
                text = label()
            }
        })
        root.addView(buttons, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(ScrollView(this).apply { addView(list) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        setContentView(root)
        root.setOnApplyWindowInsetsListener { v, insets ->
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(android.view.WindowInsets.Type.systemBars())
                v.setPadding(dp(24) + bars.left, dp(24) + bars.top, dp(24) + bars.right, dp(24) + bars.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(dp(24) + insets.systemWindowInsetLeft, dp(24) + insets.systemWindowInsetTop,
                    dp(24) + insets.systemWindowInsetRight, dp(24) + insets.systemWindowInsetBottom)
            }
            insets
        }
        refresh()
        // Titles read by older versions (UTF-8 configs read as Shift-JIS, untranslated `$8000-0001$` ids)
        // are read again once.
        if (prefs.getInt("titleVersion", 0) < TITLE_VERSION) {
            Thread {
                val edit = prefs.edit()
                for (key in prefs.getStringSet("games", emptySet()) ?: emptySet()) {
                    runCatching {
                        val folder = SafFolder(contentResolver, Uri.parse(key))
                        edit.putString("title:$key", folder.guessTitle() ?: folder.displayName())
                    }
                }
                edit.putInt("titleVersion", TITLE_VERSION).apply()
                runOnUiThread { refresh() }
            }.start()
        }
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun games(): List<Pair<Uri, String>> =
        (prefs.getStringSet("games", emptySet()) ?: emptySet()).sorted().map {
            val uri = Uri.parse(it)
            uri to (prefs.getString("title:$it", null) ?: uri.lastPathSegment ?: it)
        }

    private fun refresh() {
        list.removeAllViews()
        val games = games()
        if (games.isEmpty()) {
            list.addView(TextView(this).apply {
                text = "No games yet."
                setTextColor(Color.GRAY)
                setPadding(0, dp(24), 0, 0)
            })
        }
        for ((uri, title) in games) {
            list.addView(TextView(this).apply {
                text = title
                setTextColor(Color.WHITE)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
                setPadding(dp(12), dp(18), dp(12), dp(18))
                gravity = Gravity.CENTER_VERTICAL
                setBackgroundColor(Color.rgb(30, 40, 52))
                setOnClickListener { launch(uri) }
                setOnLongClickListener { showGameMenu(uri, title); true }
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                .apply { topMargin = dp(10) })
        }
    }

    private fun pickFolder() {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_FOLDER)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_FONTS) {
            val pending = prefs.getString(PENDING_LAUNCH, null)
            prefs.edit().remove(PENDING_LAUNCH).apply()
            if (resultCode == RESULT_OK && data != null) addFonts(data, pending)
            return
        }
        val uri = data?.data
        if (requestCode == REQUEST_EXPORT || requestCode == REQUEST_IMPORT) {
            val game = prefs.getString(PENDING_GAME, null)
            prefs.edit().remove(PENDING_GAME).apply()
            if (resultCode == RESULT_OK && uri != null && game != null) {
                if (requestCode == REQUEST_EXPORT) exportSaves(Uri.parse(game), uri) else importSaves(Uri.parse(game), uri)
            }
            return
        }
        if (requestCode != REQUEST_FOLDER || resultCode != RESULT_OK || uri == null) return
        contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        val folder = SafFolder(contentResolver, uri)
        if (NativeBridge.detectEngine(folder) == null) {
            Toast.makeText(this, "No supported game found in that folder.", Toast.LENGTH_LONG).show()
            return
        }
        val key = uri.toString()
        val set = HashSet(prefs.getStringSet("games", emptySet()) ?: emptySet())
        set.add(key)
        prefs.edit()
            .putStringSet("games", set)
            .putString("title:$key", folder.guessTitle() ?: folder.displayName())
            .apply()
        refresh()
    }

    /** About: what Nyanroid is, its licence, what it was made with, and the licence texts. */
    private fun showCredits() {
        val version = try { packageManager.getPackageInfo(packageName, 0).versionName } catch (e: Exception) { null }
        val text = """
            Nyanroid ${version.orEmpty()}

            Nyanroid is a half-baked vibe coded emulator on Android for CatSystem2 games, Diesel Engine games, White Album 2 English TL, and potentially more visual novels in the future.

            It contains no code or data from the games: play your own copies.

            Nyanroid is free software under the GNU General Public License, version 3 or later, and comes with ABSOLUTELY NO WARRANTY. Licences shows its terms and those of the software it includes. Source code: https://github.com/CosmicRunner123/Nyanroid

            Made with the help of:

            • FelineSystem2 (kokseen1): an open source remake of the CatSystem2 engine, used as a reference.
            • TriggersTools.CatSystem2 and its wiki (Robert Jordan / trigger-segfault): CatSystem2 file formats and tools, used as a reference.
            • Ghidra (NSA): used to study the engines' executables.

            Libraries included in the app:

            • stb_truetype (altered), stb_image, stb_image_write, stb_vorbis (Sean Barrett). Public domain / MIT.
            • pl_mpeg (Dominic Szablewski). MIT.
            • FFmpeg 7.1.1, for Windows Media movies. LGPL 2.1 or later.
            • Squirrel 2.2.4 (Alberto Demichelis), altered, for Diesel Engine scripts. zlib-style licence.
            • zstd 1.5.7 (Meta). BSD.
            • libwebp 1.3.1 decoder (Google). BSD.
            • FAudio 26.11's reverb (Ethan Lee, Luigi Auriemma, the MonoGame Team), altered, for Diesel Engine sound. zlib licence.
            • zlib (Jean-loup Gailly, Mark Adler), Android's own. zlib licence.
        """.trimIndent()
        val dialog = AlertDialog.Builder(this)
            .setTitle("About Nyanroid")
            .setMessage(text)
            .setNeutralButton("Licences") { _, _ -> showLicences() }
            .setPositiveButton(android.R.string.ok, null)
            .show()
        // The source link opens in the browser.
        dialog.findViewById<TextView>(android.R.id.message)?.let {
            android.text.util.Linkify.addLinks(it, android.text.util.Linkify.WEB_URLS)
            it.movementMethod = android.text.method.LinkMovementMethod.getInstance()
        }
    }

    /** The licence texts the APK carries (LICENSE and THIRD_PARTY_NOTICES.txt, see build.gradle.kts). */
    private fun showLicences() {
        AlertDialog.Builder(this)
            .setTitle("Licences")
            .setItems(arrayOf("Nyanroid: GNU General Public License v3", "Third-party software")) { _, which ->
                showTextAsset(if (which == 0) "LICENSE" else "THIRD_PARTY_NOTICES.txt")
            }
            .setNegativeButton("Close", null)
            .show()
    }

    private fun showTextAsset(name: String) {
        val text = try {
            assets.open(name).bufferedReader().use { it.readText() }
        } catch (e: Exception) {
            "($name is missing from this build.)"
        }
        val view = TextView(this).apply {
            this.text = text
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            setTextIsSelectable(true)
            setPadding(dp(16), dp(8), dp(16), dp(8))
        }
        AlertDialog.Builder(this)
            .setTitle(name)
            .setView(ScrollView(this).apply { addView(view) })
            .setPositiveButton(android.R.string.ok) { _, _ -> showLicences() }
            .show()
    }

    /** A long press on a game: its saves out to a zip, a zip of saves in, or off the list. */
    private fun showGameMenu(uri: Uri, title: String) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setItems(arrayOf("Export saves", "Import saves", "Delete saves", "Remove from library")) { _, which ->
                when (which) {
                    0 -> pickExport(uri, title)
                    1 -> confirmImport(uri, title)
                    2 -> confirmDeleteSaves(uri, title)
                    3 -> confirmRemove(uri, title)
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun pickExport(uri: Uri, title: String) {
        if (Saves.files(Saves.dirFor(this, uri)).isEmpty()) {
            Toast.makeText(this, "$title has no saves to export yet.", Toast.LENGTH_LONG).show()
            return
        }
        val name = title.replace(Regex("[\\\\/:*?\"<>|]"), "_").take(80) + " saves.zip"
        prefs.edit().putString(PENDING_GAME, uri.toString()).apply()
        startActivityForResult(Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/zip"
            putExtra(Intent.EXTRA_TITLE, name)
        }, REQUEST_EXPORT)
    }

    private fun confirmImport(uri: Uri, title: String) {
        val existing = Saves.files(Saves.dirFor(this, uri)).size
        AlertDialog.Builder(this)
            .setTitle("Import saves?")
            .setMessage(
                "$title\n\nThe zip's saves replace ALL of this game's saves, including its global data " +
                    "(settings, read text and unlocked extras)." +
                    (if (existing > 0) " The $existing file(s) there now will be deleted. Export them first to keep a copy." else "") +
                    "\n\nThis cannot be undone."
            )
            .setPositiveButton("Choose zip") { _, _ ->
                prefs.edit().putString(PENDING_GAME, uri.toString()).apply()
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                    putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
                }, REQUEST_IMPORT)
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    /** Deletes the game's whole save folder: it starts again as on a first launch. */
    private fun confirmDeleteSaves(uri: Uri, title: String) {
        val dir = Saves.dirFor(this, uri)
        val count = Saves.files(dir).size
        if (count == 0) {
            Toast.makeText(this, "$title has no saves to delete.", Toast.LENGTH_LONG).show()
            return
        }
        AlertDialog.Builder(this)
            .setTitle("Delete saves?")
            .setMessage(
                "$title\n\nAll $count save file(s) are deleted, including the global data (settings, " +
                    "read text and unlocked extras): the game starts as it does on a first launch. " +
                    "Export them first to keep a copy.\n\nThis cannot be undone."
            )
            .setPositiveButton("Delete") { _, _ ->
                val ok = dir.deleteRecursively()
                Toast.makeText(this, if (ok) "Saves deleted." else "Some saves could not be deleted.", Toast.LENGTH_LONG).show()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun exportSaves(game: Uri, target: Uri) {
        val dir = Saves.dirFor(this, game)
        Thread {
            val msg = try {
                val n = contentResolver.openOutputStream(target, "wt")?.use { Saves.export(dir, it) }
                    ?: throw java.io.IOException("cannot write there")
                "Exported $n save file(s)."
            } catch (e: Exception) {
                "Export failed: ${e.message}"
            }
            runOnUiThread { Toast.makeText(this, msg, Toast.LENGTH_LONG).show() }
        }.start()
    }

    private fun importSaves(game: Uri, source: Uri) {
        val dir = Saves.dirFor(this, game)
        Thread {
            val msg = try {
                val n = contentResolver.openInputStream(source)?.use { Saves.import(it, dir) }
                    ?: throw java.io.IOException("cannot read it")
                "Imported $n save file(s)."
            } catch (e: Exception) {
                "Import failed, saves unchanged: ${e.message}"
            }
            runOnUiThread { Toast.makeText(this, msg, Toast.LENGTH_LONG).show() }
        }.start()
    }

    private fun confirmRemove(uri: Uri, title: String) {
        AlertDialog.Builder(this)
            .setTitle("Remove from library?")
            .setMessage("$title\n\nThe game files are not touched.")
            .setPositiveButton("Remove") { _, _ ->
                val key = uri.toString()
                val set = HashSet(prefs.getStringSet("games", emptySet()) ?: emptySet())
                set.remove(key)
                prefs.edit().putStringSet("games", set).remove("title:$key")
                    .remove("fontsOk:$key").remove("fontsNoWarn:$key").apply()
                try {
                    contentResolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                } catch (_: SecurityException) {
                }
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    /** Gesture -> mouse/keyboard settings. A tap on a row picks its input. */
    private fun showControls() {
        val controls = Controls(this)
        val gestures = Gesture.values()
        fun rows() = gestures.map { "${it.label}:  ${controls[it].label}" }.toTypedArray()
        lateinit var dialog: AlertDialog
        dialog = AlertDialog.Builder(this)
            .setTitle("Touch controls")
            .setItems(rows()) { _, which ->
                val g = gestures[which]
                val choices = g.choices
                val current = choices.indexOfFirst { it.id == controls[g].id }
                AlertDialog.Builder(this)
                    .setTitle(g.label)
                    .setSingleChoiceItems(choices.map { it.label }.toTypedArray(), current) { d, i ->
                        controls[g] = choices[i]
                        d.dismiss()
                        showControls()
                    }
                    .setNegativeButton("Cancel") { _, _ -> showControls() }
                    .show()
            }
            .setNeutralButton("Defaults") { _, _ ->
                controls.reset()
                showControls()
            }
            .setPositiveButton("Done", null)
            .create()
        dialog.show()
    }

    /**
     * Starts a game once it is known to have the fonts it names: a Windows font the player has not
     * added gets a warning first (Android's font would stand in). A game found fine is not checked
     * again until the font folders change.
     */
    private fun launch(uri: Uri) {
        if (checkingFonts) return
        checkingFonts = true
        val key = uri.toString()
        Thread {
            // A game whose exe hides its archives' key: work it out first (once), saying so.
            val folder = SafFolder(contentResolver, uri)
            if (NativeBridge.needsArchiveKeySearch(folder)) {
                var notice: AlertDialog? = null
                runOnUiThread {
                    if (!isFinishing && !isDestroyed) {
                        notice = AlertDialog.Builder(this)
                            .setTitle("First start")
                            .setMessage(
                                "This game's program hides the key to its archives, so Nyanroid is working it out. " +
                                    "This takes a few seconds, and only happens once."
                            )
                            .setCancelable(false)
                            .show()
                    }
                }
                NativeBridge.findArchiveKey(folder)
                runOnUiThread { notice?.dismiss() }
            }
            val fingerprint = Fonts.fingerprint(this)
            val skip = prefs.getBoolean("fontsNoWarn:$key", false) || prefs.getString("fontsOk:$key", null) == fingerprint
            val missing = if (skip) emptyArray() else try {
                NativeBridge.missingFonts(SafFolder(contentResolver, uri), Fonts.dirs(this))
            } catch (e: Throwable) {
                emptyArray<String>()
            }
            runOnUiThread {
                checkingFonts = false
                if (isFinishing || isDestroyed) return@runOnUiThread
                if (missing.isEmpty()) {
                    if (!skip) prefs.edit().putString("fontsOk:$key", fingerprint).apply()
                    start(uri)
                } else {
                    warnMissingFonts(uri, missing)
                }
            }
        }.start()
    }

    private fun start(uri: Uri) {
        startActivity(Intent(this, GameActivity::class.java).putExtra(GameActivity.EXTRA_FOLDER, uri.toString()))
    }

    private fun warnMissingFonts(uri: Uri, missing: Array<String>) {
        val key = uri.toString()
        val title = prefs.getString("title:$key", null) ?: uri.lastPathSegment ?: "This game"
        val names = missing.map { Fonts.displayName(it) }
        val asks = if (names.size == 1) names[0] else names.dropLast(1).joinToString(", ") + " or " + names.last()
        val noWarn = android.widget.CheckBox(this).apply { text = "Don't warn again for this game" }
        val box = LinearLayout(this).apply {
            setPadding(dp(20), dp(8), dp(20), 0)
            addView(noWarn)
        }
        AlertDialog.Builder(this)
            .setTitle("Missing font")
            .setMessage(
                "$title draws its text in $asks, which Nyanroid does not have. Android's own Japanese " +
                    "font will stand in, so the text may look different and fit the game's layout less well." +
                    "\n\nIf you have the font, add its file with Add fonts."
            )
            .setView(box)
            .setPositiveButton("Play") { _, _ ->
                if (noWarn.isChecked) prefs.edit().putBoolean("fontsNoWarn:$key", true).apply()
                start(uri)
            }
            .setNeutralButton("Add fonts") { _, _ ->
                prefs.edit().putString(PENDING_LAUNCH, key).apply()
                pickFonts()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    /** The fonts games may name that Nyanroid cannot include (Windows' MS Gothic, Meiryo, ...). */
    private fun showFonts() {
        Thread {
            val bundled = Fonts.bundled(this).map { it.name }
            val added = Fonts.added(this).map { it.name }
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                val text = StringBuilder(
                    "Games draw their text in their own fonts, or in Windows fonts such as MS Gothic or " +
                        "Meiryo, which Nyanroid cannot include. If you have such fonts, add their files " +
                        "(.ttf, .ttc, .otf) here: games find them by name. Without them, Android's own " +
                        "Japanese font stands in.\n\n"
                )
                if (bundled.isNotEmpty()) text.append("Built into this build:\n").append(bundled.joinToString("\n") { "• $it" }).append("\n\n")
                text.append("Added:\n").append(if (added.isEmpty()) "None yet." else added.joinToString("\n") { "• $it" })
                val dialog = AlertDialog.Builder(this)
                    .setTitle("Fonts")
                    .setMessage(text)
                    .setPositiveButton("Add fonts") { _, _ -> pickFonts() }
                    .setNegativeButton("Close", null)
                if (added.isNotEmpty()) dialog.setNeutralButton("Remove") { _, _ -> removeFonts() }
                dialog.show()
            }
        }.start()
    }

    private fun pickFonts() {
        // Any type: font files come with many types (font/ttf, application/octet-stream, ...); what
        // is picked is checked as it is copied.
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }, REQUEST_FONTS)
    }

    /** Copies the picked fonts in; then starts the game that asked for them, or shows the list. */
    private fun addFonts(data: Intent, pending: String?) {
        val uris = mutableListOf<Uri>()
        data.clipData?.let { c -> for (i in 0 until c.itemCount) uris += c.getItemAt(i).uri }
        if (uris.isEmpty()) data.data?.let { uris += it }
        Thread {
            var added = 0
            var skipped = 0
            for (u in uris) {
                if ((try { Fonts.add(this, u) } catch (e: Exception) { null }) != null) added++ else skipped++
            }
            runOnUiThread {
                val msg = (if (added == 0) "No fonts added." else "Added $added font(s).") +
                    (if (skipped > 0) " $skipped file(s) were not fonts." else "")
                Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
                if (pending != null) launch(Uri.parse(pending)) else showFonts()
            }
        }.start()
    }

    private fun removeFonts() {
        val files = Fonts.added(this)
        val checked = BooleanArray(files.size)
        AlertDialog.Builder(this)
            .setTitle("Remove fonts")
            .setMultiChoiceItems(files.map { it.name }.toTypedArray(), checked) { _, i, on -> checked[i] = on }
            .setPositiveButton("Remove") { _, _ ->
                val n = files.indices.count { checked[it] && files[it].delete() }
                Toast.makeText(this, "Removed $n font(s).", Toast.LENGTH_SHORT).show()
                showFonts()
            }
            .setNegativeButton("Cancel") { _, _ -> showFonts() }
            .show()
    }

    companion object {
        private const val REQUEST_FOLDER = 1
        private const val REQUEST_EXPORT = 2
        private const val REQUEST_IMPORT = 3
        private const val REQUEST_FONTS = 4
        private const val PENDING_GAME = "pendingSaveGame"  // the game an export / import is for
        private const val PENDING_LAUNCH = "pendingFontsGame"  // the game the fonts being added are for
    }
}

@Suppress("unused")
private fun View.hide() {
    visibility = View.GONE
}
