# CrossScale Design Language

**Status:** Normative spec for the Compose UI (`apps/app/src/main/java/com/crossscale/ui/**`).
**Aesthetic:** 简洁 · natural · 大方 · elegant — Apple-style restraint on a Material 3 expressive-lite base. Generous whitespace, fewer borders, one accent, zero decorative noise.
**Constraints:** Pure Compose Material3 (`material3:1.5.0-alpha17`) + `material-icons-extended`. No new dependencies. Every number below is copy-paste ready.

---

## 0. Principles (operationalized)

1. **Images are the protagonist.** Chrome recedes: dark-neutral canvases, flat cards, no shadows competing with photos.
2. **One accent.** Teal (`primary`) marks exactly one thing per region: the CTA, or the active state. Never decoration.
3. **Depth by tone, not shadow.** Elevation is expressed through the `surfaceContainer` ramp and alpha scrims. No drop shadows in app code.
4. **Quiet information.** Metadata is small, muted (`onSurfaceVariant`), often monospace. Color is never the only signal and rarely the first.
5. **Fewer edges.** Cards have no borders. Hairlines exist only where content touches imagery.
6. **Mechanical consistency beats local cleverness.** If a token exists below, using anything else is a bug.

### Binding decisions (D1–D7)

| ID | Decision |
|----|----------|
| D1 | **Dynamic color is removed.** Delete the `dynamicDark/LightColorScheme` branch in `Theme.kt`. Wallpaper-derived palettes produce multi-hue schemes that violate the single-accent rule; the shipped palette is authoritative on every device. |
| D2 | **Dark-first, light parity.** Dark is the canonical expression (photo workbench); light mode uses the derived family in §2 with identical roles. |
| D3 | **Single accent discipline.** `primary` = teal, sparingly (§2.3). `secondary*` survives only as the nav-selection tint and the "queued" status chip. `tertiary*` is **reserved**: defined in the scheme (so components never fall back to library defaults) but forbidden in app code. |
| D4 | **No shadows.** Depth = `surfaceContainerLowest…Highest` ramp + scrims. The only permitted shadow is the one built into M3 `HorizontalFloatingToolbar`. |
| D5 | **Radius unification.** Content cards are always `shapes.medium` = **16dp**. Only the hero stage is larger. See §5. |
| D6 | **Icon system.** Material `Outlined` set everywhere; `Filled` variants only for selected bottom-nav state and the primary play CTA. |
| D7 | **Hairlines.** App code draws borders only with `outlineVariant`, `1.dp`, only in the three sanctioned places (§6). The stronger `outline` color is left to M3 component internals (TextField focus, SegmentedButton stroke). |

---

## 1. Color

Source of truth: `ui/theme/Color.kt` (hex values below are copied verbatim from it; do not fork them). Assembly: `ui/theme/Theme.kt`.

### 1.1 Dark (canonical)

| Slot | Hex | Slot | Hex |
|---|---|---|---|
| primary | `#7FDAC9` | background | `#0A101E` |
| onPrimary | `#00382F` | onBackground | `#E1E8F2` |
| primaryContainer | `#0E4A42` | surface | `#0F172A` |
| onPrimaryContainer | `#A5F2E2` | onSurface | `#DEE7F0` |
| secondary | `#AFC4DC` | surfaceVariant | `#1D2A40` |
| onSecondary | `#1A2C42` | onSurfaceVariant | `#BAC6D6` |
| secondaryContainer | `#273951` | surfaceContainerLowest | `#0A101E` |
| onSecondaryContainer | `#DCE7FA` | surfaceContainerLow | `#121C2E` |
| tertiary *(reserved)* | `#E8C26B` | surfaceContainer | `#162136` |
| onTertiary | `#3B2D00` | surfaceContainerHigh | `#1C2740` |
| tertiaryContainer *(reserved)* | `#564400` | surfaceContainerHighest | `#22304C` |
| onTertiaryContainer | `#FFE092` | outline | `#44536B` |
| error | `#FFB4AB` | outlineVariant | `#2B3850` |
| onError | `#690005` | inverseSurface | `#DEE7F0` |
| errorContainer | `#93000A` | inverseOnSurface | `#131C2E` |
| onErrorContainer | `#FFDAD6` | inversePrimary | `#0E6B5F` |

