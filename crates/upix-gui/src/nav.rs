//! Navigation for the 76px left rail (`desktop/src/main.cpp` `renderNavRail`).
//!
//! Three primary tabs plus two utility entries pinned to the bottom of the
//! rail: 指南 answers "how do I start", 设置 holds the engine/encoder knobs the
//! workbench no longer shows.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NavTab {
    Workbench,
    Models,
    Host,
    Guide,
    Settings,
}

impl NavTab {
    /// Primary rail tabs, in order.
    pub const ALL: [NavTab; 3] = [NavTab::Workbench, NavTab::Models, NavTab::Host];

    /// Utility entries pinned above the version stamp.
    pub const FOOTER: [NavTab; 2] = [NavTab::Guide, NavTab::Settings];

    pub fn label(self) -> &'static str {
        match self {
            NavTab::Workbench => "放大",
            NavTab::Models => "模型",
            NavTab::Host => "远程",
            NavTab::Guide => "指南",
            NavTab::Settings => "设置",
        }
    }

    pub fn icon_svg(self) -> &'static str {
        match self {
            NavTab::Workbench => crate::ui::icons::ZOOM_IN,
            NavTab::Models => crate::ui::icons::LAYERS,
            NavTab::Host => crate::ui::icons::SMARTPHONE,
            NavTab::Guide => crate::ui::icons::BOOK,
            NavTab::Settings => crate::ui::icons::GEAR,
        }
    }
}
