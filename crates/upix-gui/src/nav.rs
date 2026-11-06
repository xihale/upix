//! Page identity and titles, shared by the desktop rail and the phone tab bar.

/// The five pages (CONTRACT §8), in rail order.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Page {
    Home,
    Progress,
    Compare,
    Host,
    Models,
}

impl Page {
    pub fn title(self) -> &'static str {
        match self {
            Page::Home => "首页",
            Page::Progress => "进度",
            Page::Compare => "对比",
            Page::Host => "主机",
            Page::Models => "模型",
        }
    }

    pub const ALL: [Page; 5] = [
        Page::Home,
        Page::Progress,
        Page::Compare,
        Page::Host,
        Page::Models,
    ];
}
