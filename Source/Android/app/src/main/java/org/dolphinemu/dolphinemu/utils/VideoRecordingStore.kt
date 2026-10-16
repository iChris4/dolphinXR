// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import android.content.ContentValues
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.annotation.Keep
import org.dolphinemu.dolphinemu.DolphinApplication

/**
 * Output files of Dolphin's own video recording (the in-game VR menu's Record button). Videos go
 * straight into a pending MediaStore entry under Movies/DolphinXR, so once published they show up
 * in the headset's gallery like the system's recordings.
 */
object VideoRecordingStore {
    private const val FOLDER = "DolphinXR"

    private var pendingUri: Uri? = null

    /**
     * Creates a pending video named [displayName] and returns a writable file descriptor for it,
     * owned by the caller, or -1 (Android 9 and older, or MediaStore refused it).
     */
    @Keep
    @JvmStatic
    fun create(displayName: String): Int {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q)
            return -1

        val resolver = DolphinApplication.getAppContext().contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Video.Media.DISPLAY_NAME, displayName)
            put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
            put(MediaStore.Video.Media.RELATIVE_PATH, "${Environment.DIRECTORY_MOVIES}/$FOLDER")
            put(MediaStore.Video.Media.IS_PENDING, 1)
        }
        return try {
            val collection =
                MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
            val uri = resolver.insert(collection, values) ?: return -1
            val descriptor = resolver.openFileDescriptor(uri, "rw")
            if (descriptor == null) {
                resolver.delete(uri, null, null)
                return -1
            }
            pendingUri = uri
            descriptor.detachFd()
        } catch (e: Exception) {
            Log.error("[VideoRecordingStore] Could not create $displayName: ${e.message}")
            -1
        }
    }

    /** Publishes the video created by [create], or deletes it when [keep] is false. */
    @Keep
    @JvmStatic
    fun finish(keep: Boolean) {
        val uri = pendingUri ?: return
        pendingUri = null
        val resolver = DolphinApplication.getAppContext().contentResolver
        try {
            if (keep) {
                val values = ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }
                resolver.update(uri, values, null, null)
            } else {
                resolver.delete(uri, null, null)
            }
        } catch (e: Exception) {
            Log.error("[VideoRecordingStore] Could not finish the recording: ${e.message}")
        }
    }
}
