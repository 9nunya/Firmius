//! Stable host/workspace/thread projection for the sidebar.
use super::*;
use std::{cell::RefCell, collections::HashSet};

#[derive(Default)]
struct TreeState {
    leaves: Vec<SessionRow>,
    closed_hosts: HashSet<String>,
    closed_workspaces: HashSet<String>,
}

thread_local! { static TREE: RefCell<TreeState> = RefCell::new(TreeState::default()); }

fn workspace_key(host: &str, workspace: &str) -> String {
    format!("{host}\u{1f}{workspace}")
}

fn projected(tree: &TreeState) -> Vec<SessionRow> {
    let mut rows = Vec::new();
    let mut hosts = vec!["Local".to_owned()];
    hosts.extend(
        saved_ssh_rows()
            .into_iter()
            .map(|row| format!("{} (SSH)", row.alias)),
    );
    hosts.extend(tree.leaves.iter().map(|leaf| leaf.host.to_string()));
    let mut seen_hosts = HashSet::new();
    hosts.retain(|host| seen_hosts.insert(host.clone()));
    for host in hosts {
        rows.push(SessionRow {
            kind: "host".into(),
            key: host.clone().into(),
            collapsed: tree.closed_hosts.contains(&host),
            id: "".into(),
            title: host.clone().into(),
            detail: "".into(),
            host: host.clone().into(),
            workspace: "".into(),
            project: "".into(),
        });
        if tree.closed_hosts.contains(&host) {
            continue;
        }
        let mut workspaces = HashSet::new();
        for leaf in tree.leaves.iter().filter(|leaf| leaf.host.as_str() == host) {
            let workspace = leaf.workspace.to_string();
            let key = workspace_key(&host, &workspace);
            if workspaces.insert(key.clone()) {
                rows.push(SessionRow {
                    kind: "workspace".into(),
                    key: key.clone().into(),
                    collapsed: tree.closed_workspaces.contains(&key),
                    id: "".into(),
                    title: leaf.project.clone(),
                    detail: workspace.clone().into(),
                    host: host.clone().into(),
                    workspace: workspace.clone().into(),
                    project: leaf.project.clone(),
                });
            }
            if tree.closed_workspaces.contains(&key) {
                continue;
            }
            let mut thread = leaf.clone();
            thread.kind = "thread".into();
            thread.key = thread.id.clone();
            thread.collapsed = false;
            rows.push(thread);
        }
    }
    rows
}

fn project(ui: &MainWindow, tree: &TreeState) {
    let previous = ui.get_sessions();
    ui.set_sessions(ui_models::patch(&previous, projected(tree), |r| {
        r.key.clone()
    }));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn session(id: &str, workspace: &str, title: &str) -> SessionRow {
        SessionRow {
            kind: "thread".into(),
            key: id.into(),
            collapsed: false,
            id: id.into(),
            title: title.into(),
            detail: "".into(),
            host: "Local".into(),
            workspace: workspace.into(),
            project: workspace.rsplit('/').next().unwrap().into(),
        }
    }

    #[test]
    fn projection_has_one_fixed_row_per_tree_node_and_respects_collapse() {
        let mut tree = TreeState::default();
        tree.leaves = vec![
            session("one", "/code/Firmius", "One"),
            session("two", "/code/Firmius", "Two"),
        ];
        let rows = projected(&tree);
        assert_eq!(
            rows.iter().map(|row| row.kind.as_str()).collect::<Vec<_>>(),
            ["host", "workspace", "thread", "thread"]
        );
        tree.closed_workspaces
            .insert(workspace_key("Local", "/code/Firmius"));
        let rows = projected(&tree);
        assert_eq!(
            rows.iter().map(|row| row.kind.as_str()).collect::<Vec<_>>(),
            ["host", "workspace"]
        );
    }
}

pub(super) fn set_sessions(ui: &MainWindow, rows: Vec<SessionRow>) {
    TREE.with_borrow_mut(|tree| {
        tree.leaves = rows;
        project(ui, tree);
    });
}

pub(super) fn bind(ui: &MainWindow, state: Arc<Mutex<DesktopState>>) {
    let weak = ui.as_weak();
    ui.on_toggle_session_host(move |host| {
        if let Some(ui) = weak.upgrade() {
            TREE.with_borrow_mut(|tree| {
                let host = host.to_string();
                if !tree.closed_hosts.remove(&host) {
                    tree.closed_hosts.insert(host);
                }
                project(&ui, tree);
            });
        }
    });
    let weak = ui.as_weak();
    ui.on_toggle_session_workspace(move |key| {
        if let Some(ui) = weak.upgrade() {
            TREE.with_borrow_mut(|tree| {
                let key = key.to_string();
                if !tree.closed_workspaces.remove(&key) {
                    tree.closed_workspaces.insert(key);
                }
                project(&ui, tree);
            });
        }
    });
    let weak = ui.as_weak();
    ui.on_new_thread_in(move |host, workspace| {
        let Some(ui) = weak.upgrade() else {
            return;
        };
        navigation::new_task(&ui, &state);
        if let Ok(mut state) = state.lock() {
            if let Some(tab) = state.shell.active_mut() {
                tab.composer.workspace = if !workspace.is_empty() {
                    workspace.to_string()
                } else if host.ends_with(" (SSH)") {
                    format!("ssh://{}/", host.trim_end_matches(" (SSH)"))
                } else {
                    String::new()
                };
            }
            navigation::restore(&ui, &mut state);
        }
    });
}
