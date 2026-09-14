//! Lossless received-event journal and keyed tool lifecycle projection.
//! Transport order and domain identity belong here, never in Slint presenters.
use firmius_core::{AgentEvent, MessagePart, SessionEvent, SessionEventPayload};
use firmius_protocol::SessionSnapshot;
use std::collections::{BTreeMap, HashMap};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Phase {
    Preparing,
    Running,
    Completed,
    Failed,
    Interrupted,
}
#[derive(Clone, Debug)]
pub(crate) struct Tool {
    pub key: String,
    pub agent: String,
    pub id: String,
    pub name: String,
    pub args: String,
    pub phase: Phase,
    pub result: Option<String>,
    pub first_sequence: Option<u64>,
    pub last_sequence: Option<u64>,
    /// Live resource state is independent of whether the tool call returned.
    pub resources: BTreeMap<String, firmius_core::ToolRuntimeResource>,
}
#[derive(Default)]
pub(crate) struct SessionModel {
    pub process_output: HashMap<(String, String), (usize, Vec<u8>)>,
    pub sequence: u64,
    pub replay_sequence: u64,
    pub history_gap: Option<(u64, u64)>,
    pub events: BTreeMap<u64, SessionEvent>,
    pub tools: Vec<Tool>,
    slots: HashMap<(String, u32), usize>,
    pub needs_snapshot: bool,
}
impl SessionModel {
    fn tool(&mut self, agent: &str, index: u32, id: &str, sequence: u64) -> &mut Tool {
        let existing = (!id.is_empty())
            .then(|| {
                self.tools
                    .iter()
                    .position(|t| t.agent == agent && t.id == id)
            })
            .flatten()
            .or_else(|| {
                self.slots.get(&(agent.into(), index)).copied().filter(|i| {
                    matches!(self.tools[*i].phase, Phase::Preparing | Phase::Running)
                        && (self.tools[*i].id.is_empty()
                            || id.is_empty()
                            || self.tools[*i].id == id)
                })
            });
        let i = existing.unwrap_or_else(|| {
            let i = self.tools.len();
            self.tools.push(Tool {
                key: format!("{agent}:tool:{sequence}"),
                agent: agent.into(),
                id: id.into(),
                name: String::new(),
                args: String::new(),
                phase: Phase::Preparing,
                result: None,
                first_sequence: None,
                last_sequence: Some(sequence),
                resources: BTreeMap::new(),
            });
            i
        });
        self.slots.insert((agent.into(), index), i);
        let t = &mut self.tools[i];
        if !id.is_empty() {
            t.id = id.into();
        }
        t.last_sequence = Some(t.last_sequence.unwrap_or(0).max(sequence));
        t
    }
    fn fold(&mut self, envelope: &SessionEvent) {
        let SessionEventPayload::Agent { agent_id, event } = &envelope.payload else {
            return;
        };
        match event {
            AgentEvent::ProcessOutput { .. } => {}
            AgentEvent::ToolRuntime { id, resource } => {
                let t = self.tool(agent_id, u32::MAX, id, envelope.sequence);
                let key = match resource {
                    firmius_core::ToolRuntimeResource::Process { id, .. } => {
                        format!("process:{id}")
                    }
                    firmius_core::ToolRuntimeResource::Delegate { id, .. } => {
                        format!("delegate:{id}")
                    }
                };
                t.resources.insert(key, resource.clone());
            }
            AgentEvent::ToolCallDelta {
                index,
                id,
                name_delta,
                args_delta,
            } => {
                let t = self.tool(agent_id, *index, id, envelope.sequence);
                t.first_sequence = Some(
                    t.first_sequence
                        .unwrap_or(envelope.sequence)
                        .min(envelope.sequence),
                );
                t.name.push_str(name_delta);
                t.args.push_str(args_delta);
            }
            AgentEvent::ToolCallStarted {
                index,
                id,
                name,
                args,
            } => {
                let t = self.tool(agent_id, *index, id, envelope.sequence);
                t.name = name.clone();
                t.args = args.clone();
                t.phase = Phase::Running;
            }
            AgentEvent::ToolResult {
                index,
                id,
                name,
                ok,
                content,
            } => {
                let t = self.tool(agent_id, *index, id, envelope.sequence);
                t.name = name.clone();
                t.result = Some(content.clone());
                t.phase = if *ok { Phase::Completed } else { Phase::Failed };
            }
            AgentEvent::TurnFinished => {
                self.slots.retain(|(agent, _), _| agent != agent_id);
            }
            _ => {}
        }
    }
    pub fn receive(&mut self, event: SessionEvent) -> bool {
        if let SessionEventPayload::Agent {
            agent_id,
            event: AgentEvent::ProcessOutput { id, bytes, total },
        } = &event.payload
        {
            let entry = self
                .process_output
                .entry((agent_id.clone(), id.clone()))
                .or_default();
            if *total <= entry.0 {
                return false;
            }
            let start = total.saturating_sub(bytes.len());
            if start > entry.0 {
                entry.1.clear();
            }
            let overlap = entry.0.saturating_sub(start).min(bytes.len());
            entry.1.extend_from_slice(&bytes[overlap..]);
            entry.0 = *total;
            // This is only the inline preview; the daemon owns the complete
            // process buffer, available through the output inspector.
            if entry.1.len() > 131_072 {
                entry.1.drain(..entry.1.len() - 131_072);
            }
            self.sequence = self.sequence.max(event.sequence);
            return true;
        }
        self.receive_batch(vec![event])
    }
    pub fn receive_batch(&mut self, events: Vec<SessionEvent>) -> bool {
        let mut fresh: Vec<_> = events
            .into_iter()
            .filter(|e| !self.events.contains_key(&e.sequence))
            .collect();
        if fresh.is_empty() {
            return false;
        }
        fresh.sort_by_key(|e| e.sequence);
        let reordered = fresh[0].sequence < self.sequence;
        for event in fresh {
            if event.sequence != self.sequence + 1 {
                self.needs_snapshot = true;
            }
            self.sequence = self.sequence.max(event.sequence);
            if !reordered {
                self.fold(&event);
            }
            self.events.insert(event.sequence, event);
        }
        if reordered {
            // Recovery may fill a hole before already-received events. Fold in
            // transport order, once, so old deltas cannot append to final args.
            let previous = std::mem::take(&mut self.tools);
            self.slots.clear();
            for event in self.events.values().cloned().collect::<Vec<_>>() {
                self.fold(&event);
            }
            for old in previous {
                if let Some(tool) = self.tools.iter_mut().find(|t| {
                    t.agent == old.agent
                        && ((!old.id.is_empty() && t.id == old.id) || t.key == old.key)
                }) {
                    tool.key = old.key;
                    if tool.name.is_empty() {
                        tool.name = old.name;
                    }
                    if tool.args.is_empty() {
                        tool.args = old.args;
                    }
                    if tool.result.is_none() && old.result.is_some() {
                        tool.result = old.result;
                        tool.phase = old.phase;
                    }
                } else if old.last_sequence.is_none() {
                    self.tools.push(old);
                }
            }
        }
        true
    }
    pub fn reconcile(&mut self, snapshot: &SessionSnapshot) {
        self.receive_batch(snapshot.live_events.clone());
        for agent in &snapshot.agents {
            for message in &agent.record.history {
                for part in &message.content {
                    match part {
                        MessagePart::ToolCall { id, name, args } => {
                            if let Some(t) = self
                                .tools
                                .iter_mut()
                                .find(|t| t.agent == agent.record.id && t.id == *id)
                            {
                                t.name = name.clone();
                                t.args = args.clone();
                            } else {
                                self.tools.push(Tool {
                                    key: format!("{}:call:{id}", agent.record.id),
                                    agent: agent.record.id.clone(),
                                    id: id.clone(),
                                    name: name.clone(),
                                    args: args.clone(),
                                    phase: Phase::Running,
                                    result: None,
                                    first_sequence: None,
                                    last_sequence: None,
                                    resources: BTreeMap::new(),
                                });
                            }
                        }
                        MessagePart::ToolResult { id, content, ok } => {
                            if let Some(t) = self
                                .tools
                                .iter_mut()
                                .find(|t| t.agent == agent.record.id && t.id == *id)
                            {
                                t.result = Some(content.clone());
                                t.phase = if *ok { Phase::Completed } else { Phase::Failed };
                            }
                        }
                        _ => {}
                    }
                }
            }
        }
        for tool in &mut self.tools {
            if matches!(tool.phase, Phase::Preparing | Phase::Running)
                && !snapshot.active_turns.contains_key(&tool.agent)
                && !snapshot
                    .agents
                    .iter()
                    .any(|a| a.record.id == tool.agent && a.busy)
                && tool.last_sequence.unwrap_or(0) <= snapshot.sequence
            {
                tool.phase = Phase::Interrupted;
            }
            for resource in tool.resources.values_mut() {
                if let firmius_core::ToolRuntimeResource::Process { id, status, .. } = resource {
                    if let Some(process) = snapshot
                        .agents
                        .iter()
                        .find(|a| a.record.id == tool.agent)
                        .and_then(|a| a.processes.iter().find(|p| p.id.to_string() == *id))
                    {
                        *status = process.status;
                    }
                }
            }
        }
        self.sequence = self.sequence.max(snapshot.sequence);
        self.needs_snapshot = false;
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn event(n: u64, event: AgentEvent) -> SessionEvent {
        SessionEvent {
            session_id: "s".into(),
            sequence: n,
            at: chrono::Utc::now(),
            payload: SessionEventPayload::Agent {
                agent_id: "a".into(),
                event,
            },
        }
    }
    #[test]
    fn first_delta_keeps_identity_when_id_arrives_and_index_repeats() {
        let mut m = SessionModel::default();
        m.receive(event(
            1,
            AgentEvent::ToolCallDelta {
                index: 0,
                id: "".into(),
                name_delta: "ba".into(),
                args_delta: "{".into(),
            },
        ));
        let key = m.tools[0].key.clone();
        m.receive(event(
            2,
            AgentEvent::ToolCallDelta {
                index: 0,
                id: "call".into(),
                name_delta: "sh".into(),
                args_delta: "}".into(),
            },
        ));
        m.receive(event(
            3,
            AgentEvent::ToolCallStarted {
                index: 0,
                id: "call".into(),
                name: "bash".into(),
                args: "{}".into(),
            },
        ));
        assert_eq!(m.tools.len(), 1);
        assert_eq!(m.tools[0].key, key);
        assert_eq!(m.tools[0].phase, Phase::Running);
        let result = event(
            4,
            AgentEvent::ToolResult {
                index: 0,
                id: "call".into(),
                name: "bash".into(),
                ok: true,
                content: "done".into(),
            },
        );
        assert!(m.receive(result.clone()));
        assert!(!m.receive(result));
        assert_eq!(m.tools[0].first_sequence, Some(1));
        // Provider completion precedes tool execution; a settled slot must
        // also be safe when no later TurnFinished clears it.
        m.receive(event(5, AgentEvent::Text("next generation".into())));
        m.receive(event(
            6,
            AgentEvent::ToolCallDelta {
                index: 0,
                id: "".into(),
                name_delta: "read".into(),
                args_delta: "".into(),
            },
        ));
        assert_eq!(m.tools.len(), 2);
        assert_eq!(m.tools[0].phase, Phase::Completed);
        m.receive(event(8, AgentEvent::Text("gap".into())));
        assert!(m.needs_snapshot);
    }
    #[test]
    fn replay_fills_holes_without_regressing_terminal_state() {
        let mut m = SessionModel::default();
        m.receive(event(
            3,
            AgentEvent::ToolResult {
                index: 0,
                id: "c".into(),
                name: "bash".into(),
                ok: true,
                content: "started".into(),
            },
        ));
        let key = m.tools[0].key.clone();
        m.receive_batch(vec![
            event(
                1,
                AgentEvent::ToolCallDelta {
                    index: 0,
                    id: "c".into(),
                    name_delta: "bash".into(),
                    args_delta: "{}".into(),
                },
            ),
            event(
                2,
                AgentEvent::ToolCallStarted {
                    index: 0,
                    id: "c".into(),
                    name: "bash".into(),
                    args: "{}".into(),
                },
            ),
        ]);
        assert_eq!(m.tools.len(), 1);
        assert_eq!(m.tools[0].key, key);
        assert_eq!(m.tools[0].args, "{}");
        assert_eq!(m.tools[0].phase, Phase::Completed);
        assert_eq!(m.tools[0].first_sequence, Some(1));
        m.receive(event(
            4,
            AgentEvent::ToolRuntime {
                id: "c".into(),
                resource: firmius_core::ToolRuntimeResource::Process {
                    id: "p".into(),
                    mode: "spawn".into(),
                    status: firmius_core::ProcStatus::Running,
                },
            },
        ));
        assert_eq!(m.tools[0].phase, Phase::Completed);
        assert!(matches!(
            m.tools[0].resources["process:p"],
            firmius_core::ToolRuntimeResource::Process {
                status: firmius_core::ProcStatus::Running,
                ..
            }
        ));
        m.receive(event(
            5,
            AgentEvent::ToolRuntime {
                id: "c".into(),
                resource: firmius_core::ToolRuntimeResource::Process {
                    id: "p".into(),
                    mode: "spawn".into(),
                    status: firmius_core::ProcStatus::Exited {
                        code: 0,
                        success: true,
                    },
                },
            },
        ));
        assert_eq!(m.tools[0].resources.len(), 1);
        assert!(matches!(
            m.tools[0].resources["process:p"],
            firmius_core::ToolRuntimeResource::Process {
                status: firmius_core::ProcStatus::Exited { .. },
                ..
            }
        ));
    }
}
