package io.upix.data.repository

import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** Callers serialize reads, writes, deletes and their observable state publication. */
internal class AtomicSnapshotFile(private val file: File) {
    private val staging = File(file.path + ".new")
    private val backup = File(file.path + ".bak")

    fun readText(): String? {
        recoverInterruptedWrite()
        return try {
            file.readText()
        } catch (exception: FileNotFoundException) {
            if (file.exists()) throw exception
            null
        }
    }

    fun writeText(payload: String) {
        recoverInterruptedWrite()
        try {
            FileOutputStream(staging).use { stream ->
                stream.write(payload.toByteArray(Charsets.UTF_8))
                stream.fd.sync()
            }
            if (file.exists()) replace(file, backup)
            try {
                replace(staging, file)
            } catch (writeFailure: Throwable) {
                if (backup.exists()) {
                    try {
                        replace(backup, file)
                    } catch (restoreFailure: Throwable) {
                        writeFailure.addSuppressed(restoreFailure)
                    }
                }
                throw writeFailure
            }
            // The primary snapshot is committed; a stale backup is harmless because
            // recovery prefers an existing primary file.
            runCatching { deleteIfPresent(backup) }
        } catch (throwable: Throwable) {
            try {
                deleteIfPresent(staging)
            } catch (cleanupFailure: IOException) {
                throwable.addSuppressed(cleanupFailure)
            }
            throw throwable
        }
    }

    fun delete() {
        // Delete recovery data first: an interrupted clear must never restore it.
        deleteIfPresent(backup)
        deleteIfPresent(staging)
        deleteIfPresent(file)
    }

    private fun recoverInterruptedWrite() {
        if (file.exists()) return
        if (backup.exists()) {
            replace(backup, file)
        }
        deleteIfPresent(staging)
    }

    private fun replace(source: File, destination: File) {
        Files.move(
            source.toPath(), destination.toPath(),
            StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE
        )
    }

    private fun deleteIfPresent(target: File) {
        if (target.exists() && !target.delete()) {
            throw IOException("Failed to delete ${target.name}")
        }
    }
}

