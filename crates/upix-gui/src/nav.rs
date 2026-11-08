//! Navigation tabs for the 76px left rail (`desktop/src/main.cpp` `NavTab`).

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
            NavTab::Workbench => "工作台",
            NavTab::Models => "模型库",
            NavTab::Host => "远程主机",
        }
    }

    pub fn icon_svg(self) -> &'static str {
        match self {
            NavTab::Workbench => crate::ui::icons::WORKBENCH,
            NavTab::Models => crate::ui::icons::MODELS,
            NavTab::Host => crate::ui::icons::HOST,
        }
    }
}
