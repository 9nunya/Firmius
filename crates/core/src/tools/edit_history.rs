//! Per-agent edit history used by the built-in undo/redo tool.
//!
//! The history is intentionally keyed by session and agent. A delegated agent
//! can therefore undo its own edits without accidentally rewinding a sibling's
//! work, while the session remains the unit of shared filesystem state.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct Entry {
    pub path: String,
    pub before: Option<Vec<u8>>,
    pub after: Option<Vec<u8>>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct Transaction {
    pub label: String,
    pub entries: Vec<Entry>,
}

#[derive(Default, Serialize, Deserialize)]
struct Stack {
    undo: Vec<Transaction>,
    redo: Vec<Transaction>,
}

static HISTORIES: OnceLock<Mutex<HashMap<(String, String), Stack>>> = OnceLock::new();
/// Serializes the snapshot-and-rename portion of persistence. Without this
/// second lock, two agents editing the same session could each capture a
/// valid map and then race their fixed `.tmp` path, allowing an older
/// snapshot to overwrite the newer agent's history.
static PERSISTENCE: OnceLock<Mutex<()>> = OnceLock::new();
const MAX_TRANSACTIONS: usize = 100;

fn histories() -> &'static Mutex<HashMap<(String, String), Stack>> {
    HISTORIES.get_or_init(|| Mutex::new(HashMap::new()))
}

fn persistence_lock() -> &'static Mutex<()> {
    PERSISTENCE.get_or_init(|| Mutex::new(()))
}

pub(crate) fn record(session_id: &str, agent_id: &str, tx: Transaction) -> Result<(), String> {
    {
        let mut all = histories().lock().unwrap();
        let stack = all
            .entry((session_id.to_string(), agent_id.to_string()))
            .or_default();
        stack.undo.push(tx);
        stack.redo.clear();
        // Keep long-running autonomous agents bounded while still retaining a
        // useful trail for interactive review.
        if stack.undo.len() > MAX_TRANSACTIONS {
            stack.undo.remove(0);
        }
    }
    persist(session_id)
}

pub(crate) fn take_undo(session_id: &str, agent_id: &str) -> Option<Transaction> {
    let mut all = histories().lock().unwrap();
    let result = all
        .get_mut(&(session_id.to_string(), agent_id.to_string()))
        .and_then(|stack| stack.undo.pop());
    drop(all);
    if result.is_some() {
        let _ = persist(session_id);
    }
    result
}

pub(crate) fn take_redo(session_id: &str, agent_id: &str) -> Option<Transaction> {
    let mut all = histories().lock().unwrap();
    let result = all
        .get_mut(&(session_id.to_string(), agent_id.to_string()))
        .and_then(|stack| stack.redo.pop());
    drop(all);
    if result.is_some() {
        let _ = persist(session_id);
    }
    result
}

pub(crate) fn push_redo(session_id: &str, agent_id: &str, tx: Transaction) {
    {
        let mut all = histories().lock().unwrap();
        all.entry((session_id.to_string(), agent_id.to_string()))
            .or_default()
            .redo
            .push(tx);
        let stack = all
            .get_mut(&(session_id.to_string(), agent_id.to_string()))
            .expect("stack inserted above");
        if stack.redo.len() > MAX_TRANSACTIONS {
            stack.redo.remove(0);
        }
    }
    let _ = persist(session_id);
}

pub(crate) fn push_undo(session_id: &str, agent_id: &str, tx: Transaction) {
    {
        let mut all = histories().lock().unwrap();
        all.entry((session_id.to_string(), agent_id.to_string()))
            .or_default()
            .undo
            .push(tx);
        let stack = all
            .get_mut(&(session_id.to_string(), agent_id.to_string()))
            .expect("stack inserted above");
        if stack.undo.len() > MAX_TRANSACTIONS {
            stack.undo.remove(0);
        }
    }
    let _ = persist(session_id);
}

pub(crate) fn counts(session_id: &str, agent_id: &str) -> (usize, usize) {
    let all = histories().lock().unwrap();
    all.get(&(session_id.to_string(), agent_id.to_string()))
        .map(|s| (s.undo.len(), s.redo.len()))
        .unwrap_or_default()
}

#[derive(Serialize, Deserialize)]
struct PersistedAgentHistory {
    agent_id: String,
    stack: Stack,
}

