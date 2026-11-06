//! Theme helpers. The look is the shadcn token set nsh ships by default
//! (`nsh-base`'s neutral theme, light and dark); `UPIX_THEME` forces an
//! appearance at startup (handled in the root). No raw colors in pages.

use nsh_base::theme;
use nsh_core::prelude::*;

pub fn muted_ink() -> impl IntoReactive<AdaptiveColor> {
    move || theme().muted_foreground
}

pub fn primary_ink() -> impl IntoReactive<AdaptiveColor> {
    move || theme().primary
}
