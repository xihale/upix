# Upix Desktop — Qt 6 Quick

**Status:** the desktop workbench (`desktop/`), a Qt 6 Quick window around `upix-cli`.
**Aesthetic:** "darkroom instrument" (暗房仪器) — warm graphite canvas near-black,
one tungsten-amber signal color, Lucide icons, images as the protagonist,
quiet monospace metadata, depth by tone and hairlines, never shadow or glow.
**Stack:** C++20 + Qt 6.5+ (Quick + QuickControls2 + Svg; QApplication/Widgets linked
for native file dialogs), CMake + Ninja (`./build.sh`). The CLI stays a
subprocess: GPU-crash isolation, one interface. `QQuickStyle` is pinned to
`Basic` and imports are per-file, but the visible widgets are the hand-drawn
component layer below — `Controls.Basic` chrome (Basic-style popups, switch
colors, spinner frames, palette-derived ProgressBar fill) cannot take the
container/radius/tone language. A window-level `palette` bound to the Theme
tokens still tints the few stock pieces that remain (scrollbars, tooltips,
native dialogs). `QImageReader` allocation limit is lifted (upscaler outputs
exceed Qt's 256 MB decode ceiling).

**Component layer (`qml/`, all token-driven):**

| File | Role |
|---|---|
| `Icon.qml` | library icons — Lucide (ISC) rasterized tintable by `src/icons.cpp` at `image://icon/<kind>/<color>[?fill=1]`; 24-unit viewBox, 2px round-cap stroke |
| `Rail.qml` | 76px nav rail: wordmark, amber signal tick + tint on the active item, hairline edge |
| `Pill.qml` | button: 8px radius, 1px etched outline; `flat` text-only, `accent` = the one amber-filled action |
| `Picker.qml` | collapsed hairline row + token popup list, amber check on the selection |
| `Segmented.qml` | output format, etched track + raised 220 ms indicator |
| `TrackSlider.qml` | 2px track with ruler ticks, primary fill, ringed 14px thumb |
| `Toggle.qml` | 44x26 switch, 20px thumb, 180 ms |
| `StepRow.qml` | `[-] n [+]` hairline stepper |
| `Progress.qml` | 3px determinate bar, primary fill, 300 ms linear |

## Architecture

```
qml/Main.qml            shell: Rail | pages | inspector, toast, pairing sheet;
                        Ctrl+V paste (URLs or clipboard bitmap), Esc rejects a
                        pending pairing
qml/Workbench.qml       stage states: empty zones / image / compare / veil / strip
qml/CompareStage.qml    before|after slider, sweep-on-arrive, drag handle, 打开/显示;
                        `dividerPx` rounds the clip seam, rule and handle to one
                        column, and the grab band spans the stage (a band riding
                        on the divider reported mouse.x from a moving origin and
                        collapsed `pos` on every press)
qml/Inspector.qml       Picker (model), Segmented (format), TrackSlider
                        (quality/tile), Toggle + StepRow (processing), CTA, daemon row
qml/ModelsPage.qml      installed catalog grouped by family + downloads
qml/HostPage.qml        header (title + status dot + stop), pairing panel
                        (QR field | hairline | instrument column on a shared
                        x=56 label grid), session log strip; idle QR field
                        carries the stage's registration marks
src/desk.{h,cpp}        controller: one QVariantMap snapshot (same shape as the
                        web-era Snapshot JSON) + Q_INVOKABLE commands; 70 ms
                        coalesced snapChanged; JSON prefs at ~/.config/upix/desktop.json
src/parser.{h,cpp}      CLI line parsers (progress tiles, saved outputs, model
                        lists, daemon status, host lines) — regex port of parse.go
src/qrprovider.cpp      image://qr/<payload> via vendored Nayuki qrcodegen (MIT)
src/icons.{h,cpp}       image://icon/<kind>/<color>[?fill=1] — Lucide (ISC) paths
                        rendered tintable via QSvgRenderer
src/theme.{h,cpp}       token colors, follows system scheme, UPIX_THEME overrides
src/main.cpp            bootstrap + headless tooling: --shot/--anim/--sweep/--smoke
tests/tst_compare_drag  QTest regression: drives CompareStage with real mouse
                        events (drag tracking, clamps, one-column seam)
```

State shape: `Desk.snap` — `{cliFound, cliVersion, inputs[], cover, model, format,
quality, gpu, autoTile, tileSize, force, jobs, installed[], remote[], job{},
host{}, daemon{}, notice, noticeSeq}`. `daemon` carries `{running, label, pid,
socketName, socketMissing}`: the CLI's raw line is 60+ chars (it embeds the
socket path), so the inspector renders `pid` compactly and keeps `label` for the
tooltip. QML is a pure function of it; commands go `Q_INVOKABLE` back. CLI
discovery: `UPIX_CLI` → exe dir → `PATH`.