fn history_path(session_id: &str) -> std::path::PathBuf {
    crate::persistence::sessions_dir().join(format!("{session_id}.edits.json"))
}

fn persist(session_id: &str) -> Result<(), String> {
    let _persist_guard = persistence_lock().lock().unwrap();
    let all = histories().lock().unwrap();
    let records: Vec<_> = all
        .iter()
        .filter(|((sid, _), _)| sid == session_id)
        .map(|((_, agent_id), stack)| PersistedAgentHistory {
            agent_id: agent_id.clone(),
            stack: Stack {
                undo: stack.undo.clone(),
                redo: stack.redo.clone(),
            },
        })
        .collect();
    drop(all);
    let path = history_path(session_id);
    std::fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    // Use a unique same-directory temporary file so an interrupted writer
    // cannot collide with another session update. The final rename remains
    // atomic on the local filesystems Firmius supports.
    let tmp = path.with_extension(format!(
        "json.tmp.{}.{}",
        std::process::id(),
        uuid::Uuid::new_v4()
    ));
    std::fs::write(
        &tmp,
        serde_json::to_vec(&records).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    std::fs::rename(tmp, path).map_err(|e| e.to_string())
}

pub(crate) fn restore(session_id: &str) {
    let path = history_path(session_id);
    let Ok(bytes) = std::fs::read(path) else {
        return;
    };
    let Ok(records) = serde_json::from_slice::<Vec<PersistedAgentHistory>>(&bytes) else {
        return;
    };
    let mut all = histories().lock().unwrap();
    for record in records {
        all.insert((session_id.to_string(), record.agent_id), record.stack);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tx(label: &str) -> Transaction {
        Transaction {
            label: label.into(),
            entries: vec![Entry {
                path: "a.txt".into(),
                before: Some(b"old".to_vec()),
                after: Some(b"new".to_vec()),
            }],
        }
    }

    #[test]
    fn histories_are_isolated_per_agent_and_redo_is_separate() {
        let session = format!("history-test-{}", uuid::Uuid::new_v4());
        let _ = record(&session, "a", tx("a1"));
        let _ = record(&session, "b", tx("b1"));
        assert_eq!(counts(&session, "a"), (1, 0));
        assert_eq!(counts(&session, "b"), (1, 0));
        let a = take_undo(&session, "a").unwrap();
        push_redo(&session, "a", a);
        assert_eq!(counts(&session, "a"), (0, 1));
        assert_eq!(counts(&session, "b"), (1, 0));
    }

    #[test]
    fn new_edit_clears_redo() {
        let session = format!("history-test-{}", uuid::Uuid::new_v4());
        let _ = record(&session, "a", tx("a1"));
        let old = take_undo(&session, "a").unwrap();
        push_redo(&session, "a", old);
        let _ = record(&session, "a", tx("a2"));
        assert_eq!(counts(&session, "a"), (1, 0));
    }

    #[test]
    fn persisted_history_restores_each_agent_stack() {
        let session = format!("history-test-{}", uuid::Uuid::new_v4());
        record(&session, "a", tx("a1")).unwrap();
        let undone = take_undo(&session, "a").unwrap();
        push_redo(&session, "a", undone);
        {
            let mut all = histories().lock().unwrap();
            all.retain(|(sid, _), _| sid != &session);
        }
        assert_eq!(counts(&session, "a"), (0, 0));
        restore(&session);
        assert_eq!(counts(&session, "a"), (0, 1));
        let _ = std::fs::remove_file(history_path(&session));
    }

    #[test]
    fn concurrent_agents_keep_one_complete_session_sidecar() {
        let session = format!("history-test-{}", uuid::Uuid::new_v4());
        let mut workers = Vec::new();
        for index in 0..8 {
            let session = session.clone();
            workers.push(std::thread::spawn(move || {
                record(&session, &format!("agent-{index}"), tx("parallel")).unwrap();
            }));
        }
        for worker in workers {
            worker.join().unwrap();
        }

        {
            let mut all = histories().lock().unwrap();
            all.retain(|(sid, _), _| sid != &session);
        }
        restore(&session);
        for index in 0..8 {
            assert_eq!(counts(&session, &format!("agent-{index}")), (1, 0));
        }
        let _ = std::fs::remove_file(history_path(&session));
    }
}