(`surfaceTint` = primary, `surfaceBright` `#36415C`, `surfaceDim` `#0A101E`: theme-internal only, never referenced by app code.)

### 1.2 Light

| Slot | Hex | Slot | Hex |
|---|---|---|---|
| primary | `#0E6B5F` | background | `#F5FAF8` |
| onPrimary | `#FFFFFF` | onBackground | `#171D1B` |
| primaryContainer | `#A4F2E4` | surface | `#FFFFFF` |
| onPrimaryContainer | `#00201B` | onSurface | `#1A2120` |
| secondary | `#445C77` | surfaceVariant | `#DDE5E3` |
| onSecondary | `#FFFFFF` | onSurfaceVariant | `#404947` |
| secondaryContainer | `#D5E4F9` | surfaceContainerLowest | `#F0F5F3` |
| onSecondaryContainer | `#001B2E` | surfaceContainerLow | `#F5FAF8` |
| tertiary *(reserved)* | `#875F00` | surfaceContainer | `#EFF5F3` |
| onTertiary | `#FFFFFF` | surfaceContainerHigh | `#E9EFED` |
| tertiaryContainer *(reserved)* | `#FFDF9E` | surfaceContainerHighest | `#E3EAE8` |
| onTertiaryContainer | `#291A00` | outline | `#707877` |
| error | `#BA1A1A` | outlineVariant | `#C0C9C7` |
| onError | `#FFFFFF` | inverseSurface | `#2F3130` |
| errorContainer | `#FFDAD6` | inverseOnSurface | `#F0F1EF` |
| onErrorContainer | `#410002` | inversePrimary | `#A5F2E2` |

### 1.3 Usage rules

**Canvas hierarchy (the "elevation" ramp):**

- `background` — screen canvas (Scaffold `containerColor`). Text on it: `onBackground`.
- `surface` — modal layers only: bottom sheets, dialogs.
- `surfaceContainerLowest` — recessed wells: additive action tiles (`StageActionTile`).
- `surfaceContainerLow` — quiet text wells (log console container).
- `surfaceContainer` — **the card color.** Every grouping card, banner, NavigationBar/Rail/Drawer chrome.
- `surfaceContainerHigh` — thumbnail placeholder wells behind loading images.
- `surfaceContainerHighest` — tiny floating controls over imagery (thumbnail delete circle).

**Text colors:** `onSurface` default; `onSurfaceVariant` for all supporting/metadata text and passive icons; `onBackground` only for text directly on the canvas. Never alpha-fade text — pick the muted slot instead.

**Accent (`primary`) appears only in:**
1. Filled Button (CTA) fill and its spinner.
2. Progress indicators (determinate bar, spinners).
3. `Switch` checked track, `SegmentedButton` selected stroke (M3 defaults — do not override).
4. Connection-status dot/text when CONNECTED/CONNECTING (top-bar indicator).
5. "Running" status chip container.

Everything else visible is neutral. If a new feature reaches for `primary`, it needs a spec amendment.

**Error** only for genuine failure/destructive: connection error banner, corrupted-file labels, failed chips, cancel-processing TextButton. Never decorative, never for "delete" icons sitting quietly in a row (those are `onSurfaceVariant` until confirmed).

**Status chip palette (only multi-container usage; from `QueueSheet`):**

| State | Container / content |
|---|---|
| Neutral / info | `surfaceVariant` / `onSurfaceVariant` |
| Running / active | `primaryContainer` / `onPrimaryContainer` |
| Queued / pending | `secondaryContainer` / `onSecondaryContainer` |
| Failed | `errorContainer` / `onErrorContainer` |

**Scrims (the only translucency in the app):** stage progress overlay `surface.copy(alpha = 0.55f)`; ModalBottomSheet scrim `scrim.copy(alpha = 0.18f)`.

---

## 2. Typography

Source of truth: `ui/theme/Type.kt`. System font family only (`FontFamily.Default`); monospace via `FontFamily.Monospace`. All slots are explicitly defined — never rely on library defaults. **Do not introduce custom fonts.**

