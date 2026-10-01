package org.openbooks.android

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import androidx.core.content.FileProvider
import java.io.File

/** File plumbing: books live in app-private storage; exports go through the share sheet or SAF. */
object Files {
    const val EXTENSION = ".obk"
    const val MAX_IMPORT_BYTES = 64L * 1024 * 1024

    fun books(dir: File): List<File> =
        dir.listFiles { f -> f.isFile && f.name.endsWith(EXTENSION) }?.sortedByDescending { it.lastModified() } ?: emptyList()

    /** True when the file starts with the encrypted-books magic ("OBKCRYPT"). */
    fun isEncrypted(file: File): Boolean = runCatching {
        file.inputStream().use { s ->
            val head = ByteArray(8)
            s.read(head) == 8 && String(head, Charsets.US_ASCII) == "OBKCRYPT"
        }
    }.getOrDefault(false)

    /** "Acme Rockets, Inc." -> "Acme Rockets Inc" (safe as a file name everywhere). */
    fun safeName(name: String): String =
        name.map { if (it.isLetterOrDigit() || it == ' ' || it == '-' || it == '_') it else ' ' }
            .joinToString("").trim().replace(Regex("\\s+"), " ").take(60).ifEmpty { "Books" }

    fun uniqueFile(dir: File, base: String, extension: String): File {
        var f = File(dir, base + extension)
        var n = 2
        while (f.exists()) f = File(dir, "$base ($n)$extension").also { n++ }
        return f
    }

    fun displayName(context: Context, uri: Uri): String? =
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }

    /** Copies a picked books file into app storage and returns the copy. */
    fun importBooks(context: Context, uri: Uri, dir: File): File {
        val name = displayName(context, uri)?.removeSuffix(EXTENSION) ?: "Imported"
        val target = uniqueFile(dir, safeName(name), EXTENSION)
        val input = context.contentResolver.openInputStream(uri) ?: throw CoreException("error", "could not read that file")
        input.use { src ->
            target.outputStream().use { dst ->
                val buffer = ByteArray(64 * 1024)
                var total = 0L
                while (true) {
                    val n = src.read(buffer)
                    if (n < 0) break
                    total += n
                    if (total > MAX_IMPORT_BYTES) {
                        dst.close()
                        target.delete()
                        throw CoreException("error", "that file is too large to be a books file")
                    }
                    dst.write(buffer, 0, n)
                }
            }
        }
        val head = target.inputStream().use { s -> ByteArray(9).also { s.read(it) } }
        val text = String(head, Charsets.ISO_8859_1)
        if (!text.startsWith("OBKCRYPT") && !text.startsWith("OPENBOOKS")) {
            target.delete()
            throw CoreException("error", "that is not an OpenBooks file")
        }
        return target
    }

    fun copyToUri(context: Context, file: File, uri: Uri) {
        val out = context.contentResolver.openOutputStream(uri, "wt") ?: throw CoreException("error", "could not write there")
        out.use { dst -> file.inputStream().use { it.copyTo(dst) } }
    }

    fun readText(context: Context, uri: Uri, limit: Long = 16L * 1024 * 1024): String {
        val input = context.contentResolver.openInputStream(uri) ?: throw CoreException("error", "could not read that file")
        val bytes = input.use { s ->
            val out = java.io.ByteArrayOutputStream()
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val n = s.read(buffer)
                if (n < 0) break
                out.write(buffer, 0, n)
                if (out.size() > limit) throw CoreException("error", "that file is too large")
            }
            out.toByteArray()
        }
        return String(bytes, Charsets.UTF_8).removePrefix("﻿")
    }

    private fun contentUri(context: Context, file: File): Uri =
        FileProvider.getUriForFile(context, context.packageName + ".files", file)

    fun share(context: Context, file: File, mime: String) {
        val uri = contentUri(context, file)
        val send = Intent(Intent.ACTION_SEND)
            .setType(mime)
            .putExtra(Intent.EXTRA_STREAM, uri)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        send.clipData = ClipData.newRawUri(file.name, uri)
        context.startActivity(Intent.createChooser(send, "Share ${file.name}").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    fun view(context: Context, file: File, mime: String): Boolean {
        val intent = Intent(Intent.ACTION_VIEW)
            .setDataAndType(contentUri(context, file), mime)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        return runCatching { context.startActivity(intent) }.isSuccess
    }
}
