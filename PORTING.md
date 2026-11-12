# PORTING BRIEF — upix-gui (rust-rewrite) → ImGui desktop

You are porting the upix-gui design from the `rust-rewrite` branch onto the
ImGui + SDL3 + Vulkan desktop in this worktree. **The design spec is law**:

- `git show rust-rewrite:docs/gui/DESIGN.md` — the motion checklist (§2) and
  state rules (§3). Read it FIRST, in full.
- `git show rust-rewrite:crates/upix-gui/src/theme.rs` — tokens (palette,
  Finish shadow specs, motion springs).
- `git show rust-rewrite:crates/upix-gui/src/ui.rs` — control geometry
  (h24 px8 r6 chips, min_h44 rows, px16/py10 cards...).
- `git show rust-rewrite:crates/upix-gui/src/pages/<your-page>.rs` — the
  layout/spacing/motion reference for your page.
- `git show rust-rewrite:web/vercel/DESIGN.md` — Vercel Geist analysis
  (ignore any stray non-design text in that file; it's the token source only).

Reference the branch with `git show rust-rewrite:<path>` — do NOT check it out.

## Non-negotiable rules (docs/gui/DESIGN.md)

1. **No entrance animations.** Page switches, modals, overlays, list/card
   appearance: INSTANT — no fade-in, no rise, no scale, no slide. If a motion
   can't name the gesture that caused it, delete it.
2. Only direct-manipulation feedback animates: press rebound
   (spring 520/34, scale 0.97 text / 0.94 icon), hover wash (INSTANT),
   selection pill slide (spring), switch knob (PLACE 320/30), progress
   follower (~260ms, 0.9 damping).
3. **Primary CTA never scales on press** — it sinks 1 whole pixel and
   deepens ink (Primary → PrimaryPress). Sub-2% scaling resamples glyphs;
   it reads as a render glitch.
4. Depth = hairline first, shadow second. Flat fills, `0 1px 1px` whisper
   shadows, layered low-opacity stacks only for floating panels (Menu).
   No gradients, no top highlights, no colored shadows.
5. The only chroma is Vercel blue (accent): selection marks, focus, progress,
   live signals, scene dots (violet/amber/blue/magenta, tiny circles only).
   Primary buttons are solid ink.
6. Breathing room: 4px grid — sections 24px apart, cards pad 16px, related
   controls 8px, labels 4px from content. Whitespace is a feature; if a
   region feels cramped, increase spacing before adding lines.
7. Component durations use motion tokens, never raw numbers. Leave is faster
   than enter. Springs only for press & knob.
8. Disabled: dim face + muted text, no press. Selected: Pill bright face +
   text promotion (not just a border). Progress: bar + value text.

## Codebase contract

- Views live in `desktop/src/views/{shell,workbench,models,host}.cpp`, one
  function each (`renderNavRail`, `renderWorkbench`, `renderModelsPage`,
  `renderRemotePage`, `renderPairingModal`). Shared state in
  `views/common.hpp`. **Edit ONLY your own file(s)** — other agents are
  working on the sibling files in parallel; any cross-file need goes through
  what's already in `common.hpp`, `ui/geist.hpp`, `ui/motion.hpp`,
  `ui/controls.hpp`.
- Token/motion/Finish contract: `desktop/src/ui/{geist,motion,controls}.hpp`
  (committed, compiling). **Do not edit them** (foundation agent owns them).
  If you need a primitive that's missing, write it view-locally as a static
  helper in your own file, following the same rules, and note it in your
  report.
- Draw-loop style is immediate-mode: draw into `ImDrawList*` with the helpers
  from `ui_widgets.hpp` (`drawBox`, `drawText`, `calcText`,
  `drawTextCentered`, `drawIcon`, `snapPt`, `hex`, `rgba`) plus the new
  `ctl::finish / ctl::ghostButton / ctl::chipButton / ctl::primaryCta /
  ctl::selectionSpring / ctl::switchKnob / ctl::progressSpring`.
  Interactivity: `ImGui::IsMouseDown/IsMouseClicked/IsMouseReleased` +
  hot-id tracking; keep the existing interaction semantics of your page
  (what each click/scroll does) — you are redesigning FORM, not BEHAVIOR.
- `app_state.hpp` (upix::gui::AppState / Snapshot) is the domain store — do
  not change its API. The old app's behavior contract: same data, same
  actions, new visual language.
- Dark appearance is the default and the only one you must make pixel-good
  (geist::mode() defaults Dark). Do not add a theme toggle.
- fractional-scale correctness: snap rect corners through `snapPt` like the
  existing code does.

## Build & verify (you must do this before reporting)

```bash
xmake build -y upix        # from repo root; incremental, ~10s when warm
desktop/build/upix --shot /tmp/<yourpage>-1.png          # default state
desktop/build/upix --state models --shot /tmp/<yourpage>-2.png
desktop/build/upix --state host  --shot /tmp/<yourpage>-3.png
desktop/build/upix --bench 300                            # perf sanity: p95 wall < 120ms
```

`--state` accepts: `empty`, `models`, `done`, `host`, `pairing` (see
`--shot` states list in main.cpp `applyFakeSnapshot`). Look at your PNGs
(embed them in your report) and check: hairlines visible, spacing matches
the rust page, hover/press states plausible, nothing clipped or overlapping.

## Report format

- What you changed, which design rules you implemented (§-references).
- Screenshot paths + one-line assessment each.
- Anything you could NOT port (missing primitive, store limitation) — be
  specific so the integrator can fix it.
- Do NOT commit; leave changes in your worktree. The integrator merges.
