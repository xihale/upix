package io.upix.data.repository

import android.content.Context
import android.provider.DocumentsContract
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import androidx.core.net.toUri

/**
 * 文件夹输入源的轻量查询：给 UI 的「这是整个文件夹」标识（舞台角标、
 * 预览信息栏）用。「添加文件夹」选入的源是 SAF 树 URI；与处理时的完整
 * 解析（InputSourceRepository，逐图解码探测）不同，这里只按 MIME/扩展名
 * 枚举计数、不做解码探测，大文件夹也能即时刷新。
 */
object FolderInputInfo {

    /** 源是否为「添加文件夹」选入的 SAF 树 URI。 */
    fun isFolderSource(source: String): Boolean {
        val uri = runCatching { source.trim().toUri() }.getOrNull() ?: return false
        return uri.scheme?.lowercase() == "content" && DocumentsContract.isTreeUri(uri)
    }

    /**
     * 统计文件夹内（含子目录，与默认 processNestedFolders=true 一致）可处理的
     * 图片数。非文件夹源、无权限或枚举失败返回 null，UI 回退为不带计数的
     * 「文件夹」文案。
     */
    suspend fun countImages(context: Context, source: String): Int? {
        if (!isFolderSource(source)) {
            return null
        }
        return withContext(Dispatchers.IO) {
            val treeUri = source.trim().toUri()
            val rootDocumentId = runCatching { DocumentsContract.getTreeDocumentId(treeUri) }
                .getOrNull() ?: return@withContext null
            val pendingDocumentIds = ArrayDeque<String>().apply { add(rootDocumentId) }
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME
            )
            var count = 0
            while (pendingDocumentIds.isNotEmpty()) {
                val documentId = pendingDocumentIds.removeFirst()
                val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, documentId)
                try {
                    context.contentResolver.query(childrenUri, projection, null, null, null)?.use { cursor ->
                        val idIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                        val mimeIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE)
                        val nameIndex = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                        while (cursor.moveToNext()) {
                            val childId = if (idIndex >= 0) cursor.getString(idIndex) else null
                            val mimeType = if (mimeIndex >= 0) cursor.getString(mimeIndex) else null
                            if (mimeType == DocumentsContract.Document.MIME_TYPE_DIR) {
                                if (!childId.isNullOrBlank()) {
                                    pendingDocumentIds.addLast(childId)
                                }
                                continue
                            }
                            val displayName = if (nameIndex >= 0) {
                                cursor.getString(nameIndex)
                            } else {
                                null
                            } ?: childId?.substringAfterLast(':') ?: continue
                            if (isImageMimeOrName(mimeType = mimeType, name = displayName)) {
                                count += 1
                            }
                        }
                    } ?: return@withContext null
                } catch (_: SecurityException) {
                    return@withContext null
                } catch (_: IllegalArgumentException) {
                    return@withContext null
                }
            }
            count
        }
    }
}
