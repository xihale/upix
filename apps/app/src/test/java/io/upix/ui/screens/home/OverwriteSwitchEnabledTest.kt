package io.upix.ui.screens.home

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 覆盖原图开关的冲突互锁：手动选了具体输出格式（非跟随原图）时锁定为关；
 * 已开启时保持可用以便关闭；跟随原图时随时可用。
 */
class OverwriteSwitchEnabledTest {

    @Test
    fun followOriginalFormatKeepsOverwriteUsable() {
        assertTrue(overwriteSwitchEnabled(outputFormatFollowsOriginal = true, overwriteOriginal = false))
        assertTrue(overwriteSwitchEnabled(outputFormatFollowsOriginal = true, overwriteOriginal = true))
    }

    @Test
    fun explicitFormatLocksOverwriteWhenOff() {
        assertFalse(overwriteSwitchEnabled(outputFormatFollowsOriginal = false, overwriteOriginal = false))
    }

    @Test
    fun explicitFormatKeepsOverwriteUsableWhenAlreadyOn() {
        // 兼容旧版本持久化的冲突组合：必须允许用户把覆盖原图关回去。
        assertTrue(overwriteSwitchEnabled(outputFormatFollowsOriginal = false, overwriteOriginal = true))
    }
}
