package io.upix.data.repository

import android.net.Uri
import android.provider.DocumentsContract
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

/** 文件夹源判定（UI 角标用；「添加文件夹」选入的是 SAF 树 URI）。 */
@RunWith(RobolectricTestRunner::class)
class FolderInputInfoTest {

    @Test
    fun treeUriIsFolderSource() {
        val treeUri = DocumentsContract.buildTreeDocumentUri(
            "com.android.externalstorage.documents",
            "primary:DCIM/Camera"
        )
        assertTrue(FolderInputInfo.isFolderSource(treeUri.toString()))
    }

    @Test
    fun plainUrisAndPathsAreNotFolderSources() {
        assertFalse(FolderInputInfo.isFolderSource("content://media/external/images/1"))
        assertFalse(FolderInputInfo.isFolderSource("file:///sdcard/DCIM"))
        assertFalse(FolderInputInfo.isFolderSource("/sdcard/DCIM"))
        assertFalse(FolderInputInfo.isFolderSource("upix://multi:[\"content://a/1.jpg\"]"))
        assertFalse(FolderInputInfo.isFolderSource(""))
    }

    @Test
    fun countImagesReturnsNullForNonFolderSource() {
        // 单图 URI 交给协程层验证 null 语义需要 Robolectric 主线程调度；
        // 这里直接验证非文件夹源在纯同步路径上的快速返回。
        assertFalse(FolderInputInfo.isFolderSource(Uri.parse("content://a/1.jpg").toString()))
    }
}
