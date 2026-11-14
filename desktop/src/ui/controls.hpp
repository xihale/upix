// ui/controls.hpp — Geist Finish system + wired control primitives.
// Spec: rust-rewrite crates/upix-gui/src/theme.rs (Finish Look tables) and
// ui.rs (geometry). Shadows copy y / blur / spread / alpha from those tables.
//
// Immediate mode: helpers take an ImDrawList and rects, return true on
// click-release, and keep per-ID spring state in motion.hpp.
// No entrance motion. Hover wash is a snap, not a tween.
#pragma once

#include "ui/controls/primitives.hpp"
#include "ui/controls/surfaces.hpp"
#include "ui/controls/chips.hpp"
#include "ui/controls/buttons.hpp"
#include "ui/controls/inputs.hpp"
