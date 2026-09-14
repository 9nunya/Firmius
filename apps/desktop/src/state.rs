//! Desktop application model. This is deliberately independent from
//! Slint so event handlers and background workers can share it safely.

#[derive(Default)]
pub(crate) struct DesktopState {
    /// One fixed connection per session plus an application catalog connection.
    /// Multiple tabs and viewports share the session connection and its model.
    pub(crate) clients: std::collections::HashMap<String, firmius_client::DaemonClient>,
    pub(crate) models: std::collections::HashMap<String, crate::session_model::SessionModel>,
    pub(crate) shell: crate::shell::Shell,
    pub(crate) documents:
        std::collections::HashMap<(Option<String>, String), Vec<crate::shell::DocumentRow>>,
    pub(crate) snapshots: std::collections::HashMap<String, firmius_protocol::SessionSnapshot>,
    pub(crate) output_requests:
        std::collections::HashMap<(String, String), firmius_protocol::Request>,
    pub(crate) model_picker_target: Option<String>,
    pub(crate) pending_permissions: Vec<firmius_core::PendingPermissionRequest>,
    pub(crate) permission_policies:
        std::collections::HashMap<String, firmius_core::PermissionPolicy>,
    pub(crate) last_goal_id: Option<firmius_core::GoalId>,
    pub(crate) last_workflow_path: Option<std::path::PathBuf>,
}

impl DesktopState {
    pub(crate) fn active_session(&self) -> Option<String> {
        self.shell.active().and_then(|t| t.route.session.clone())
    }
    pub(crate) fn focused_agent(&self) -> Option<String> {
        self.shell.active().and_then(|t| t.route.agent.clone())
    }
    pub(crate) fn active_view(&self) -> &str {
        self.shell
            .active()
            .map(|t| t.route.view.name())
            .unwrap_or("conversation")
    }

    /// Cache background-session snapshots without changing viewport selection.
    /// Sequence comparisons are scoped to a session and reset on daemon epochs.
    pub(crate) fn accept_snapshot(&mut self, snapshot: &firmius_protocol::SessionSnapshot) -> bool {
        if self
            .snapshots
            .get(&snapshot.session_id)
            .is_some_and(|previous| previous.sequence > snapshot.sequence)
        {
            return false;
        }
        self.models
            .entry(snapshot.session_id.clone())
            .or_default()
            .reconcile(snapshot);
        for pane in &mut self.shell.viewports {
            for tab in &mut pane.tabs {
                if tab.route.session.as_deref() == Some(&snapshot.session_id)
                    && tab.route.view == crate::shell::View::Conversation
                {
                    tab.title = tab
                        .route
                        .agent
                        .as_ref()
                        .and_then(|id| snapshot.agents.iter().find(|a| a.record.id == *id))
                        .and_then(|a| a.record.label.clone())
                        .or_else(|| snapshot.title.clone())
                        .unwrap_or_else(|| "Session".into());
                }
            }
        }
        let mut visible = snapshot.clone();
        if let Some(model) = self.models.get(&snapshot.session_id) {
            // Replay can finish after the snapshot's watermark. Keep its tail
            // visible even when the receiver later discards those duplicates.
            visible.live_events.extend(
                model
                    .events
                    .range(snapshot.sequence.saturating_add(1)..)
                    .map(|(_, event)| event.clone()),
            );
        }
        self.snapshots.insert(snapshot.session_id.clone(), visible);
        true
    }

    /// Merge the daemon's hot-path projection without cloning or replacing
    /// durable agent histories and live event tails.
    pub(crate) fn apply_status(&mut self, status: &firmius_protocol::SessionStatus) -> bool {
        let Some(snapshot) = self.snapshots.get_mut(&status.session_id) else {
            return false;
        };
        if status.sequence < snapshot.sequence {
            return false;
        }
        snapshot.title = status.title.clone();
        snapshot.sequence = status.sequence;
        snapshot.primary_agent_id = status.primary_agent_id.clone();
        snapshot.hierarchy = status.hierarchy.clone();
        snapshot.work = status.work.clone();
        snapshot.active_turns = status.active_turns.clone();
        snapshot.active_delegates = status.active_delegates;
        for agent in &status.agents {
            if let Some(existing) = snapshot.agents.iter_mut().find(|a| a.record.id == agent.id) {
                existing.record.provider_id = agent.provider_id.clone();
                existing.record.model = agent.model.clone();
                existing.record.effort = agent.effort.clone();
                existing.record.workdir = agent.workdir.clone();
                existing.record.label = agent.label.clone();
                existing.usage = agent.usage.clone();
                existing.total_usage = agent.total_usage.clone();
                existing.busy = agent.busy;
                existing.processes = agent.processes.clone();
                existing.todo = agent.todo.clone();
            } else {
                snapshot.agents.push(firmius_protocol::AgentSnapshot {
                    record: firmius_core::AgentRecord {
                        id: agent.id.clone(),
                        provider_id: agent.provider_id.clone(),
                        model: agent.model.clone(),
                        effort: agent.effort.clone(),
                        system_prompt: None,
                        persona: None,
                        temperature: None,
                        max_tokens: None,
                        workdir: agent.workdir.clone(),
                        label: agent.label.clone(),
                        metadata: Default::default(),
                        history: Default::default(),
                        mailbox: Vec::new(),
                        active_goal_id: None,
                        todo: Default::default(),
                        compaction: None,
                    },
                    usage: agent.usage.clone(),
                    total_usage: agent.total_usage.clone(),
                    busy: agent.busy,
                    processes: agent.processes.clone(),
                    todo: agent.todo.clone(),
                });
            }
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn snapshot(id: &str, sequence: u64) -> firmius_protocol::SessionSnapshot {
        firmius_protocol::SessionSnapshot {
            session_id: id.into(),
            sequence,
            title: None,
            primary_agent_id: "lead".into(),
            agents: vec![],
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new(id, sequence, Default::default()),
            active_turns: Default::default(),
            active_delegates: 0,
            live_events: vec![],
        }
    }
    #[test]
    fn late_previous_session_and_older_events_cannot_replace_visible_state() {
        let mut state = DesktopState::default();
        state.shell.open(
            crate::shell::Route {
                session: Some("current".into()),
                agent: None,
                view: crate::shell::View::Conversation,
            },
            "Current".into(),
        );
        assert!(state.accept_snapshot(&snapshot("current", 20)));
        assert!(state.accept_snapshot(&snapshot("previous", 30)));
        assert!(!state.accept_snapshot(&snapshot("current", 19)));
        assert!(state.accept_snapshot(&snapshot("current", 20))); // focused-agent re-projection
        assert_eq!(state.snapshots["current"].sequence, 20);
        assert_eq!(state.active_session().as_deref(), Some("current"));
    }
    #[test]
    fn replay_tail_remains_visible_when_snapshot_watermark_is_older() {
        let mut state = DesktopState::default();
        let event = firmius_core::SessionEvent {
            session_id: "s".into(),
            sequence: 11,
            at: chrono::Utc::now(),
            payload: firmius_core::SessionEventPayload::Agent {
                agent_id: "lead".into(),
                event: firmius_core::AgentEvent::Text("new delta".into()),
            },
        };
        state
            .models
            .entry("s".into())
            .or_default()
            .receive(event.clone());
        state.accept_snapshot(&snapshot("s", 10));
        assert_eq!(state.snapshots["s"].live_events.len(), 1);
        assert!(!state.models.get_mut("s").unwrap().receive(event));
        assert_eq!(state.snapshots["s"].live_events[0].sequence, 11);
    }
}