### 2.1 Role → style mapping (this is the whole scale)

| Role | Style | Spec |
|---|---|---|
| Top bar title | `titleLarge` | 22/28, Regular |
| Card title | `titleMedium` | 16/24, Medium |
| Compact card title | `titleSmall` | 14/20, Medium |
| List-row headline | `bodyLarge` | 16/24, Regular |
| Body / sheet prose | `bodyMedium` | 14/20, Regular |
| Supporting / metadata / helper | `bodySmall` + `onSurfaceVariant` | 12/16 |
| In-card overline label (e.g. "模型设置") | `labelMedium` + `onSurfaceVariant` | 12/16, Medium |
| Chip text | `labelSmall` | 11/16, Medium |
| Section header | `SectionLabelStyle` | labelLarge (14/20, Medium) + letterSpacing **1.2sp**, `onSurfaceVariant` |
| Machine values (paths, scales, fingerprints, model/file names) | `ValueMono` via `valueStyleFor(text)` | titleSmall 14/20 Monospace |
| Log console body & per-entry header | `LogMonoStyle` | bodySmall 12/16 Monospace |
| CTA label | `titleMedium` | 16/24, Medium |
| Empty-state hint | `bodyMedium` + `onSurfaceVariant` | 14/20 |

### 2.2 Monospace + CJK fallback rule

Monospace has no CJK glyphs. Any string that may contain non-Latin characters goes through:

```kotlin
Text(text = value, style = valueStyleFor(value))   // falls back to titleSmall when non-ASCII present
```

Never hand-roll `.copy(fontFamily = FontFamily.Monospace)` at call sites.

> **Amendment (2026-08-24): per-atomic-value classification; separators are styling-neutral.**
> Rows that join several machine values with a decorative separator (e.g. ModelCard's
> `x2 · 2MB`, separator U+00B7) must classify via the vararg overload
> `valueStyleFor(part1, part2, …)` — one atomic value per argument — instead of passing the
> joined string. The separator itself never participates in the ASCII test: classifying the
> joined string would let the non-Latin separator alone force the whole row off the mono grid,
> contradicting the machine-value role in §2.1. If any atomic value contains non-Latin text,
> the whole row falls back to the system scale together.
>
> **Amendment (2026-08-24): LogViewer per-entry header mapped to `LogMonoStyle`.** The former
> 11/16 `labelSmall + Monospace` helper (`monoLabelStyle`, previously in `ui/components`) was
> an unsanctioned fourth mono token and has been removed. Header hierarchy inside a log entry
> is expressed with color only, not size.

### 2.3 Weights

Only Regular and Medium exist in this design. Bold/Black/SemiBold are forbidden. Emphasis = Medium + color, never size jumps.

Display/headline slots remain defined in `Type.kt` (baseline values) but are currently unused; they may not be adopted without a spec change.

---

## 3. Spacing & layout grid

Source of truth: `ui/theme/Spacing.kt`. Base grid **4dp**.

| Token | Value | Use |
|---|---|---|
| `Spacing.xs` | 4 | Chip vertical padding; micro gaps inside log entries |
| `Spacing.sm` | 8 | Between related controls; icon-to-text in tight groups; chip horizontal padding |
| `Spacing.md` | 12 | Default gap between sibling blocks *inside* a card; leading-icon-to-label in rows |
| `Spacing.lg` | 16 | **Card inner padding** (all sides); gap between sections on a screen; floating dock margins |
| `Spacing.xl` | 20 | Sheet content horizontal padding; preview sheet gutters |
| `Spacing.xxl` | 24 | Reserved (screen gutter on medium+ windows) |

**Layout invariants:**

- Screen gutter: `LocalScreenMargin.current` — 16dp compact / 24dp medium+ window class. Never hardcode.
- Content column max width: `MaxContentWidth = 840.dp`, top-centered.
- Vertical rhythm on a scrolling screen: `Arrangement.spacedBy(Spacing.lg)` between major sections; `md` inside cards.
- Touch targets ≥ 48dp: use `IconButton`, or `minimumInteractiveComponentSize()` when the visual element is smaller (see the 22dp delete circle).
- **Padding/gap values must come from `Spacing.*`.** Raw `.dp` is allowed only for intrinsic component *sizes* (Appendix A list).

