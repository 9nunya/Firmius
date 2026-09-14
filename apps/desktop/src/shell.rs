//! UI-independent shell. A tab owns a route and local state; a viewport owns
//! tabs. Session identity is never tab identity. Features render routes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum View {
    Conversation,
    Work,
    Changes,
    Runtime,
    Output(String),
    Settings,
    Feature(String),
}
impl View {
    pub fn name(&self) -> &str {
        match self {
            Self::Conversation => "conversation",
            Self::Work => "work",
            Self::Changes => "changes",
            Self::Runtime => "runtime",
            Self::Output(_) => "output",
            Self::Settings => "settings",
            Self::Feature(name) => name,
        }
    }
    pub fn parse(s: &str) -> Self {
        match s {
            "work" => Self::Work,
            "changes" => Self::Changes,
            "runtime" => Self::Runtime,
            "settings" => Self::Settings,
            "conversation" => Self::Conversation,
            name => Self::Feature(name.into()),
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Route {
    pub session: Option<String>,
    pub agent: Option<String>,
    pub view: View,
}
/// Local editor transaction. Acknowledgements cannot erase a newer revision.
#[derive(Clone, Debug, Default)]
pub(crate) struct ComposerState {
    pub text: String,
    pub revision: u64,
    pub pending: Option<u64>,
    pub error: String,
    pub provider: String,
    pub model: String,
    pub effort: String,
    pub workspace: String,
}
impl ComposerState {
    pub fn edit(&mut self, text: String) {
        if self.text != text {
            self.text = text;
            self.revision += 1;
        }
    }
    pub fn begin(&mut self) -> Option<(u64, String)> {
        if self.pending.is_some() || self.text.trim().is_empty() {
            return None;
        }
        self.pending = Some(self.revision);
        self.error.clear();
        Some((self.revision, self.text.clone()))
    }
    pub fn finish(&mut self, revision: u64, result: Result<(), String>) {
        if self.pending != Some(revision) {
            return;
        }
        self.pending = None;
        match result {
            Ok(()) if self.revision == revision => self.edit(String::new()),
            Ok(()) => {}
            Err(error) => self.error = error,
        }
    }
}
#[derive(Clone, Debug)]
pub(crate) struct Tab {
    pub id: String,
    pub title: String,
    pub route: Route,
    pub composer: ComposerState,
    pub reading: crate::reading::ReadingState,
    pub expanded: String,
    pub workflow_open: bool,
}
#[derive(Clone, Debug)]
pub(crate) struct Viewport {
    pub id: String,
    pub tabs: Vec<Tab>,
    pub selected: Option<String>,
}
#[derive(Debug)]
pub(crate) struct Shell {
    pub viewports: Vec<Viewport>,
    pub focused: String,
    pub layout: crate::layout::Layout,
    pub closed_tabs: Vec<Tab>,
    pub sidebar_collapsed: bool,
    next_id: u64,
}
impl Default for Shell {
    fn default() -> Self {
        Self {
            viewports: vec![Viewport {
                id: "viewport-0".into(),
                tabs: vec![],
                selected: None,
            }],
            focused: "viewport-0".into(),
            layout: crate::layout::Layout::Pane("viewport-0".into()),
            closed_tabs: vec![],
            sidebar_collapsed: false,
            next_id: 1,
        }
    }
}
impl Shell {
    fn normalize_focus(&mut self) {
        self.prune_empty_panes();
        if self.viewports.is_empty() {
            self.viewports.push(Viewport {
                id: "viewport-0".into(),
                tabs: vec![],
                selected: None,
            });
            self.layout = crate::layout::Layout::Pane("viewport-0".into());
        }
        if !self.viewports.iter().any(|pane| pane.id == self.focused) {
            self.focused = self.viewports[0].id.clone();
        }
        if let Some(pane) = self
            .viewports
            .iter_mut()
            .find(|pane| pane.id == self.focused)
        {
            if pane
                .selected
                .as_ref()
                .is_none_or(|id| !pane.tabs.iter().any(|tab| &tab.id == id))
            {
                pane.selected = pane.tabs.first().map(|tab| tab.id.clone());
            }
        }
    }

    fn prune_empty_panes(&mut self) {
        let keep_empty = self.viewports.len() == 1;
        if !keep_empty {
            let focused_empty = self
                .viewports
                .iter()
                .find(|pane| pane.id == self.focused)
                .is_some_and(|pane| pane.tabs.is_empty());
            if focused_empty {
                if let Some(neighbor) = self
                    .viewports
                    .iter()
                    .find(|pane| pane.id != self.focused && !pane.tabs.is_empty())
                {
                    self.focused = neighbor.id.clone();
                }
            }
            self.viewports.retain(|pane| !pane.tabs.is_empty());
        }
        let panes = self
            .viewports
            .iter()
            .map(|pane| pane.id.clone())
            .collect::<Vec<_>>();
        self.layout = self
            .layout
            .clone()
            .prune(&panes)
            .unwrap_or_else(|| crate::layout::Layout::Pane(self.focused.clone()));
    }

    pub fn focus_pane(&mut self, id: &str) -> bool {
        if self.viewports.iter().any(|pane| pane.id == id) && self.focused != id {
            self.focused = id.into();
            true
        } else {
            false
        }
    }

    pub fn focus_adjacent(&mut self, direction: &str) -> bool {
        let Some(next) = crate::layout::adjacent_pane(&self.layout, &self.focused, direction)
        else {
            return false;
        };
        self.focus_pane(&next)
    }

    pub fn move_active_tab(&mut self, direction: &str) -> bool {
        let Some(tab_id) = self.active().map(|tab| tab.id.clone()) else {
            return false;
        };
        let Some(target) = crate::layout::adjacent_pane(&self.layout, &self.focused, direction)
        else {
            return false;
        };
        self.drop_tab(&tab_id, &target, "center");
        true
    }

    pub fn restored(
        viewports: Vec<Viewport>,
        focused: String,
        layout: crate::layout::Layout,
        closed_tabs: Vec<Tab>,
    ) -> Self {
        // IDs are opaque outside the shell. A timestamp range avoids collisions
        // with IDs from earlier launches, including closed recoverable tabs.
        let next_id = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis()
            .min(u64::MAX as u128) as u64;
        Self {
            viewports,
            focused,
            layout,
            closed_tabs,
            sidebar_collapsed: false,
            next_id,
        }
    }
    pub fn reopen_closed(&mut self) {
        let Some(tab) = self.closed_tabs.pop() else {
            return;
        };
        if let Some(pane) = self.viewports.iter_mut().find(|p| p.id == self.focused) {
            pane.selected = Some(tab.id.clone());
            pane.tabs.push(tab);
        }
    }

    fn id(&mut self, kind: &str) -> String {
        let id = format!("{kind}-{}", self.next_id);
        self.next_id += 1;
        id
    }
    pub fn tab(&self, id: &str) -> Option<&Tab> {
        self.viewports
            .iter()
            .flat_map(|p| &p.tabs)
            .find(|t| t.id == id)
    }
    pub fn tab_mut(&mut self, id: &str) -> Option<&mut Tab> {
        self.viewports
            .iter_mut()
            .flat_map(|p| &mut p.tabs)
            .find(|t| t.id == id)
    }
    pub fn viewport(&self) -> &Viewport {
        self.viewports
            .iter()
            .find(|p| p.id == self.focused)
            .expect("focused viewport exists")
    }
    pub fn active(&self) -> Option<&Tab> {
        let p = self.viewport();
        p.tabs.iter().find(|t| Some(&t.id) == p.selected.as_ref())
    }
    pub fn active_mut(&mut self) -> Option<&mut Tab> {
        let p = self.viewports.iter_mut().find(|p| p.id == self.focused)?;
        p.tabs
            .iter_mut()
            .find(|t| Some(&t.id) == p.selected.as_ref())
    }
    pub fn open(&mut self, route: Route, title: String) {
        let id = self.id("tab");
        let p = self
            .viewports
            .iter_mut()
            .find(|p| p.id == self.focused)
            .unwrap();
        let tab = if let Some(t) = p.tabs.iter().find(|t| t.route == route) {
            t.id.clone()
        } else {
            p.tabs.push(Tab {
                id: id.clone(),
                title,
                route,
                composer: ComposerState::default(),
                reading: Default::default(),
                expanded: String::new(),
                workflow_open: false,
            });
            id
        };
        p.selected = Some(tab);
    }
    pub fn select(&mut self, id: &str) {
        if let Some(p) = self
            .viewports
            .iter_mut()
            .find(|p| p.tabs.iter().any(|t| t.id == id))
        {
            self.focused = p.id.clone();
            p.selected = Some(id.into());
        }
    }
    pub fn close(&mut self, id: &str) {
        if let Some(tab) = self.tab(id).cloned() {
            self.closed_tabs.push(tab);
        }
        for p in &mut self.viewports {
            p.tabs.retain(|t| t.id != id);
            if p.selected.as_deref() == Some(id) {
                p.selected = p.tabs.last().map(|t| t.id.clone());
            }
        }
        self.normalize_focus();
    }
    pub fn split(&mut self) {
        self.split_axis(crate::layout::Axis::Horizontal);
    }
    /// Move the actual tab, preserving its editor revision and reading anchor.
    pub fn drop_tab(&mut self, tab_id: &str, target: &str, edge: &str) {
        let Some(source) = self
            .viewports
            .iter()
            .position(|p| p.tabs.iter().any(|t| t.id == tab_id))
        else {
            return;
        };
        if !self.viewports.iter().any(|p| p.id == target) {
            return;
        }
        if self.viewports[source].id == target
            && (edge == "center" || self.viewports[source].tabs.len() == 1)
        {
            return;
        }
        if !matches!(edge, "left" | "right" | "top" | "bottom" | "center") {
            return;
        }
        let index = self.viewports[source]
            .tabs
            .iter()
            .position(|t| t.id == tab_id)
            .unwrap();
        let tab = self.viewports[source].tabs.remove(index);
        if self.viewports[source].selected.as_deref() == Some(tab_id) {
            self.viewports[source].selected =
                self.viewports[source].tabs.last().map(|t| t.id.clone());
        }
        if edge == "center" {
            let pane = self.viewports.iter_mut().find(|p| p.id == target).unwrap();
            pane.selected = Some(tab.id.clone());
            pane.tabs.push(tab);
            self.focused = target.into();
        } else {
            let id = self.id("viewport");
            let split = self.id("split");
            let axis = if matches!(edge, "left" | "right") {
                crate::layout::Axis::Horizontal
            } else {
                crate::layout::Axis::Vertical
            };
            self.layout.split(target, id.clone(), split.clone(), axis);
            if matches!(edge, "left" | "top") {
                self.layout.reverse_split(&split);
            }
            self.focused = id.clone();
            self.viewports.push(Viewport {
                id,
                selected: Some(tab.id.clone()),
                tabs: vec![tab],
            });
        }
        self.normalize_focus();
    }

    pub fn select_tab_offset(&mut self, offset: isize) {
        let pane = self.viewport();
        if pane.tabs.is_empty() {
            return;
        }
        let current = pane
            .selected
            .as_ref()
            .and_then(|selected| pane.tabs.iter().position(|tab| &tab.id == selected))
            .unwrap_or_default();
        let len = pane.tabs.len() as isize;
        let next = (current as isize + offset).rem_euclid(len) as usize;
        let id = pane.tabs[next].id.clone();
        self.select(&id);
    }

    pub fn select_tab_number(&mut self, number: usize) {
        if number == 0 {
            return;
        }
        let pane = self.viewport();
        let Some(tab) = pane.tabs.get(
            number
                .saturating_sub(1)
                .min(pane.tabs.len().saturating_sub(1)),
        ) else {
            return;
        };
        let id = tab.id.clone();
        self.select(&id);
    }
    pub fn split_axis(&mut self, axis: crate::layout::Axis) {
        let Some(mut tab) = self.active().cloned() else {
            return;
        };
        tab.id = self.id("tab");
        tab.composer = ComposerState::default();
        let id = self.id("viewport");
        let split_id = self.id("split");
        self.layout.split(&self.focused, id.clone(), split_id, axis);
        self.focused = id.clone();
        self.viewports.push(Viewport {
            id,
            selected: Some(tab.id.clone()),
            tabs: vec![tab],
        });
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn acknowledgement_preserves_newer_draft_and_error_preserves_original() {
        let mut editor = ComposerState::default();
        editor.edit("first".into());
        let (revision, text) = editor.begin().unwrap();
        assert_eq!(text, "first");
        assert!(editor.begin().is_none());
        editor.edit("second".into());
        editor.finish(revision, Ok(()));
        assert_eq!(editor.text, "second");
        let (revision, _) = editor.begin().unwrap();
        editor.finish(revision, Err("offline".into()));
        assert_eq!(editor.text, "second");
        assert_eq!(editor.error, "offline");
        let (revision, _) = editor.begin().unwrap();
        editor.finish(revision, Ok(()));
        assert!(editor.text.is_empty());
    }
    #[test]
    fn independent_tabs_and_viewports() {
        let mut s = Shell::default();
        let route = Route {
            session: Some("s".into()),
            agent: Some("main".into()),
            view: View::Conversation,
        };
        s.open(route.clone(), "main".into());
        let first = s.active().unwrap().id.clone();
        s.active_mut().unwrap().composer.edit("draft".into());
        s.open(
            Route {
                agent: Some("child".into()),
                ..route.clone()
            },
            "child".into(),
        );
        s.open(
            Route {
                view: View::Changes,
                ..route
            },
            "changes".into(),
        );
        assert_eq!(s.viewport().tabs.len(), 3);
        s.split();
        assert_eq!(s.viewports.len(), 2);
        s.select(&first);
        assert_eq!(s.active().unwrap().composer.text, "draft");
        s.close(&first);
        assert!(s.active().is_some());
    }

    fn conversation(session: &str, agent: &str) -> Route {
        Route {
            session: Some(session.into()),
            agent: Some(agent.into()),
            view: View::Conversation,
        }
    }

    #[test]
    fn closing_last_tab_in_a_split_prunes_the_empty_pane_and_focuses_a_neighbor() {
        let mut shell = Shell::default();
        shell.open(conversation("s", "main"), "main".into());
        let first = shell.active().unwrap().id.clone();
        shell.split();
        assert_eq!(shell.viewports.len(), 2);
        let second = shell.active().unwrap().id.clone();
        assert_ne!(first, second);
        shell.close(&second);
        assert_eq!(shell.viewports.len(), 1);
        assert_eq!(shell.focused, shell.viewports[0].id);
        assert_eq!(shell.active().unwrap().id, first);
        match &shell.layout {
            crate::layout::Layout::Pane(id) => assert_eq!(id, &shell.focused),
            other => panic!("expected a single pane, got {other:?}"),
        }
    }

    #[test]
    fn empty_welcome_pane_is_kept_only_when_it_is_the_last_viewport() {
        let mut shell = Shell::default();
        assert!(shell.viewports[0].tabs.is_empty());
        shell.normalize_focus();
        assert_eq!(shell.viewports.len(), 1);
        shell.open(conversation("s", "main"), "main".into());
        shell.split();
        let extra = shell.viewports[1].id.clone();
        shell.viewports[1].tabs.clear();
        shell.viewports[1].selected = None;
        shell.focused = extra;
        shell.normalize_focus();
        assert_eq!(shell.viewports.len(), 1);
        assert!(shell.active().is_some());
    }

    #[test]
    fn focus_adjacent_and_move_tab_follow_split_geometry() {
        let mut shell = Shell::default();
        shell.open(conversation("s", "main"), "main".into());
        let first = shell.active().unwrap().id.clone();
        shell.split();
        let second = shell.active().unwrap().id.clone();
        assert!(shell.focus_adjacent("left"));
        assert_eq!(shell.active().unwrap().id, first);
        assert!(shell.move_active_tab("right"));
        assert_eq!(shell.viewports.len(), 1);
        assert_eq!(shell.active().unwrap().id, first);
        assert!(shell.viewport().tabs.iter().any(|tab| tab.id == second));
    }
}

/// Portable content for a feature that renders a document/list into a viewport.
#[derive(Clone, Debug)]
pub(crate) struct DocumentRow {
    pub author: String,
    pub body: String,
    pub tone: String,
    pub detail: String,
}
