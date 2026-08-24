# CrossScale Compose UI 审计报告（audit-ui 节点产物）

> ⚠️ **并发漂移警告**：审计期间（05:50Z 检测）另一 agent 正在并行修改本目录——13 个 UI 文件已有未提交改动（+944/−391）：AdaptiveNavigation、BottomNavigationBar、LogViewer、ModelCard、ProcessingProgress、RemoteModelsDialog、QRScannerScreen、ConnectionScreen、HelpScreen、LogsScreen、ModelsScreen、SettingsScreen、BottomActionBar、HeroArea。**本报告的 file:line 证据对应我读取时的快照（改动前基线）**；上述文件的具体行号与个别条目（如 ModelsScreen/SettingsScreen/HeroArea 大改区）需在改动落定后复核，跨切面结论（§3 表）预计仍方向成立。未列入上表的文件（theme/*、navigation/*、HomeScreen、ParameterCard、CheckpointSection、QueueSheet、MainActivity、CrossScaleApp、scanner 其余、input）在我读后未被修改。

日期: 2026-08-24 · 范围: `apps/app/src/main/java/com/crossscale/ui/**`（全部 31 文件）+ `MainActivity.kt` + `CrossScaleApp.kt` · 只读审计，未改任何代码。

---

## 1. 应用是什么 / 各屏职责

CrossScale 是一款 Android 图片超分辨率（Super-Resolution）工具：用手机本地 NCNN 推理跑放大模型，或在「严格 1:1」模式下通过加密 TCP 把任务分发给局域网内的 PC 协同处理；支持批量队列、断点续传、输出目录/格式配置。整体是深色优先的「专业影像工作台」气质。

| 屏幕 | 路由 | 职责 |
|---|---|---|
| HomeScreen (`ui/screens/HomeScreen.kt`) | home | 核心工作台：图片舞台（选图/文件夹）→ 参数卡（模型/倍率/输出）→ 断点续传卡 → 底部悬浮 Dock（开始 CTA + 恢复 + 队列入口）；处理中舞台遮罩进度；模型/格式选择底部弹层；批量队列 sheet |
| ConnectionScreen (`ui/screens/ConnectionScreen.kt`) | devices | 手机↔PC 配对：已连状态卡（地址/指纹/断开）、连接中 spinner、未连时扫码/手动输入入口；手动连接对话框、TOFU 指纹确认框 |
| SettingsScreen (`ui/screens/SettingsScreen.kt`) | settings | 分组卡设置列表（模型管理/日志/帮助、流加密开关、关于）；medium+ 转 MD3 list-detail 双栏，detail pane 内嵌子页 |
| ModelsScreen (`ui/screens/ModelsScreen.kt`) | settings/models | 模型库管理：本机/PC 双端卡片（上传/下载/删除）+ 远程（GitHub release）模型下载对话框 |
| LogsScreen (`ui/screens/LogsScreen.kt`) | settings/logs | 日志：级别 FilterChip 过滤、文本搜索、复制/清空、等宽查看器（长按复制单条） |
| HelpScreen (`ui/screens/HelpScreen.kt`) | help | 三步文字说明（极简到近乎空） |
| QRScannerScreen (`ui/scanner/QRScannerScreen.kt`) | （非导航，条件全屏） | 相机扫码配对：沉浸式系统栏、权限流程、zxing 解码 |

导航形态自适应：compact 底部 NavigationBar / medium Rail / expanded+ 常驻抽屉（`CrossScaleApp.kt` + `AdaptiveNavigation.kt`），页面边距经 `LocalScreenMargin` 下发。

### 先说好消息（骨架不差）

主题层相当完整：全套 M3 语义色板（Dark/Light 各 ~40 slot，`Color.kt`）、`MaterialExpressiveTheme` + Expressive MotionScheme（`Theme.kt`）、4dp 网格 Spacing token（`Spacing.kt`）、统一转场/按压动效工具（`Motion.kt` 的 `pressable`/`pressScale`/`smoothProgress`）。**全 UI 无一处硬编码 `Color(0x…)`**，暗色模式靠 colorScheme 角色天然成立。「垃圾感」不是来自缺规范，而是来自下面这些**执行不一致和细节锈蚀**。

---

## 2. 逐文件问题清单

约定：`文件:行号` 为证据；severity 🔴 高 / 🟡 中 / 🟢 低（打磨项）。

### 壳与主题

**MainActivity.kt**
- 🟢 无实质问题。edge-to-edge + Surface(background) 干净。

**CrossScaleApp.kt**
- 🟡 `ScannerMode.isActive` 全局可变单例直连 shell 布局分支（L39-45）：隐藏耦合，扫码页的布局契约散落在两处。
- 🟢 compact 分支 `WindowInsets(0,0,0,0)` + 注释解释了 insets 收口策略——正确但极脆弱，任何新屏忘记自带 Scaffold 就会顶到状态栏（无机制防护）。

**ui/theme/Theme.kt**
- 🟡 L103-108：Android 12+ 一律走动态取色，精心调的 Studio Dark 品牌 Teal/板岩色板在主流设备上**永远不可见**（成死代码）。若品牌感是诉求，这是第一刀该决策的地方。
- 🟢 L124-125 注释自述「不再注入品牌化 CrossScaleShapes」——形状完全落回 M3 基线，无品牌形状语言。

**ui/theme/Color.kt**
- 🟢 L72 与 L80：`LightBackground` 与 `LightSurfaceContainerLow` 同值 `0xFFF5FAF8`，浅色下卡与背景零对比（依赖 outline 才能分层面）。

**ui/theme/Type.kt**
- 🟡 定义了完整 display/headline 阶梯（L12-53）但**全 UI 无一使用**；实际只用到 title/body/label——字阶名义齐全、实际只用一半，「大字号层级」从未被表达。
- 🟡 L138 `valueStyleFor()` 是唯一权威的「值字体」启发式，但组件里另有 4 处私有变体（见 ModelCard/ProcessingProgress/ParameterCard/HomeScreen）。

**ui/theme/Spacing.kt**
- 🟢 缺 `lg`~`xxl` 之间的档（如 28.dp），导致 AdaptiveNavigation 里出现裸 `28.dp`。

**ui/theme/Motion.kt**
- 🔴 **注释与代码系统性失同步**（正是「细节锈蚀」实锤）：
  - L49-50 注释「alpha（200ms）」，实际 `EnterTween = 140`（L39）；
  - L52-53 注释「高度展开（250ms）」，实际 `ExpandTween = 180`（L40）。
  - 下游注释跟着错：HomeScreen.kt:275「200ms ease-out」、CheckpointSection.kt:65「250ms ease-out」。
- 🟢 L29 自认无法做 reduced-motion 降级（平台限制，诚实）。

### 导航

**ui/navigation/Screen.kt**
- 🟢 干净。仅 Help/子页无 selectedIcon（合理）。

**ui/navigation/Navigation.kt**
- 🟡 HomeScreen 调用点 **40+ 参数回调墙**（L130-221）：接线集中是有意设计（见 L46-51 文档），但已是改动高危区。
- 🟡 L73-77 + L260-263：`isStreamEncrypted` 用 `rememberSaveable` 复制 SharedPreferences 的真值——同一设置存在两个真相源，别处写入不会回流。
- 🟡 L107-127：checkpoint 相关两个 `LaunchedEffect`（backup 可用性、`lastClearedTaskId` 消费一次清 hero 图）逻辑精细，重写必碎。
- 🟢 `DestinationContainer`（L341-350）每目的地重复铺 background。

### 组件

**ui/components/AppTopBar.kt**
- 🟡 `CrossScaleTopAppBar` 的 `scrollBehavior` 参数无人使用——三个主 tab 屏滚动时顶栏**永不变色/无 elevation 反馈**，是「平、死」观感的直接来源之一。
- 🟢 `SecondaryScreenScaffold` 与各屏自己写的 `Box(contentAlignment=TopCenter)+widthIn(MaxContentWidth)` 是同一套限宽样板的两份拷贝。

**ui/components/BottomNavigationBar.kt**
- 🟢 干净（TalkBack 双报已处理，L29）。

**ui/components/AdaptiveNavigation.kt**
- 🟡 抽屉头部裸魔法数：`28.dp`(L119,121)、`16.dp`(L127)、`2.dp`(L141)，绕过 Spacing。
- 🟢 品牌区只有一行 `titleSmall` 灰字应用名（L122-126），expanded 形态毫无品牌表现。

**ui/components/SectionHeader.kt**
- 🟢 干净，token 使用正确。

**ui/components/ConnectionDialog.kt**
- 🟡 错误反馈不对称：port 校验失败有 `supportingText`（L60-64），host 为空只有红框**无文案**（L42-51）。
- 🟢 默认端口 `"9527"` 硬编码字符串（L33）。

**ui/components/FingerprintConfirmDialog.kt**
- 🟢 干净（安全 TOFU 流程，文案居中有心思）。

**ui/components/LogViewer.kt**
- 🟡 `levelColor()`（L91-101）：WARNING/INFO/DEBUG 三级同色 `onSurfaceVariant`——日志分级过滤做了、颜色语义没做，WARNING 毫无警示感。
- 🟡 L57-65：`combinedClickable` 空 `onClick` 仍保留点击涟漪——点了没有任何反馈结果，死的交互暗示。
- 🟢 `LocalClipboardManager` 已废弃（应 `LocalClipboard`）。

**ui/components/ModelCard.kt**
- 🟡 L44 私造 `labelSmall.copy(Monospace)` 规格样式——第五处等宽变体（theme 有现成 `ValueMono`/`LogMonoStyle`）。
- 🟢 L52-57 不对称 padding（start lg / end sm / top-bottom md）：为行尾图标让位可以理解，但与 QueueItemCard 的纯 ListItem 节奏不同。

**ui/components/ProcessingProgress.kt**
- 🟡 L92 全限定名内联调用 `androidx.compose.material3.ButtonDefaults…`（非惯用 Compose 写法）。
- 🟡 L75、L84 两处 `ValueMono.copy(fontSize = …labelSmall.fontSize)` 私有变体。
- 🟢 L43 遮罩 `surface.copy(alpha = 0.55f)` 裸透明度，与其他 scrim（queue sheet 0.18f）无 token 关系。

**ui/components/RemoteModelsDialog.kt**
- 🟡 对话框标题行：加载时 spinner **替换**刷新按钮（L59-71）——同一位置控件跳变、宽度抖动。
- 🟡 进度条 `fillMaxWidth(0.35f)`（L169,183）裸分数；且此处进度条为默认粗高，QueueSheet 运行条却自定义 `2.dp`——同类元素两种规格。
- 🟢 L222-226 `formatSizeBytes` 放 UI 组件层（应为 util），`internal` 可见性被跨包共用。

### 主屏族

**ui/screens/HomeScreen.kt**
- 🔴 L234-290 整段缩进错乱（ImageStage/ParameterCard/AnimatedVisibility 比 Column 体浅一级）——格式即门面，这是最直观的「潦草」信号。
- 🟡 L442-445：`BottomDockClearance = 56+16+24`、`SnackbarAboveDockPadding = 56+16+12` 用加法表达式当 token——比裸魔法数进步，但仍是 Dock 高度的第二真相源（Dock 改高这里就烂）。
- 🟡 L394：仅队列 sheet 单独覆盖 `scrimColor.copy(alpha = 0.18f)`，同屏另外两个 sheet 用默认 scrim——同一交互三处两种遮罩浓度。
- 🟡 L166 vs L322/365：queue sheet 的 sheetState 提升为 val，model/format sheet 却在分支里内联 `rememberModalBottomSheetState`——不一致的生命周期姿势。
- 🟡 L503-507 连接状态色映射：CONNECTING 与 CONNECTED 同为 `primary`，状态点无法区分「正在连」与「已连」（仅文字不同）。
- 🟢 L471 `"$statusLabel $endpoint"` 手工拼串；L560-566 格式名常量混资源串。
- 🔴（行为红线）L171-189 连接态 snackbar 的 prev-state 比较、L193-199 fallback 消息先清后显——都是防重放的精细逻辑。

**ui/screens/home/HeroArea.kt**
- 🟡 空态两大分区用裸 `.clickable`（L188、L205），而缩略图/动作块用 `pressable`（带按压缩放）——**同一个舞台上两套按压手感**。
- 🟡 删除角标（L374-394）：视觉 22dp 圆 + `offset(3,-3)` 出血 + `outline` 强描边；相邻动作瓦片却是 `outlineVariant` 弱描边（L409）——描边权重语言不一。13.dp 的 Close 图标偏小。
- 🟡 空态斜线分割很有性格，但左右两区**只有图标没有文字标签**（可发现性差；「图片/文件夹」概念全靠猜）。
- 🟢 L98 舞台高度 180↔320 直切（有 animateContentSize 兜底）；L340/L406 72dp 瓦片尺寸在两个组件里重复声明。
- 🟢 L283、L400 全限定名内联（`PaddingValues`、`ImageVector`——后者明明 L53 已 import）。
- 🔴（行为红线）`produceState` 解码管线 + `settled` 双态防闪烁（L236-252 等）、`SlashSideShape` 几何（L146-169）、MIC 触控目标技巧（L376-385）。

**ui/screens/home/BottomActionBar.kt**
- 🟡 主 CTA 在处理中 `enabled=false` 且同时显示 spinner（L62-81）——禁用态 + 加载态双信号叠加；而遮罩层里已有「取消」。CTA 变灰与遮罩取消按钮功能重叠（冗余 chrome）。
- 🟢 `HorizontalFloatingToolbar`（Expressive）是好方向；`56.dp`/`22.dp` 裸值绕过 token。

**ui/screens/home/ParameterCard.kt**
- 🟡 L278-283 内联复刻 `valueStyleFor` 启发式（`any { it.code > 127 }`）——theme 已有同名函数却不用，行为还可能漂移（>0x7F vs >127 此处恰好等价）。
- 🟡 并排两行的标题风格不一致：「模型」行用 `labelMedium` 小帽题（L131），「输出路径」行用 `bodyLarge` 正题（L215）——同级信息两种层级。
- 🟡 行内节奏不一：模型行 `vertical = md`(L116)、输出行 `vertical = sm`(L200)；「使用默认输出」TextButton 左对齐悬在卡内（L235-240），打破卡片对齐网格。
- 🔴（行为红线）整行 `toggleable(Role.Switch)` 模式、overwriteOriginal→格式行禁用链、backupOriginal `enabled = overwriteOriginal` 依赖（L296-314）。

**ui/screens/home/CheckpointSection.kt**
- 🔴 L134 `IconButton(Modifier.size(32.dp))`、L167/L179 `size(28.dp)`——**亲手打穿 MD3 48dp 最小触控目标**；而 QueueSheet.kt:444 的注释明确写着「不覆盖尺寸……保证 48dp」。同一团队两条规矩打架，这是最典型的「不一致」证据。
- 🟡 动作行混装：FilledTonalButton + 2×IconButton + weight Spacer + 展开 IconButton（L103-146），主次不清。
- 🟢 卡片 `shapes.medium`，与隔壁 ParameterCard `shapes.large` 不一致（同屏两种卡半径）。

**ui/screens/home/QueueSheet.kt**
- 🟡 L427-431 运行中进度条强制 `height(2.dp)`，与他处默认轨道粗细不符。
- 🟡 L379-388 「只重试失败」TextButton 挤压 `contentPadding(horizontal = sm)`，触感局促。
- 🟢 状态胶囊 `CircleShape`（L483）与首页顶栏的状态圆点（HomeScreen L522-527）是两套状态视觉语言。
- 🔴（行为红线）SwipeToDismiss 按 RUNNING 门控（L255-265）、`rememberAllocationSummary` 十二键缓存（L537-568）、`inputSourceLabel` 递归解析（L570-587）、`animateItem` + key。

### 其余屏

**ui/screens/ConnectionScreen.kt**
- 🔴 L101-130、L143 缩进再次错乱（错误横幅块与 `when` 块半悬空缩进）。
- 🟡 错误横幅里的「忽略」是 errorContainer 上的 TextButton（L125-127）——容器底色上放低强调按钮，对比度与点击欲都弱；与 snackbar 的 dismissal 手势语言也不同。
- 🟢 `StatusCardScaffold` 居中骨架不错；扫码/手动 = FilledTonal+Outlined 组合，但主操作强调级别与 Home（Filled）/Checkpoint（Tonal）三屏三个样。

**ui/screens/ModelsScreen.kt**
- 🟡 L83-107 三按钮排：前两个带字，第三个（远程下载）纯图标 OutlinedButton——同排按钮信息密度突变，且 icon-only 挤在文字按钮行里很怪。
- 🟡 L77-113 两段说明性 `bodySmall` 灰字裸排在列表上方（含长目录路径，无省略/mono 处理）——信息层级扁平。
- 🟡 L67-69 与 HomeScreen L315-317 的「远程列表为空才自动拉取」逻辑重复实现。
- 🔴（行为红线）`deleteTarget` 登记式确认删除（L63-64,175-196）。

**ui/screens/SettingsScreen.kt**
- 🟡 `SettingsEntryRow`（L322-375）手搓 Row 复刻 ListItem——ParameterCard 文档声称「行组件全部使用 M3 官方 ListItem 以获得正确字阶/高度/内边距」，设置页却整套手搓：同一 app 两套行组件体系。
- 🟡 流加密开关行（L241-273）：`Switch(onCheckedChange=…)` 直接挂开关，**没有** ParameterCard 的整行 `toggleable(Role.Switch)` 无障碍模式——同 app 两套开关行范式。
- 🟢 L295 全限定 `android.net.Uri.parse`；L286 版本 fallback `"-"`；`PROJECT_URL` 常量埋在 UI 层（L304）。

**ui/screens/LogsScreen.kt**
- 🟡 控件顺序反常规：清除/复制按钮在最上（L85-101），搜索框和过滤 chips 反而在其下（L108-131）——工具栏惯例应是「筛选在前、动作殿后」，现在高频动线（搜索）被推到中段。
- 🟡 FilterChip 直接渲染枚举名 `level.name`（L128）——中文界面里冒出 DEBUG/INFO/WARNING 英文原词（若有意保留术语应进 strings）。
- 🟢 空日志无空态文案，只剩一个空容器。
- 🟢 `Surface(shapes.extraSmall)` 日志容器半径(4dp)是刻意的「控制台」质感，可与卡片族区分——但无注释说明是有意的。
- 🔴（行为红线）ordinal 序列化方案（L46-50）、`asReversed` 免排序（L59-75）。

**ui/screens/HelpScreen.kt**
- 🔴 全 app 最破的一屏：3 行 `"${index+1}. 正文"` 裸文本（L28-38）。无卡片、无图标、无步骤视觉、无分组——与全 app 的卡片语言完全脱节，「没做完」的观感主要来源之一。

### scanner / input

**ui/scanner/QRScannerScreen.kt**
- 🔴 L186 注释承诺「scrim 遮罩 + 顶部关闭按钮 + **中央取景框** + 底部提示」，实际代码**既无关闭按钮也无取景框**——文档谎言级失配；扫码无取景框引导也伤成功率。
- 🟡 错误提示用 `CircleShape` 胶囊承载可能换行的长错误串（L254-266）——多行文本塞胶囊，圆角语义崩坏。
- 🟡 L270 提示文字 `inverseOnSurface` 打在**不可预测的相机画面**上——角色正确但对比度无保证。
- 🟡 L149、L170 代码内拼中文全角括号 `"（$raw）"`——应走资源格式串。
- 🔴（行为红线）相机生命周期全套：analyzer 背压双 AtomicBoolean（L128-156）、unbind 时序（L176-183）、系统栏隐藏/恢复（L104-114）、权限永久拒绝兜底（L225-240）。

**ui/scanner/ScannerMode.kt** —— 🟢 极简可用；全局单例是测试性味道（见 CrossScaleApp 条）。

**ui/scanner/QrPayloadDecoder.kt** —— ✅ 纯算法，旋转矩阵四个方向推导正确。不要碰。

**ui/input/InputSelectionController.kt**
- 🟢 L262-298 每次重组重新赋值 5 个 lateinit launcher（无害但难看）；日志 tag 仍是迁移前的 `"CrossScaleApp"`（L81 等，类已搬走）。
- 🔴（行为红线）写权限与队列启动的原子性（`pendingQueueStartBatchId`，L210-225）、IO 线程授权持久化+解码探测（L96-103）、自动入队副作用——全部不许动。

---

## 3. 跨文件不一致总表

| # | 维度 | 观察到的变体 | 证据举例 |
|---|---|---|---|
| 1 | 卡片圆角 | **3 种**：large(16) 参数卡/连接状态卡/设置分组卡；medium(12) ModelCard/断点卡/队列项卡；extraSmall(4) 日志容器 | ParameterCard.kt:104 vs CheckpointSection.kt:67 vs LogsScreen.kt:135 |
| 2 | 主操作按钮强调 | **3 级漂移**：Filled（Home CTA、2 个对话框确认、扫码授权）；FilledTonal（断开/续传/刷新/导入/队列启停/扫二维码）；Outlined（手动输入/复制/清除/打开设置）——无角色分配表，同级操作跨屏强调忽高忽低 | BottomActionBar:62 / CheckpointSection:107 / ModelsScreen:84,92 / LogsScreen:89,92 / ConnectionScreen:253,262 |
| 3 | 按钮排里的 icon-only | **3 种姿势**：Models 第三钮 icon-only OutlinedButton；Checkpoint Tonal+双 icon；QueueSheet 头部 Tonal+双 icon+close | ModelsScreen:100 / CheckpointSection:110-128 / QueueSheet:150-167 |
| 4 | 列表行组件 | **2 套体系**：官方 ListItem（参数卡高级区/队列项/选择 sheet）vs 手搓 Row（设置页全部行、参数卡模型/输出两行、顶栏连接状态） | ParameterCard:244 vs SettingsScreen:322 |
| 5 | 开关行无障碍范式 | **2 种**：整行 `toggleable(Role.Switch)`+指示器（参数卡）vs 裸 `Switch(onCheckedChange)`（设置页） | ParameterCard:344 vs SettingsScreen:269 |
| 6 | 按压反馈 | **2 种手感**：`pressable`（缩放+涟漪，HeroArea/参数卡/队列卡）vs 裸 clickable（空态分区/删除角标/选择 sheet 项/设置行/日志行） | HeroArea:346 vs HeroArea:188, HomeScreen:474, SettingsScreen:335 |
| 7 | 等宽「值字体」变体 | **5 种**：ValueMono / LogMonoStyle / labelSmall+Monospace 私搭(ModelCard) / ValueMono.copy(size)(ProcessingProgress×2) / 内联 >127 启发式(ParameterCard) | Type.kt:120,125 / ModelCard:44 / ProcessingProgress:75,84 / ParameterCard:278 |
| 8 | 进度指示规格 | **4 种**：默认高 Linear(RemoteModelsDialog/StageOverlay)、2.dp 定制(QueueSheet:430)、stroke 2.5 spinner(BottomActionBar:70, ConnectionScreen:186)、20dp spinner(ModelCard/RemoteDialog) | — |
| 9 | 状态可视化语言 | **3 种**：彩点+文字(顶栏)、容器色胶囊(队列)、纯文字/双端小图标(断点卡/ModelCard) | HomeScreen:517 / QueueSheet:457 / CheckpointSection:85 |
| 10 | Sheet 头部构造 | **3 种**：titleMedium+右侧动作(模型 sheet)、纯 titleMedium(格式 sheet)、自制统计头+3 动作(队列 sheet) | HomeScreen:326,369 / QueueSheet:109 |
| 11 | Scrim 浓度 | 默认 scrim vs 0.18f(仅队列 sheet) vs 0.55f 表面遮罩(处理遮罩) | HomeScreen:394 / ProcessingProgress:43 |
| 12 | IconButton 尺寸纪律 | 默认 48dp(QueueItemAction 明文守规矩) vs 32dp/28dp 强改(CheckpointSection 打穿触控目标) | QueueSheet:444 vs CheckpointSection:134,167,179 |
| 13 | 注释与代码失同步 | Motion 时长(140≠"200"、180≠"250")、扫码页「取景框+关闭按钮」不存在、下游多处转场毫秒注释跟着错 | Motion.kt:39-56 / QRScannerScreen:186 |
| 14 | 代码卫生 | 内联全限定名 ×9（PaddingValues/ImageVector/ButtonDefaults/Uri/数据模型…）；两文件大段缩进错乱 | §2 各条 |
| 15 | （正面）颜色纪律 | **0 处** `Color(0x…)` 硬编码于 theme 外；暗色全覆盖 | grep 全量验证 |

---

## 4. 重写安全性分类

### 🟢 可放手重排/重绘（纯视觉，契约不变）
| 文件 | 理由与注意 |
|---|---|
| HelpScreen.kt | 全静态；怎么重做都不破坏行为 |
| SettingsScreen.kt 的 EntryRow/GroupCard/开关行 | 换 ListItem/toggleable 即可；保留 `onStreamEncryptedChange` 回调签名与 `PROJECT_URL` 跳转 try-catch |
| LogsScreen.kt 布局序/Chip 文案/空态 | **保留** ordinal 序列化与 `asReversed` 过滤管线 |
| ModelsScreen.kt 按钮排/说明排版 | **保留** deleteTarget 确认流、LaunchedEffect 拉取条件 |
| ModelCard.kt 内距/规格样式 | props 与 busy/disable 语义不变 |
| RemoteModelsDialog.kt 标题行/进度宽 | 四态渲染分支（installed/downloading/failed/idle）逐支保留 |
| ProcessingProgress.kt 样式 | 保留 `task ?: return` 早退与 lambda 进度 API |
| AppTopBar.kt / SecondaryScreenScaffold | 可统一限宽样板、接 scrollBehavior |
| Motion.kt | 先修注释（或反向把常量改成注释值——二选一对齐） |
| HomeScreen.kt 缩进修复 + sheet 头统一 | 纯格式/样式；showX 状态机不动 |
| ConnectionScreen.kt 缩进 + 错误横幅重绘 | 保留 connectionState 三态分支与两对话框接线 |

### 🔴 有精巧逻辑，动皮不动骨（state/callbacks/VM 契约绝不能变）
| 文件 | 必须保全的逻辑 |
|---|---|
| Navigation.kt | VM factory 解析、自动重连、`lastClearedTaskId` 一次性清图、backup 可用性 effect、streamEncrypted 双源（重写时应收敛为单源，属行为变更需单独立项） |
| HomeScreen.kt | **40+ 参数签名冻结**；snackbar prev-state 防重放；availableScales 家族过滤；prettyOutputPath SAF 解析；queueCount 口径（PENDING+RUNNING） |
| HeroArea.kt | produceState/settled 解码管线、SlashSideShape 几何、MIC 触控技巧、解码目标 px 常量的缓存键耦合 |
| QueueSheet.kt | SwipeToDismiss RUNNING 门控、rememberAllocationSummary 缓存键全集、inputSourceLabel 递归、item key/animateItem |
| CheckpointSection.kt | discard 二次确认、take(10) 截断计数、canRetryFailedOnly 门控（修触控目标时只改 Modifier，不改回调） |
| ParameterCard.kt | toggleable 角色、overwriteOriginal 禁用链、AnimatedContent 联动 |
| QRScannerScreen.kt | 相机/analyzer/权限/系统栏全套生命周期（只允许在其 UI 层加取景框/关闭钮） |
| QrPayloadDecoder.kt | 像素算法，禁止触碰 |
| InputSelectionController.kt | 权限原子性、IO 调度、入队副作用、launcher 注册结构 |
| CrossScaleApp.kt / Theme.kt / Color.kt | 断点/insets 收口协议；是否退出动态取色是产品决策不是重构 |
| ConnectionDialog/FingerprintConfirmDialog | 校验规则与 TOFU 安全流程文案行为 |

---

## 5. 面向「简洁/自然/大方/优雅」的收敛建议（供后续节点排期）

1. **一张「角色表」定死按钮语言**：Filled=每屏唯一主动作；FilledTonal=次级实心；Outlined=第三级；icon-only 一律 IconButton（不再出现在文字按钮排里）。
2. **一种卡片**：容器卡统一 `shapes.large` + `surfaceContainer`；日志控制台可留 extraSmall 但要注释立此存照。修 CheckpointSection 的 32/28dp IconButton。
3. **一种行**：全 app 收敛到 M3 ListItem（删 SettingsEntryRow 手搓版），开关行统一 toggleable 模式；参数卡两行标题层级拉齐。
4. **一种按压手感**：裸 clickable 全部过 `pressable`；空态斜线分区补文字标签。
5. **品牌决策**：A12+ 动态取色 vs Studio Dark 色板二选一（当前品牌色基本白做）。
6. **细节除锈**：修两处缩进、9 处内联全限定名、Motion 注释失同步、扫码页取景框+关闭按钮兑现、WARNING 日志给 tertiary 色、HelpScreen 重做成步骤卡。
