package dev.nyanroid

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File

/**
 * The fonts games draw their text with, besides their own: fonts built into this build of the app
 * (the project's fonts/ folder, empty in the public source) and fonts the player adds, such as the
 * Windows fonts many games name (MS Gothic, Meiryo). Without them Android's own Japanese font
 * stands in.
 */
object Fonts {
    private val EXTENSIONS = listOf(".ttf", ".ttc", ".otf")

    fun isFontName(name: String) = EXTENSIONS.any { name.endsWith(it, ignoreCase = true) }

    /** The fonts the player added (Android/data/dev.nyanroid/files/fonts). */
    fun userDir(context: Context): File =
        (context.getExternalFilesDir("fonts") ?: File(context.filesDir, "fonts")).apply { mkdirs() }

    fun added(context: Context): List<File> =
        userDir(context).listFiles()?.filter { it.isFile && isFontName(it.name) }?.sortedBy { it.name.lowercase() } ?: emptyList()

    /** The APK's built-in fonts, copied once to app storage (where native code reads them by path). */
    fun bundledDir(context: Context): File {
        val dir = File(context.filesDir, "fonts_bundled").apply { mkdirs() }
        val names = context.assets.list("")?.filter { isFontName(it) } ?: emptyList()
        for (name in names) {
            val out = File(dir, name)
            val size = try { context.assets.openFd(name).use { it.length } } catch (e: Exception) { -1L }
            if (out.exists() && out.length() == size) continue
            context.assets.open(name).use { input -> out.outputStream().use { input.copyTo(it) } }
        }
        // A font this build no longer has goes, so the engine does not find an old copy of a face.
        dir.listFiles()?.filter { it.name !in names }?.forEach { it.delete() }
        return dir
    }

    fun bundled(context: Context): List<File> =
        bundledDir(context).listFiles()?.filter { isFontName(it.name) }?.sortedBy { it.name.lowercase() } ?: emptyList()

    /** The folders the engine searches by face name, newline-separated (built-in, then added). */
    fun dirs(context: Context): String = "${bundledDir(context).absolutePath}\n${userDir(context).absolutePath}"

    /** What the font folders hold now (names and sizes): a game whose fonts were found stays fine
     *  until this changes. */
    fun fingerprint(context: Context): String =
        (bundled(context) + added(context)).joinToString("|") { "${it.name}:${it.length()}" }

    /**
     * Android's own font with Japanese characters: Noto Sans CJK on most devices (a variable font on
     * recent ones, which the engine draws at its regular weight), else an older Japanese font, else
     * the Latin Roboto.
     */
    fun systemFallback(): String {
        val dir = File("/system/fonts")
        for (name in listOf("NotoSansCJK-Regular.ttc", "NotoSansCJKjp-Regular.otf", "NotoSansJP-Regular.otf",
                "DroidSansJapanese.ttf", "DroidSansFallbackFull.ttf", "DroidSansFallback.ttf")) {
            val f = File(dir, name)
            if (f.exists()) return f.absolutePath
        }
        dir.listFiles()?.firstOrNull {
            it.name.contains("Sans", true) && (it.name.contains("CJK", true) || it.name.contains("JP"))
        }?.let { return it.absolutePath }
        return File(dir, "Roboto-Regular.ttf").absolutePath
    }

    /** A Windows face name as players know it: "Meiryo (メイリオ)". */
    fun displayName(face: String): String {
        val english = mapOf(
            "ＭＳ ゴシック" to "MS Gothic", "ＭＳ Ｐゴシック" to "MS PGothic", "MS UI Gothic" to "MS UI Gothic",
            "ＭＳ 明朝" to "MS Mincho", "ＭＳ Ｐ明朝" to "MS PMincho", "メイリオ" to "Meiryo",
            "游ゴシック" to "Yu Gothic", "游明朝" to "Yu Mincho",
        )[face.trim()]
        return if (english != null) "$english ($face)" else face
    }

    /**
     * Copies a picked font into the added fonts. Returns its name, or null if it is not a font
     * (judged by its name and its first bytes: TrueType, OpenType or a collection).
     */
    fun add(context: Context, uri: Uri): String? {
        val resolver = context.contentResolver
        var name = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        } ?: uri.lastPathSegment ?: return null
        name = name.substringAfterLast('/').replace(Regex("[\\\\:*?\"<>|]"), "_")
        if (!isFontName(name)) return null
        val head = ByteArray(4)
        val read = resolver.openInputStream(uri)?.use { it.read(head) } ?: return null
        val tag = String(head, Charsets.ISO_8859_1)
        val ok = read == 4 && (tag == "ttcf" || tag == "OTTO" || tag == "true" ||
            (head[0] == 0.toByte() && head[1] == 1.toByte() && head[2] == 0.toByte() && head[3] == 0.toByte()))
        if (!ok) return null
        val out = File(userDir(context), name)
        val tmp = File(out.parentFile, "$name.part")
        resolver.openInputStream(uri)?.use { input -> tmp.outputStream().use { input.copyTo(it) } } ?: return null
        if (!tmp.renameTo(out)) {
            tmp.delete()
            return null
        }
        return name
    }
}