---

## 4. Shape

Redefine once in `Theme.kt` (currently falls back to the M3 baseline 4/8/12/16/28 — too sharp for the target aesthetic):

```kotlin
shapes = Shapes(
    extraSmall = RoundedCornerShape(8.dp),   // log console wells, tiny inline code chips
    small      = RoundedCornerShape(12.dp),  // thumbnails, action tiles, media boxes
    medium     = RoundedCornerShape(16.dp),  // ALL grouping cards, banners, dialogs' inner groups
    large      = RoundedCornerShape(20.dp),  // hero image stage, large media frames
    extraLarge = RoundedCornerShape(24.dp),  // ModalBottomSheet, AlertDialog
)
```

**Mapping rule (exhaustive):**

| Element | Slot | dp |
|---|---|---|
| Grouping card / settings group / status card / empty-state panel / error banner / queue-item card | `shapes.medium` | 16 |
| Hero image stage, preview media frame | `shapes.large` | 20 |
| Thumbnails & 72dp action tiles | `shapes.small` | 12 |
| Log console well | `shapes.extraSmall` | 8 |
| Sheets & dialogs | `shapes.extraLarge` | 24 |
| Chips, badges, status pills, delete circle | `CircleShape` (fully rounded) | — |

App code references **only** `MaterialTheme.shapes.*` or `CircleShape` — raw `RoundedCornerShape(...)` in screens/components is a lint failure (today: zero occurrences; keep it that way).

---

## 5. Elevation & layering policy

1. **No shadows in app code.** `shadowElevation`/`defaultElevation` must not appear under `ui/`. Depth is the container-color ramp (§1.3).
2. Flat chrome: `TopAppBar` and screens share `background`; no elevation tint on scroll (scrollBehavior may fade/expand per M3 default, but bars stay background-colored).
3. The single sanctioned shadow: `HorizontalFloatingToolbar`'s built-in projection (it floats over imagery).
4. Sheets dim the world with scrim `alpha 0.18f`; the processing overlay dims the stage with `surface @ 55%`.
5. `tonalElevation` is not set manually anywhere; if a Surface ever looks "sunken", choose a lower `surfaceContainer*` slot instead.

---

## 6. Borders (hairlines)

Exactly three sanctioned hairlines, all `1.dp`, all `outlineVariant` unless noted:

1. Empty-stage slash divider (Canvas `drawLine`, 1dp, `outlineVariant`) — the signature graphic.
2. Additive action tile border (`StageActionTile`, `outlineVariant`) — signals "drop zone", not decoration.
3. Thumbnail delete circle border — **change from `outline` to `outlineVariant`** (soften; edit listed in §10).

Nothing else gets a border: no card outlines, no dividers between rows (spacing + tone separate rows), no `HorizontalDivider` inside cards unless separating scrollable content lists (log console excluded — it has none).

---

## 7. Motion

