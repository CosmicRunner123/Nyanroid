package dev.nyanroid

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract

/**
 * Read-only access to a game folder the user picked with the Storage Access Framework.
 * Native code calls [list] and [openFd] (through JNI) and reads files with pread() on the
 * returned descriptors, so game data is used in place without copying.
 * Names are matched case-insensitively, as the games were authored for Windows.
 */
class SafFolder(private val resolver: ContentResolver, val treeUri: Uri) {
    private data class Child(val name: String, val docId: String, val isDir: Boolean)

    private val rootDocId: String = DocumentsContract.getTreeDocumentId(treeUri)
    private val childrenCache = HashMap<String, List<Child>>()

    private fun children(docId: String): List<Child> = childrenCache.getOrPut(docId) {
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, docId)
        val out = ArrayList<Child>()
        resolver.query(
            uri,
            arrayOf(
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
            ),
            null, null, null,
        )?.use { c ->
            while (c.moveToNext()) {
                val mime = c.getString(2) ?: ""
                out.add(Child(c.getString(0) ?: "", c.getString(1), mime == DocumentsContract.Document.MIME_TYPE_DIR))
            }
        }
        out
    }

    /** Names of regular files directly in the folder. Called from native code. */
    fun list(): Array<String> = children(rootDocId).filter { !it.isDir }.map { it.name }.toTypedArray()

    /** Names of regular files in subfolder `sub` (case-insensitive). Called from native code. */
    fun listDir(sub: String): Array<String> {
        val dir = children(rootDocId).firstOrNull { it.isDir && it.name.equals(sub, ignoreCase = true) }
            ?: return emptyArray()
        return children(dir.docId).filter { !it.isDir }.map { it.name }.toTypedArray()
    }

    /** Display name of the picked folder. */
    fun displayName(): String = treeUri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':') ?: "game"

    /**
     * Opens `path` (may contain '/' separators) read-only and returns a detached file
     * descriptor owned by the caller, or -1 if it does not exist. Called from native code.
     */
    fun openFd(path: String): Int {
        var docId = rootDocId
        val parts = path.split('/', '\\').filter { it.isNotEmpty() }
        for ((i, part) in parts.withIndex()) {
            val child = children(docId).firstOrNull { it.name.equals(part, ignoreCase = true) } ?: return -1
            if (i < parts.size - 1 && !child.isDir) return -1
            docId = child.docId
        }
        return try {
            val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId)
            resolver.openFileDescriptor(uri, "r")?.detachFd() ?: -1
        } catch (e: Exception) {
            -1
        }
    }

    /** Window title from config/startup.xml (CatSystem2 games), if readable. */
    fun guessTitle(): String? {
        val text = readText("config/startup.xml") ?: return null
        val raw = Regex("<title>(.*?)</title>", RegexOption.DOT_MATCHES_ALL).find(text)?.groupValues?.get(1)?.trim()
            ?: return null
        // `$8000-0001$<original>`: a string of the game's language table, as the engine resolves it (the
        // default language's language_<prefix>.txt: the id on one line, the text on the next).
        val id = Regex("^(\\$[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}\\$)(.*)$", RegexOption.DOT_MATCHES_ALL).find(raw)
            ?: return raw
        val fallback = id.groupValues[2].trim()
        val lang = Regex("<language>(.*?)</language>", RegexOption.DOT_MATCHES_ALL).find(text)?.groupValues?.get(1)
        val prefix = lang?.let {
            val def = Regex("<default>\\s*(\\d+)\\s*</default>").find(it)?.groupValues?.get(1)?.toIntOrNull() ?: 0
            Regex("<L%02d>.*?<prefix>(.*?)</prefix>".format(def), RegexOption.DOT_MATCHES_ALL).find(it)?.groupValues?.get(1)
        }
        val table = prefix?.let { readText("language_$it.txt") } ?: return fallback.ifEmpty { raw }
        val lines = table.lines().map { it.trimEnd('\r') }
        val at = lines.indexOf(id.groupValues[1])
        val value = if (at >= 0 && at + 1 < lines.size) lines[at + 1].trim() else ""
        return if (value.isNotEmpty() && value != "\$empty\$" && value != "\$nr\$") value else fallback.ifEmpty { raw }
    }

    /** A game text file: UTF-8 if its XML declaration or BOM says so (or it decodes as UTF-8), else Shift-JIS. */
    private fun readText(path: String): String? {
        val fd = openFd(path)
        if (fd < 0) return null
        return try {
            android.os.ParcelFileDescriptor.adoptFd(fd).use { pfd ->
                val bytes = java.io.FileInputStream(pfd.fileDescriptor).readBytes()
                val head = String(bytes, 0, minOf(bytes.size, 100), Charsets.ISO_8859_1)
                val declared = Regex("encoding=\"([^\"]+)\"").find(head)?.groupValues?.get(1)?.lowercase()
                val utf8 = when {
                    bytes.size >= 3 && bytes[0] == 0xEF.toByte() && bytes[1] == 0xBB.toByte() && bytes[2] == 0xBF.toByte() -> true
                    declared != null -> declared.replace("-", "") == "utf8"
                    else -> runCatching {
                        Charsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(bytes)); true
                    }.getOrDefault(false)
                }
                String(bytes, if (utf8) Charsets.UTF_8 else charset("Shift_JIS")).removePrefix("\uFEFF")
            }
        } catch (e: Exception) {
            null
        }
    }
}
