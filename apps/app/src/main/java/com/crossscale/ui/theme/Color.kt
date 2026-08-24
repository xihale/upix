package com.crossscale.ui.theme

import androidx.compose.ui.graphics.Color

/**
 * CrossScale 规范色板（normative，docs/DESIGN_LANGUAGE.md §1）。
 * 品牌锚点：Teal #4C9D9A + 深板岩 #0F172A（启动图标）。
 * 深色为主表达（D2：界面退后，图片为主角），浅色为同族推导。
 * 本文件是唯一允许出现 Color(0x…) 字面量的地方；hex 与 spec §1 逐字一致，禁止分叉。
 *
 * 用法纪律（spec D3 / §1.3）：
 * - 单一强调色：primary 只出现在 CTA、进度指示、连接状态点/文本、Running 状态 chip。
 * - tertiary* 为保留槽位：在 scheme 中定义（防止组件回退库默认值），但 app 代码禁止引用。
 * - surfaceBright/Dim、inverse* 仅主题内部使用，app 代码不得直接引用。
 * - 全 app 唯一允许的半透明：舞台遮罩 surface@55%、sheet scrim scrim@18%。
 *
 * Dark/Light 前缀仅用于 Theme.kt 组装 colorScheme。
 */

// ---------- Dark（主表达 / canonical） ----------
val DarkPrimary = Color(0xFF7FDAC9)
val DarkOnPrimary = Color(0xFF00382F)
val DarkPrimaryContainer = Color(0xFF0E4A42)
val DarkOnPrimaryContainer = Color(0xFFA5F2E2)

val DarkSecondary = Color(0xFFAFC4DC)
val DarkOnSecondary = Color(0xFF1A2C42)
val DarkSecondaryContainer = Color(0xFF273951)
val DarkOnSecondaryContainer = Color(0xFFDCE7FA)

// 保留槽位（D3）：定义但不允许 app 代码使用。
val DarkTertiary = Color(0xFFE8C26B)
val DarkOnTertiary = Color(0xFF3B2D00)
val DarkTertiaryContainer = Color(0xFF564400)
val DarkOnTertiaryContainer = Color(0xFFFFE092)

val DarkBackground = Color(0xFF0A101E)
val DarkOnBackground = Color(0xFFE1E8F2)
val DarkSurface = Color(0xFF0F172A)
val DarkOnSurface = Color(0xFFDEE7F0)
val DarkSurfaceVariant = Color(0xFF1D2A40)
val DarkOnSurfaceVariant = Color(0xFFBAC6D6)

// 表面容器梯度即"层级"表达（D4：无阴影，靠色调分层）。
val DarkSurfaceContainerLowest = Color(0xFF0A101E)
val DarkSurfaceContainerLow = Color(0xFF121C2E)
val DarkSurfaceContainer = Color(0xFF162136)
val DarkSurfaceContainerHigh = Color(0xFF1C2740)
val DarkSurfaceContainerHighest = Color(0xFF22304C)

// 以下仅主题内部使用，app 代码不得引用。
val DarkSurfaceTint = DarkPrimary
val DarkSurfaceBright = Color(0xFF36415C)
val DarkSurfaceDim = Color(0xFF0A101E)

val DarkInverseSurface = Color(0xFFDEE7F0)
val DarkInverseOnSurface = Color(0xFF131C2E)
val DarkInversePrimary = Color(0xFF0E6B5F)

val DarkOutline = Color(0xFF44536B)          // 交给 M3 组件内部（TextField 焦点等）
val DarkOutlineVariant = Color(0xFF2B3850)   // app 内 hairline 只用这个

val DarkError = Color(0xFFFFB4AB)
val DarkOnError = Color(0xFF690005)
val DarkErrorContainer = Color(0xFF93000A)
val DarkOnErrorContainer = Color(0xFFFFDAD6)

// ---------- Light ----------
val LightPrimary = Color(0xFF0E6B5F)
val LightOnPrimary = Color(0xFFFFFFFF)
val LightPrimaryContainer = Color(0xFFA4F2E4)
val LightOnPrimaryContainer = Color(0xFF00201B)

val LightSecondary = Color(0xFF445C77)
val LightOnSecondary = Color(0xFFFFFFFF)
val LightSecondaryContainer = Color(0xFFD5E4F9)
val LightOnSecondaryContainer = Color(0xFF001B2E)

// 保留槽位（D3）：定义但不允许 app 代码使用。
val LightTertiary = Color(0xFF875F00)
val LightOnTertiary = Color(0xFFFFFFFF)
val LightTertiaryContainer = Color(0xFFFFDF9E)
val LightOnTertiaryContainer = Color(0xFF291A00)

val LightBackground = Color(0xFFF5FAF8)
val LightOnBackground = Color(0xFF171D1B)
val LightSurface = Color(0xFFFFFFFF)
val LightOnSurface = Color(0xFF1A2120)
val LightSurfaceVariant = Color(0xFFDDE5E3)
val LightOnSurfaceVariant = Color(0xFF404947)

// 表面容器梯度即"层级"表达（D4：无阴影，靠色调分层）。
val LightSurfaceContainerLowest = Color(0xFFF0F5F3)
val LightSurfaceContainerLow = Color(0xFFF5FAF8)
val LightSurfaceContainer = Color(0xFFEFF5F3)
val LightSurfaceContainerHigh = Color(0xFFE9EFED)
val LightSurfaceContainerHighest = Color(0xFFE3EAE8)

// 以下仅主题内部使用，app 代码不得引用。
val LightSurfaceTint = LightPrimary
val LightSurfaceBright = Color(0xFFF9FDFB)
val LightSurfaceDim = Color(0xFFD6DBD9)

val LightInverseSurface = Color(0xFF2F3130)
val LightInverseOnSurface = Color(0xFFF0F1EF)
val LightInversePrimary = Color(0xFFA5F2E2)

val LightOutline = Color(0xFF707877)          // 交给 M3 组件内部（TextField 焦点等）
val LightOutlineVariant = Color(0xFFC0C9C7)   // app 内 hairline 只用这个

val LightError = Color(0xFFBA1A1A)
val LightOnError = Color(0xFFFFFFFF)
val LightErrorContainer = Color(0xFFFFDAD6)
val LightOnErrorContainer = Color(0xFF410002)