## Tokens (dark / light)

Darkroom instrument: warm graphite neutrals + one tungsten-amber signal.
bg `#131316`/`#f4f3ef` · surface `#1a1a1f`/`#ffffff` · container
`#202026`/`#ecebe6` · container-low `#1c1c21`/`#f0efeb` · high `#26262c`/`#e3e2dc`
· highest `#323239`/`#d9d8d1` · outline `#2d2d34`/`#d6d5cd` · primary
`#f2a93b`/`#a26600` · on-primary `#221503`/`#ffffff` · error `#ff7361`/`#c23a2b`
· muted `#a2a2ac`/`#71706a`. One amber moment per region (CTA, active nav tick,
selection check, download/progress fill); error coral only reports errors.

Control grammar: hairline outlines over fills, 8px radius on controls, 12 on
stages/sheets, 999 only on switches. Amber is filled only for the primary
action. Numbers, paths, sizes, logs, fingerprints: monospace. Empty stage =
film-carrier registration frame (four corner marks) around the drop hint.

Radii: stage/sheet 12, cards/popups 10, controls 8, switches 999. Type: 18 page,
17 sheet title, 15 CTA/empty-hint, 14 body/rows, 13 seg/meta, 12 labels, 11 log,
10 rail version; metadata monospace.

## Motion (three basics)

1. **Entry:** rail → main → inspector rise 420/480/480 ms stagger 0/60/120 ms
   (items boot-hidden at opacity 0), `cubic-bezier(0.16,1,0.3,1)`.
2. **Page swap:** instant visibility flip (no fade — the old opacity Behavior
   never fired; removed).
3. **Compare sweep:** on result arrive, divider animates x 0.04 → 0.62, 720 ms
   ease-out; drag clamps 0.02–0.98. Plus: veil fade 240 ms, progress width 300 ms
   linear, switch thumb 180 ms, seg indicator 220 ms, sheet pop 240 ms, toast
   180 ms.

Evidence: `shots-qt/empty-entry-{start,mid,end}.png`, `shots-qt/sweep-{start,mid,end}.png`
(captured with `--anim` / `--sweep`, software-rendered, no real display needed).

## Headless verification

```bash
ctest --test-dir build --output-on-failure     # compare-divider regression
UPIX_CLI=…/upix-cli ./build/upix --smoke       # real job, exit 0/1
./build/upix --shot empty,done,models,host --out shots-qt
QT_SCALE_FACTOR=2 ./build/upix --shot done --out shots-qt/zoom   # 200 %
UPIX_THEME=light ./build/upix --shot done --out shots-light
```

`--shot` implies fake fixture data (`UPIX_FAKE`); states match the web-era
fixtures. Fixture art lives in `assets/scene{,-after}.svg`.

`tst_compare_drag` exists because the screenshot tooling cannot see gestures:
the divider bugs (a press collapsing `pos` to the clamp floor, and the seam
painted as a second line) were invisible in every `--shot`/`--sweep` frame and
only appeared under a real drag. It loads the shipped `CompareStage.qml` and
sends real `QMouseEvent`s. Keep the two drag assertions in step with the
component: they are the only automated coverage of pointer interaction.

## Parity notes

- Folder `--gpu` only applies to directory runs. Single images never pass
  `--auto-tile` — on the CLI it gates out the warm-daemon delegation — so
  every single-image job runs through the hot engine (the CLI spawns one and
  falls back to the inline process, GPU→CPU, if delegation is unavailable);
  a pinned tile size rides in the ServeRequest. Tiling/GPU/jobs flags are
  folder semantics, surfaced as hints in the inspector.
- WebP/AVIF thumbnails decode only if the corresponding Qt imageformats plugin
  is installed (`qt6-imageformats` / libavif); otherwise the cell stays quiet.
- No native menu bar (webview build had one); keyboard parity: Ctrl+O,
  Ctrl+Shift+O, Ctrl+V (paste image), Ctrl+1/2/3, Ctrl+Return (start),
  Ctrl+. (cancel), Esc (reject pairing).
- Prefs file is the same JSON as the previous Go build, so settings survive the swap.
