//! Versioned desktop-only local layout and drafts. Never stores daemon credentials.
use super::*;
use crate::layout::{Axis, Layout};
use crate::shell::{ComposerState, Shell, Tab, Viewport};
use serde_json::{Value, json};

fn layout_value(layout: &Layout) -> Value {
    match layout {
        Layout::Pane(id) => json!({"pane": id}),
        Layout::Split {
            id,
            axis,
            ratio,
            first,
            second,
        } => {
            json!({"id": id, "axis": if *axis == Axis::Horizontal {"horizontal"} else {"vertical"}, "ratio": ratio, "first": layout_value(first), "second": layout_value(second)})
        }
    }
}
fn parse_layout(value: &Value, depth: usize) -> Option<Layout> {
    if depth > 16 {
        return None;
    }
    if let Some(id) = value["pane"].as_str() {
        return Some(Layout::Pane(id.into()));
    }
    Some(Layout::Split {
        id: value["id"].as_str()?.into(),
        axis: match value["axis"].as_str()? {
            "horizontal" => Axis::Horizontal,
            "vertical" => Axis::Vertical,
            _ => return None,
        },
        ratio: (value["ratio"].as_f64()? as f32).clamp(0.25, 0.75),
        first: Box::new(parse_layout(&value["first"], depth + 1)?),
        second: Box::new(parse_layout(&value["second"], depth + 1)?),
    })
}
fn tab_value(tab: &Tab) -> Value {
    json!({"id":tab.id,"title":tab.title,"session":tab.route.session,"agent":tab.route.agent,"view":tab.route.view.name(),"output_key": match &tab.route.view { View::Output(key) => Some(key), _ => None },
        "draft":tab.composer.text,"provider":tab.composer.provider,"model":tab.composer.model,"effort":tab.composer.effort,"workspace":tab.composer.workspace,
        "expanded":tab.expanded,"workflow_open":tab.workflow_open,"scroll_y":tab.reading.y,"following":tab.reading.following})
}
fn parse_tab(value: &Value) -> Option<Tab> {
    let string = |key: &str| value[key].as_str().unwrap_or_default().to_owned();
    let mut reading = crate::reading::ReadingState::default();
    reading.scroll(
        value["scroll_y"].as_f64().unwrap_or_default() as f32,
        value["following"].as_bool().unwrap_or(true),
    );
    Some(Tab {
        id: value["id"].as_str()?.into(),
        title: string("title"),
        route: Route {
            session: value["session"].as_str().map(str::to_owned),
            agent: value["agent"].as_str().map(str::to_owned),
            view: if value["view"].as_str()? == "output" {
                View::Output(string("output_key"))
            } else {
                View::parse(value["view"].as_str()?)
            },
        },
        composer: ComposerState {
            text: string("draft"),
            provider: string("provider"),
            model: string("model"),
            effort: string("effort"),
            workspace: string("workspace"),
            ..Default::default()
        },
        reading,
        expanded: string("expanded"),
        workflow_open: value["workflow_open"].as_bool().unwrap_or(false),
    })
}
pub(super) fn encode(shell: &Shell) -> Value {
    json!({"version":1,"focused":shell.focused,"layout":layout_value(&shell.layout),
        "sidebar_collapsed":shell.sidebar_collapsed,
        "panes":shell.viewports.iter().map(|p| json!({"id":p.id,"selected":p.selected,"tabs":p.tabs.iter().map(tab_value).collect::<Vec<_>>()})).collect::<Vec<_>>(),
        "closed":shell.closed_tabs.iter().map(tab_value).collect::<Vec<_>>()})
}
pub(super) fn decode(value: &Value) -> Option<Shell> {
    if value["version"].as_u64()? != 1 {
        return None;
    }
    let mut ids = std::collections::HashSet::new();
    let mut panes = Vec::new();
    for pane in value["panes"].as_array()? {
        let id = pane["id"].as_str()?.to_owned();
        if !ids.insert(id.clone()) {
            return None;
        }
        let tabs: Vec<_> = pane["tabs"]
            .as_array()?
            .iter()
            .map(parse_tab)
            .collect::<Option<_>>()?;
        for tab in &tabs {
            if !ids.insert(tab.id.clone()) {
                return None;
            }
        }
        let selected = pane["selected"]
            .as_str()
            .filter(|id| tabs.iter().any(|t| t.id == *id))
            .map(str::to_owned)
            .or_else(|| tabs.first().map(|t| t.id.clone()));
        panes.push(Viewport { id, tabs, selected });
    }
    if panes.is_empty() {
        return None;
    }
    let layout = parse_layout(&value["layout"], 0)?;
    let (mut regions, mut handles) = (vec![], vec![]);
    layout.regions(Default::default(), &mut regions, &mut handles);
    let leaf_ids = regions
        .iter()
        .map(|(id, _)| id)
        .collect::<std::collections::HashSet<_>>();
    if leaf_ids.len() != panes.len()
        || regions.len() != panes.len()
        || panes.iter().any(|p| !leaf_ids.contains(&p.id))
    {
        return None;
    }
    for h in handles {
        if !ids.insert(h.id) {
            return None;
        }
    }
    let focused = value["focused"]
        .as_str()
        .filter(|id| panes.iter().any(|p| p.id == *id))
        .unwrap_or(&panes[0].id)
        .to_owned();
    let closed = value["closed"]
        .as_array()
        .map(|a| a.iter().filter_map(parse_tab).collect())
        .unwrap_or_default();
    let mut shell = Shell::restored(panes, focused, layout, closed);
    shell.sidebar_collapsed = value["sidebar_collapsed"].as_bool().unwrap_or(false);
    Some(shell)
}
fn path() -> Option<std::path::PathBuf> {
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE"))?;
    Some(
        std::path::PathBuf::from(home)
            .join(".firmius")
            .join("desktop-state.json"),
    )
}
pub(super) fn load() -> Option<Shell> {
    let bytes = fs::read(path()?).ok()?;
    decode(&serde_json::from_slice::<Value>(&bytes).ok()?)
}
pub(super) fn save(shell: &Shell) -> Result<(), String> {
    if std::env::args().any(|arg| arg.starts_with("--preview=")) {
        return Ok(());
    }
    let path = path().ok_or("Home directory is unavailable")?;
    fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    let mut options = fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = options.open(&temporary).map_err(|e| e.to_string())?;
    file.write_all(encode(shell).to_string().as_bytes())
        .map_err(|e| e.to_string())?;
    file.sync_all().map_err(|e| e.to_string())?;
    fs::rename(temporary, path).map_err(|e| e.to_string())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn layout_and_independent_drafts_round_trip_without_pending_requests() {
        let mut shell = Shell::default();
        shell.open(
            Route {
                session: Some("s".into()),
                agent: Some("a".into()),
                view: View::Conversation,
            },
            "A".into(),
        );
        shell.active_mut().unwrap().composer.edit("unsent".into());
        shell.active_mut().unwrap().composer.begin();
        shell.split_axis(Axis::Vertical);
        shell.active_mut().unwrap().composer.edit("second".into());
        let restored = decode(&encode(&shell)).unwrap();
        assert_eq!(restored.viewports.len(), 2);
        assert_eq!(restored.viewports[0].tabs[0].composer.text, "unsent");
        assert_eq!(restored.viewports[1].tabs[0].composer.text, "second");
        assert!(restored.viewports[0].tabs[0].composer.pending.is_none());
    }
    #[test]
    fn sidebar_collapse_survives_layout_round_trip() {
        let mut shell = Shell::default();
        shell.sidebar_collapsed = true;
        shell.open(
            Route {
                session: Some("s".into()),
                agent: Some("a".into()),
                view: View::Conversation,
            },
            "A".into(),
        );
        let restored = decode(&encode(&shell)).unwrap();
        assert!(restored.sidebar_collapsed);
        assert_eq!(restored.viewports.len(), 1);
    }
    #[test]
    fn incompatible_or_inconsistent_layout_is_rejected() {
        assert!(decode(&json!({"version":2})).is_none());
        let shell = Shell::default();
        let mut value = encode(&shell);
        value["layout"] = json!({"pane":"missing"});
        assert!(decode(&value).is_none());
    }
}
