package dev.nyanroid

import android.content.Context
import android.net.Uri
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * A game's saves: one folder in app storage per game folder (the game folder itself is read-only),
 * holding the save slots and the global data (settings, read text, unlocked extras). The library
 * exports it as a zip and imports one back.
 */
object Saves {
    /** The save folder of the game at [uri], keyed by the folder's name and the URI's hash. */
    fun dirFor(context: Context, uri: Uri): File {
        val tree = Uri.decode(uri.lastPathSegment ?: uri.toString())
        val name = tree.substringAfterLast('/').substringAfterLast(':').ifEmpty { "game" }
            .replace(Regex("[^A-Za-z0-9._ -]"), "_")
        val hash = "%08x".format(uri.toString().hashCode())
        return File(context.filesDir, "saves/$name-$hash")
    }

    /** The files a save folder holds (its subfolders' too), empty when it has none. */
    fun files(dir: File): List<File> =
        if (!dir.isDirectory) emptyList() else dir.walkTopDown().filter { it.isFile }.toList()

    /** Writes every file of [dir] into a zip, by its path inside the folder. Returns how many. */
    fun export(dir: File, out: OutputStream): Int {
        val all = files(dir)
        ZipOutputStream(out.buffered()).use { zip ->
            for (f in all) {
                zip.putNextEntry(ZipEntry(f.relativeTo(dir).invariantSeparatorsPath).apply { time = f.lastModified() })
                f.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
            }
        }
        return all.size
    }

    /**
     * Replaces everything in [dir] with the files of a zip. The zip is unpacked into a folder next
     * to [dir] first, and nothing is replaced unless that succeeds: a zip with folders in it, an
     * entry that would land outside the folder (an absolute path or ".."), or a zip with no files is
     * refused. Returns how many.
     */
    fun import(input: InputStream, dir: File): Int {
        val parent = dir.parentFile ?: throw IOException("no saves folder")
        parent.mkdirs()
        val staging = File(parent, ".import-${dir.name}")
        staging.deleteRecursively()
        staging.mkdirs()
        var count = 0
        try {
            ZipInputStream(input.buffered()).use { zip ->
                val root = staging.canonicalFile
                while (true) {
                    val e = zip.nextEntry ?: break
                    // Save folders are flat (save0000.nsv, global.nsv, WA2's system.nsv ...): a zip
                    // with folders in it (an APK, another app's archive) is not an export of ours.
                    if (e.isDirectory || e.name.trim('/').contains('/') || e.name.contains('\\'))
                        throw IOException("not a Nyanroid save export (it contains folders)")
                    val target = File(staging, e.name).canonicalFile
                    if (!target.path.startsWith(root.path + File.separator)) throw IOException("bad entry ${e.name}")
                    target.parentFile?.mkdirs()
                    target.outputStream().use { zip.copyTo(it) }
                    if (e.time > 0) target.setLastModified(e.time)
                    count++
                }
            }
            if (count == 0) throw IOException("the zip has no files")
            dir.deleteRecursively()
            if (!staging.renameTo(dir)) {
                staging.copyRecursively(dir, overwrite = true)
                staging.deleteRecursively()
            }
            return count
        } catch (e: Exception) {
            staging.deleteRecursively()
            throw e
        }
    }
}
