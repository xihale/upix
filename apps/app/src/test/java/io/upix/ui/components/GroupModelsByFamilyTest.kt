package io.upix.ui.components

import io.upix.data.model.ModelInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * 模型家族分组：已知前缀按固定顺序成组且行名剥掉家族前缀；自定义导入模型
 * 按去倍率词干殿后；裸名（无 -xN 后缀）组不出重复标题。
 */
class GroupModelsByFamilyTest {

    private fun model(name: String) = ModelInfo(
        name = name,
        scaleFactor = name.substringAfterLast("-x", missingDelimiterValue = "").removePrefix("x").toIntOrNull() ?: 0,
        totalSize = 1024L,
        paramPath = "$name.param",
        binPath = "$name.bin"
    )

    @Test
    fun knownFamiliesGroupInFixedOrderWithStrippedLabels() {
        val groups = groupModelsByFamily(
            listOf(
                model("Real-CUGAN-pro-denoise-x2"),
                model("Real-ESRGANv3-anime-x2"),
                model("Real-ESRGAN-x4plus-anime"),
                model("Real-CUGAN-se-conservative-x3")
            )
        )
        assertEquals(
            listOf("Real-ESRGAN v3", "Real-ESRGAN x4plus", "Real-CUGAN pro", "Real-CUGAN se"),
            groups.map { group -> group.label }
        )
        assertEquals(listOf("anime-x2"), groups[0].items.map { it.label })
        assertEquals(listOf("anime"), groups[1].items.map { it.label })
        assertEquals(listOf("denoise-x2"), groups[2].items.map { it.label })
        assertEquals(listOf("conservative-x3"), groups[3].items.map { it.label })
    }

    @Test
    fun exactFamilyNameKeepsLastSegmentAsLabel() {
        // Real-ESRGAN-x4plus 本体：剥前缀后为空，回落末段 x4plus。
        val groups = groupModelsByFamily(listOf(model("Real-ESRGAN-x4plus")))
        assertEquals(listOf("x4plus"), groups.single().items.map { it.label })
    }

    @Test
    fun customModelsGroupByStemAfterKnownFamilies() {
        val groups = groupModelsByFamily(
            listOf(
                model("AnimeJaNai-x2"),
                model("Real-ESRGANv3-general-x4"),
                model("AnimeJaNai-x4")
            )
        )
        assertEquals(listOf("Real-ESRGAN v3", "AnimeJaNai"), groups.map { it.label })
        assertEquals(listOf("x2", "x4"), groups[1].items.map { it.label })
    }

    @Test
    fun bareCustomNameOmitsDuplicateHeader() {
        val groups = groupModelsByFamily(listOf(model("mymodel")))
        assertNull(groups.single().label)
        assertEquals(listOf("mymodel"), groups.single().items.map { it.label })
    }

    @Test
    fun emptyCatalogProducesNoGroups() {
        assertEquals(emptyList<ModelFamilyGroup<ModelInfo>>(), groupModelsByFamily(emptyList()))
    }

    @Test
    fun genericGroupingWorksWithArbitraryEntryType() {
        // 远程下载 sheet 直接对 RemoteModel 分组：同一分类法按名字提取器生效。
        data class Named(val name: String)

        val groups = groupByFamilyPrefix(
            listOf(Named("Real-CUGAN-pro-denoise-x3"), Named("FooBar-x2"))
        ) { it.name }
        assertEquals(listOf("Real-CUGAN pro", "FooBar"), groups.map { it.label })
        assertEquals(listOf("denoise-x3", "x2"), groups.flatMap { group -> group.items.map { it.label } })
    }
}
