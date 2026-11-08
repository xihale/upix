//! Navigation tabs for the 76px left rail (`desktop/src/main.cpp` `renderNavRail`).

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NavTab {
    Workbench,
    Models,
    Host,
}

impl NavTab {
    pub const ALL: [NavTab; 3] = [NavTab::Workbench, NavTab::Models, NavTab::Host];

    pub fn label(self) -> &'static str {
        match self {
            NavTab::Workbench => "放大",
            NavTab::Models => "模型",
            NavTab::Host => "远程",
        }
    }

    pub fn icon_svg(self) -> &'static str {
        match self {
            NavTab::Workbench => crate::ui::icons::ZOOM_IN,
            NavTab::Models => crate::ui::icons::LAYERS,
            NavTab::Host => crate::ui::icons::SMARTPHONE,
        }
    }
}
