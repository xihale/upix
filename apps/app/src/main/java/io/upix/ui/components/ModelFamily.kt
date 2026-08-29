package io.upix.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import io.upix.data.model.ModelInfo
import io.upix.ui.theme.Spacing
import io.upix.ui.theme.valueStyleFor

/**
 * 模型家族分组：模型选择 sheet 与远程下载 sheet 共用的目录组织方式。
 * 已知家族按固定顺序成组（轻量推荐系在前），行名剥掉家族前缀只留短名。
 */

/** 已知家族前缀 → 分组标题；列表顺序即卡片顺序。 */
private val KnownModelFamilies = listOf(
    "Real-ESRGANv3" to "Real-ESRGAN v3",
    "Real-ESRGAN-x4plus" to "Real-ESRGAN x4plus",
    "Real-CUGAN-pro" to "Real-CUGAN pro",
    "Real-CUGAN-se" to "Real-CUGAN se"
)

/** 分组内单行：[value] 是回传的完整条目，[label] 是去家族前缀的行短名。 */
internal data class FamilyEntry<T>(val value: T, val label: String)

/** 一个家族分组：[label] 为 null 时不渲染分区标题（如无倍率后缀的自定义裸名）。 */
internal data class ModelFamilyGroup<T>(val label: String?, val items: List<FamilyEntry<T>>)

/** ModelInfo 便捷重载：本地模型库（模型选择 sheet）按家族分组。 */
internal fun groupModelsByFamily(models: List<ModelInfo>): List<ModelFamilyGroup<ModelInfo>> =
    groupByFamilyPrefix(models) { it.name }

/**
 * 按名字做家族分组，条目类型无关（本地 ModelInfo 与远程 RemoteModel 共用）：
 * 已知前缀命中已知组（顺序固定），其余（导入的自定义模型）按去倍率词干——
 * 名字去掉 -xN 后缀，与参数卡倍率系列的推导一致——各自成组殿后，词干与
 * 行名重复的裸名组不出标题。
 */
internal fun <T> groupByFamilyPrefix(items: List<T>, nameOf: (T) -> String): List<ModelFamilyGroup<T>> {
    val sorted = items.sortedBy(nameOf)
    val groups = mutableListOf<ModelFamilyGroup<T>>()
    val assigned = mutableSetOf<String>()
    for ((prefix, label) in KnownModelFamilies) {
        val members = sorted.filter { value ->
            val name = nameOf(value)
            name.startsWith("$prefix-") || name == prefix
        }
        if (members.isEmpty()) {
            continue
        }
        members.forEach { assigned += nameOf(it) }
        groups += ModelFamilyGroup(
            label = label,
            items = members.map { value ->
                FamilyEntry(value = value, label = stripFamilyPrefix(nameOf(value), prefix))
            }
        )
    }
    val custom = sorted
        .filter { nameOf(it) !in assigned }
        .groupBy { value ->
            nameOf(value).substringBeforeLast("-x", missingDelimiterValue = nameOf(value))
        }
    for ((stem, members) in custom) {
        groups += ModelFamilyGroup(
            label = stem.takeIf { members.any { value -> nameOf(value) != stem } },
            items = members.map { value ->
                FamilyEntry(
                    value = value,
                    label = nameOf(value).removePrefix("$stem-").ifEmpty { nameOf(value) }
                )
            }
        )
    }
    return groups
}

/** 去家族前缀得行短名；名字恰为前缀本体（如 Real-ESRGAN-x4plus）时取末段。 */
private fun stripFamilyPrefix(name: String, prefix: String): String {
    val withDash = "$prefix-"
    if (name.startsWith(withDash)) {
        return name.removePrefix(withDash)
    }
    return name.substringAfterLast('-')
}

/**
 * 家族分组卡：surfaceContainer medium 卡，行贴卡片边缘（无内边距）——
 * 选中行的染色通铺到卡边、由卡片裁角，单项家族整体读作一张选中卡，
 * 而非卡中嵌套圆角药丸。模型选择 sheet 与远程下载 sheet 共用。
 */
@Composable
internal fun ModelFamilyCard(content: @Composable ColumnScope.() -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(content = content)
    }
}

/**
 * 模型行基座：透明 ListItem，贴分组卡边缘，不做逐行裁剪——选中行的
 * surfaceContainerHighest 染色通铺整行、由卡片形状裁角（分组卡内的浮动
 * 控件色）。前置固定 20dp 槽位保证行名对齐，勾仅在选中时画出。行名走
 * §2.1 机值槽（valueStyleFor → ValueMono）。[isSelected] 补 selected
 * 无障碍语义——染色本身不进无障碍树；[onClick] 为空时行不可点（如下载行）。
 */
@Composable
internal fun ModelListRow(
    label: String,
    isSelected: Boolean = false,
    onClick: (() -> Unit)? = null,
    supporting: (@Composable () -> Unit)? = null,
    trailing: @Composable () -> Unit
) {
    ListItem(
        supportingContent = supporting,
        leadingContent = {
            if (isSelected) {
                Icon(
                    imageVector = Icons.Outlined.Check,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.size(20.dp)
                )
            } else {
                Spacer(modifier = Modifier.size(20.dp))
            }
        },
        trailingContent = trailing,
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier
            .fillMaxWidth()
            .background(
                if (isSelected) {
                    MaterialTheme.colorScheme.surfaceContainerHighest
                } else {
                    Color.Transparent
                }
            )
            .then(
                if (onClick != null) {
                    Modifier.clickable(role = Role.RadioButton, onClick = onClick)
                } else {
                    Modifier
                }
            )
            .then(if (isSelected) Modifier.semantics { selected = true } else Modifier)
    ) {
        Text(
            text = label,
            style = valueStyleFor(label),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

/** 分组列表空态（§9.9 配方）：安静卡片 + 24dp 弱色图标 + 一句话。 */
@Composable
internal fun ModelFamilyEmptyCard(text: String) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainer
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = Spacing.xxl),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(Spacing.sm)
        ) {
            Icon(
                imageVector = Icons.Outlined.Memory,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(24.dp)
            )
            Text(
                text = text,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}
