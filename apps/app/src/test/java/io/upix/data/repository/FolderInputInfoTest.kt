package io.upix.data.repository

import android.provider.DocumentsContract
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import kotlinx.coroutines.runBlocking

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
        // 非文件夹源在进入协程/SAF 查询前快速返回 null（countImages 的第一道
        // 门）。空源与 upix://multi: 源同样落在这条路径上。
        assertNull(
            runBlocking { FolderInputInfo.countImages(context = RuntimeEnvironment.getApplication(), source = "") }
        )
        assertNull(
            runBlocking {
                FolderInputInfo.countImages(
                    context = RuntimeEnvironment.getApplication(),
                    source = "content://media/external/images/1"
                )
            }
        )
    }
}