Base: `MaterialExpressiveTheme` + `MotionScheme.expressive()` (component springs/shape morphs are inherited — don't fight them). Custom choreography lives in `ui/theme/Motion.kt` and is exhaustive:

| Token | Spec | Used for |
|---|---|---|
| `PressMotionSpec` | 100ms, emphasized-decelerate | press scale to **0.97** (`pressScale`/`pressable`) |
| `EnterMotionSpec` | 140ms fade, decelerate | `AnimatedContent` text swaps, visibility fades |
| `EnterSizeSpec` | 140ms | sync'd size+fade reveals |
| `ExpandSizeSpec` | 180ms | `animateContentSize` accordions (parameter card, checkpoint details, stage 180↔320dp) |
| `NavEnter/NavExit` | 400ms decel / 200ms accel, pure fade | navigation transitions (no slide, pop == push) |
| `smoothProgress` | 300ms linear | determinate progress values |

**Rules:**
- Animate **transform and alpha only**. No slide-ins, no parallax, no bounce/overshoot beyond the expressive scheme's springs.
- No looping/decorative animation: no shimmer skeletons, no pulsing dots, no animated gradients. Loading = quiet placeholders (empty `surfaceContainerHigh` well) + determinate progress wherever percentage is known.
- Chevron expand = 180° rotate; content = `animateContentSize(ExpandSizeSpec)`.
- Known limitation (documented in `Motion.kt`): Compose has no global reduced-motion switch yet; do not add per-screen motion toggles ad hoc.
- Haptics: long-press-copy only (`HapticFeedbackType.LongPress`). No vibration elsewhere.

---

## 8. Icons

- Set: `androidx.compose.material.icons.outlined.*` (extended library is already a dependency).
- **Filled exceptions (complete list):** `Icons.Filled.PlayArrow` in the start CTA; `selectedIcon` filled variants for bottom-nav/rail/drawer selected state. Nothing else.
- Sizes: **24dp** default (`ListItem` leading, IconButton); **20dp** inline row trailing/leading compact icons; **22dp** inside buttons/tiles; **13–16dp** micro icons inside chips/tiny circles.
- Tint: passive = `onSurfaceVariant`; standard = inherit (`onSurface`); selected/active = `primary` (via component defaults); destructive *confirmation* contexts = `error`.
- Decorative images get `contentDescription = null` when a text label is adjacent (TalkBack single-read pattern already used in nav bars).
- No multicolor icons, no custom vector assets, no emoji-as-icons. The Canvas slash divider (§6.1) is the only bespoke graphic.

---

## 9. Component patterns

Recipes reference existing composables — implementers extend these, they don't fork them.

### 9.1 Screens
- Primary tab screens: `Scaffold(containerColor = background)` + `CrossScaleTopAppBar` (small, `titleLarge`, no custom background) + scrolling `Column(widthIn(max = 840.dp), spacedBy(Spacing.lg), padding(horizontal = LocalScreenMargin))`.
- Secondary screens: `SecondaryScreenScaffold(titleRes, onBack)` exclusively — owns back arrow (`Icons.AutoMirrored.Filled.ArrowBack`) and inset handling. Sub-screens render no second title.

### 9.2 Cards
`Surface(fillMaxWidth, shape = MaterialTheme.shapes.medium /*16*/, color = surfaceContainer)` wrapping a `Column(padding(Spacing.lg), spacedBy(Spacing.md or sm))`. Canonical instances: `ParameterCard` (after radius move), `CheckpointSection`, `SettingsGroupCard` (after move), `ModelCard`, `StatusCard` (after move). Cards are never bordered, never shadowed, never tappable as a whole — rows inside carry interaction.

### 9.3 Buttons (closed set)
| Kind | Use | Notes |
|---|---|---|
| `Button` (filled) | One primary CTA per context | Dock CTA height 56dp (Expressive L); busy state swaps icon → `CircularProgressIndicator(22dp, strokeWidth 2.5dp)` and disables |
| `FilledTonalButton` | Rare secondary affirmative (checkpoint "resume") | At most one per screen |
| `TextButton` | Cancel / quiet actions | Cancel-processing variant overrides `contentColor = error` |
| `IconButton` | Ghost row actions | 20dp icon, `onSurfaceVariant`; destructive ones confirm via dialog first |

Forbidden: outlined buttons, elevated buttons, custom-filled variants, buttons with borders.

### 9.4 List rows
M3 `ListItem` with `ListItemDefaults.colors(containerColor = Color.Transparent)` inside cards. Two-line pattern: headline `bodyLarge` + supporting `bodySmall/onSurfaceVariant`. Navigate rows trail a 20dp `ChevronRight`. Switch rows: whole row `toggleable(role = Role.Switch)`, `Switch(onCheckedChange = null)` as indicator only. Leading icons 24dp `onSurfaceVariant`. Row internal spacing comes from ListItem defaults — do not pad rows manually beyond section spacing.

### 9.5 Section headers
`SectionHeader(text)` sits **above** its group card: `SectionLabelStyle`, `onSurfaceVariant`, padding h-xs/v-xs. Uppercase styling is *not* applied (CJK-friendly); tracking does the work.

### 9.6 Top bars
Small `TopAppBar` only — no large/collapsing bars. Title `titleLarge`, no logo in the bar. Actions are `IconButton`s (ghost pattern); the Home top bar carries exactly one composite action: the connection status indicator (status semantics via color: `primary` connected/connecting, `onSurfaceVariant` disconnected, `labelMedium`).

### 9.7 Bottom navigation (+ adaptive)
`NavigationBar(containerColor = surfaceContainer)`, three fixed destinations (Home / Devices / Settings). Selected pill keeps the M3 default `secondaryContainer` — **deliberately not teal**: selection is orientation, not action. Outlined icon idle ↔ filled icon selected, label always visible. Medium windows: `NavigationRail` same container; expanded: `PermanentNavigationDrawer` (sheet `surfaceContainer`, drawer header = app name in `titleSmall/onSurfaceVariant`). Selection logic and `navigateTo` are shared (§10 dedupe).

### 9.8 Progress states
- **Stage overlay** (processing): full-stage scrim `surface@55%`; bottom-aligned block: smoothed `LinearProgressIndicator(progress = { smoothProgress(x) })` full width → status line `titleSmall` → counts `labelSmall/onSurfaceVariant` → current filename mono `labelSmall` → error mono in `error` → `TextButton(error)` cancel. This exact stack is `ProcessingProgress.StageProgressOverlay`; reuse it, don't restyle per screen.
- **Inline busy:** `CircularProgressIndicator(20dp, strokeWidth 2dp)` replacing an action icon.
- **Unknown duration** (connecting): indeterminate spinner inside `ConnectingCard`; no fake percentages anywhere.

### 9.9 Empty states
Two grades, no illustrations, no marketing copy:
1. **Hero empty** (`ImageStage`/`EmptyStage`): 180dp stage, `surfaceContainer`, slash divider splits image/folder halves; each half is itself tappable (40dp `onSurfaceVariant` icon centered) — no redundant "Add" button.
2. **List empty** (e.g. models): `Surface(surfaceContainer, shapes.medium)`, centered column — 24dp `onSurfaceVariant` icon + one `bodyMedium/onSurfaceVariant` sentence. No CTA duplication when the screen already has an add affordance.

Corrupted-media fallbacks follow the same recipe with `error`-colored short label ("已损坏") instead of hints.

### 9.10 Sheets & dialogs
- Pickers/queues: `ModalBottomSheet(shape = extraLarge, containerColor = surface, scrim 18%, skipPartiallyExpanded)`, content padded `horizontal = Spacing.xl`, options as transparent `ListItem` rows (HomeScreen picker-sheet pattern).
- Confirmations/forms: stock `AlertDialog` (theme supplies extraLarge 24). Fingerprint dialog centers a `ValueMono` fingerprint block. Destructive confirms use plain `TextButton`s — no red-filled buttons.
- Dialog text column rhythm: `spacedBy(Spacing.sm/md)`; body `bodyMedium`, notes `bodySmall`.

### 9.11 Badges & chips
Count badge caps display at "99+". Recolor from error-red default to `Badge(colors = BadgeDefaults.colorsWith(containerColor = primaryContainer, contentColor = onPrimaryContainer)))` — counts are information, not alarms (edit listed in §10). Status chips: `Surface(CircleShape)`, `labelSmall`, padding h-sm/v-xs, container per the §1.3 status table.

---

## 10. Migration plan (mechanical edits)

Ordered, each independently compilable:

1. **`theme/Theme.kt`** — delete the `Build.VERSION.SDK_INT >= S` dynamic-color branch (D1); add the `Shapes(...)` block from §4 to `MaterialExpressiveTheme(shapes = ...)`.
2. **Radius moves** — switch `shapes.large` → `shapes.medium` on the four card Surfaces: `home/ParameterCard.kt`, `screens/SettingsScreen.kt` (`SettingsGroupCard`), `screens/ConnectionScreen.kt` (`StatusCard`), `screens/ModelsScreen.kt` (empty-state Surface). Hero `ImageStage` and preview media frame stay `large`.
3. **Hairline softening** — `home/HeroArea.kt` thumbnail delete circle: border color `outline` → `outlineVariant`.
4. **Badge recolor** — `home/BottomActionBar.kt` queue `Badge` → `primaryContainer`/`onPrimaryContainer` (§9.11).
5. **navigateTo dedupe** — make `AdaptiveNavigation.kt`'s private `navigateTo` internal; delete the inline copy in `BottomNavigationBar.kt` and call the shared fun.
6. ✅ **Done — File rename** — `ui/screens/home/BottomActionBar.kt` → `ui/screens/home/BottomActionDock.kt` (file name matches its only composable; real location is `screens/home/`, not `components/` as first drafted).
7. **Mono hygiene** — `ModelCard.kt`: replace local `specTextStyle` with `valueStyleFor(...)` (§2.2).

**Merge candidates (verify during `components-refactor`, then execute):**
- Extract `ThumbnailCell(bitmapState, size, onClick…)` shared by `HeroArea.StageThumbnail` and the queue-sheet thumbnail cell (both decode via `decodeQueueThumbnailWithLimit` and render `small`-radius wells).
- `ConnectionDialog` vs `ConnectionScreen` manual form share field/validation logic; extract a small `HostPortFields` state holder rather than merging the composables (dialog stays for quick-pair flow).

**Deletions:** none required — every existing composable has a live caller. `tertiary*` colors stay defined but banned (D3).

---

## 11. Governance

**Adding a token:** spacing/shapes/type/color changes land only in `ui/theme/*` with a PR note updating this doc. New semantic slots require a decision entry (D8+) here first.

**Review checklist (blocker if any fail):**
- [ ] No `Color(0x…)` literals outside `ui/theme/`
- [ ] No raw `RoundedCornerShape(`/`CircleShape` misuse outside the §4 mapping (`CircleShape` only for chips/badges/circles)
- [ ] No `shadowElevation`/`defaultElevation`/manual `tonalElevation` under `ui/`
- [ ] Padding/gap values are `Spacing.*`; raw dp only per Appendix A
- [ ] `primary` usage passes the §1.3 five-item allowlist
- [ ] No `tertiary*`, `surfaceBright/Dim`, `outline` (manual) in app code
- [ ] New icons are Outlined (or one of the two Filled exceptions)
- [ ] Animations use Motion.kt tokens; nothing loops

Quick CI-able greps (all must return empty):
```bash
grep -rn "Color(0x" apps/app/src/main/java/com/crossscale/ui --include="*.kt" | grep -v "/theme/"
grep -rn "RoundedCornerShape(" apps/app/src/main/java/com/crossscale/ui --include="*.kt"
grep -rn "shadowElevation\|defaultElevation\|tonalElevation" apps/app/src/main/java/com/crossscale/ui --include="*.kt"
```

---

## Appendix A — Approved raw-dp component sizes

180dp (empty stage) · 320dp (loaded stage) · 56dp (dock CTA height) · 72dp (thumbnail/action tile cells) · 220–520dp (preview media box bounds) · 22dp visual / 48dp hit target (thumbnail delete circle) · icon sizes 13/16/20/22/24/40 · spinner 20dp(stroke 2)/22dp(stroke 2.5) · 840dp (max content width). Anything else needs an appendix update.

## Appendix B — Composable disposition

| Disposition | Composables |
|---|---|
| Keep as-is | `CrossScaleTopAppBar`, `SecondaryScreenScaffold`, `SectionHeader`, `BottomNavigationBar`+`AdaptiveNavigation*`, `ImageStage`/`EmptyStage`/`PreviewBottomSheet`, `ParameterCard`, `CheckpointSection`, `ModelCard`, `StageProgressOverlay`, `LogViewer`, `RemoteModelsDialog`, `ConnectionDialog`, `FingerprintConfirmDialog`, `QueueSheet`, `pressable`/`pressScale`/`smoothProgress` |
| Edit (§10) | `Theme.kt`, `ParameterCard`, `SettingsGroupCard`, `StatusCard`, Models empty state, `HeroArea` circle, dock `Badge`, `BottomNavigationBar` |
| Rename | ✅ Done: `ui/screens/home/BottomActionBar.kt` → `ui/screens/home/BottomActionDock.kt` |
| Merge candidate | `HostPortFields` extraction · shared `ThumbnailCell`: ✅ done, now `home/ThumbnailCell.kt` (used by `HeroArea` and `QueueSheet`) |
| Delete | — (nothing dead found) |
