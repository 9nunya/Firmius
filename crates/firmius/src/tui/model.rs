//! App state and the pure update function. One match block for keys, one
//! fold function for agent events, no I/O anywhere in this file.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossterm::event::{
    Event as TermEvent, KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseEvent, MouseEventKind,
};
use firmius_client::DaemonClient;
use firmius_core::partial_json::PartialJson;
use firmius_core::{
    AccountRecord, Agent, AgentConfig, AgentError, AgentEvent, Context, EffortMode, FirmiusConfig,
    McpManager, Message, MessageOrigin, MessagePart, MessageRole, ModelCapability,
    PendingPermissionRequest, PermissionMode, PermissionPolicy, PersonaManager, PersonaUse, ProcId,
    ProcInfo, ProviderManager, QuotaSnapshot, Session, SessionEvent, SessionHandle, ToolRegistry,
    UserSettings, WebSearchAction, WebSearchMode, WorkSnapshot,
};
use firmius_protocol::SessionSnapshot;
use ratatui::text::Line;
use tokio_util::sync::CancellationToken;

use super::command;
use super::composer::{Composer, ComposerSubmission, PASTE_BLOCK_THRESHOLD, StoredPaste};
use super::event::AppEvent;
use super::modal::ModalSurface;
use super::presentation::{
    ArrivalCueTracker, DisclosureMode, HitSubtarget, PresentationSettings, SemanticId,
    TranscriptEvent,
};
use super::run::{self, RunLiveness};
use super::runtime_state::{GoalValidationAdapter, ToolExecutionState};
use super::theme::{self, Theme};
use super::todo;
use super::work;

// ---------------------------------------------------------------------------
// Transcript items — the data every renderer consumes
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub enum ToolState {
    /// The provider is still streaming the tool name/arguments.
    Preparing(Instant),
    Running(Instant),
    Done {
        ok: bool,
        bytes: usize,
        /// Tool-specific failure payload retained for the presenter.
        error: Option<String>,
    },
    /// Call recorded in persisted history with no result (turn was cut).
    Interrupted,
}

fn append_work_completion_summaries(
    snapshot: &WorkSnapshot,
    transcripts: &mut HashMap<String, Vec<Item>>,
    primary_id: &str,
) {
    for graph in snapshot.state.graphs.values() {
        if graph.status != firmius_core::GraphStatus::Completed {
            continue;
        }
        let key = graph.id.to_string();
        let already_present = transcripts.values().any(|items| {
            items.iter().any(|item| match item {
                Item::Note(text) => workflow_summary_key(text) == Some(key.as_str()),
                _ => false,
            })
        });
        if already_present {
            continue;
        }
        let agent_id = graph
            .owner_agent_id
            .as_deref()
            .unwrap_or(primary_id)
            .to_string();
        transcripts
            .entry(agent_id)
            .or_default()
            .push(workflow_summary_item(graph));
    }
}

fn workflow_summary_key(text: &str) -> Option<&str> {
    text.strip_prefix(WORKFLOW_SUMMARY_MARKER)
        .and_then(|rest| rest.split_once('\n'))
        .map(|(key, _)| key)
}

fn workflow_summary_item(graph: &firmius_core::WorkGraph) -> Item {
    let completed = graph
        .view_order
        .iter()
        .filter_map(|id| graph.nodes.get(id))
        .filter(|node| {
            matches!(
                node.status,
                firmius_core::ExecutionStatus::Succeeded
                    | firmius_core::ExecutionStatus::Cancelled
                    | firmius_core::ExecutionStatus::Skipped
            )
        })
        .count();
    Item::Note(format!(
        "{WORKFLOW_SUMMARY_MARKER}{}\n✓ Workflow complete: {} · {completed} completed",
        graph.id, graph.title
    ))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PhraseScenario {
    Thinking,
    Writing,
    PreparingEdit,
    PreparingTask,
    WaitingDelegate,
    WaitingWorkgraph,
    WaitingProcess,
    RunningTool,
}

const THINKING_PHRASES: &[&str] = &[
    "Reasoning through this with precision..",
    "Thinking about it...",
    "Consulting my experts..",
    "Let me think about that..",
];
const WRITING_PHRASES: &[&str] = &[
    "Writing this up..",
    "Flowing the tokens..",
    "Emitting the bytes..",
];
const EDIT_PHRASES: &[&str] = &[
    "Making sure this edit works..",
    "Lining up the diff confetti..",
    "Putting the latent into a patch..",
];
const TASK_PHRASES: &[&str] = &[
    "Utilizing the orchestra..",
    "Building a durable graph..",
    "Setting the node links..",
];
const DELEGATE_PHRASES: &[&str] = &[
    "Vibecoding just a bit..",
    "Waiting on an agent..",
    "Hatching a helper..",
];
const WORKGRAPH_PHRASES: &[&str] = &[
    "Breaking open the bee hive..",
    "Unleashing the storm- No, wait, I mean swarm..",
    "I'm letting the dogs out..",
];
const PROCESS_PHRASES: &[&str] = &[
    "Tempting the terminal to terminate...",
    "Speaking to the machine..",
    "Telling bash to bust the beans..",
];
const TOOL_PHRASES: &[&str] = &[
    "I'm doing something, I promise..",
    "Pumping the function call..",
    "Sending input data..",
];

fn phrase_catalogue(scenario: PhraseScenario) -> &'static [&'static str] {
    match scenario {
        PhraseScenario::Thinking => THINKING_PHRASES,
        PhraseScenario::Writing => WRITING_PHRASES,
        PhraseScenario::PreparingEdit => EDIT_PHRASES,
        PhraseScenario::PreparingTask => TASK_PHRASES,
        PhraseScenario::WaitingDelegate => DELEGATE_PHRASES,
        PhraseScenario::WaitingWorkgraph => WORKGRAPH_PHRASES,
        PhraseScenario::WaitingProcess => PROCESS_PHRASES,
        PhraseScenario::RunningTool => TOOL_PHRASES,
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionCompletionState {
    Loading,
    Ready,
}

#[derive(Debug, Clone)]
pub struct TranscriptHitRegion {
    pub event_id: SemanticId,
    pub subtarget: HitSubtarget,
    /// The disclosure affordance occupies only the header's semantic span;
    /// text outside this interval remains available for terminal selection.
    pub left: u16,
    pub right: u16,
    pub top: u16,
    pub bottom: u16,
}

fn default_system_prompt() -> &'static str {
    firmius_core::prompts::OPERATING_PROMPT
}

/// Housekeeping refreshes touch every live agent and may snapshot histories,
/// so they must not run at the animation frame rate.  Input and bus events
/// still update the model immediately; this only gates the best-effort
/// roster/process/usage refresh performed by `refresh_async`.
pub(crate) const ASYNC_REFRESH_INTERVAL: Duration = Duration::from_millis(100);

pub(crate) fn async_refresh_due(last: Option<Instant>, now: Instant) -> bool {
    last.is_none_or(|at| now.duration_since(at) >= ASYNC_REFRESH_INTERVAL)
}

/// Hosted search presentation state. Distinct from [`ToolState`] so a search
/// item can never be confused with `Item::ToolCall { name: "web_search" }`.
#[derive(Debug, Clone)]
pub enum SearchState {
    Preparing(Instant),
    Done,
    Interrupted,
}

#[derive(Debug, Clone)]
pub enum CompactionPhase {
    Scheduled,
    Running(Instant),
    Finished,
    Discarded,
    Failed(String),
}

#[derive(Debug, Clone)]
pub struct CompactionItem {
    pub generation: u64,
    pub summary: String,
    pub phase: CompactionPhase,
}

/// Extract the stable `key=value` ids returned by process/delegate tools.
pub fn result_field(result: &str, field: &str) -> Option<String> {
    result.lines().find_map(|line| {
        line.split_whitespace().find_map(|word| {
            word.strip_prefix(&format!("{field}="))
                .filter(|value| !value.is_empty())
                .map(str::to_string)
        })
    })
}

#[derive(Debug, Clone)]
pub enum Item {
    User(String),
    Text(String),
    /// A message delivered by another agent. The sender id is durable; the
    /// renderer resolves its current roster label without parsing the body.
    AgentMessage {
        sender_id: String,
        text: String,
    },
    /// A system/control-plane message. This is intentionally separate from
    /// ordinary notes so system autohide cannot depend on text markers.
    SystemMessage {
        text: String,
    },
    /// A durable assignment result delivered to an agent. Assignment
    /// identity remains available even when the human-readable summary changes.
    AssignmentCompletion {
        child_agent_id: String,
        assignment_id: String,
        text: String,
    },
    /// A reasoning block. A block is active only while it is the transcript
    /// tail of a busy turn; its lifecycle is structural rather than timed.
    Thinking {
        text: String,
    },
    ToolCall {
        /// Provider tool-call id, when available, used to merge streaming
        /// deltas into the eventual started/result presentation.
        stream_id: Option<String>,
        stream_index: u32,
        name: String,
        args: String,
        /// The tool result, when one has arrived. Bash uses this to resolve
        /// the process id; keeping it on the item also makes resumed/live
        /// rendering use the same correlation data.
        result: Option<String>,
        state: ToolState,
    },
    /// Hosted web search. A dedicated sibling of tool lines — never a ToolCall.
    WebSearch {
        id: String,
        action: WebSearchAction,
        state: SearchState,
    },
    Compaction(CompactionItem),
    Note(String),
}

/// Marker shared by Firmius-generated coordination messages. It is stripped
/// by the renderer and lets autohide remain independent of message wording.
pub const SYSTEM_MESSAGE_MARKER: &str = "\u{200b}firmius-system\u{200b}";

/// Prefix for TUI-owned workflow completion notes. The graph id is kept in
/// the private prefix so recovery can re-derive the note without appending a
/// duplicate, while the visible transcript contains only the human summary.
const WORKFLOW_SUMMARY_MARKER: &str = "\u{200b}firmius-workflow-summary\u{200b}";

pub fn marked_system_message(text: impl Into<String>) -> String {
    format!("{SYSTEM_MESSAGE_MARKER}{}", text.into())
}

pub fn is_system_message(text: &str) -> bool {
    text.starts_with(SYSTEM_MESSAGE_MARKER)
}

pub fn visible_message_text(text: &str) -> &str {
    if let Some(rest) = text.strip_prefix(WORKFLOW_SUMMARY_MARKER) {
        return rest.split_once('\n').map_or(rest, |(_, visible)| visible);
    }
    text.strip_prefix(SYSTEM_MESSAGE_MARKER).unwrap_or(text)
}

#[derive(Debug, Clone)]
pub struct CompletionItem {
    pub insert: String,
    pub label: String,
    pub detail: String,
}

#[derive(Debug, Clone)]
pub struct CompletionState {
    pub items: Vec<CompletionItem>,
    pub selected: usize,
}

#[derive(Debug, Clone)]
pub enum LivePhraseAnim {
    Steady,
    FadingOut {
        from: String,
        to: String,
        started_tick: usize,
    },
    FadingIn {
        to: String,
        started_tick: usize,
    },
}

/// Return a small score when `query` is a case-insensitive subsequence of
/// `candidate`. Lower scores are better, with contiguous and prefix matches
/// naturally winning over scattered matches.
fn fuzzy_score(query: &str, candidate: &str) -> Option<usize> {
    let query = query.to_lowercase();
    let candidate = candidate.to_lowercase();
    if query.is_empty() {
        return Some(0);
    }

    let mut cursor = 0;
    let mut score = 0;
    let mut previous = None;
    for needle in query.chars() {
        let relative = candidate[cursor..].find(needle)?;
        let found = cursor + relative;
        score += relative + found / 32;
        if let Some(previous) = previous
            && found == previous + 1
        {
            score = score.saturating_sub(2);
        };
        previous = Some(found);
        cursor = found + needle.len_utf8();
    }
    Some(score)
}

pub(super) fn effort_from_name(name: &str) -> EffortMode {
    EffortMode {
        name: name.to_string(),
        thinking_budget_tokens: None,
        reasoning_effort: Some(name.to_string()),
    }
}

fn summarize_user_message(message: &Message) -> Option<String> {
    let text = message
        .content
        .iter()
        .filter_map(|part| match part {
            MessagePart::Text(t) => Some(t.clone()),
            MessagePart::Image(_) => Some("[image]".to_string()),
            MessagePart::Thinking { .. }
            | MessagePart::ToolCall { .. }
            | MessagePart::ToolResult { .. }
            | MessagePart::WebSearch { .. } => None,
        })
        .collect::<Vec<_>>()
        .join("\n");
    (!text.is_empty()).then_some(text)
}

/// Fold one live agent event into a transcript.
pub fn fold_event(items: &mut Vec<Item>, ev: &AgentEvent) {
    match ev {
        AgentEvent::ToolRuntime { .. } => {},
        AgentEvent::ProcessOutput { .. } => {},
        AgentEvent::Thinking(d) => match items.last_mut() {
            Some(Item::Thinking { text }) => text.push_str(d),
            _ => items.push(Item::Thinking { text: d.clone() }),
        },
        AgentEvent::UserMessage(message) => {
            items.push(Item::User(message.clone()));
        }
        AgentEvent::InboundMessage { sender_id, message } => {
            let text = summarize_user_message(message).unwrap_or_default();
            if text.is_empty() {
                return;
            }
            match message.effective_provenance().origin {
                MessageOrigin::Assignment => {
                    let assignment_id = message
                        .correlation
                        .assignment_id
                        .clone()
                        .unwrap_or_default();
                    let child_agent_id = message
                        .correlation
                        .sender_id
                        .clone()
                        .filter(|id| !id.is_empty())
                        .unwrap_or_else(|| sender_id.clone());
                    if !items.iter().any(|item| {
                        matches!(item, Item::AssignmentCompletion { assignment_id: id, .. } if id == &assignment_id)
                    }) {
                        items.push(Item::AssignmentCompletion {
                            child_agent_id,
                            assignment_id,
                            text,
                        });
                    }
                }
                MessageOrigin::Peer => items.push(Item::AgentMessage {
                    sender_id: sender_id.clone(),
                    text,
                }),
                MessageOrigin::Legacy | MessageOrigin::PromptStack | MessageOrigin::Human => {
                    items.push(Item::User(text));
                }
                MessageOrigin::Assistant | MessageOrigin::Tool | MessageOrigin::Compaction => {
                    items.push(Item::SystemMessage { text });
                }
            }
        }
        // Permission lifecycle is rendered by the focused gate/control-plane
        // presenter. It is deliberately not duplicated as transcript notes
        // after every tool call.
        AgentEvent::PermissionRequested { .. } | AgentEvent::PermissionResolved { .. } => {}
        AgentEvent::Text(d) => {
            match items.last_mut() {
                Some(Item::Text(t)) => t.push_str(d),
                _ => items.push(Item::Text(d.clone())),
            }
        }
        AgentEvent::RetryScheduled {
            account_id,
            attempt,
            delay_ms,
            switched,
            class,
        } => {
            let action = if *switched {
                format!("switching to {account_id}")
            } else {
                format!("retrying on {account_id}")
            };
            let delay = if *delay_ms >= 1000 {
                format!("{:.2}s", *delay_ms as f64 / 1000.0)
            } else {
                format!("{delay_ms}ms")
            };
            items.push(Item::Note(marked_system_message(format!(
                "retry: {action} for attempt {attempt} after {} in {delay}",
                class.label()
            ))));
        }
        AgentEvent::ToolCallDelta {
            index,
            id,
            name_delta,
            args_delta,
        } => {
            // ToolCallStarted is emitted only after the provider has finished
            // streaming the assistant message. Create the running item now,
            // so the TUI can present the tool as soon as its first delta lands.
            //
            // Correlation is by stable id when the backend supplies one, else
            // by the generation-scoped tool index. A name-only fallback was
            // previously too loose: with several tools streaming in parallel,
            // an args delta for one call could land on another call's
            // `Preparing` placeholder, corrupting the rendered args.
            let existing = items.iter_mut().rev().find_map(|item| match item {
                Item::ToolCall {
                    stream_id,
                    stream_index,
                    state: ToolState::Preparing(_),
                    ..
                } if (!id.is_empty()
                    && (stream_id.as_deref() == Some(id.as_str())
                        || (stream_id.is_none() && *stream_index == *index)))
                    || (id.is_empty() && *stream_index == *index) =>
                {
                    Some(item)
                }
                _ => None,
            });
            if let Some(Item::ToolCall {
                stream_id,
                name,
                args,
                ..
            }) = existing
            {
                if stream_id.is_none() && !id.is_empty() {
                    *stream_id = Some(id.clone());
                }
                if !name_delta.is_empty() {
                    *name = name_delta.clone();
                }
                args.push_str(args_delta);
            } else {
                items.push(Item::ToolCall {
                    stream_id: (!id.is_empty()).then(|| id.clone()),
                    stream_index: *index,
                    name: name_delta.clone(),
                    args: args_delta.clone(),
                    result: None,
                    state: ToolState::Preparing(Instant::now()),
                });
            }
        }
        AgentEvent::ToolCallStarted {
            index,
            id,
            name,
            args,
        } => {
            let existing = reconcile_tool_item(items, id, name, false);
            if let Some(Item::ToolCall {
                stream_id,
                args: current_args,
                state,
                ..
            }) = existing
            {
                if stream_id.is_none() && !id.is_empty() {
                    *stream_id = Some(id.clone());
                }
                *current_args = args.clone();
                // A snapshot can contain the persisted result while the
                // corresponding lifecycle events are replayed from the
                // journal.  Never regress a terminal presenter to Running.
                if !matches!(state, ToolState::Done { .. }) {
                    *state = ToolState::Running(Instant::now());
                }
            } else {
                items.push(Item::ToolCall {
                    stream_id: (!id.is_empty()).then(|| id.clone()),
                    stream_index: *index,
                    name: name.clone(),
                    args: args.clone(),
                    result: None,
                    state: ToolState::Running(Instant::now()),
                });
            }
        }
        AgentEvent::ToolResult {
            index,
            id,
            name,
            ok,
            content,
        } => {
            let existing = reconcile_tool_item(items, id, name, true);
            match existing {
                Some(Item::ToolCall {
                    state,
                    result: stored_result,
                    ..
                }) => {
                    *stored_result = Some(content.clone());
                    *state = ToolState::Done {
                        ok: *ok,
                        bytes: content.len(),
                        error: (!*ok).then(|| content.clone()),
                    }
                }
                Some(_) => {}
                None => items.push(Item::ToolCall {
                    stream_id: (!id.is_empty()).then(|| id.clone()),
                    stream_index: *index,
                    name: name.clone(),
                    args: String::new(),
                    result: Some(content.clone()),
                    state: ToolState::Done {
                        ok: *ok,
                        bytes: content.len(),
                        error: (!*ok).then(|| content.clone()),
                    },
                }),
            }
        }
        AgentEvent::CompactionScheduled { generation } => {
            if !items.iter().rev().any(|item| {
                matches!(item, Item::Compaction(compaction) if compaction.generation == *generation)
            }) {
                items.push(Item::Compaction(CompactionItem {
                    generation: *generation,
                    summary: String::new(),
                    phase: CompactionPhase::Scheduled,
                }));
            }
        }
        AgentEvent::CompactionStarted { generation } => {
            update_compaction(items, *generation, |item| {
                item.phase = CompactionPhase::Running(Instant::now());
            });
        }
        AgentEvent::CompactionDelta { generation, delta } => {
            update_compaction(items, *generation, |item| {
                item.summary.push_str(delta);
                if !matches!(item.phase, CompactionPhase::Running(_)) {
                    item.phase = CompactionPhase::Running(Instant::now());
                }
            });
        }
        AgentEvent::CompactionFinished { generation } => {
            update_compaction(items, *generation, |item| {
                item.phase = CompactionPhase::Finished;
            });
        }
        AgentEvent::CompactionDiscarded { generation } => {
            update_compaction(items, *generation, |item| {
                item.phase = CompactionPhase::Discarded;
            });
        }
        AgentEvent::CompactionFailed { generation, error } => {
            update_compaction(items, *generation, |item| {
                item.phase = CompactionPhase::Failed(error.clone());
            });
        }
        AgentEvent::Usage(_) | AgentEvent::TurnFinished | AgentEvent::BusyChanged { .. } => {}
        AgentEvent::WebSearchStarted { id } => {
            let existing = items.iter_mut().rev().find(|item| match item {
                Item::WebSearch {
                    id: existing,
                    state: SearchState::Preparing(_),
                    ..
                } => existing == id,
                _ => false,
            });
            if existing.is_none() {
                items.push(Item::WebSearch {
                    id: id.clone(),
                    action: WebSearchAction::Other,
                    state: SearchState::Preparing(Instant::now()),
                });
            }
        }
        AgentEvent::WebSearchFinished { id, action } => {
            let existing = items.iter_mut().rev().find(|item| match item {
                Item::WebSearch { id: existing, .. } => existing == id,
                _ => false,
            });
            if let Some(Item::WebSearch {
                action: current,
                state,
                ..
            }) = existing
            {
                *current = action.clone();
                *state = SearchState::Done;
            } else {
                items.push(Item::WebSearch {
                    id: id.clone(),
                    action: action.clone(),
                    state: SearchState::Done,
                });
            }
        }
    }
}

fn update_compaction(
    items: &mut Vec<Item>,
    generation: u64,
    update: impl FnOnce(&mut CompactionItem),
) {
    if let Some(Item::Compaction(item)) = items.iter_mut().rev().find(
        |item| matches!(item, Item::Compaction(compaction) if compaction.generation == generation),
    ) {
        update(item);
    } else {
        let mut item = CompactionItem {
            generation,
            summary: String::new(),
            phase: CompactionPhase::Scheduled,
        };
        update(&mut item);
        items.push(Item::Compaction(item));
    }
}

/// Reconcile a streamed `Preparing` placeholder with a later finalized tool
/// event (`ToolCallStarted` or `ToolResult`). Correlation is by provider id
/// when present; when the id is missing or unmatched, fall back to the tool
/// name — the "actual tool" a real event can always be linked to.
///
/// The streamed tool index is deliberately not a fallback key here: providers
/// number their deltas in their own space (content-block index, accumulator
/// slot, or a constant `0` for Responses-style backends) while the finalized
/// events carry the agent's positional index, so an index match across those
/// two spaces is coincidence rather than identity.
fn reconcile_tool_item<'a>(
    items: &'a mut [Item],
    id: &str,
    name: &str,
    allow_running: bool,
) -> Option<&'a mut Item> {
    fn state_ok(state: &ToolState, allow_running: bool) -> bool {
        if allow_running {
            matches!(state, ToolState::Preparing(_) | ToolState::Running(_))
        } else {
            matches!(state, ToolState::Preparing(_))
        }
    }

    if !id.is_empty() {
        let position = items.iter().rev().position(|item| match item {
            Item::ToolCall {
                stream_id, state, ..
            } => {
                (state_ok(state, allow_running)
                    // Results can be replayed from the daemon's bounded journal
                    // after history has already persisted the result. Matching
                    // an already-done call by its stable id makes that replay
                    // idempotent instead of appending a second tool item.
                    || matches!(state, ToolState::Done { .. } | ToolState::Interrupted))
                    && stream_id.as_deref() == Some(id)
            }
            _ => false,
        });
        if let Some(position) = position {
            return Some(&mut items[items.len() - 1 - position]);
        }
    }

    // Most recent placeholder whose streamed name matches the final tool.
    {
        let position = items.iter().rev().position(|item| match item {
            Item::ToolCall { name: n, state, .. } => state_ok(state, allow_running) && n == name,
            _ => false,
        });
        if let Some(position) = position {
            return Some(&mut items[items.len() - 1 - position]);
        }
    }

    // A placeholder whose name never streamed can only be linked by being the
    // most recent unnamed one.
    items.iter_mut().rev().find(|item| match item {
        Item::ToolCall { name: n, state, .. } => state_ok(state, allow_running) && n.is_empty(),
        _ => false,
    })
}

/// Derive transcript items from a persisted/live history. Also the recovery
/// path for bus lag, resume rendering, and subagent views. Persisted tool
/// results are paired by their stable provider call id, never by position.
pub fn items_from_history(history: &Context) -> Vec<Item> {
    let mut items = Vec::new();
    // `TurnFinished` commits the assistant message to history *before* tool
    // execution runs, so a missing result alone is not evidence that a call
    // was interrupted: the turn may simply still be executing it. A call
    // only finalizes as interrupted when committed history shows the turn
    // moved past it — any later non-tool message (the generation loop
    // continuing, or a new turn beginning) proves the result never landed.
    let turn_ended_after: Vec<bool> = {
        let mut saw_turn_message = false;
        let mut flags = vec![false; history.len()];
        for (index, msg) in history.iter().enumerate().rev() {
            flags[index] = saw_turn_message;
            if msg.role != MessageRole::Tool {
                saw_turn_message = true;
            }
        }
        flags
    };

    // (item position, history message index) for every tool call pushed from
    // an assistant message, so unmatched calls can be classified afterwards.
    let mut calls: Vec<(usize, usize)> = Vec::new();
    for (message_index, msg) in history.iter().enumerate() {
        match msg.role {
            MessageRole::System => {}
            MessageRole::User => {
                if let Some(text) = summarize_user_message(msg) {
                    items.push(Item::User(text));
                }
            }
            MessageRole::Assistant => {
                for part in &msg.content {
                    match part {
                        MessagePart::Thinking { content, .. } if !content.is_empty() => {
                            items.push(Item::Thinking {
                                text: content.clone(),
                            })
                        }
                        MessagePart::Text(t) if !t.is_empty() => items.push(Item::Text(t.clone())),
                        MessagePart::ToolCall { id, name, args } => {
                            calls.push((items.len(), message_index));
                            items.push(Item::ToolCall {
                                stream_id: Some(id.clone()),
                                stream_index: 0,
                                name: name.clone(),
                                args: args.clone(),
                                result: None,
                                state: ToolState::Interrupted,
                            });
                        }
                        MessagePart::WebSearch { id, action } => items.push(Item::WebSearch {
                            id: id.clone(),
                            action: action.clone(),
                            state: SearchState::Interrupted,
                        }),
                        _ => {}
                    }
                }
            }
            MessageRole::Tool => {
                for part in &msg.content {
                    if let MessagePart::ToolResult { id, content, ok } = part {
                        let call = items.iter_mut().rev().find_map(|it| match it {
                            Item::ToolCall {
                                stream_id: Some(call_id),
                                result,
                                state: s @ ToolState::Interrupted,
                                ..
                            } if call_id == id => Some((result, s)),
                            _ => None,
                        });
                        if let Some((result, state)) = call {
                            *result = Some(content.clone());
                            *state = ToolState::Done {
                                ok: *ok,
                                bytes: content.len(),
                                error: (!*ok).then(|| content.clone()),
                            };
                        }
                    }
                }
            }
        }
    }
    for (position, message_index) in calls {
        if turn_ended_after[message_index] {
            continue;
        }
        if let Item::ToolCall { state, .. } = &mut items[position]
            && matches!(state, ToolState::Interrupted)
        {
            // Still pending: the turn may not have reached this call yet, so
            // keep it reconcileable by call id instead of marking it errored.
            *state = ToolState::Preparing(Instant::now());
        }
    }
    items
}

/// Merge durable tool results into a transcript that may contain a newer live
/// tail. Routine remote refreshes intentionally preserve that tail, but the
/// snapshot's history is still authoritative for calls that have finished in
/// the daemon. In particular, this settles an edit that was Running in the
/// UI when the result was persisted before the next refresh arrived.
fn reconcile_history_results(items: &mut Vec<Item>, history: &Context) {
    let calls: Vec<(String, String)> = history
        .iter()
        .filter(|message| message.role == MessageRole::Assistant)
        .flat_map(|message| message.content.iter())
        .filter_map(|part| match part {
            MessagePart::ToolCall { id, name, .. } => Some((id.clone(), name.clone())),
            _ => None,
        })
        .collect();
    for result in history
        .iter()
        .filter(|message| message.role == MessageRole::Tool)
        .flat_map(|message| message.content.iter())
    {
        let MessagePart::ToolResult { id, content, ok } = result else {
            continue;
        };
        let Some((_, name)) = calls.iter().find(|(call_id, _)| call_id == id) else {
            continue;
        };
        fold_event(
            items,
            &AgentEvent::ToolResult {
                index: 0,
                id: id.clone(),
                name: name.clone(),
                ok: *ok,
                content: content.clone(),
            },
        );
    }
}

/// Settle history-seeded pending presenters against daemon liveness after a
/// snapshot rebuild. Both rules use committed evidence, never event order:
///
/// 1. An agent with no active turn cannot be executing anything, so calls
///    left pending by history replay were cut and finalize as interrupted.
/// 2. A delegate call whose subagent still holds an active turn is working —
///    the parent transcript must not show it errored while the child
///    presenter is still live, so it is revived to running.
pub(crate) fn reconcile_snapshot_transcripts(
    transcripts: &mut HashMap<String, Vec<Item>>,
    active_turns: &HashMap<String, uuid::Uuid>,
    hierarchy: &HashMap<String, firmius_protocol::HierarchySnapshot>,
) {
    for (agent_id, items) in transcripts.iter_mut() {
        if active_turns.contains_key(agent_id) {
            continue;
        }
        for item in items.iter_mut() {
            if let Item::ToolCall { state, .. } = item
                && matches!(state, ToolState::Preparing(_) | ToolState::Running(_))
            {
                *state = ToolState::Interrupted;
            }
        }
    }
    for (child_id, node) in hierarchy {
        let (Some(parent_id), Some(call_id)) = (&node.parent_id, &node.spawned_via_tool_call_id)
        else {
            continue;
        };
        if !active_turns.contains_key(child_id) {
            continue;
        }
        let Some(items) = transcripts.get_mut(parent_id) else {
            continue;
        };
        for item in items.iter_mut() {
            if let Item::ToolCall {
                stream_id: Some(id),
                state,
                ..
            } = item
                && id == call_id
                && matches!(state, ToolState::Interrupted)
            {
                *state = ToolState::Running(Instant::now());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

#[derive(Debug, Default)]
pub struct Viewport {
    /// Number of rendered lines above the bottom position. Zero means follow
    /// the newest output, so scroll direction stays symmetric without needing
    /// the rendered transcript height in the input handler.
    pub offset: usize,
    /// Latest measured distance from the live bottom.  Keeping this in the
    /// viewport lets wheel events clamp immediately instead of accumulating
    /// invisible debt above the oldest line.
    pub max_offset: std::cell::Cell<usize>,
    pub follow: bool,
    /// Absolute first rendered line while the user is browsing history.  A
    /// cell keeps this presentation anchor mutable from the render pass even
    /// though the model itself is shared with ratatui's draw callback.
    pub anchor_top: std::cell::Cell<Option<usize>>,
    pub anchor_key: std::cell::RefCell<Option<(String, u16)>>,
}

pub struct RenderCache {
    pub focused_id: String,
    pub width: u16,
    pub disclosure: DisclosureMode,
    pub thinking: super::presentation::ThinkingMode,
    pub tail_lines: super::presentation::TailLines,
    pub auto_expand_presenters: bool,
    pub hide_task_tools: bool,
    pub hide_todo_tools: bool,
    pub autohide_system_messages: bool,
    /// Identity of the durable work state folded into the cached lines. The
    /// work view now renders inside the transcript, so a work change with no
    /// explicit cache clear must still rebuild it.
    pub work_signature: (u64, Option<firmius_core::GraphId>),
    pub lines: Vec<Line<'static>>,
    /// Cache line indexes whose title glint is animation-driven. The layout
    /// and presenter content remain cached, while the color pass is reapplied
    /// on every frame so a tick does not make live transcript rows appear
    /// frozen or laggy.
    pub animated_lines: Vec<usize>,
    /// Semantic event -> rendered inclusive line range.  Ranges are retained
    /// for viewport bookkeeping only; disclosure hit targets are header-only.
    pub event_ranges: Vec<(SemanticId, usize, usize)>,
    /// Measured disclosure affordances: (event id, subtarget, cache line
    /// index, column start, column end) in semantic-line coordinates. Hit
    /// rectangles are derived from these after viewport selection, so a
    /// click lands on the label the user actually sees — including the
    /// visible `expand`/`collapse` text — rather than on a fixed few cells.
    pub affordances: Vec<(SemanticId, HitSubtarget, usize, usize, usize)>,
}

/// Bytes of process output retained for a bash live-tail window.
pub const HOST_TAIL_BYTES: usize = 2048;

/// Incremental tail window for one process.
///
/// `offset` is the host buffer offset already consumed, so each refresh
/// reads only newly produced bytes. `bytes` is the retained window, capped
/// at `HOST_TAIL_BYTES`, kept as raw bytes rather than a `String` so a
/// multi-byte character split across two reads is not corrupted.
#[derive(Clone, Default)]
pub struct HostTailState {
    pub offset: usize,
    pub bytes: Vec<u8>,
}

impl HostTailState {
    /// Append newly read bytes and trim the window back to its cap.
    pub fn push(&mut self, chunk: &[u8]) {
        self.bytes.extend_from_slice(chunk);
        if self.bytes.len() > HOST_TAIL_BYTES {
            let excess = self.bytes.len() - HOST_TAIL_BYTES;
            self.bytes.drain(..excess);
        }
    }

    /// The retained window as displayable text.
    pub fn tail(&self) -> String {
        String::from_utf8_lossy(&self.bytes).into_owned()
    }
}

impl Viewport {
    pub fn scroll(&mut self, delta: isize) {
        // Any explicit navigation establishes a new anchor on the next draw;
        // retaining the old absolute line would make a scroll appear to do
        // nothing when streamed content arrived between key events.
        self.anchor_top.set(None);
        *self.anchor_key.borrow_mut() = None;
        if delta < 0 {
            self.offset = self
                .offset
                .saturating_add((-delta) as usize)
                .min(self.max_offset.get());
            self.follow = false;
        } else {
            self.offset = self.offset.saturating_sub(delta as usize);
            self.follow = self.offset == 0;
        }
    }

    /// Install a freshly measured transcript extent.  Layout changes may make
    /// an old browsing offset invalid without a wheel event, so normalize it
    /// here rather than allowing stale offset debt to survive a resize,
    /// collapse, or history replacement.
    pub fn set_max_offset(&mut self, max_offset: usize) {
        let previous_max = self.max_offset.replace(max_offset);
        let clamped = self.offset.min(max_offset);
        if clamped != self.offset || max_offset < previous_max {
            // A cached absolute anchor was derived from the old extent. It
            // must not override the relative position after any content
            // contraction, even when that relative offset remains valid.
            self.anchor_top.set(None);
            *self.anchor_key.borrow_mut() = None;
        }
        self.offset = clamped;
        if self.offset == 0 {
            self.follow = true;
        }
    }
}

pub enum Action {
    Continue,
    Quit,
    /// Text to send to the focused agent, with the turn's cancel token
    /// (already stored in `Model.cancel` for Esc).
    Submit {
        agent_id: String,
        message: Message,
        token: CancellationToken,
    },
    /// Daemon-backed submission. `None` creates the first session before
    /// submitting; an id targets an agent in the attached session.
    SubmitRemote {
        agent_id: Option<String>,
        message: Message,
    },
    /// Save the current daemon session and create a new SSH-backed session.
    OpenRemoteSession {
        workspace: String,
    },
    QueueRemote {
        agent_id: String,
        message: Message,
    },
    CancelRemote {
        turn_id: uuid::Uuid,
    },
    RewindRemote {
        agent_id: String,
        turns: usize,
    },
    EditHistory {
        agent_id: String,
        action: String,
    },
    SetModelRemote {
        agent_id: String,
        provider_id: String,
        model: String,
        effort: Option<EffortMode>,
    },
    SetPersonaRemote {
        agent_id: String,
        persona: Option<String>,
        delegated: bool,
    },
    SetTitleRemote(Option<String>),
    ExportRemote(Option<String>),
    RefreshRemote,
    /// Bus lagged: transcripts must be re-derived from histories (async).
    RebuildTranscripts,
    UpdateCheck,
    Save,
    Compact,
    Resume(Option<String>),
    /// Open a setup wizard. `None` = pick the kind first.
    OpenLogin {
        kind: Option<String>,
    },
    /// Show stored accounts and quota for a provider.
    OpenAccounts {
        provider: String,
    },
    /// A wizard finished: persist and register the new account.
    RegisterAccount {
        record: AccountRecord,
    },
    OpenPersonas,
    /// Open the settings modal (retry policy, general options).
    OpenSettings,
    /// Open the first-run launchpad. Unlike a mandatory wizard, this remains
    /// available later and never blocks normal composer input.
    OpenOnboarding,
    /// Seed the welcome transcript with a concise workflow tour.
    BeginOnboardingTour,
    /// Manage MCP servers (list, add, start, stop, restart, remove).
    Mcp(super::command::McpAction),
    /// Manage a durable goal through the daemon goal API.
    Goal(super::command::GoalAction),
    /// Open the command palette (Ctrl+K).
    OpenCommandPalette,
    /// Open the fuzzy workflow-file picker.
    OpenWorkflowPicker,
    /// Load a workflow file into the composer, optionally submitting it.
    LoadWorkflow {
        path: String,
        run: bool,
    },
    /// Put a command with arguments in the composer for completion/editing.
    InsertCommand(String),
    /// Save the current session and return to the welcome screen.
    NewSession,
    /// Copy this text to the system clipboard (performed by the event loop
    /// so unit tests never have to talk to a real clipboard).
    CopyText(String),
    OpenPermissions,
    MemoryQuery(String),
    ResolvePermission {
        resolution: firmius_protocol::PermissionResolution,
    },
    SetPermissionPolicy {
        policy: PermissionPolicy,
        expected_revision: u64,
    },
}

/// How long a confirmably-final todo rail stays visible before collapsing to
/// the bottom-bar count.
const TODO_FINAL_HOLD: Duration = Duration::from_secs(8);

pub struct Model {
    /// Present when runtime ownership lives in `firmiusd`. Live Agent and
    /// Session handles remain daemon-side; this model consumes data snapshots.
    pub daemon: Option<DaemonClient>,
    pub permission_policy: Option<PermissionPolicy>,
    pub pending_permission: Option<PendingPermissionRequest>,
    pub permission_activity: Vec<String>,
    /// Epoch of the daemon that produced `remote_snapshot`. Kept after a
    /// disconnect because the client is cleared while reconnecting.
    remote_epoch: Option<uuid::Uuid>,
    /// Prevents a burst of connection-loss notifications from starting
    /// multiple reconnect attempts for the same TUI.
    pub reconnect_in_progress: bool,
    pub remote_snapshot: Option<SessionSnapshot>,
    pub remote_turn_id: Option<uuid::Uuid>,
    pub remote_refreshed_at: Option<Instant>,
    /// Prevents periodic daemon refresh requests from overlapping when a
    /// daemon response takes longer than the refresh interval.
    pub(crate) remote_refresh_in_flight: bool,
    /// Last best-effort local housekeeping refresh.  Keeping this separate
    /// from `tick_phase` lets the spinner remain smooth without repeatedly
    /// cloning agent histories every 33ms.
    pub(crate) last_async_refresh: Option<Instant>,
    pub session: Option<SessionHandle>,
    /// Authoritative work state used by the renderer.  It is never populated
    /// from task tool result text; lag, gaps, and focus changes reload it from
    /// the session-owned snapshot.
    pub work_snapshot: Option<WorkSnapshot>,
    /// Ephemeral clocks and activity labels for the structured run panel.
    pub run_liveness: RunLiveness,
    /// Test-only seam for the native todo rail. Production renders project the
    /// rail from the owning session (`agent_todo`) or the daemon's typed
    /// projection, so no such escape hatch is compiled into the product.
    #[cfg(test)]
    pub todo_rail_override: Option<todo::TodoRail>,
    /// When the focused rail first became confirmably final. A finished
    /// checklist shows one confirmation, then collapses to the bottom bar
    /// instead of holding a permanent row.
    todo_final_hold: RefCell<Option<(u64, Instant)>>,
    /// Last unified session-bus sequence folded by the TUI.  A gap means the
    /// broadcast receiver lagged (or a session was swapped), so canonical
    /// snapshot recovery is required.
    pub session_event_sequence: u64,
    pub primary: Option<Arc<Agent>>,
    pub primary_id: String,
    pub focused_id: String,
    pub provider_id: String,
    /// The model to use when the first message creates the agent.
    pub model: String,
    /// The reasoning effort to use when the first message creates the agent.
    pub effort: Option<EffortMode>,
    pub tools: Arc<ToolRegistry>,
    /// Interior-mutable: `/login` registers accounts into it mid-session.
    /// A std mutex — every access is a brief sync read/write, no awaits held.
    pub manager: Arc<std::sync::Mutex<ProviderManager>>,
    pub personas: Arc<PersonaManager>,
    pub settings: Arc<std::sync::Mutex<UserSettings>>,
    /// Umbrella config (retry policy + general options), shared with the app so
    /// edits made in the settings modal take effect live. Persisted to
    /// `~/.firmius/config.json` on save.
    pub config: Arc<std::sync::Mutex<FirmiusConfig>>,
    /// Live MCP server manager; the `/mcp` command drives it, and discovered
    /// tools are registered into `tools`.
    pub mcp: Arc<McpManager>,
    /// Live handles for persona/model changes on whichever agent is focused.
    pub agents: HashMap<String, Arc<Agent>>,
    /// agent_id -> transcript items (created lazily on first event).
    pub transcripts: HashMap<String, Vec<Item>>,
    /// Renderer-independent semantic projection of the live transcript. The
    /// legacy `transcripts` collection remains the rendering source for now.
    pub semantic_transcripts: HashMap<String, Vec<TranscriptEvent>>,
    pub presentation_settings: PresentationSettings,
    pub arrival_cues: ArrivalCueTracker,
    pub tool_execution: HashMap<String, ToolExecutionState>,
    /// Typed projection of durable goal events. Goal events are state input,
    /// never generic transcript notes or user/assistant messages.
    pub goal_validation: GoalValidationAdapter,
    /// Locally echoed mailbox submissions awaiting their daemon UserMessage
    /// event. This makes queued input visible immediately without rendering it
    /// twice when the active turn eventually drains the mailbox.
    pending_remote_user_echoes: HashMap<String, std::collections::VecDeque<String>>,
    /// Initial submit rows are rendered optimistically too. The daemon may
    /// subsequently publish the corresponding user event, so keep a separate
    /// acknowledgement queue for those rows (they are not mailbox items and
    /// must not appear in the pending-message rail).
    pending_initial_user_echoes: HashMap<String, std::collections::VecDeque<String>>,
    /// (agent_id, label) in insertion order; refreshed by the app loop.
    pub roster: Vec<(String, String)>,
    pub composer: Composer,
    /// Paste store; composer segments reference these by 1-based id.
    pub pastes: Vec<StoredPaste>,
    pub busy: bool,
    pub active_agent_id: Option<String>,
    pub turn_started: Option<Instant>,
    pub cancel: Option<CancellationToken>,
    pub tick_phase: usize,
    pub live_phrase: String,
    pub live_phrase_anim: LivePhraseAnim,
    phrase_scenario: Option<PhraseScenario>,
    phrase_choice: usize,
    phrase_last_cycle: usize,
    /// Transient status-bar note with its creation time (TTL applied in view).
    pub note: Option<(String, Instant)>,
    pub viewport: Viewport,
    /// Background counts for the bottom bar, refreshed by the app loop.
    pub bg_procs: usize,
    pub bg_agents: usize,
    /// Process id -> last output tail, for bash live-tail presentations.
    /// Process ids are authoritative; command lines are not (the host wraps
    /// modern commands in `bash -lc`).
    pub host_tails: HashMap<ProcId, String>,
    /// Point-in-time process metadata for the focused agent. Unlike the
    /// output tails this remains useful after a process exits, so the status
    /// bar can explain what an operational turn left behind.
    pub process_statuses: Vec<ProcInfo>,
    /// Incremental read state backing `host_tails`, keyed by process id.
    ///
    /// `Host::peek(id, since)` copies `buffer[since..]`. Peeking from `0` on
    /// every tick therefore duplicated the *entire* output buffer of every
    /// tracked process, several times per second, only to keep the last
    /// `HOST_TAIL_BYTES`. For a long-lived process with megabytes of output
    /// that is a multi-megabyte allocate-and-free per tick, which fragments
    /// the allocator and makes resident memory climb even while idle.
    /// Remembering the consumed offset makes each tick read only new bytes.
    pub host_tail_state: HashMap<ProcId, HostTailState>,
    /// Agents with a locally accepted but not yet daemon-acknowledged turn.
    /// This is the submission identity boundary between "the user pressed
    /// Enter" and "the daemon accepted the turn": snapshots that arrive in
    /// that window carry no ActiveTurn yet, so without this marker the UI
    /// would read the session as idle and later present the accepted turn
    /// as a fresh (duplicate-looking) message.
    pub local_turn_intent: HashSet<String>,
    /// Composer draft captured when an idle remote submission was accepted,
    /// restored if the daemon rejects the turn (busy/transport failure).
    pub draft_before_remote_submit: Option<String>,
    /// Resolved intent labels. During argument streaming these may
    /// temporarily be keyed by the provider tool-call id (or `index:<n>`);
    /// results move them to their real process/delegate id.
    pub proc_intents: HashMap<String, String>,
    pub delegate_intents: HashMap<String, String>,
    pub completion: Option<CompletionState>,
    pub session_completion: SessionCompletionState,
    pub session_summaries: Option<Vec<firmius_core::SessionSummary>>,
    /// The completed text for which the user dismissed the completion menu.
    /// Tick-driven model refreshes must not immediately reopen that menu.
    pub completion_dismissed: Option<String>,
    /// Focused agent context usage, refreshed from its latest provider usage.
    pub ctx_used: u32,
    pub ctx_max: u32,
    /// Latest quota snapshot for the focused agent's provider, if that kind
    /// exposes meters. Refreshed on an interval so the CTX bar stays live.
    pub quota: Option<QuotaSnapshot>,
    pub quota_error: Option<String>,
    pub quota_provider_id: Option<String>,
    /// Model-provided effort modes keyed by agent id.
    pub agent_efforts: HashMap<String, Vec<EffortMode>>,
    /// Parent agent id -> (delegate tool-call id, child agent id). Matching by
    /// tool-call id prevents a new streaming delegate call from inheriting a
    /// previous call's output window.
    pub delegate_children: HashMap<String, Vec<(Option<String>, String)>>,
    /// Child agent id -> parent agent id, refreshed from the session hierarchy.
    pub parent_by_agent: HashMap<String, String>,
    pub render_cache: RefCell<Option<RenderCache>>,
    pub expanded_events: HashSet<(String, SemanticId)>,
    /// Screen-space cards from the last draw, used for direct mouse toggles.
    pub transcript_hits: RefCell<Vec<TranscriptHitRegion>>,
    /// The open modal, if any. While `Some`, keys go to it, not the composer.
    pub modal: Option<Box<dyn ModalSurface>>,
    /// Persona selected on the welcome screen before the first agent exists.
    pub pending_persona: Option<String>,
    /// Active theme, initialized from `UserSettings.theme`.
    pub theme: Theme,
    /// Composer submissions, oldest first. Seeded from `UserSettings` so
    /// Up/Down recall works across restarts.
    pub prompt_history: Vec<String>,
    /// Index into `prompt_history` while the user is browsing with Up/Down.
    /// `None` means the composer is a live draft, not a recalled prompt.
    pub prompt_history_index: Option<usize>,
    /// Composer contents captured when history browsing started, restored
    /// when the user arrows back past the newest entry.
    pub draft_before_history: Option<String>,
}

impl Model {
    /// Adapt the current legacy items into semantic events without changing
    /// the renderer's existing folding or layout behavior.
    pub fn semantic_events(&self, agent_id: &str) -> Vec<TranscriptEvent> {
        self.transcripts
            .get(agent_id)
            .into_iter()
            .flat_map(|items| items.iter().enumerate())
            .map(|(ordinal, item)| TranscriptEvent::from_item(item, ordinal as u64 + 1))
            .collect()
    }

    pub fn refresh_semantic_transcript(&mut self, agent_id: &str) {
        let events = self.semantic_events(agent_id);
        self.semantic_transcripts
            .insert(agent_id.to_string(), events);
    }

    pub fn semantic_event(&self, agent_id: &str, item: usize) -> Option<TranscriptEvent> {
        self.transcripts
            .get(agent_id)
            .and_then(|items| items.get(item))
            .map(|event| TranscriptEvent::from_item(event, item as u64 + 1))
    }

    pub(crate) fn submit_loaded_workflow(&mut self) -> Action {
        self.submit()
    }
    pub fn new(
        session: Option<SessionHandle>,
        primary: Option<Arc<Agent>>,
        mut provider_id: String,
        manager: Arc<std::sync::Mutex<ProviderManager>>,
        mut model: String,
        tools: Arc<ToolRegistry>,
        personas: Arc<PersonaManager>,
        settings: Arc<std::sync::Mutex<UserSettings>>,
        config: Arc<std::sync::Mutex<FirmiusConfig>>,
        mcp: Arc<McpManager>,
    ) -> Self {
        let primary_id = primary
            .as_ref()
            .map(|agent| agent.id.clone())
            .unwrap_or_else(|| "welcome".to_string());
        let mut transcripts = HashMap::new();
        if let Some(agent) = &primary {
            transcripts.insert(primary_id.clone(), items_from_history(&agent.history()));
        }
        let mut agents = HashMap::new();
        if let Some(agent) = &primary {
            agent.attach_runtime(manager.clone(), settings.clone());
            agent.attach_firmius_config(config.clone());
            agents.insert(agent.id.clone(), agent.clone());
        }
        let has_primary = primary.is_some();
        let active_theme = settings
            .lock()
            .unwrap()
            .theme
            .as_deref()
            .and_then(theme::by_name)
            .unwrap_or_else(theme::default_theme);
        let mut effort = None;
        if !has_primary
            && let Some(preferred) = settings.lock().unwrap().preferred_default_model().cloned()
            && manager
                .lock()
                .unwrap()
                .schema(&preferred.provider_id)
                .is_some()
        {
            provider_id = preferred.provider_id;
            model = preferred.model;
            effort = preferred.effort.as_deref().map(effort_from_name);
        }
        let prompt_history = settings.lock().unwrap().prompt_history.clone();
        let mut model = Self {
            daemon: None,
            permission_policy: session
                .as_ref()
                .map(|session| session.permission_broker.policy()),
            pending_permission: None,
            permission_activity: Vec::new(),
            remote_epoch: None,
            reconnect_in_progress: false,
            remote_snapshot: None,
            remote_turn_id: None,
            remote_refreshed_at: None,
            remote_refresh_in_flight: false,
            last_async_refresh: None,
            session,
            work_snapshot: None,
            run_liveness: RunLiveness::default(),
            #[cfg(test)]
            todo_rail_override: None,
            todo_final_hold: RefCell::new(None),
            session_event_sequence: 0,
            primary,
            primary_id: primary_id.clone(),
            focused_id: primary_id.clone(),
            provider_id,
            model,
            effort,
            tools,
            manager,
            personas,
            settings,
            config,
            mcp,
            agents,
            transcripts,
            semantic_transcripts: HashMap::new(),
            presentation_settings: PresentationSettings::default(),
            arrival_cues: ArrivalCueTracker::default(),
            tool_execution: HashMap::new(),
            goal_validation: GoalValidationAdapter::default(),
            pending_remote_user_echoes: HashMap::new(),
            pending_initial_user_echoes: HashMap::new(),
            roster: if has_primary {
                vec![(primary_id, "main".to_string())]
            } else {
                Vec::new()
            },
            composer: Composer::new(),
            pastes: Vec::new(),
            busy: false,
            active_agent_id: None,
            turn_started: None,
            cancel: None,
            tick_phase: 0,
            live_phrase: "idle".to_string(),
            live_phrase_anim: LivePhraseAnim::Steady,
            phrase_scenario: None,
            phrase_choice: 0,
            phrase_last_cycle: 0,
            note: None,
            viewport: Viewport {
                offset: 0,
                max_offset: std::cell::Cell::new(0),
                follow: true,
                anchor_top: std::cell::Cell::new(None),
                anchor_key: std::cell::RefCell::new(None),
            },
            bg_procs: 0,
            bg_agents: 0,
            host_tails: HashMap::new(),
            process_statuses: Vec::new(),
            host_tail_state: HashMap::new(),
            local_turn_intent: HashSet::new(),
            draft_before_remote_submit: None,
            proc_intents: HashMap::new(),
            delegate_intents: HashMap::new(),
            completion: None,
            session_completion: SessionCompletionState::Ready,
            session_summaries: None,
            completion_dismissed: None,
            ctx_used: 0,
            ctx_max: 0,
            quota: None,
            quota_error: None,
            quota_provider_id: None,
            agent_efforts: HashMap::new(),
            delegate_children: HashMap::new(),
            parent_by_agent: HashMap::new(),
            render_cache: RefCell::new(None),
            expanded_events: HashSet::new(),
            transcript_hits: RefCell::new(Vec::new()),
            modal: None,
            pending_persona: None,
            theme: active_theme,
            prompt_history,
            prompt_history_index: None,
            draft_before_history: None,
        };
        model.reload_work_snapshot();
        model.refresh_semantic_projection();
        model
    }

    pub fn has_agent(&self) -> bool {
        self.primary.is_some() || self.remote_snapshot.is_some()
    }

    pub fn attach_daemon(&mut self, daemon: DaemonClient, snapshot: Option<SessionSnapshot>) {
        let epoch_changed = self
            .remote_epoch
            .is_some_and(|old| old != daemon.endpoint().epoch);
        if epoch_changed {
            // Event sequence numbers are daemon/session-local. Never compare
            // a fresh daemon's sequence against the old process watermark.
            self.session_event_sequence = 0;
            self.remote_snapshot = None;
            self.transcripts.clear();
            self.semantic_transcripts.clear();
        }
        self.remote_epoch = Some(daemon.endpoint().epoch);
        self.daemon = Some(daemon);
        if let Some(snapshot) = snapshot {
            self.replace_remote_snapshot(snapshot);
        }
    }

    pub fn replace_remote_snapshot(&mut self, snapshot: SessionSnapshot) {
        // Snapshot requests are asynchronous and may complete after a live
        // event has already been folded.  Never let an older response move
        // the UI watermark backwards or rebuild transcripts from stale
        // persisted history, which would erase the live tail.
        if snapshot.sequence < self.session_event_sequence
            && self
                .remote_snapshot
                .as_ref()
                .is_some_and(|current| current.session_id == snapshot.session_id)
        {
            return;
        }
        let replace_transcripts = self
            .remote_snapshot
            .as_ref()
            .is_none_or(|current| current.session_id != snapshot.session_id)
            || snapshot.sequence > self.session_event_sequence;
        let _previous_sequence = self.session_event_sequence;
        self.session = None;
        self.primary = None;
        self.agents.clear();
        self.primary_id = snapshot.primary_agent_id.clone();
        if !snapshot
            .agents
            .iter()
            .any(|agent| agent.record.id == self.focused_id)
        {
            self.focused_id = self.primary_id.clone();
        }
        if replace_transcripts {
            let mut transcripts: HashMap<String, Vec<Item>> = snapshot
                .agents
                .iter()
                .map(|agent| {
                    (
                        agent.record.id.clone(),
                        items_from_history(&agent.record.history),
                    )
                })
                .collect();
            // Persisted history cannot contain the current in-flight assistant
            // message yet. Fold the daemon's bounded live journal after it.
            for event in &snapshot.live_events {
                // `live_events` is already restricted by the daemon to the
                // uncommitted tail of active turns. Fold the complete tail:
                // on gap recovery `previous_sequence` may be newer than the
                // missing deltas that forced the rebuild.
                if event.sequence <= snapshot.sequence
                    && let firmius_core::SessionEventPayload::Agent { agent_id, event } =
                        &event.payload
                {
                    fold_event(transcripts.entry(agent_id.clone()).or_default(), event);
                }
            }
            // History alone cannot tell a cut call from one still executing;
            // daemon liveness (active turns, live subagents) settles the rest.
            reconcile_snapshot_transcripts(
                &mut transcripts,
                &snapshot.active_turns,
                &snapshot.hierarchy,
            );
            self.transcripts = transcripts;
            self.semantic_transcripts = self
                .transcripts
                .keys()
                .map(|id| (id.clone(), self.semantic_events(id)))
                .collect();
        }
        append_work_completion_summaries(
            &snapshot.work,
            &mut self.transcripts,
            &snapshot.primary_agent_id,
        );
        self.semantic_transcripts = self
            .transcripts
            .keys()
            .map(|id| (id.clone(), self.semantic_events(id)))
            .collect();
        self.reapply_pending_user_echoes();
        self.roster = snapshot
            .agents
            .iter()
            .enumerate()
            .map(|(index, agent)| {
                (
                    agent.record.id.clone(),
                    agent.record.label.clone().unwrap_or_else(|| {
                        if index == 0 {
                            "main".into()
                        } else {
                            format!("agent {index}")
                        }
                    }),
                )
            })
            .collect();
        self.work_snapshot = Some(snapshot.work.clone());
        if snapshot.sequence >= self.session_event_sequence {
            self.session_event_sequence = snapshot.sequence;
        }
        self.remote_turn_id = snapshot.active_turns.get(&self.focused_id).copied();
        let focused_busy = snapshot.active_turns.contains_key(&self.focused_id)
            || snapshot
                .agents
                .iter()
                .any(|agent| agent.record.id == self.focused_id && agent.busy);
        // A local submit can race the first CreateSession snapshot, which has
        // no ActiveTurn yet. The local turn intent is a submission identity
        // boundary (not text de-duplication): retain the busy state until its
        // TurnAccepted acknowledgement (or terminal event) arrives. Queued
        // mailbox echoes keep their own pending list; both signals represent
        // locally initiated work the daemon has not yet reported as settled.
        let locally_submitted = self.local_turn_intent.contains(&self.focused_id)
            || self
                .pending_remote_user_echoes
                .get(&self.focused_id)
                .is_some_and(|pending| !pending.is_empty());
        self.busy = focused_busy || locally_submitted;
        self.active_agent_id = if focused_busy {
            Some(self.focused_id.clone())
        } else if locally_submitted {
            Some(self.focused_id.clone())
        } else {
            snapshot.active_turns.keys().next().cloned().or_else(|| {
                snapshot
                    .agents
                    .iter()
                    .find(|agent| agent.busy)
                    .map(|agent| agent.record.id.clone())
            })
        };
        if self.busy && self.turn_started.is_none() {
            self.turn_started = Some(Instant::now());
        }
        if !self.busy {
            self.turn_started = None;
        }
        if let Some(primary) = snapshot
            .agents
            .iter()
            .find(|agent| agent.record.id == snapshot.primary_agent_id)
        {
            self.provider_id = primary.record.provider_id.clone();
            self.model = primary.record.model.clone();
            self.effort = primary.record.effort.clone();
        }
        self.delegate_children.clear();
        self.parent_by_agent.clear();
        for (agent_id, node) in &snapshot.hierarchy {
            if let Some(parent_id) = &node.parent_id {
                self.parent_by_agent
                    .insert(agent_id.clone(), parent_id.clone());
                self.delegate_children
                    .entry(parent_id.clone())
                    .or_default()
                    .push((node.spawned_via_tool_call_id.clone(), agent_id.clone()));
            }
        }
        self.bg_agents = snapshot.active_delegates;
        if let Some(focused) = snapshot
            .agents
            .iter()
            .find(|agent| agent.record.id == self.focused_id)
        {
            self.ctx_used = focused.usage.input_tokens;
            self.ctx_max = self
                .manager
                .lock()
                .unwrap()
                .model_info_for(&focused.record.provider_id, &focused.record.model)
                .map(|info| info.context_window)
                .unwrap_or(0);
            self.bg_procs = focused
                .processes
                .iter()
                .filter(|process| matches!(process.status, firmius_core::ProcStatus::Running))
                .count();
            self.process_statuses = focused.processes.clone();
            let live: std::collections::HashSet<_> =
                focused.processes.iter().map(|process| process.id).collect();
            self.host_tails.retain(|id, _| live.contains(id));
            self.host_tail_state.retain(|id, _| live.contains(id));
        } else {
            self.ctx_used = 0;
            self.ctx_max = 0;
            self.bg_procs = 0;
            self.process_statuses.clear();
            self.host_tails.clear();
            self.host_tail_state.clear();
        }
        self.remote_snapshot = Some(snapshot);
        self.remote_refreshed_at = Some(Instant::now());
        self.clear_render_cache();
    }

    /// Apply a routine remote refresh without replacing an in-flight local
    /// transcript tail.  The daemon persists completed turns separately from
    /// its live event journal, so a snapshot can legitimately contain older
    /// histories than events already folded by this UI.
    pub fn refresh_remote_snapshot(
        &mut self,
        snapshot: SessionSnapshot,
        preserve_transcript: bool,
    ) {
        if !preserve_transcript || self.remote_snapshot.is_none() {
            self.replace_remote_snapshot(snapshot);
            return;
        }
        let histories: Vec<(String, Context)> = snapshot
            .agents
            .iter()
            .map(|agent| (agent.record.id.clone(), agent.record.history.clone()))
            .collect();
        let existing = self.transcripts.clone();
        self.replace_remote_snapshot(snapshot);
        for (agent_id, transcript) in existing {
            self.transcripts.insert(agent_id, transcript);
        }
        for (agent_id, history) in histories {
            reconcile_history_results(self.transcripts.entry(agent_id).or_default(), &history);
        }
        if let Some(snapshot) = &self.remote_snapshot {
            append_work_completion_summaries(
                &snapshot.work,
                &mut self.transcripts,
                &snapshot.primary_agent_id,
            );
        }
        self.refresh_semantic_projection();
    }

    /// Apply the daemon's compact live projection without replacing durable
    /// histories or rebuilding transcript projections.
    pub fn apply_remote_status(&mut self, status: firmius_protocol::SessionStatus) {
        let Some(snapshot) = self.remote_snapshot.as_mut() else {
            return;
        };
        if snapshot.session_id != status.session_id || status.sequence < self.session_event_sequence
        {
            return;
        }
        snapshot.title = status.title;
        snapshot.sequence = snapshot.sequence.max(status.sequence);
        snapshot.primary_agent_id = status.primary_agent_id;
        snapshot.hierarchy = status.hierarchy;
        snapshot.work = status.work;
        snapshot.active_turns = status.active_turns;
        snapshot.active_delegates = status.active_delegates;
        self.work_snapshot = Some(snapshot.work.clone());
        append_work_completion_summaries(
            &snapshot.work,
            &mut self.transcripts,
            &snapshot.primary_agent_id,
        );
        // Status refreshes carry the live hierarchy too. Keep the presenter
        // lookup in lockstep with it; otherwise a status tick can replace the
        // hierarchy and make an otherwise healthy delegate preview vanish
        // until the next full snapshot rebuilds this derived index.
        self.delegate_children.clear();
        self.parent_by_agent.clear();
        for (agent_id, node) in &snapshot.hierarchy {
            if let Some(parent_id) = &node.parent_id {
                self.parent_by_agent
                    .insert(agent_id.clone(), parent_id.clone());
                self.delegate_children
                    .entry(parent_id.clone())
                    .or_default()
                    .push((node.spawned_via_tool_call_id.clone(), agent_id.clone()));
            }
        }
        for agent in status.agents {
            if let Some(existing) = snapshot.agents.iter_mut().find(|a| a.record.id == agent.id) {
                existing.record.provider_id = agent.provider_id;
                existing.record.model = agent.model;
                existing.record.effort = agent.effort;
                existing.record.workdir = agent.workdir;
                existing.record.label = agent.label;
                existing.usage = agent.usage;
                existing.total_usage = agent.total_usage;
                existing.busy = agent.busy;
                existing.processes = agent.processes;
                // A compact status can race the dedicated Todo event. Do not
                // erase a projection already received from that event merely
                // because this status snapshot omitted it.
                if let Some(incoming) = agent.todo
                    && existing
                        .todo
                        .as_ref()
                        .is_none_or(|current| incoming.revision >= current.revision)
                {
                    existing.todo = Some(incoming);
                }
            } else {
                snapshot.agents.push(firmius_protocol::AgentSnapshot {
                    record: firmius_core::AgentRecord {
                        id: agent.id,
                        provider_id: agent.provider_id,
                        model: agent.model,
                        effort: agent.effort,
                        system_prompt: None,
                        persona: None,
                        temperature: None,
                        max_tokens: None,
                        workdir: agent.workdir,
                        label: agent.label,
                        metadata: Default::default(),
                        history: Default::default(),
                        mailbox: Vec::new(),
                        active_goal_id: None,
                        todo: Default::default(),
                        compaction: None,
                    },
                    usage: agent.usage,
                    total_usage: agent.total_usage,
                    busy: agent.busy,
                    processes: agent.processes,
                    todo: agent.todo,
                });
            }
        }
        self.remote_refreshed_at = Some(Instant::now());
        self.clear_render_cache();
    }

    fn ensure_started(&mut self) -> Result<(), String> {
        if self.daemon.is_some() {
            return Err("daemon session must be created by the event loop".into());
        }
        if self.primary.is_some() {
            return Ok(());
        }
        let mut provider_id = {
            let manager = self.manager.lock().unwrap();
            if manager.schema(&self.provider_id).is_some() {
                self.provider_id.clone()
            } else {
                manager
                    .provider_ids()
                    .first()
                    .map(|id| (*id).to_string())
                    .ok_or_else(|| "no provider configured — use /login first".to_string())?
            }
        };
        let mut model_name = self.model.clone();
        let mut effort = self.effort.clone();
        let preferred = {
            let settings = self.settings.lock().unwrap();
            match self.pending_persona.as_deref() {
                Some(persona_id) => settings
                    .preferred_model(persona_id)
                    .cloned()
                    .or_else(|| settings.preferred_default_model().cloned()),
                None => settings.preferred_default_model().cloned(),
            }
        };
        if let Some(preferred) = preferred {
            let manager = self.manager.lock().unwrap();
            if manager.schema(&preferred.provider_id).is_none() {
                return Err(format!(
                    "preferred model unavailable: {}/{}",
                    preferred.provider_id, preferred.model
                ));
            }
            provider_id = preferred.provider_id;
            model_name = preferred.model;
            effort = preferred.effort.as_deref().map(effort_from_name);
        }
        let provider = self.manager.lock().unwrap().build(&provider_id)?;
        let config = AgentConfig {
            provider_id: provider_id.clone(),
            model: model_name.clone(),
            system_prompt: Some(default_system_prompt().into()),
            max_tokens: Some(32900),
            effort: effort.clone(),
            persona: self.pending_persona.clone(),
            ..Default::default()
        };
        let session = Session::new_handle();
        let agent = session.spawn_agent_with_personas(
            provider,
            self.tools.clone(),
            config,
            self.personas.clone(),
        );
        agent.attach_runtime(self.manager.clone(), self.settings.clone());
        agent.attach_firmius_config(self.config.clone());
        self.session = Some(session);
        self.reload_work_snapshot();
        self.session_event_sequence = self
            .work_snapshot
            .as_ref()
            .map_or(0, |snapshot| snapshot.work_sequence);
        self.primary = Some(agent.clone());
        self.primary_id = agent.id.clone();
        self.focused_id = self.primary_id.clone();
        self.provider_id = provider_id;
        self.model = model_name;
        self.effort = effort;
        self.agents.insert(agent.id.clone(), agent.clone());
        self.roster = vec![(self.primary_id.clone(), "main".to_string())];
        self.transcripts.insert(
            self.primary_id.clone(),
            items_from_history(&agent.history()),
        );
        Ok(())
    }

    pub fn clear_render_cache(&self) {
        self.render_cache.borrow_mut().take();
    }

    /// Keep the renderer's typed event projection synchronized with folded
    /// transcript state. This is intentionally cheap and stable: event IDs
    /// are derived from stream IDs or durable ordinals, never screen rows.
    pub fn refresh_semantic_projection(&mut self) {
        let ids: Vec<String> = self.transcripts.keys().cloned().collect();
        for id in ids {
            self.refresh_semantic_transcript(&id);
        }
    }

    fn phrase_scenario(&self) -> PhraseScenario {
        if matches!(
            self.focused_transcript().last(),
            Some(Item::Thinking { .. })
        ) && self.busy
        {
            return PhraseScenario::Thinking;
        }
        if matches!(self.focused_transcript().last(), Some(Item::Text(_))) {
            return PhraseScenario::Writing;
        }
        if let Some(Item::ToolCall { name, state, .. }) = self.focused_transcript().last()
            && matches!(state, ToolState::Preparing(_) | ToolState::Running(_))
        {
            return match name.as_str() {
                "edit" => PhraseScenario::PreparingEdit,
                "task" | "workflow" => PhraseScenario::PreparingTask,
                "delegate" => PhraseScenario::WaitingDelegate,
                "bash" | "process" => PhraseScenario::WaitingProcess,
                _ => PhraseScenario::RunningTool,
            };
        }
        if let Some(Item::WebSearch { state, .. }) = self.focused_transcript().last()
            && matches!(state, SearchState::Preparing(_))
        {
            return PhraseScenario::RunningTool;
        }
        if self.live_run().is_some()
            || self
                .work_snapshot
                .as_ref()
                .is_some_and(|snapshot| !snapshot.state.active_graph_by_agent.is_empty())
        {
            return PhraseScenario::WaitingWorkgraph;
        }
        PhraseScenario::Thinking
    }

    fn desired_activity_phrase(&mut self) -> String {
        let scenario = self.phrase_scenario();
        let cycle_ready = self.tick_phase.saturating_sub(self.phrase_last_cycle) >= 90;
        if self.phrase_scenario != Some(scenario) || cycle_ready {
            self.phrase_scenario = Some(scenario);
            let choices = phrase_catalogue(scenario);
            // A stable pseudo-random walk avoids flicker while still making
            // repeated turns feel varied without depending on wall-clock RNG.
            self.phrase_choice = self
                .tick_phase
                .wrapping_mul(1_103_515_245)
                .wrapping_add(12_345)
                % choices.len();
            self.phrase_last_cycle = self.tick_phase;
        }
        phrase_catalogue(scenario)[self.phrase_choice].to_string()
    }

    fn phrase_step_ticks() -> usize {
        2
    }

    fn fade_ticks_for(text: &str) -> usize {
        text.chars().count().max(1) * Self::phrase_step_ticks()
    }

    fn sync_live_phrase(&mut self) {
        let desired = if self.busy {
            self.desired_activity_phrase()
        } else {
            "idle".to_string()
        };
        match &mut self.live_phrase_anim {
            LivePhraseAnim::Steady => {
                if self.live_phrase != desired {
                    self.live_phrase_anim = LivePhraseAnim::FadingOut {
                        from: self.live_phrase.clone(),
                        to: desired,
                        started_tick: self.tick_phase,
                    };
                }
            }
            LivePhraseAnim::FadingOut {
                from,
                to,
                started_tick,
            } => {
                *to = desired;
                if self.tick_phase.saturating_sub(*started_tick) >= Self::fade_ticks_for(from) {
                    let next = to.clone();
                    self.live_phrase_anim = LivePhraseAnim::FadingIn {
                        to: next,
                        started_tick: self.tick_phase,
                    };
                }
            }
            LivePhraseAnim::FadingIn { to, started_tick } => {
                if self.tick_phase.saturating_sub(*started_tick) >= Self::fade_ticks_for(to) {
                    self.live_phrase = to.clone();
                    self.live_phrase_anim = LivePhraseAnim::Steady;
                    if self.live_phrase != desired {
                        self.live_phrase_anim = LivePhraseAnim::FadingOut {
                            from: self.live_phrase.clone(),
                            to: desired,
                            started_tick: self.tick_phase,
                        };
                    }
                }
            }
        }
    }

    pub fn flash(&mut self, msg: &str) {
        self.note = Some((msg.to_string(), Instant::now()));
    }

    fn cycle_permission_mode(&mut self) -> Action {
        let Some(policy) = self.permission_policy.clone() else {
            self.flash("permission policy unavailable");
            return Action::Continue;
        };
        let next = match policy.mode {
            PermissionMode::Default => PermissionMode::Auto,
            PermissionMode::Auto => PermissionMode::Yolo,
            PermissionMode::Yolo | PermissionMode::Custom(_) => PermissionMode::Default,
        };
        if matches!(next, PermissionMode::Yolo) && !policy.yolo_confirmed {
            self.flash("Yolo requires confirmation in /permissions");
            return Action::OpenPermissions;
        }
        let mut next_policy = policy;
        next_policy.mode = next;
        Action::SetPermissionPolicy {
            expected_revision: next_policy.revision,
            policy: next_policy,
        }
    }

    fn remember_prompt(&mut self, prompt: &str) {
        let prompt = prompt.trim();
        if prompt.is_empty() {
            return;
        }
        if self.prompt_history.last().map(String::as_str) == Some(prompt) {
            self.prompt_history_index = None;
            self.draft_before_history = None;
            return;
        }
        self.prompt_history.push(prompt.to_string());
        const CAP: usize = 200;
        if self.prompt_history.len() > CAP {
            let excess = self.prompt_history.len() - CAP;
            self.prompt_history.drain(..excess);
        }
        self.prompt_history_index = None;
        self.draft_before_history = None;
        let mut next = self.settings.lock().unwrap().clone();
        next.push_prompt(prompt);
        if next.save().is_ok() {
            *self.settings.lock().unwrap() = next;
        }
    }

    fn recall_prompt(&mut self, older: bool) {
        if self.prompt_history.is_empty() {
            return;
        }
        if self.prompt_history_index.is_none() {
            self.draft_before_history = Some(self.composer.text(&self.pastes));
        }
        let last = self.prompt_history.len() - 1;
        let next = match self.prompt_history_index {
            None if older => Some(last),
            None => return,
            Some(0) if older => Some(0),
            Some(i) if older => Some(i - 1),
            Some(i) if i >= last => None,
            Some(i) => Some(i + 1),
        };
        self.prompt_history_index = next;
        match next {
            Some(i) => self.composer.replace_text(&self.prompt_history[i]),
            None => {
                let draft = self.draft_before_history.take().unwrap_or_default();
                self.composer.replace_text(&draft);
            }
        }
    }

    fn last_assistant_text(&self) -> Option<String> {
        self.focused_transcript()
            .iter()
            .rev()
            .find_map(|item| match item {
                Item::Text(t) if !t.trim().is_empty() => Some(t.clone()),
                _ => None,
            })
    }

    fn transcript_plain(&self) -> String {
        let mut out = String::new();
        for item in self.focused_transcript() {
            match item {
                Item::User(t) => {
                    out.push_str("You: ");
                    out.push_str(t);
                    out.push_str("\n\n");
                }
                Item::Text(t) => {
                    out.push_str(t);
                    out.push_str("\n\n");
                }
                Item::AgentMessage { sender_id, text } => {
                    out.push_str(&format!("[message from {sender_id}]\n{text}\n\n"));
                }
                Item::SystemMessage { text } => {
                    out.push_str(&format!("[system]\n{text}\n\n"));
                }
                Item::AssignmentCompletion {
                    child_agent_id,
                    assignment_id,
                    text,
                } => {
                    out.push_str(&format!(
                        "[assignment {assignment_id} from {child_agent_id}]\n{text}\n\n"
                    ));
                }
                Item::Thinking { text, .. } => {
                    out.push_str("[thinking] ");
                    out.push_str(text);
                    out.push_str("\n\n");
                }
                Item::Note(t) => {
                    out.push_str(t);
                    out.push_str("\n\n");
                }
                Item::ToolCall {
                    name, args, result, ..
                } => {
                    out.push_str(&format!("[{name}] {args}\n"));
                    if let Some(result) = result {
                        out.push_str(result);
                        out.push('\n');
                    }
                    out.push('\n');
                }
                Item::WebSearch { action, state, .. } => {
                    let verb = match state {
                        SearchState::Preparing(_) => "searching",
                        SearchState::Done | SearchState::Interrupted => "searched",
                    };
                    let subject = action
                        .subject()
                        .map(|s| format!(" \"{s}\""))
                        .unwrap_or_default();
                    out.push_str(&format!("[{verb}{subject}]\n\n"));
                }
                Item::Compaction(item) => {
                    let state = match &item.phase {
                        CompactionPhase::Scheduled => "queued",
                        CompactionPhase::Running(_) => "running",
                        CompactionPhase::Finished => "finished",
                        CompactionPhase::Discarded => "discarded",
                        CompactionPhase::Failed(_) => "failed",
                    };
                    out.push_str(&format!("[compaction {state}]\n"));
                    if !item.summary.is_empty() {
                        out.push_str(&item.summary);
                        out.push('\n');
                    }
                    out.push('\n');
                }
            }
        }
        out
    }

    pub fn session_title_label(&self) -> String {
        if let Some(snapshot) = &self.remote_snapshot {
            return snapshot
                .title
                .clone()
                .unwrap_or_else(|| "(untitled)".into());
        }
        self.session
            .as_ref()
            .and_then(|session| session.title())
            .unwrap_or_else(|| {
                if self.has_agent() {
                    "(untitled)".into()
                } else {
                    "welcome".into()
                }
            })
    }

    pub fn reset_to_welcome(&mut self) {
        self.remote_snapshot = None;
        self.remote_turn_id = None;
        self.remote_refreshed_at = None;
        self.remote_refresh_in_flight = false;
        self.session = None;
        self.work_snapshot = None;
        self.session_event_sequence = 0;
        self.primary = None;
        self.primary_id = "welcome".into();
        self.focused_id = self.primary_id.clone();
        self.agents.clear();
        self.transcripts.clear();
        self.pending_remote_user_echoes.clear();
        self.pending_initial_user_echoes.clear();
        self.local_turn_intent.clear();
        self.draft_before_remote_submit = None;
        self.roster.clear();
        self.busy = false;
        self.active_agent_id = None;
        self.turn_started = None;
        self.cancel = None;
        self.live_phrase = "idle".into();
        self.live_phrase_anim = LivePhraseAnim::Steady;
        self.phrase_scenario = None;
        self.phrase_choice = 0;
        self.phrase_last_cycle = 0;
        self.viewport.offset = 0;
        self.viewport.max_offset.set(0);
        self.viewport.follow = true;
        self.viewport.anchor_top.set(None);
        *self.viewport.anchor_key.borrow_mut() = None;
        self.completion = None;
        self.agent_efforts.clear();
        self.delegate_children.clear();
        self.parent_by_agent.clear();
        self.proc_intents.clear();
        self.delegate_intents.clear();
        self.host_tails.clear();
        self.process_statuses.clear();
        self.host_tail_state.clear();
        self.modal = None;
        self.pending_persona = None;
        self.ctx_used = 0;
        self.ctx_max = 0;
        self.quota = None;
        self.quota_error = None;
        self.quota_provider_id = None;
        self.bg_procs = 0;
        self.bg_agents = 0;
        self.clear_render_cache();
    }

    fn persist_model_preference(
        &self,
        persona_id: Option<&str>,
        provider_id: &str,
        model: &str,
        effort: Option<&str>,
    ) -> Result<(), String> {
        let mut settings = self.settings.lock().unwrap();
        let mut next = settings.clone();
        match persona_id {
            Some(persona_id) => next.set_preferred_model_and_effort(
                persona_id,
                provider_id,
                model,
                effort.map(str::to_string),
            ),
            None => next.set_preferred_default(provider_id, model, effort.map(str::to_string)),
        }
        next.save().map_err(|error| error.to_string())?;
        *settings = next;
        Ok(())
    }

    pub fn focused_transcript(&self) -> &[Item] {
        self.transcripts
            .get(&self.focused_id)
            .map_or(&[][..], |v| v)
    }

    pub fn focus_label(&self) -> String {
        if !self.has_agent() {
            return "welcome".to_string();
        }
        self.roster
            .iter()
            .find(|(id, _)| id == &self.focused_id)
            .map(|(_, label)| label.clone())
            .unwrap_or_else(|| "agent".to_string())
    }

    pub fn cycle_focus(&mut self, dir: i32) {
        if self.roster.len() <= 1 {
            return;
        }
        let idx = self
            .roster
            .iter()
            .position(|(id, _)| id == &self.focused_id)
            .unwrap_or(0);
        let n = self.roster.len() as i32;
        let next = (idx as i32 + dir).rem_euclid(n) as usize;
        self.focused_id = self.roster[next].0.clone();
        self.reload_work_snapshot();
        if let Some(agent) = self.agents.get(&self.focused_id).cloned() {
            let history = agent.history();
            let transcript = self.transcripts.entry(self.focused_id.clone()).or_default();
            if transcript.is_empty() && !history.is_empty() {
                *transcript = items_from_history(&history);
            }
        }
        self.viewport.follow = true;
        self.clear_render_cache();
        self.refresh_completion();
    }

    pub fn focused_persona_id(&self) -> Option<String> {
        if let Some(snapshot) = &self.remote_snapshot {
            return snapshot
                .agents
                .iter()
                .find(|agent| agent.record.id == self.focused_id)
                .and_then(|agent| agent.record.persona.clone());
        }
        if self.primary.is_none() {
            return self.pending_persona.clone();
        }
        self.agents
            .get(&self.focused_id)
            .and_then(|agent| agent.config().persona)
    }

    pub fn focused_model_status(&self) -> (String, String, String) {
        if let Some(agent) = self.remote_snapshot.as_ref().and_then(|snapshot| {
            snapshot
                .agents
                .iter()
                .find(|agent| agent.record.id == self.focused_id)
        }) {
            return (
                agent.record.provider_id.clone(),
                agent.record.model.clone(),
                agent
                    .record
                    .effort
                    .as_ref()
                    .map(|effort| effort.name.clone())
                    .unwrap_or_else(|| "default".into()),
            );
        }
        let config = self
            .agents
            .get(&self.focused_id)
            .map(|agent| agent.config())
            .or_else(|| {
                self.primary
                    .as_ref()
                    .filter(|agent| agent.id == self.focused_id)
                    .map(|agent| agent.config())
            });
        match config {
            Some(config) => (
                config.provider_id,
                config.model,
                config
                    .effort
                    .map(|effort| effort.name)
                    .unwrap_or_else(|| "default".into()),
            ),
            None => (
                self.provider_id.clone(),
                self.model.clone(),
                self.effort
                    .as_ref()
                    .map(|effort| effort.name.clone())
                    .unwrap_or_else(|| "default".into()),
            ),
        }
    }

    pub fn cycle_focused_persona(&mut self) -> Action {
        if self.focused_id != self.primary_id && !self.has_agent() {
            return Action::Continue;
        }
        let delegated = self.parent_by_agent.contains_key(&self.focused_id);
        let ids: Vec<Option<String>> = if delegated {
            self.personas
                .list()
                .into_iter()
                .map(|p| Some(p.id))
                .collect()
        } else {
            std::iter::once(None)
                .chain(
                    self.personas
                        .main_personas()
                        .into_iter()
                        .map(|p| Some(p.id)),
                )
                .collect()
        };
        if ids.is_empty() {
            self.flash("no personas available");
            return Action::Continue;
        }
        let current = self.focused_persona_id();
        let next = ids
            .iter()
            .position(|id| *id == current)
            .map(|idx| ids[(idx + 1) % ids.len()].clone())
            .unwrap_or_else(|| ids[0].clone());
        if self.daemon.is_some() && self.remote_snapshot.is_some() {
            self.flash(&format!(
                "persona: {}",
                next.as_deref().unwrap_or("Default")
            ));
            return Action::SetPersonaRemote {
                agent_id: self.focused_id.clone(),
                persona: next,
                delegated,
            };
        }
        if let Err(e) = self.apply_persona(next.clone()) {
            self.flash(&e);
        } else {
            self.flash(&format!(
                "persona: {}",
                next.as_deref().unwrap_or("Default")
            ));
        }
        Action::Continue
    }

    fn apply_persona(&mut self, persona_id: Option<String>) -> Result<(), String> {
        if self.primary.is_none() {
            let preferred = if let Some(id) = persona_id.as_deref() {
                let persona = self
                    .personas
                    .get(id)
                    .ok_or_else(|| format!("persona not found: {id}"))?;
                if persona.background {
                    return Err(format!("persona '{id}' is delegate-only"));
                }
                let settings = self.settings.lock().unwrap();
                settings
                    .preferred_model(id)
                    .cloned()
                    .or_else(|| settings.preferred_default_model().cloned())
            } else {
                self.settings
                    .lock()
                    .unwrap()
                    .preferred_default_model()
                    .cloned()
            };
            if let Some(preferred) = preferred {
                if self
                    .manager
                    .lock()
                    .unwrap()
                    .schema(&preferred.provider_id)
                    .is_none()
                {
                    return Err(format!(
                        "preferred model unavailable: {}/{}",
                        preferred.provider_id, preferred.model
                    ));
                }
                self.provider_id = preferred.provider_id;
                self.model = preferred.model;
                self.effort = preferred.effort.as_deref().map(effort_from_name);
            }
            self.pending_persona = persona_id;
            return Ok(());
        }
        let agent = self
            .agents
            .get(&self.focused_id)
            .cloned()
            .ok_or_else(|| "focused agent is unavailable".to_string())?;
        if agent.is_busy() {
            return Err("busy: persona changes wait for that agent's turn".to_string());
        }
        let use_context = if self.parent_by_agent.contains_key(&self.focused_id) {
            PersonaUse::Delegate
        } else {
            PersonaUse::Main
        };
        let preferred = {
            let settings = self.settings.lock().unwrap();
            match persona_id.as_deref() {
                Some(id) => settings
                    .preferred_model(id)
                    .cloned()
                    .or_else(|| settings.preferred_default_model().cloned()),
                None => settings.preferred_default_model().cloned(),
            }
        };
        if let Some(pref) = &preferred
            && self
                .manager
                .lock()
                .unwrap()
                .schema(&pref.provider_id)
                .is_none()
        {
            return Err(format!(
                "preferred model unavailable: {}/{}",
                pref.provider_id, pref.model
            ));
        }
        let preferred_provider = preferred
            .as_ref()
            .map(|pref| {
                self.manager
                    .lock()
                    .unwrap()
                    .build(&pref.provider_id)
                    .map(|provider| (pref, provider))
            })
            .transpose()?;
        agent
            .set_persona(persona_id, use_context)
            .map_err(|e| match e {
                AgentError::Busy => "busy: persona changes wait for the turn".to_string(),
                other => other.to_string(),
            })?;
        if let Some((pref, provider)) = preferred_provider {
            agent
                .set_provider(pref.provider_id.clone(), provider)
                .map_err(|e| e.to_string())?;
            agent
                .update_config(|config| {
                    config.model = pref.model.clone();
                    config.effort = pref.effort.as_deref().map(effort_from_name);
                })
                .map_err(|e| e.to_string())?;
        }
        if let Some(pref) = preferred
            && agent.id == self.primary_id
        {
            self.provider_id = pref.provider_id;
            self.model = pref.model;
            self.effort = pref.effort.as_deref().map(effort_from_name);
        }
        Ok(())
    }

    pub fn scroll(&mut self, delta: isize) {
        self.viewport.scroll(delta);
    }

    fn mouse(&mut self, event: MouseEvent) {
        match event.kind {
            MouseEventKind::ScrollUp => self.scroll(-3),
            MouseEventKind::ScrollDown => self.scroll(3),
            // Disclosure is pointer-only and commits on release.  A mouse
            // down may become a terminal-selection drag, so toggling there
            // stole normal selection and opened rows accidentally.
            MouseEventKind::Up(crossterm::event::MouseButton::Left) => {
                let hit = self
                    .transcript_hits
                    .borrow()
                    .iter()
                    .find(|hit| {
                        event.column >= hit.left
                            && event.column <= hit.right
                            && event.row >= hit.top
                            && event.row <= hit.bottom
                            && matches!(
                                hit.subtarget,
                                HitSubtarget::Header
                                    | HitSubtarget::ThinkingHeader
                                    | HitSubtarget::LiveOutputHeader
                                    | HitSubtarget::Inspector
                            )
                    })
                    .cloned();
                if let Some(hit) = hit {
                    let event_key = (self.focused_id.clone(), hit.event_id.clone());
                    if !self.expanded_events.remove(&event_key) {
                        self.expanded_events.insert(event_key);
                    }
                    self.clear_render_cache();
                }
            }
            _ => {}
        }
    }

    pub(crate) fn refresh_completion(&mut self) {
        if self.modal.is_some() {
            self.completion = None;
            return;
        }
        let text = self.composer.text(&self.pastes);
        if self.completion_dismissed.as_deref() == Some(text.as_str()) {
            self.completion = None;
            return;
        }
        // Refreshes also happen from the 33ms housekeeping tick.  Keep the
        // user's highlighted item across those refreshes; otherwise an arrow
        // key appears to work for one frame and then jumps back to item zero.
        let selected_insert = self
            .completion
            .as_ref()
            .and_then(|completion| completion.items.get(completion.selected))
            .map(|item| item.insert.clone());
        if !text.starts_with('/') {
            self.completion = None;
            return;
        }

        let mut items = Vec::new();
        if !text.contains(char::is_whitespace) {
            if text == "/effort" {
                self.push_effort_completions(&mut items, "");
            }
            for info in command::table() {
                if info.name.starts_with(&text) {
                    let insert = if info.args.is_empty() {
                        info.name.to_string()
                    } else {
                        format!("{} ", info.name)
                    };
                    items.push(CompletionItem {
                        insert,
                        label: info.name.to_string(),
                        detail: format!(
                            "{}{}",
                            info.args,
                            if info.args.is_empty() { "" } else { "  " }
                        ) + info.help,
                    });
                }
            }
        } else if let Some(space) = text.find(char::is_whitespace) {
            let head = &text[..space];
            let partial = text[space..].trim_start();
            match head {
                "/resume" => {
                    if let Some(sessions) = &self.session_summaries {
                        for session in sessions {
                            let score = fuzzy_score(partial, &session.title)
                                .into_iter()
                                .chain(fuzzy_score(partial, &session.id))
                                .min();
                            if score.is_some() {
                                items.push(CompletionItem {
                                    insert: format!("/resume {}", session.id),
                                    label: session.id.clone(),
                                    detail: format!(
                                        "{}  ·  {} agents",
                                        session.title, session.agent_count
                                    ),
                                });
                            }
                        }
                    } else {
                        items.push(CompletionItem {
                            insert: text.clone(),
                            label: "loading sessions…".into(),
                            detail: "reading saved sessions".into(),
                        });
                    }
                }
                "/model" => {
                    for (account, provider, id) in
                        self.manager.lock().unwrap().model_choices_by_kind()
                    {
                        let reference = format!("{provider}/{id}");
                        if fuzzy_score(partial, &reference).is_some()
                            || fuzzy_score(partial, &id).is_some()
                            || fuzzy_score(partial, &provider).is_some()
                        {
                            items.push(CompletionItem {
                                insert: format!("/model {reference}"),
                                label: reference,
                                detail: format!("model · provider {provider} · account {account}"),
                            });
                        }
                    }
                }
                "/login" => {
                    for kind in self.manager.lock().unwrap().kinds() {
                        if fuzzy_score(partial, kind.name()).is_some() {
                            items.push(CompletionItem {
                                insert: format!("/login {}", kind.name()),
                                label: kind.name().to_string(),
                                detail: kind.display_name().to_string(),
                            });
                        }
                    }
                }
                "/accounts" => {
                    let providers = self.manager.lock().unwrap().kind_names();
                    for provider in providers {
                        if fuzzy_score(partial, &provider).is_some() {
                            items.push(CompletionItem {
                                insert: format!("/accounts {provider}"),
                                label: provider,
                                detail: "stored accounts and quota".into(),
                            });
                        }
                    }
                }
                "/effort" => self.push_effort_completions(&mut items, partial),
                "/theme" => {
                    for available in theme::all() {
                        if partial.is_empty() || fuzzy_score(partial, available.name).is_some() {
                            items.push(CompletionItem {
                                insert: format!("/theme {}", available.name),
                                label: available.name.to_string(),
                                detail: "color theme".into(),
                            });
                        }
                    }
                }
                "/search" => {
                    let modes = self.web_search_completion_modes();
                    for (name, detail) in modes {
                        if partial.is_empty() || fuzzy_score(partial, name).is_some() {
                            items.push(CompletionItem {
                                insert: format!("/search {name}"),
                                label: name.to_string(),
                                detail: detail.into(),
                            });
                        }
                    }
                }
                _ => {}
            }
        }

        if let Some(space) = text.find(char::is_whitespace) {
            let query = text[space..].trim_start();
            items.sort_by_key(|item| {
                fuzzy_score(query, &item.label)
                    .into_iter()
                    .chain(fuzzy_score(query, &item.detail))
                    .min()
                    .unwrap_or(usize::MAX)
            });
        }
        if items.is_empty() {
            self.completion = None;
        } else {
            let selected = selected_insert
                .and_then(|insert| items.iter().position(|item| item.insert == insert))
                .unwrap_or(0);
            self.completion = Some(CompletionState { items, selected });
        }
    }

    pub(crate) fn session_completion_needed(&self) -> bool {
        let text = self.composer.text(&self.pastes);
        let Some(space) = text.find(char::is_whitespace) else {
            return false;
        };
        &text[..space] == "/resume" && self.session_summaries.is_none()
    }

    pub(crate) fn apply_session_summaries(
        &mut self,
        result: Result<Vec<firmius_core::SessionSummary>, String>,
    ) {
        self.session_completion = SessionCompletionState::Ready;
        if let Ok(sessions) = &result {
            self.session_summaries = Some(sessions.clone());
        }
        self.refresh_completion();
    }

    fn push_effort_completions(&self, items: &mut Vec<CompletionItem>, partial: &str) {
        let efforts = self.effort_modes_for_focused();
        for effort in efforts {
            if partial.is_empty() || fuzzy_score(partial, &effort.name).is_some() {
                items.push(CompletionItem {
                    insert: format!("/effort {}", effort.name),
                    label: effort.name,
                    detail: "selected model reasoning effort".into(),
                });
            }
        }
    }

    fn effort_modes_for_model(&self, provider_id: &str, model: &str) -> Vec<EffortMode> {
        let manager = self.manager.lock().unwrap();
        manager
            .model_info_for(provider_id, model)
            .or_else(|| {
                manager
                    .schema(provider_id)
                    .is_none()
                    .then(|| manager.model_info(model))
                    .flatten()
            })
            .map(|info| info.effort_modes.clone())
            .unwrap_or_default()
    }

    fn effort_modes_for_focused(&self) -> Vec<EffortMode> {
        if let Some(efforts) = self.agent_efforts.get(&self.focused_id)
            && !efforts.is_empty()
        {
            return efforts.clone();
        }
        let (provider_id, model, _) = self.focused_model_status();
        self.effort_modes_for_model(&provider_id, &model)
    }

    fn focused_web_search_capability(&self) -> Option<firmius_core::LLMWebSearch> {
        let agent = self
            .agents
            .get(&self.focused_id)
            .cloned()
            .or_else(|| self.primary.clone());
        if let Some(agent) = agent {
            return agent.provider().capabilities().web_search;
        }
        None
    }

    fn web_search_completion_modes(&self) -> Vec<(&'static str, &'static str)> {
        let mut modes = vec![("off", "disable hosted web search")];
        if let Some(cap) = self.focused_web_search_capability() {
            for mode in cap.modes {
                let detail = match mode {
                    WebSearchMode::Cached => "cached hosted search",
                    WebSearchMode::Indexed => "indexed hosted search",
                    WebSearchMode::Live => "live hosted search",
                };
                modes.push((mode.as_str(), detail));
            }
        } else {
            modes.extend([
                ("cached", "cached hosted search"),
                ("indexed", "indexed hosted search"),
                ("live", "live hosted search"),
            ]);
        }
        modes
    }

    fn warn_if_search_unsupported(&mut self) {
        let policy = self.config.lock().unwrap().general.web_search.clone();
        let Some(policy) = policy else {
            return;
        };
        let Ok(mode) = policy.parse::<WebSearchMode>() else {
            return;
        };
        match self.focused_web_search_capability() {
            None => self.flash("this provider does not support web search"),
            Some(cap) if !cap.supports(mode) => self.flash(&format!(
                "this provider does not support {} search",
                mode.as_str()
            )),
            Some(_) => {}
        }
    }

    fn persist_web_search(&mut self, mode: Option<&str>) -> Result<(), String> {
        let mut config = self.config.lock().unwrap();
        let mut next = config.clone();
        next.general.web_search = mode.map(str::to_string);
        next.save().map_err(|error| error.to_string())?;
        *config = next;
        Ok(())
    }

    fn apply_search_command(&mut self, mode: Option<String>) {
        match mode.as_deref() {
            None => {
                let current = self
                    .config
                    .lock()
                    .unwrap()
                    .general
                    .web_search
                    .clone()
                    .unwrap_or_else(|| "off".into());
                let cap = self.focused_web_search_capability();
                let available = match cap {
                    Some(cap) => cap
                        .modes
                        .iter()
                        .map(|m| m.as_str())
                        .chain(std::iter::once("off"))
                        .collect::<Vec<_>>()
                        .join(", "),
                    None => "off (this provider does not support web search)".to_string(),
                };
                self.flash(&format!("search: {current} · available: {available}"));
            }
            Some("off") => match self.persist_web_search(None) {
                Ok(()) => self.flash("search: off · preference saved"),
                Err(error) => self.flash(&format!("search save failed: {error}")),
            },
            Some(raw) => {
                let Ok(parsed) = raw.parse::<WebSearchMode>() else {
                    self.flash(&format!(
                        "unknown search mode '{raw}' — use cached, indexed, live, or off"
                    ));
                    return;
                };
                if let Some(cap) = self.focused_web_search_capability() {
                    if !cap.supports(parsed) {
                        self.flash(&format!("this provider does not support {raw} search"));
                        return;
                    }
                } else {
                    self.flash("this provider does not support web search");
                    return;
                }
                match self.persist_web_search(Some(parsed.as_str())) {
                    Ok(()) => {
                        self.flash(&format!("search: {} · preference saved", parsed.as_str()))
                    }
                    Err(error) => self.flash(&format!("search save failed: {error}")),
                }
            }
        }
    }

    fn completion_move(&mut self, dir: i32) -> bool {
        let Some(completion) = &mut self.completion else {
            return false;
        };
        let len = completion.items.len() as i32;
        completion.selected = (completion.selected as i32 + dir).rem_euclid(len) as usize;
        true
    }

    fn reapply_pending_user_echoes(&mut self) {
        for (agent_id, pending) in &self.pending_remote_user_echoes {
            let items = self.transcripts.entry(agent_id.clone()).or_default();
            for summary in pending {
                let already = items.iter().rev().any(|item| match item {
                    Item::User(text) => text == summary,
                    _ => false,
                });
                if !already {
                    items.push(Item::User(summary.clone()));
                }
            }
        }
    }

    /// Mailbox input waiting for the focused agent's next drain. Daemon mode
    /// has no live `Agent` handles, so this reads the snapshot mailbox plus
    /// locally echoed QueueRemote submissions.
    pub fn pending_user_messages(&self) -> Vec<String> {
        let mut messages = Vec::new();
        if let Some(agent) = self.agents.get(&self.focused_id) {
            messages.extend(agent.pending_messages());
        }
        if let Some(snapshot) = &self.remote_snapshot
            && let Some(agent) = snapshot
                .agents
                .iter()
                .find(|agent| agent.record.id == self.focused_id)
        {
            for message in &agent.record.mailbox {
                if let Some(summary) = summarize_user_message(message) {
                    messages.push(summary);
                }
            }
        }
        if let Some(pending) = self.pending_remote_user_echoes.get(&self.focused_id) {
            for summary in pending {
                if !messages.iter().any(|existing| existing == summary) {
                    messages.push(summary.clone());
                }
            }
        }
        messages
    }

    fn accept_completion(&mut self) -> bool {
        let Some(completion) = self.completion.take() else {
            return false;
        };
        let Some(item) = completion.items.get(completion.selected) else {
            return false;
        };
        self.composer.replace_text(&item.insert);
        true
    }

    pub fn replace_session(&mut self, session: SessionHandle, primary: Arc<Agent>) {
        self.session = Some(session);
        self.primary = Some(primary.clone());
        self.agents.clear();
        self.agents.insert(primary.id.clone(), primary.clone());
        primary.attach_firmius_config(self.config.clone());
        self.pending_persona = None;
        self.primary_id = primary.id.clone();
        self.focused_id = self.primary_id.clone();
        self.provider_id = primary.config().provider_id.clone();
        self.model = primary.config().model.clone();
        self.effort = primary.config().effort.clone();
        self.transcripts.clear();
        self.transcripts.insert(
            self.primary_id.clone(),
            items_from_history(&primary.history()),
        );
        self.roster = vec![(self.primary_id.clone(), "main".into())];
        self.viewport.follow = true;
        self.completion = None;
        self.agent_efforts.clear();
        self.delegate_children.clear();
        self.parent_by_agent.clear();
        self.proc_intents.clear();
        self.delegate_intents.clear();
        self.modal = None;
        self.prompt_history_index = None;
        self.draft_before_history = None;
        self.quota = None;
        self.quota_error = None;
        self.quota_provider_id = None;
        self.reload_work_snapshot();
    }

    pub fn reload_work_snapshot(&mut self) {
        let snapshot = self
            .remote_snapshot
            .as_ref()
            .map(|snapshot| snapshot.work.clone())
            .or_else(|| self.session.as_ref().map(|session| session.work_snapshot()));
        self.work_snapshot = snapshot;
        if let Some(snapshot) = self.work_snapshot.clone() {
            append_work_completion_summaries(
                &snapshot,
                &mut self.transcripts,
                &self.primary_id.clone(),
            );
            self.refresh_semantic_projection();
        }
    }

    pub fn work_view(&self, max_lines: usize) -> work::WorkView {
        let Some(snapshot) = self.work_snapshot.as_ref() else {
            return work::WorkView::default();
        };
        // A focused child with a live parent assignment gets a parent
        // context row; a focused graph owner with live assignments gets
        // assignment summary rows. Otherwise this is a plain checklist.
        if let Some(parent_id) = self.parent_by_agent.get(&self.focused_id) {
            let parent_graph_id = snapshot.state.active_graph_by_agent.get(parent_id).copied();
            return work::WorkView::for_child(
                snapshot,
                &self.focused_id,
                parent_graph_id,
                max_lines,
            );
        }
        let owns_active_graph = snapshot
            .state
            .active_graph_by_agent
            .get(&self.focused_id)
            .and_then(|graph_id| snapshot.state.graphs.get(graph_id))
            .is_some_and(|graph| {
                graph.owner_agent_id.as_deref() == Some(self.focused_id.as_str())
                    && graph
                        .assignments
                        .values()
                        .any(|assignment| assignment.released_at.is_none())
            });
        if owns_active_graph {
            return work::WorkView::for_parent(snapshot, &self.focused_id, max_lines);
        }
        work::WorkView::for_agent(snapshot, &self.focused_id, max_lines)
    }

    /// Project one agent's native todo rail.
    ///
    /// The rail is never reconstructed from `todo` tool output. When a daemon
    /// owns the runtime the typed status/snapshot projection is authoritative;
    /// otherwise the embedded session's canonical ledger is read directly.
    pub fn todo_rail_for(&self, agent_id: &str) -> todo::TodoRail {
        if let Some(snapshot) = &self.remote_snapshot {
            let agent = snapshot
                .agents
                .iter()
                .find(|agent| agent.record.id == agent_id);
            let Some(agent) = agent else {
                return todo::TodoRail::absent();
            };
            return match &agent.todo {
                Some(projection) => todo::TodoRail::from_dto(projection),
                // A missing projection means the ledger is quarantined or the
                // daemon does not track one; never render that as an empty
                // checklist of completed work.
                None => todo::TodoRail::unavailable("no canonical todo projection"),
            };
        }
        match &self.session {
            Some(session) => {
                if session.agent(agent_id).is_none() {
                    return todo::TodoRail::absent();
                }
                match session.agent_todo(agent_id) {
                    Ok(ledger) => todo::TodoRail::from_ledger(&ledger),
                    Err(error) => todo::TodoRail::unavailable(error),
                }
            }
            None => todo::TodoRail::absent(),
        }
    }

    /// Project the focused agent's native todo rail.
    pub fn todo_rail(&self) -> todo::TodoRail {
        // Test-only seam: layout tests inject a rail instead of building a
        // session. Production rails always come from a real projection.
        #[cfg(test)]
        if let Some(rail) = &self.todo_rail_override {
            return rail.clone();
        }
        self.todo_rail_for(&self.focused_id)
    }

    /// The todo rail the view should draw, or `None` when the focused agent
    /// has nothing to show. Records when a confirmably-final rail first
    /// appeared so [`Self::todo_rail_collapsed`] can hide it afterwards.
    pub fn visible_todo_rail(&self) -> Option<todo::TodoRail> {
        let rail = self.todo_rail();
        if rail.is_empty() && rail.warning().is_none() {
            self.todo_final_hold.borrow_mut().take();
            return None;
        }
        if rail.completion_confirmed() {
            let mut hold = self.todo_final_hold.borrow_mut();
            if !matches!(*hold, Some((revision, _)) if revision == rail.revision) {
                *hold = Some((rail.revision, Instant::now()));
            }
        } else {
            self.todo_final_hold.borrow_mut().take();
        }
        Some(rail)
    }

    /// Whether a confirmably-final rail has finished its brief confirmation
    /// and should collapse to the bottom-bar count.
    pub fn todo_rail_collapsed(&self) -> bool {
        let rail = self.todo_rail();
        if !rail.completion_confirmed() {
            return false;
        }
        self.todo_final_hold.borrow().is_some_and(|(revision, at)| {
            revision == rail.revision && at.elapsed() > TODO_FINAL_HOLD
        })
    }

    /// Resolve the compact checklist's presentation state at the UI boundary.
    /// A delegated assignment is durable before its worker emits an execution
    /// event, so its canonical `Pending` status must not be presented as the
    /// ordinary queued state (or be aliased to `Running`).
    pub(crate) fn work_presentation(&self, row: &work::WorkLine) -> work::WorkPresentation {
        if row.assigned && row.status == firmius_core::ExecutionStatus::Pending {
            work::WorkPresentation::Starting
        } else {
            work::WorkPresentation::from_status(row.status)
        }
    }

    /// The graph currently being driven by `task launch`, projected into
    /// dependency stages. Plain unstructured checklists deliberately stay in
    /// the compact work view even if launched.
    pub fn live_run(&self) -> Option<firmius_core::work::LiveGraph> {
        let graph_id = self.run_liveness.graph_id()?;
        let graph = self.work_snapshot.as_ref()?.graph(graph_id)?;
        let live = firmius_core::work::project_live(graph);
        live.structured.then_some(live)
    }

    pub fn delegate_child(
        &self,
        parent_id: &str,
        _ordinal: usize,
        tool_call_id: Option<&str>,
    ) -> Option<&str> {
        self.delegate_children.get(parent_id).and_then(|children| {
            let id = tool_call_id.filter(|id| !id.is_empty())?;
            children
                .iter()
                .find(|(spawned_id, _)| spawned_id.as_deref() == Some(id))
                .map(|(_, child)| child.as_str())
        })
    }

    /// Keep intent visible while a tool call is still being assembled, then
    /// re-key it when the tool returns its authoritative id.
    fn resolve_intent(&mut self, event: &AgentEvent, agent_id: &str) {
        let provisional = |id: &str, index: u32| {
            if !id.is_empty() {
                id.to_string()
            } else {
                format!("index:{index}")
            }
        };
        match event {
            AgentEvent::ToolCallDelta {
                index,
                id,
                name_delta: _,
                args_delta: _,
            } => {
                let args = self
                    .transcripts
                    .get(agent_id)
                    .into_iter()
                    .flatten()
                    .rev()
                    .find_map(|item| match item {
                        Item::ToolCall {
                            stream_id,
                            stream_index,
                            name,
                            args,
                            ..
                        } if (stream_id.as_deref() == Some(id.as_str()) && !id.is_empty())
                            || *stream_index == *index =>
                        {
                            Some((name.as_str(), args.as_str()))
                        }
                        _ => None,
                    });
                let Some((name, args)) = args else { return };
                // Only these tools expose an intent used by the TUI. Avoid
                // reparsing a growing edit patch (or any unrelated payload)
                // on every streamed argument delta.
                if name != "bash" && name != "delegate" {
                    return;
                }
                let Some(intent) = PartialJson::parse(&args).str("intent").map(str::to_owned)
                else {
                    return;
                };
                let key = provisional(id, *index);
                if name == "bash" {
                    self.proc_intents.insert(key, intent);
                } else if name == "delegate" {
                    self.delegate_intents.insert(key, intent);
                }
            }
            AgentEvent::ToolCallStarted {
                index,
                id,
                name,
                args,
            } => {
                let Some(intent) = PartialJson::parse(args).str("intent").map(str::to_owned) else {
                    return;
                };
                if name == "bash" {
                    let key = provisional(id, *index);
                    let intent = self
                        .proc_intents
                        .remove(&format!("index:{index}"))
                        .unwrap_or(intent);
                    self.proc_intents.insert(key, intent);
                } else if name == "delegate" {
                    let key = provisional(id, *index);
                    let intent = self
                        .delegate_intents
                        .remove(&format!("index:{index}"))
                        .unwrap_or(intent);
                    self.delegate_intents.insert(key, intent);
                }
            }
            AgentEvent::ToolResult {
                index,
                id,
                name,
                content,
                ..
            } => {
                let old_key = provisional(id, *index);
                if name == "bash"
                    && let Some(proc_id) = result_field(content, "proc_id")
                    && let Some(intent) = self.proc_intents.remove(&old_key)
                {
                    self.proc_intents.insert(proc_id, intent);
                } else if name == "delegate"
                    && let Some(delegate_id) = result_field(content, "delegate_id")
                    && let Some(intent) = self.delegate_intents.remove(&old_key)
                {
                    self.delegate_intents.insert(delegate_id, intent);
                }
            }
            _ => {}
        }
    }

    // ------------------------------------------------------------------
    // Update
    // ------------------------------------------------------------------

    pub fn update(&mut self, ev: AppEvent) -> Action {
        self.refresh_semantic_projection();
        match ev {
            AppEvent::Sessions(_) => Action::Continue,
            AppEvent::Tick => {
                self.tick_phase = self.tick_phase.wrapping_add(1);
                if let Some((_, at)) = &self.note
                    && at.elapsed().as_secs() >= 3
                {
                    self.note = None;
                }
                self.sync_live_phrase();
                Action::Continue
            }
            AppEvent::RemoteTurnDone {
                agent_id,
                turn_id,
                result,
            } => {
                self.clear_render_cache();
                if self.remote_turn_id == Some(turn_id) {
                    self.remote_turn_id = None;
                }
                // The daemon settled this turn, so any locally recorded
                // submission intent for the agent is now accounted for.
                self.local_turn_intent.remove(&agent_id);
                if let Err(error) = result {
                    self.flash(&format!("{agent_id}: {error}"));
                }
                // A session may have several daemon-owned turns. Never infer
                // global idleness from one completion; refresh the daemon's
                // authoritative active-turn map instead.
                Action::Continue
            }
            // The event loop applies refresh payloads because it owns the
            // daemon client and host-tail merge. Keep the model reducer
            // exhaustive for direct/unit callers that feed it an event.
            AppEvent::RemoteRefresh { .. } => Action::Continue,
            AppEvent::RemoteStatus(status) => {
                self.apply_remote_status(status);
                Action::Continue
            }
            AppEvent::RemoteReconnected { .. } => Action::Continue,
            AppEvent::PermissionRequested(request) => {
                self.pending_permission = Some(request.clone());
                self.permission_activity
                    .push(format!("request: {}", request.tool));
                if self.permission_activity.len() > 50 {
                    self.permission_activity.remove(0);
                }
                Action::OpenPermissions
            }
            AppEvent::PermissionResolved {
                request_id,
                decision,
            } => {
                self.permission_activity
                    .push(format!("resolved {request_id}: {:?}", decision));
                self.pending_permission = self
                    .pending_permission
                    .take()
                    .filter(|r| r.request_id != request_id);
                Action::Continue
            }
            AppEvent::Goal(event) => {
                self.goal_validation.apply(&event);
                Action::Continue
            }
            AppEvent::Quota(result) => {
                match result {
                    Ok(snapshot) => {
                        if let Some(account_id) = self.quota_provider_id.as_deref() {
                            self.manager
                                .lock()
                                .unwrap()
                                .cache_account_quota(account_id, snapshot.clone());
                        }
                        self.quota = Some(snapshot);
                        self.quota_error = None;
                    }
                    Err(error) => self.quota_error = Some(error),
                }
                Action::Continue
            }
            AppEvent::Bus(SessionEvent {
                payload,
                sequence,
                session_id,
                ..
            }) => {
                if let Some(snapshot) = &self.work_snapshot
                    && snapshot.session_id != session_id
                {
                    return Action::Continue;
                }
                if sequence <= self.session_event_sequence {
                    return Action::Continue;
                }
                if sequence != self.session_event_sequence.saturating_add(1) {
                    // A gap in the bus-contiguity watermark means an event
                    // was missed (or reordered): rebuild every transcript
                    // from canonical agent history and reload the work
                    // snapshot, rather than silently dropping whatever
                    // event follows the gap.
                    // Keep the last contiguous watermark. Advancing it to the
                    // event after the gap would make an equal-sequence
                    // snapshot look unchanged and skip transcript rebuilding.
                    return Action::RebuildTranscripts;
                }
                self.session_event_sequence = sequence;
                match payload {
                    firmius_core::SessionEventPayload::Work(envelope) => {
                        match &envelope.event {
                            firmius_core::work::WorkEvent::RunStarted { run_id, graph_id } => {
                                self.run_liveness.run_started(run_id.clone(), *graph_id);
                            }
                            firmius_core::work::WorkEvent::RunConcluded { run_id, .. } => {
                                self.run_liveness.run_concluded(run_id);
                            }
                            _ => {}
                        }
                        // `work::fold_event` is not load-bearing here: the
                        // canonical session snapshot is the single source
                        // of truth for work state, and reloading it is
                        // cheap (an `Arc`-backed clone), so folding first
                        // would only be discarded work. Reload directly.
                        self.clear_render_cache();
                        if self.daemon.is_some() {
                            // The daemon's Work event is a delta. Ask the
                            // authoritative snapshot/status path for the new
                            // projection immediately instead of waiting for
                            // focus or a later recovery refresh.
                            Action::RefreshRemote
                        } else {
                            self.reload_work_snapshot();
                            Action::Continue
                        }
                    }
                    firmius_core::SessionEventPayload::Agent { agent_id, event } => {
                        if agent_id == self.focused_id {
                            match event {
                                AgentEvent::BusyChanged { busy } => {
                                    self.busy = busy;
                                    if busy {
                                        self.active_agent_id = Some(agent_id.clone());
                                        if self.turn_started.is_none() {
                                            self.turn_started = Some(Instant::now());
                                        }
                                    } else {
                                        self.active_agent_id = None;
                                        self.turn_started = None;
                                        self.cancel = None;
                                    }
                                    self.sync_live_phrase();
                                    self.clear_render_cache();
                                    return Action::Continue;
                                }
                                _ => {}
                            }
                        }
                        if let AgentEvent::ProcessOutput { id, bytes, total } = &event {
                            self.session_event_sequence = sequence;
                            if let Ok(proc_id) = id.parse() {
                                let state = self.host_tail_state.entry(proc_id).or_default();
                                state.offset = (*total).max(state.offset);
                                state.push(bytes);
                                self.host_tails.insert(proc_id, state.tail());
                            }
                            return Action::Continue;
                        }
                        self.clear_render_cache();
                        let initial_user_echo = match &event {
                            AgentEvent::UserMessage(message) => {
                                let consume = |pending: &mut HashMap<
                                    String,
                                    std::collections::VecDeque<String>,
                                >| {
                                    pending
                                        .get_mut(&agent_id)
                                        .and_then(|queue| {
                                            (queue.front() == Some(message))
                                                .then(|| queue.pop_front())
                                        })
                                        .flatten()
                                        .is_some()
                                };
                                // Initial turns and mailbox turns both have
                                // optimistic rows, but only mailbox rows are
                                // displayed in the pending rail.
                                consume(&mut self.pending_initial_user_echoes)
                            }
                            _ => false,
                        };
                        // Mailbox messages are only pending until the agent
                        // actually starts consuming them. Remove their rail
                        // echo here, but do fold the event into the chat.
                        if let AgentEvent::UserMessage(message) = &event {
                            let _ = match self.pending_remote_user_echoes.get_mut(&agent_id) {
                                Some(queue) => {
                                    queue.front() == Some(message) && queue.pop_front().is_some()
                                }
                                None => false,
                            };
                        }
                        if !initial_user_echo {
                            fold_event(
                                self.transcripts.entry(agent_id.clone()).or_default(),
                                &event,
                            );
                        }
                        self.refresh_semantic_transcript(&agent_id);
                        self.resolve_intent(&event, &agent_id);
                        if let Some(activity) = run::activity_phrase(&event) {
                            self.run_liveness.note_activity(&agent_id, activity);
                        }
                        if matches!(event, AgentEvent::TurnFinished) {
                            // `TurnFinished` commits the assistant message
                            // *before* tool execution, so activity is still
                            // live. Authoritative completion is `TurnDone`.
                        } else if agent_id == self.focused_id
                            && self.remote_turn_id.is_none()
                            && !self.busy
                        {
                            // Daemon-initiated turns (goal launches, mailbox
                            // wakes) never pass through a local submit, so
                            // streaming activity is the only signal that a
                            // turn is live. Reflect it before the phrase
                            // sync; the authoritative completion arrives as
                            // `TurnCompleted` (or the next snapshot refresh).
                            self.busy = true;
                            if self.turn_started.is_none() {
                                self.turn_started = Some(Instant::now());
                            }
                            self.active_agent_id = Some(agent_id.clone());
                        }
                        if agent_id == self.focused_id
                            && matches!(event, AgentEvent::CompactionFinished { .. })
                        {
                            self.ctx_used = 0;
                        }
                        if agent_id == self.focused_id
                            && let AgentEvent::Usage(usage) = &event
                        {
                            self.ctx_used = usage.input_tokens;
                        }
                        self.sync_live_phrase();
                        Action::Continue
                    }
                    firmius_core::SessionEventPayload::Todo { agent_id, .. } => {
                        // The rail projects live canonical state each frame, so
                        // an embedded invalidation only has to drop the cached
                        // transcript (which carries the embedded work block).
                        let _ = agent_id;
                        self.clear_render_cache();
                        Action::Continue
                    }
                    _ => Action::Continue,
                }
            }
            AppEvent::Todo(event) => {
                if let Some(snapshot) = self.remote_snapshot.as_mut()
                    && snapshot.session_id == event.session_id
                    && let Some(agent) = snapshot
                        .agents
                        .iter_mut()
                        .find(|agent| agent.record.id == event.agent_id)
                    && agent
                        .todo
                        .as_ref()
                        .is_none_or(|current| event.projection.revision >= current.revision)
                {
                    agent.todo = Some(event.projection);
                }
                self.clear_render_cache();
                Action::Continue
            }
            AppEvent::BusLagged(_) => Action::RebuildTranscripts,
            AppEvent::RemoteRecovery(reason) => {
                self.flash(&format!("daemon resync: {reason}"));
                Action::RefreshRemote
            }
            AppEvent::RemoteDisconnected(reason) => {
                self.busy = false;
                self.remote_turn_id = None;
                self.flash(&reason);
                Action::Continue
            }
            AppEvent::RemoteShutdown => {
                self.busy = false;
                self.remote_turn_id = None;
                self.flash("daemon is shutting down");
                Action::Continue
            }
            AppEvent::WorkRecovery => {
                self.reload_work_snapshot();
                Action::Continue
            }
            AppEvent::Compaction { agent_id, event } => {
                self.clear_render_cache();
                fold_event(
                    self.transcripts.entry(agent_id.clone()).or_default(),
                    &event,
                );
                self.resolve_intent(&event, &agent_id);
                if agent_id == self.focused_id
                    && matches!(event, AgentEvent::CompactionStarted { .. })
                {
                    self.busy = true;
                    self.sync_live_phrase();
                }
                if matches!(
                    event,
                    AgentEvent::CompactionFinished { .. }
                        | AgentEvent::CompactionDiscarded { .. }
                        | AgentEvent::CompactionFailed { .. }
                ) {
                    if agent_id == self.focused_id
                        && matches!(event, AgentEvent::CompactionFinished { .. })
                    {
                        self.ctx_used = 0;
                    }
                    // A compaction event is part of the surrounding turn. Do
                    // not clear the turn-level busy/cancellation state here;
                    // `TurnDone`/`RemoteTurnDone` is the authoritative end.
                    if agent_id == self.focused_id {
                        self.sync_live_phrase();
                    }
                }
                Action::Continue
            }
            AppEvent::TurnDone(res) => {
                self.clear_render_cache();
                self.busy = false;
                self.turn_started = None;
                self.cancel = None;
                self.active_agent_id = None;
                self.local_turn_intent.clear();
                self.pending_initial_user_echoes.clear();
                self.sync_live_phrase();
                if let Err(e) = res {
                    if e.contains("cancelled") {
                        self.flash("cancelled");
                    } else {
                        self.transcripts
                            .entry(self.primary_id.clone())
                            .or_default()
                            .push(Item::Note(format!("error: {e}")));
                        self.flash("turn failed");
                    }
                }
                Action::Continue
            }
            AppEvent::Term(te) => match te {
                TermEvent::Key(k) if k.kind != KeyEventKind::Release => self.key(k),
                TermEvent::Mouse(mouse) => {
                    self.mouse(mouse);
                    Action::Continue
                }
                TermEvent::Paste(text) => {
                    if text.chars().count() > PASTE_BLOCK_THRESHOLD {
                        self.pastes.push(StoredPaste::Text(text));
                        self.composer.insert_paste_block(self.pastes.len());
                    } else {
                        self.composer.insert_str(&text);
                    }
                    self.refresh_completion();
                    Action::Continue
                }
                _ => Action::Continue,
            },
        }
    }

    /// The one and only key dispatch.
    fn key(&mut self, k: KeyEvent) -> Action {
        use KeyCode as C;
        let m = k.modifiers;
        // Any new key is an opportunity to show completion again.  This is
        // cleared here (rather than only in the character arms) so cursor
        // edits and paste-related key sequences cannot leave the dismissal
        // stuck on an old input string.
        self.completion_dismissed = None;
        let action = match k.code {
            C::Char('c') if m.contains(KeyModifiers::CONTROL) => return Action::Quit,
            // Crossterm normally reports control letters in lower case, but
            // terminals differ when Shift is held (and some report the
            // resulting upper-case character).  Accept both spellings so
            // Ctrl+N/P and Ctrl+Shift+B cannot accidentally fall through to
            // the composer.
            C::Char('n' | 'N') if m.contains(KeyModifiers::CONTROL) => {
                self.cycle_focus(1);
                Action::Continue
            }
            // Ctrl+P navigates to the parent agent when focused on a child;
            // Ctrl+K opens the command palette.
            C::Char('p' | 'P') if m.contains(KeyModifiers::CONTROL) => {
                if let Some(parent) = self.parent_by_agent.get(&self.focused_id).cloned() {
                    self.focused_id = parent;
                    self.reload_work_snapshot();
                    self.viewport.follow = true;
                    self.clear_render_cache();
                }
                Action::Continue
            }
            C::Char('k') if m.contains(KeyModifiers::CONTROL) => Action::OpenCommandPalette,
            // Ctrl+H is intentionally unused by the current command deck and
            // is the portable fallback for terminals that cannot emit Alt+Tab.
            C::Char('h') if m.contains(KeyModifiers::CONTROL) => self.cycle_permission_mode(),
            C::Tab if m.contains(KeyModifiers::ALT) => self.cycle_permission_mode(),
            C::Char('b' | 'B')
                if m.contains(KeyModifiers::CONTROL) && m.contains(KeyModifiers::SHIFT) =>
            {
                self.cycle_focus(-1);
                Action::Continue
            }
            // A few terminal/keymap combinations encode Ctrl+Shift+B as an
            // upper-case character but omit the SHIFT modifier.  Treat that
            // unambiguously upper-case control character the same way.
            C::Char('B') if m.contains(KeyModifiers::CONTROL) => {
                self.cycle_focus(-1);
                Action::Continue
            }
            C::Char('u') if m.contains(KeyModifiers::CONTROL) => {
                self.composer.clear();
                Action::Continue
            }
            C::Esc => {
                if self.completion.take().is_some() {
                    self.completion_dismissed = Some(self.composer.text(&self.pastes));
                    return Action::Continue;
                }
                if let Some(c) = &self.cancel {
                    c.cancel();
                    self.flash("cancelling…");
                } else if let Some(turn_id) = self.remote_turn_id {
                    self.flash("cancelling…");
                    return Action::CancelRemote { turn_id };
                }
                Action::Continue
            }
            C::Enter => {
                if m.contains(KeyModifiers::ALT) || m.contains(KeyModifiers::SHIFT) {
                    self.composer.newline();
                    Action::Continue
                } else if self.completion.is_some() {
                    let accepted = self.accept_completion();
                    // If the command completion leaves the cursor at an
                    // empty argument, show that argument's suggestions
                    // immediately. Do not reopen the menu for a completed
                    // argument, since the next Enter should submit it.
                    if accepted
                        && self
                            .composer
                            .text(&self.pastes)
                            .ends_with(char::is_whitespace)
                    {
                        self.refresh_completion();
                    } else if accepted {
                        self.completion_dismissed = Some(self.composer.text(&self.pastes));
                    }
                    return Action::Continue;
                } else {
                    self.submit()
                }
            }
            C::Backspace if m.contains(KeyModifiers::ALT) => {
                self.composer.backspace_word();
                Action::Continue
            }
            C::Backspace => {
                self.composer.backspace();
                Action::Continue
            }
            C::Delete => {
                self.composer.delete();
                Action::Continue
            }
            C::Left if m.contains(KeyModifiers::ALT) => {
                self.composer.word_left();
                Action::Continue
            }
            C::Left => {
                self.composer.left();
                Action::Continue
            }
            C::Right if m.contains(KeyModifiers::ALT) => {
                self.composer.word_right();
                Action::Continue
            }
            C::Right => {
                self.composer.right();
                Action::Continue
            }
            // Readline-style beginning/end-of-line shortcuts.  Refresh the
            // display layout first because Composer movement is deliberately
            // line-aware and its cache is normally refreshed by rendering.
            C::Char('a' | 'A') if m.contains(KeyModifiers::CONTROL) => {
                self.composer.lines(&self.pastes);
                self.composer.home();
                Action::Continue
            }
            C::Char('e' | 'E') if m.contains(KeyModifiers::CONTROL) => {
                self.composer.lines(&self.pastes);
                self.composer.end();
                Action::Continue
            }
            // Some terminals encode Alt+Left/Right as the readline-style
            // escape sequences Alt+b/Alt+f.  Crossterm exposes those as
            // modified character events, so handle them as navigation
            // rather than letting the characters reach the composer.
            C::Char('b') if m.contains(KeyModifiers::ALT) && !m.contains(KeyModifiers::CONTROL) => {
                self.composer.word_left();
                Action::Continue
            }
            C::Char('f') if m.contains(KeyModifiers::ALT) && !m.contains(KeyModifiers::CONTROL) => {
                self.composer.word_right();
                Action::Continue
            }
            C::Char('y') if m.contains(KeyModifiers::CONTROL) => match self.last_assistant_text() {
                Some(text) => Action::CopyText(text),
                None => {
                    self.flash("nothing to copy");
                    Action::Continue
                }
            },
            C::Up if self.completion_move(-1) => return Action::Continue,
            C::Down if self.completion_move(1) => return Action::Continue,
            C::Up => {
                let lines = self.composer.lines(&self.pastes);
                let (row, _) = self.composer.cursor_pos(&self.pastes);
                if lines.len() <= 1 || row == 0 {
                    self.recall_prompt(true);
                } else {
                    self.composer.up();
                }
                Action::Continue
            }
            C::Down => {
                let lines = self.composer.lines(&self.pastes);
                let (row, _) = self.composer.cursor_pos(&self.pastes);
                if self.prompt_history_index.is_some()
                    && (lines.len() <= 1 || row + 1 >= lines.len())
                {
                    self.recall_prompt(false);
                } else {
                    self.composer.down();
                }
                Action::Continue
            }
            C::PageUp => {
                self.scroll(-10);
                Action::Continue
            }
            C::PageDown => {
                self.scroll(10);
                Action::Continue
            }
            C::Home if m.contains(KeyModifiers::CONTROL) => {
                self.viewport.follow = false;
                self.viewport.offset = usize::MAX / 4;
                self.viewport.anchor_top.set(None);
                self.clear_render_cache();
                Action::Continue
            }
            C::End if m.contains(KeyModifiers::CONTROL) => {
                self.viewport.follow = true;
                self.viewport.offset = 0;
                self.viewport.anchor_top.set(None);
                Action::Continue
            }
            C::Home => {
                self.composer.home();
                Action::Continue
            }
            C::End => {
                self.composer.end();
                Action::Continue
            }
            C::Tab if self.completion.is_some() => {
                if self.accept_completion() {
                    self.completion_dismissed = Some(self.composer.text(&self.pastes));
                }
                return Action::Continue;
            }
            C::BackTab => self.cycle_focused_persona(),
            C::Char(c) if !m.contains(KeyModifiers::CONTROL) => {
                self.composer.insert_char(c);
                Action::Continue
            }
            _ => Action::Continue,
        };
        self.refresh_completion();
        action
    }

    fn submit(&mut self) -> Action {
        let Some(submission) = self.composer.submission(&self.pastes) else {
            return Action::Continue;
        };
        if let ComposerSubmission::Text(text) = &submission
            && text.starts_with('/')
        {
            self.composer.clear();
            self.prompt_history_index = None;
            self.draft_before_history = None;
            return self.run_command(text);
        }
        if self.busy {
            if self.daemon.is_some() {
                let message = match submission {
                    ComposerSubmission::Text(text) => {
                        self.remember_prompt(&text);
                        Message::text(MessageRole::User, text)
                    }
                    ComposerSubmission::Message(message) => message,
                };
                if let Some(summary) = summarize_user_message(&message) {
                    self.pending_remote_user_echoes
                        .entry(self.focused_id.clone())
                        .or_default()
                        .push_back(summary);
                    self.clear_render_cache();
                }
                self.composer.clear();
                return Action::QueueRemote {
                    agent_id: self.focused_id.clone(),
                    message,
                };
            }
            match submission {
                ComposerSubmission::Text(text) => {
                    self.remember_prompt(&text);
                    self.composer.clear();
                    if let Some(agent) = self.agents.get(&self.focused_id) {
                        if let Err(error) = agent.submit_and_wake(text) {
                            self.flash(&format!("message save failed: {error}"));
                        }
                    }
                }
                ComposerSubmission::Message(message) => {
                    self.composer.clear();
                    if let Some(agent) = self.agents.get(&self.focused_id) {
                        if let Err(error) = agent.submit_message_and_wake(message) {
                            self.flash(&format!("message save failed: {error}"));
                        }
                    }
                }
            }
            return Action::Continue;
        }
        if self.daemon.is_some() {
            let message = match submission {
                ComposerSubmission::Text(text) => Message::text(MessageRole::User, text),
                ComposerSubmission::Message(message) => message,
            };
            // Remember the draft before clearing so a rejected submission
            // (busy daemon, transport failure) can be restored verbatim.
            let draft = self.composer.text(&self.pastes);
            if let Some(summary) = summarize_user_message(&message) {
                self.remember_prompt(&summary);
                // Render accepted intent immediately. The daemon's initial
                // user message is committed to history before streaming, but
                // it does not emit a UserMessage event (that event is for
                // mailbox input injected during a turn). Without this local
                // echo, the prompt remains invisible until a snapshot reload.
                self.transcripts
                    .entry(self.focused_id.clone())
                    .or_default()
                    .push(Item::User(summary));
                self.pending_initial_user_echoes
                    .entry(self.focused_id.clone())
                    .or_default()
                    .push_back(summarize_user_message(&message).unwrap_or_default());
            }
            self.composer.clear();
            self.clear_render_cache();
            // Record the submission identity before the request leaves. The
            // idle-submit path previously set `busy` without any marker the
            // snapshot reconciler could read, so a snapshot that raced the
            // request (or the turn after a queue) showed the session idle
            // while the daemon was already executing this turn.
            self.draft_before_remote_submit = Some(draft);
            self.local_turn_intent.insert(self.focused_id.clone());
            self.busy = true;
            self.active_agent_id = self
                .remote_snapshot
                .as_ref()
                .map(|_| self.focused_id.clone());
            self.turn_started = Some(Instant::now());
            self.viewport.follow = true;
            self.sync_live_phrase();
            return Action::SubmitRemote {
                agent_id: self
                    .remote_snapshot
                    .as_ref()
                    .map(|_| self.focused_id.clone()),
                message,
            };
        }
        if let Err(e) = self.ensure_started() {
            self.flash(&e);
            return Action::Continue;
        }
        self.warn_if_search_unsupported();
        let message = match submission {
            ComposerSubmission::Text(text) => Message::text(MessageRole::User, text),
            ComposerSubmission::Message(message) => message,
        };
        let has_image = message
            .content
            .iter()
            .any(|part| matches!(part, MessagePart::Image(_)));
        if has_image {
            let (provider_id, model_id, _) = self.focused_model_status();
            if !self.manager.lock().unwrap().model_supports(
                &provider_id,
                &model_id,
                ModelCapability::Image,
            ) {
                self.flash("current model does not support image inputs");
                return Action::Continue;
            }
        }
        if let Some(summary) = summarize_user_message(&message) {
            self.remember_prompt(&summary);
        }
        self.composer.clear();
        let agent_id = self.focused_id.clone();
        if let Some(summary) = summarize_user_message(&message) {
            self.transcripts
                .entry(agent_id.clone())
                .or_default()
                .push(Item::User(summary));
        }
        self.clear_render_cache();
        self.busy = true;
        self.active_agent_id = Some(agent_id.clone());
        self.turn_started = Some(Instant::now());
        self.viewport.follow = true;
        let token = CancellationToken::new();
        self.cancel = Some(token.clone());
        self.sync_live_phrase();
        Action::Submit {
            agent_id,
            message,
            token,
        }
    }

    pub(crate) fn run_command(&mut self, line: &str) -> Action {
        self.clear_render_cache();
        use command::Command;
        let parsed = match command::parse(line) {
            Ok(command) => command,
            Err(e) => {
                self.flash(&e.to_string());
                return Action::Continue;
            }
        };
        if self.busy && !command::busy_ok(&parsed) {
            self.flash("busy — try again after the turn");
            return Action::Continue;
        }
        match parsed {
            Command::Quit => Action::Quit,
            Command::Help => {
                let help = command::help_text();
                self.transcripts
                    .entry(self.primary_id.clone())
                    .or_default()
                    .push(Item::Note(help));
                Action::Continue
            }
            Command::Status => {
                let (session, provider, model) = self
                    .primary
                    .as_ref()
                    .map(|agent| {
                        (
                            agent.session_id.clone(),
                            agent.config().provider_id.clone(),
                            agent.config().model.clone(),
                        )
                    })
                    .unwrap_or_else(|| ("welcome".into(), "none".into(), self.model.clone()));
                let title = self.session_title_label();
                self.transcripts
                    .entry(self.primary_id.clone())
                    .or_default()
                    .push(Item::Note(format!(
                        "session={} · title={} · agents={} · provider={} · model={} · {}",
                        session,
                        title,
                        self.roster.len(),
                        provider,
                        model,
                        if self.busy { "busy" } else { "idle" },
                    )));
                Action::Continue
            }
            Command::UpdateCheck => Action::UpdateCheck,
            Command::SshHosts => {
                let saved = self.settings.lock().unwrap().remote_hosts.clone();
                match firmius_core::discover_ssh_hosts() {
                    Ok(hosts) if hosts.is_empty() => self
                        .transcripts
                        .entry(self.primary_id.clone())
                        .or_default()
                        .push(Item::Note(
                            "no concrete SSH hosts found in ~/.ssh/config or ~/.ssh/known_hosts"
                                .into(),
                        )),
                    Ok(hosts) => {
                        let lines = hosts
                            .into_iter()
                            .map(|host| {
                                format!(
                                    "{}\t{}\t{}\t{}\t{}",
                                    host.alias,
                                    host.hostname.unwrap_or_else(|| "-".into()),
                                    host.user.unwrap_or_else(|| "-".into()),
                                    host.port
                                        .map(|port| port.to_string())
                                        .unwrap_or_else(|| "22".into()),
                                    format!(
                                        "{}{}",
                                        if host.configured {
                                            "config"
                                        } else {
                                            "known_hosts"
                                        },
                                        saved
                                            .iter()
                                            .find(|saved| saved.alias == host.alias)
                                            .map(|saved| format!(" saved:{}", saved.directory))
                                            .unwrap_or_default()
                                    )
                                )
                            })
                            .collect::<Vec<_>>()
                            .join("\n");
                        self.transcripts
                            .entry(self.primary_id.clone())
                            .or_default()
                            .push(Item::Note(lines));
                    }
                    Err(error) => self.flash(&format!("SSH discovery failed: {error}")),
                }
                Action::Continue
            }
            Command::SshAdd { alias, directory } => {
                match firmius_core::discover_ssh_hosts() {
                    Ok(hosts) if !hosts.iter().any(|host| host.alias == alias) => {
                        self.flash(&format!("SSH host not discovered: {alias}"));
                    }
                    Ok(_) => {
                        let message = {
                            let mut settings = self.settings.lock().unwrap();
                            match settings.save_remote_host(alias.clone(), directory.clone()) {
                                Ok(()) => match settings.save() {
                                    Ok(()) => format!("saved SSH host {alias} → {directory}"),
                                    Err(error) => format!("SSH host save failed: {error}"),
                                },
                                Err(error) => format!("SSH host invalid: {error}"),
                            }
                        };
                        self.flash(&message);
                    }
                    Err(error) => self.flash(&format!("SSH discovery failed: {error}")),
                }
                Action::Continue
            }
            Command::SshSaved { alias } => {
                let directory = self
                    .settings
                    .lock()
                    .unwrap()
                    .remote_hosts
                    .iter()
                    .find(|saved| saved.alias == alias)
                    .map(|saved| saved.directory.clone());
                match directory {
                    Some(directory) => Action::OpenRemoteSession {
                        workspace: format!("ssh://{alias}/{}", directory.trim_start_matches('/')),
                    },
                    None => {
                        self.flash(&format!("no saved SSH host: {alias}"));
                        Action::Continue
                    }
                }
            }
            Command::Ssh { alias, directory } => Action::OpenRemoteSession {
                workspace: format!("ssh://{alias}/{}", directory.trim_start_matches('/')),
            },
            Command::Save => Action::Save,
            Command::Compact => {
                if self.daemon.is_some() && self.remote_snapshot.is_some() {
                    self.composer.clear();
                    self.busy = true;
                    self.active_agent_id = Some(self.primary_id.clone());
                    self.turn_started = Some(Instant::now());
                    self.sync_live_phrase();
                    return Action::Compact;
                }
                let Some(agent) = self.primary.clone() else {
                    self.flash("no active session");
                    return Action::Continue;
                };
                self.composer.clear();
                self.busy = true;
                self.active_agent_id = Some(agent.id.clone());
                self.turn_started = Some(Instant::now());
                self.sync_live_phrase();
                Action::Compact
            }
            Command::Agents => {
                let roster = self
                    .roster
                    .iter()
                    .map(|(id, label)| format!("{label}: {id}"))
                    .collect::<Vec<_>>()
                    .join("\n");
                self.transcripts
                    .entry(self.primary_id.clone())
                    .or_default()
                    .push(Item::Note(if roster.is_empty() {
                        "no agents".into()
                    } else {
                        roster
                    }));
                Action::Continue
            }
            Command::Clear => {
                self.transcripts.insert(self.focused_id.clone(), Vec::new());
                Action::Continue
            }
            Command::Login { kind } => Action::OpenLogin { kind },
            Command::Accounts { provider } => Action::OpenAccounts { provider },
            Command::Personas => Action::OpenPersonas,
            Command::Settings => Action::OpenSettings,
            Command::Onboarding => Action::OpenOnboarding,
            Command::Mcp { action } => Action::Mcp(action),
            Command::Workflow { action } => match action {
                command::WorkflowAction::Picker => Action::OpenWorkflowPicker,
                command::WorkflowAction::Insert { path } => {
                    Action::LoadWorkflow { path, run: false }
                }
                command::WorkflowAction::Run { path } => Action::LoadWorkflow { path, run: true },
                command::WorkflowAction::List => {
                    let text = super::workflow::discover()
                        .into_iter()
                        .map(|f| f.label)
                        .collect::<Vec<_>>()
                        .join("\n");
                    self.transcripts
                        .entry(self.primary_id.clone())
                        .or_default()
                        .push(Item::Note(if text.is_empty() {
                            "no workflows found".into()
                        } else {
                            text
                        }));
                    Action::Continue
                }
            },
            Command::Rewind { turns } => {
                if self.daemon.is_some() && self.remote_snapshot.is_some() {
                    return Action::RewindRemote {
                        agent_id: self.focused_id.clone(),
                        turns,
                    };
                }
                let Some(primary) = &self.primary else {
                    self.flash("no active session");
                    return Action::Continue;
                };
                match primary.rewind(turns) {
                    Ok(removed) => {
                        self.transcripts.insert(
                            self.primary_id.clone(),
                            items_from_history(&primary.history()),
                        );
                        self.flash(&format!("rewound {removed} messages"));
                    }
                    Err(e) => self.flash(&e.to_string()),
                }
                Action::Continue
            }
            Command::EditHistory { action } => Action::EditHistory {
                agent_id: self.focused_id.clone(),
                action,
            },
            Command::Model { id } => {
                let Some((provider_id, model_id)) = id.split_once('/') else {
                    self.flash("model must use provider/model format");
                    return Action::Continue;
                };
                if provider_id.is_empty() || model_id.is_empty() {
                    self.flash("model must use provider/model format");
                    return Action::Continue;
                }
                let provider_id = provider_id.to_string();
                let model_id = model_id.to_string();
                let (provider_id, model_id) = {
                    let manager = self.manager.lock().unwrap();
                    manager
                        .account_for_model(&provider_id, &model_id)
                        .unwrap_or((provider_id, model_id))
                };
                let known_provider = self.manager.lock().unwrap().schema(&provider_id).is_some();
                let supported_efforts = self.effort_modes_for_model(&provider_id, &model_id);
                if self.daemon.is_some() && self.remote_snapshot.is_some() {
                    let next_effort = self.effort.clone().filter(|current| {
                        supported_efforts
                            .iter()
                            .any(|supported| supported.name == current.name)
                    });
                    let persona_id = self.focused_persona_id();
                    if let Err(error) = self.persist_model_preference(
                        persona_id.as_deref(),
                        &provider_id,
                        &model_id,
                        next_effort.as_ref().map(|effort| effort.name.as_str()),
                    ) {
                        self.flash(&format!("preference save failed: {error}"));
                        return Action::Continue;
                    }
                    return Action::SetModelRemote {
                        agent_id: self.focused_id.clone(),
                        provider_id,
                        model: model_id,
                        effort: next_effort,
                    };
                }
                if let Some(primary) = &self.primary {
                    let result = if known_provider {
                        match self.manager.lock().unwrap().build(&provider_id) {
                            Ok(provider) => primary.set_provider(provider_id.clone(), provider),
                            Err(e) => Err(AgentError::Trajectory(e)),
                        }
                    } else {
                        Err(AgentError::Trajectory(format!(
                            "provider not configured: {provider_id}"
                        )))
                    };
                    match result.and_then(|()| {
                        primary.update_config(|config| {
                            config.model = model_id.clone();
                            if config.effort.as_ref().is_some_and(|current| {
                                !supported_efforts
                                    .iter()
                                    .any(|supported| supported.name == current.name)
                            }) {
                                config.effort = None;
                            }
                        })
                    }) {
                        Ok(()) => {
                            self.model = model_id.clone();
                            self.provider_id = provider_id.clone();
                            self.effort = primary.config().effort;
                            let persona_id = primary.config().persona;
                            match self.persist_model_preference(
                                persona_id.as_deref(),
                                &provider_id,
                                &model_id,
                                self.effort.as_ref().map(|effort| effort.name.as_str()),
                            ) {
                                Ok(()) => self.flash(&format!(
                                    "model: {provider_id}/{model_id} · preference saved"
                                )),
                                Err(error) => self.flash(&format!(
                                    "model changed, but preference save failed: {error}"
                                )),
                            }
                        }
                        Err(AgentError::Busy) => {
                            self.flash("busy — model changes wait for the turn")
                        }
                        Err(e) => self.flash(&e.to_string()),
                    }
                } else {
                    let persona_id = self.pending_persona.clone();
                    let next_effort = self.effort.clone().filter(|current| {
                        supported_efforts
                            .iter()
                            .any(|supported| supported.name == current.name)
                    });
                    match self.persist_model_preference(
                        persona_id.as_deref(),
                        &provider_id,
                        &model_id,
                        next_effort.as_ref().map(|effort| effort.name.as_str()),
                    ) {
                        Ok(()) => {
                            self.model = model_id.clone();
                            self.provider_id = provider_id.clone();
                            self.effort = next_effort;
                            self.flash(&format!(
                                "model: {provider_id}/{model_id} · preference saved"
                            ));
                        }
                        Err(error) => self.flash(&format!("preference save failed: {error}")),
                    }
                }
                Action::Continue
            }
            Command::Effort { name } => {
                let supported = self
                    .effort_modes_for_focused()
                    .iter()
                    .any(|mode| mode.name == name);
                if !supported {
                    self.flash(&format!("effort '{name}' is not supported by this model"));
                    return Action::Continue;
                }
                let effort = effort_from_name(&name);
                if self.daemon.is_some() && self.remote_snapshot.is_some() {
                    let (provider_id, model, _) = self.focused_model_status();
                    let persona_id = self.focused_persona_id();
                    if let Err(error) = self.persist_model_preference(
                        persona_id.as_deref(),
                        &provider_id,
                        &model,
                        Some(&effort.name),
                    ) {
                        self.flash(&format!("preference save failed: {error}"));
                        return Action::Continue;
                    }
                    return Action::SetModelRemote {
                        agent_id: self.focused_id.clone(),
                        provider_id,
                        model,
                        effort: Some(effort),
                    };
                }
                if let Some(agent) = self.agents.get(&self.focused_id).cloned().or_else(|| {
                    self.primary
                        .as_ref()
                        .filter(|agent| agent.id == self.focused_id)
                        .cloned()
                }) {
                    match agent.update_config(|config| config.effort = Some(effort.clone())) {
                        Ok(()) => {
                            if agent.id == self.primary_id {
                                self.effort = Some(effort.clone());
                            }
                            let config = agent.config();
                            let persist_for_persona = config.persona.clone();
                            let persist =
                                if persist_for_persona.is_some() || agent.id == self.primary_id {
                                    self.persist_model_preference(
                                        persist_for_persona.as_deref(),
                                        &config.provider_id,
                                        &config.model,
                                        Some(&effort.name),
                                    )
                                } else {
                                    Ok(())
                                };
                            if let Err(error) = persist {
                                self.flash(&format!(
                                    "effort changed, but preference save failed: {error}"
                                ));
                            } else {
                                self.flash(&format!("effort: {name} · preference saved"));
                            }
                        }
                        Err(AgentError::Busy) => {
                            self.flash("busy — effort changes wait for the turn")
                        }
                        Err(e) => self.flash(&e.to_string()),
                    }
                } else {
                    self.effort = Some(effort.clone());
                    let persona_id = self.pending_persona.clone();
                    match self.persist_model_preference(
                        persona_id.as_deref(),
                        &self.provider_id,
                        &self.model,
                        Some(&effort.name),
                    ) {
                        Ok(()) => self.flash("effort preference saved for the next session"),
                        Err(error) => self.flash(&format!(
                            "effort changed, but preference save failed: {error}"
                        )),
                    }
                }
                Action::Continue
            }
            Command::Theme { name } => {
                let Some(next) = theme::by_name(&name) else {
                    self.flash(&format!(
                        "unknown theme '{name}' — available: {}",
                        theme::all()
                            .iter()
                            .map(|t| t.name)
                            .collect::<Vec<_>>()
                            .join(", ")
                    ));
                    return Action::Continue;
                };
                let mut next_settings = self.settings.lock().unwrap().clone();
                next_settings.theme = Some(next.name.to_string());
                match next_settings.save() {
                    Ok(()) => {
                        *self.settings.lock().unwrap() = next_settings;
                        self.theme = next;
                        self.clear_render_cache();
                        self.flash(&format!("theme: {} · preference saved", next.name));
                    }
                    Err(error) => self.flash(&format!(
                        "theme changed, but preference save failed: {error}"
                    )),
                }
                Action::Continue
            }
            Command::Resume { id } => Action::Resume(id),
            Command::Title { title } => {
                if self.daemon.is_some() && self.remote_snapshot.is_some() {
                    if title.is_none() {
                        self.flash(&format!("title: {}", self.session_title_label()));
                        return Action::Continue;
                    }
                    return Action::SetTitleRemote(title);
                }
                let Some(session) = self.session.clone() else {
                    self.flash("no active session");
                    return Action::Continue;
                };
                match title {
                    None => {
                        let current = session.title().unwrap_or_else(|| "(untitled)".into());
                        self.flash(&format!("title: {current}"));
                    }
                    Some(name) => {
                        session.set_title(Some(name.clone()));
                        match session.save() {
                            Ok(()) => self.flash(&format!("title: {name}")),
                            Err(e) => self.flash(&format!("title set, save failed: {e}")),
                        }
                    }
                }
                Action::Continue
            }
            Command::Copy { all } => {
                let text = if all {
                    self.transcript_plain()
                } else {
                    self.last_assistant_text().unwrap_or_default()
                };
                if text.trim().is_empty() {
                    self.flash("nothing to copy");
                    Action::Continue
                } else {
                    Action::CopyText(text)
                }
            }
            Command::Export { path } => {
                if self.daemon.is_some() && self.remote_snapshot.is_some() {
                    return Action::ExportRemote(path);
                }
                let Some(session) = self.session.clone() else {
                    self.flash("no active session");
                    return Action::Continue;
                };
                match session.snapshot_record() {
                    Ok(record) => {
                        let markdown = firmius_core::session_to_markdown(&record);
                        let dest = path.unwrap_or_else(|| {
                            let slug = session
                                .title()
                                .unwrap_or_else(|| format!("session-{}", session.id));
                            let slug: String = slug
                                .chars()
                                .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
                                .collect();
                            let slug = slug.trim_matches('-');
                            format!("{slug}.md")
                        });
                        match std::fs::write(&dest, markdown) {
                            Ok(()) => self.flash(&format!("exported {dest}")),
                            Err(e) => self.flash(&format!("export failed: {e}")),
                        }
                    }
                    Err(e) => self.flash(&format!("export failed: {e}")),
                }
                Action::Continue
            }
            Command::Permissions => Action::OpenPermissions,
            Command::Memory { query } => Action::MemoryQuery(query),
            Command::New => Action::NewSession,
            Command::Search { mode } => {
                self.apply_search_command(mode);
                Action::Continue
            }
            Command::Goal { action } => {
                if self.daemon.is_some() {
                    Action::Goal(action)
                } else {
                    match action {
                        // The embedded runtime has no goal repository yet;
                        // preserve the useful natural-language path by
                        // submitting the description as a normal turn.
                        command::GoalAction::Create { description, .. } => {
                            self.composer.replace_text(&description);
                            self.submit()
                        }
                        _ => {
                            self.flash("goal API requires the daemon");
                            Action::Continue
                        }
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        Action, CompactionPhase, CompletionItem, CompletionState, Item, Model, SearchState,
        ToolState, TranscriptHitRegion, Viewport, async_refresh_due, fold_event, fuzzy_score,
        reconcile_snapshot_transcripts, result_field,
    };
    use crate::tui::composer::{Composer, ComposerSubmission, PastedImage, StoredPaste};
    use crate::tui::event::AppEvent;
    use crate::tui::presentation::{HitSubtarget, SemanticId};
    use crate::tui::work::{WorkLine, WorkPresentation};
    use crossterm::event::{
        Event as TermEvent, KeyCode, KeyEvent, KeyModifiers, MouseEvent, MouseEventKind,
    };
    use firmius_core::{
        AccountRecord, Agent, AgentConfig, AgentEvent, ApiType, CodexKind, EffortMode,
        ExecutionStatus, FirmiusConfig, McpManager, Message, MessagePart, MessageRole,
        ModelCapabilities, ModelCapability, ModelInfo, PersonaManager, ProviderManager,
        ProviderSchema, SessionEvent, ToolRegistry, UserSettings, WebSearchAction,
    };
    use futures::StreamExt;
    use std::path::PathBuf;
    use std::sync::Arc;
    use std::time::{Duration, Instant};

    #[test]
    fn semantic_transcript_adapter_preserves_tool_identity_across_refresh() {
        let mut model = snapshot_test_model();
        let agent_id = model.focused_id.clone();
        model.transcripts.insert(
            agent_id.clone(),
            vec![Item::ToolCall {
                stream_id: Some("provider-call-1".into()),
                stream_index: 0,
                name: "bash".into(),
                args: "{}".into(),
                result: None,
                state: ToolState::Running(Instant::now()),
            }],
        );
        model.refresh_semantic_transcript(&agent_id);
        let first = model.semantic_events(&agent_id);
        model.refresh_semantic_transcript(&agent_id);
        let second = model.semantic_transcripts[&agent_id].clone();
        assert_eq!(first[0].id, second[0].id);
        assert_eq!(
            first[0].disclosure,
            super::super::presentation::DisclosurePolicy::LiveOutput
        );
    }

    #[test]
    fn default_prompt_uses_shared_operating_policy() {
        assert_eq!(
            super::default_system_prompt(),
            firmius_core::prompts::OPERATING_PROMPT
        );
    }

    #[test]
    fn async_refresh_is_throttled_below_animation_rate() {
        let now = Instant::now();
        assert!(async_refresh_due(None, now));
        assert!(!async_refresh_due(
            Some(now),
            now + Duration::from_millis(99)
        ));
        assert!(async_refresh_due(
            Some(now),
            now + Duration::from_millis(100)
        ));
    }

    #[test]
    fn model_resolves_assigned_pending_work_to_starting_not_running() {
        let model = snapshot_test_model();
        let row = WorkLine {
            node_id: firmius_core::NodeId::new(),
            agent_id: None,
            title: "delegated check".into(),
            detail: None,
            status: ExecutionStatus::Pending,
            glyph: "○",
            assigned: true,
            presentation: WorkPresentation::Pending,
        };
        assert_eq!(model.work_presentation(&row), WorkPresentation::Starting);
        assert_ne!(
            model.work_presentation(&row),
            WorkPresentation::Running,
            "delegated startup must not alias running"
        );
    }

    #[test]
    fn result_field_extracts_proc_ids() {
        let id = "550e8400-e29b-41d4-a716-446655440000";
        assert_eq!(
            result_field(&format!("proc_id={id}"), "proc_id").as_deref(),
            Some(id)
        );
        assert_eq!(result_field("output only", "proc_id"), None);
    }

    #[test]
    fn compaction_events_fold_into_one_streaming_transcript_item() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::CompactionScheduled { generation: 4 },
        );
        fold_event(&mut items, &AgentEvent::CompactionStarted { generation: 4 });
        fold_event(
            &mut items,
            &AgentEvent::CompactionDelta {
                generation: 4,
                delta: "hello ".into(),
            },
        );
        fold_event(
            &mut items,
            &AgentEvent::CompactionDelta {
                generation: 4,
                delta: "world".into(),
            },
        );
        fold_event(
            &mut items,
            &AgentEvent::CompactionFinished { generation: 4 },
        );

        assert_eq!(items.len(), 1);
        let Item::Compaction(item) = &items[0] else {
            panic!("expected compaction item");
        };
        assert_eq!(item.generation, 4);
        assert_eq!(item.summary, "hello world");
        assert!(matches!(item.phase, CompactionPhase::Finished));
    }

    fn temp_settings(name: &str) -> (PathBuf, Arc<std::sync::Mutex<UserSettings>>) {
        let path = std::env::temp_dir()
            .join(format!(
                "firmius-tui-settings-{name}-{}-{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            ))
            .join("settings.json");
        let settings = UserSettings::load_from_path(&path).unwrap();
        (path, Arc::new(std::sync::Mutex::new(settings)))
    }

    fn test_provider_manager() -> ProviderManager {
        let mut manager = ProviderManager::new();
        manager.register_schema(ProviderSchema {
            id: "test-provider".into(),
            api_type: ApiType::OpenAI,
            base_url: Some("http://localhost".into()),
            api_key_env: None,
            models: vec![
                ModelInfo {
                    id: "text-only".into(),
                    context_window: 128_000,
                    max_output_tokens: Some(8_192),
                    capabilities: ModelCapabilities::from([
                        ModelCapability::Text,
                        ModelCapability::ToolUse,
                    ]),
                    effort_modes: Vec::new(),
                },
                ModelInfo {
                    id: "vision".into(),
                    context_window: 128_000,
                    max_output_tokens: Some(8_192),
                    capabilities: ModelCapabilities::from([
                        ModelCapability::Text,
                        ModelCapability::Image,
                        ModelCapability::ToolUse,
                    ]),
                    effort_modes: Vec::new(),
                },
            ],
        });
        manager.set_api_key("test-provider", "test-key");
        manager
    }

    #[test]
    fn fuzzy_score_accepts_subsequences_and_rejects_non_matches() {
        assert!(fuzzy_score("rsm", "/resume").is_some());
        assert!(fuzzy_score("son", "sonnet-4").is_some());
        assert!(fuzzy_score("title", "Fix the title bug").is_some());
        assert!(fuzzy_score("xyz", "/resume").is_none());
    }

    #[test]
    fn fuzzy_score_prefers_contiguous_matches() {
        assert!(fuzzy_score("son", "sonnet") < fuzzy_score("son", "season-of-nothing"));
    }

    #[test]
    fn tool_call_delta_creates_running_presentation_before_tool_starts() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::ToolCallDelta {
                index: 0,
                id: "call-1".into(),
                name_delta: "bash".into(),
                args_delta: r#"{"command":"ls"}"#.into(),
            },
        );

        assert!(matches!(
            items.as_slice(),
            [Item::ToolCall {
                stream_id: Some(id),
                name,
                args,
                state: ToolState::Preparing(_),
                ..
            }] if id == "call-1" && name == "bash" && args == r#"{"command":"ls"}"#
        ));
    }

    #[test]
    fn replayed_tool_lifecycle_does_not_regress_completed_edit_to_running() {
        let history = vec![
            Message::with_parts(
                MessageRole::Assistant,
                [MessagePart::ToolCall {
                    id: "edit-call".into(),
                    name: "edit".into(),
                    args: "{}".into(),
                }],
            ),
            Message::tool_results([MessagePart::ToolResult {
                id: "edit-call".into(),
                content: "updated file".into(),
                ok: true,
            }]),
        ];
        let mut items = super::items_from_history(&history);

        // Snapshot history can already contain the result while the live
        // journal still replays the lifecycle events that preceded it.
        fold_event(
            &mut items,
            &AgentEvent::ToolCallStarted {
                index: 0,
                id: "edit-call".into(),
                name: "edit".into(),
                args: "{}".into(),
            },
        );
        assert!(matches!(
            items.as_slice(),
            [Item::ToolCall {
                state: ToolState::Done { ok: true, .. },
                result: Some(result),
                ..
            }] if result == "updated file"
        ));
    }

    #[test]
    fn persisted_tool_results_pair_by_call_id_not_reverse_order() {
        let history = vec![
            Message::with_parts(
                MessageRole::Assistant,
                [
                    MessagePart::ToolCall {
                        id: "call-a".into(),
                        name: "first".into(),
                        args: "{}".into(),
                    },
                    MessagePart::ToolCall {
                        id: "call-b".into(),
                        name: "second".into(),
                        args: "{}".into(),
                    },
                ],
            ),
            // Providers may persist results in completion order, not call
            // order.  Positional pairing would swap these two outputs.
            Message::tool_results([
                MessagePart::ToolResult {
                    id: "call-b".into(),
                    content: "result-b".into(),
                    ok: true,
                },
                MessagePart::ToolResult {
                    id: "call-a".into(),
                    content: "result-a".into(),
                    ok: true,
                },
            ]),
        ];

        let items = super::items_from_history(&history);
        assert!(matches!(
            &items[0],
            Item::ToolCall { stream_id: Some(id), result: Some(result), .. }
                if id == "call-a" && result == "result-a"
        ));
        assert!(matches!(
            &items[1],
            Item::ToolCall { stream_id: Some(id), result: Some(result), .. }
                if id == "call-b" && result == "result-b"
        ));
    }

    #[test]
    fn history_call_without_result_stays_pending_while_turn_may_still_execute() {
        // TurnFinished commits the assistant message before tool execution, so
        // a snapshot can legitimately capture a call with no result yet.
        let history = vec![
            Message::text(MessageRole::User, "run the checks"),
            Message::with_parts(
                MessageRole::Assistant,
                [MessagePart::ToolCall {
                    id: "call-1".into(),
                    name: "bash".into(),
                    args: "{}".into(),
                }],
            ),
        ];
        let items = super::items_from_history(&history);
        assert!(matches!(
            items.iter().find_map(|item| match item {
                Item::ToolCall {
                    stream_id: Some(id),
                    state,
                    ..
                } if id == "call-1" => Some(state),
                _ => None,
            }),
            Some(ToolState::Preparing(_))
        ));
    }

    #[test]
    fn history_call_without_result_finalizes_once_history_moves_past_it() {
        // A later non-tool message is committed evidence that the turn ended
        // without producing this call's result.
        let history = vec![
            Message::text(MessageRole::User, "run the checks"),
            Message::with_parts(
                MessageRole::Assistant,
                [MessagePart::ToolCall {
                    id: "call-1".into(),
                    name: "bash".into(),
                    args: "{}".into(),
                }],
            ),
            Message::text(MessageRole::User, "never mind, do this instead"),
        ];
        let items = super::items_from_history(&history);
        assert!(matches!(
            items.iter().find_map(|item| match item {
                Item::ToolCall {
                    stream_id: Some(id),
                    state,
                    ..
                } if id == "call-1" => Some(state),
                _ => None,
            }),
            Some(ToolState::Interrupted)
        ));
    }

    #[test]
    fn live_events_reconcile_history_seeded_pending_call_by_id() {
        let history = vec![
            Message::text(MessageRole::User, "run the checks"),
            Message::with_parts(
                MessageRole::Assistant,
                [MessagePart::ToolCall {
                    id: "call-1".into(),
                    name: "bash".into(),
                    args: "{}".into(),
                }],
            ),
        ];
        let mut items = super::items_from_history(&history);

        fold_event(
            &mut items,
            &AgentEvent::ToolCallStarted {
                index: 0,
                id: "call-1".into(),
                name: "bash".into(),
                args: r#"{"command":"make"}"#.into(),
            },
        );
        // The transcript keeps the user message ahead of the tool call; the
        // live event must upgrade that same item, not append a duplicate.
        assert_eq!(items.len(), 2);
        assert!(matches!(
            &items[1],
            Item::ToolCall {
                args,
                state: ToolState::Running(_),
                ..
            } if args == r#"{"command":"make"}"#
        ));

        fold_event(
            &mut items,
            &AgentEvent::ToolResult {
                index: 0,
                id: "call-1".into(),
                name: "bash".into(),
                ok: true,
                content: "ok".into(),
            },
        );
        assert_eq!(items.len(), 2);
        assert!(matches!(
            &items[1],
            Item::ToolCall {
                result: Some(content),
                state: ToolState::Done { ok: true, .. },
                ..
            } if content == "ok"
        ));
    }

    #[test]
    fn reversed_order_parallel_results_reconcile_by_call_id() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::ToolCallStarted {
                index: 0,
                id: "call-a".into(),
                name: "bash".into(),
                args: "{}".into(),
            },
        );
        fold_event(
            &mut items,
            &AgentEvent::ToolCallStarted {
                index: 1,
                id: "call-b".into(),
                name: "bash".into(),
                args: "{}".into(),
            },
        );
        // Results complete out of start order; positional pairing would swap
        // the outcomes.
        fold_event(
            &mut items,
            &AgentEvent::ToolResult {
                index: 1,
                id: "call-b".into(),
                name: "bash".into(),
                ok: false,
                content: "boom".into(),
            },
        );
        fold_event(
            &mut items,
            &AgentEvent::ToolResult {
                index: 0,
                id: "call-a".into(),
                name: "bash".into(),
                ok: true,
                content: "done".into(),
            },
        );

        assert_eq!(items.len(), 2);
        assert!(matches!(
            &items[0],
            Item::ToolCall {
                stream_id: Some(id),
                result: Some(content),
                state: ToolState::Done { ok: true, .. },
                ..
            } if id == "call-a" && content == "done"
        ));
        assert!(matches!(
            &items[1],
            Item::ToolCall {
                stream_id: Some(id),
                result: Some(content),
                state: ToolState::Done { ok: false, .. },
                ..
            } if id == "call-b" && content == "boom"
        ));
    }

    #[test]
    fn clicking_transcript_hit_toggles_its_card() {
        let mut model = snapshot_test_model();
        model
            .transcript_hits
            .borrow_mut()
            .push(TranscriptHitRegion {
                event_id: SemanticId::transcript(5),
                subtarget: HitSubtarget::ThinkingHeader,
                left: 0,
                right: 3,
                top: 5,
                bottom: 5,
            });
        model.mouse(MouseEvent {
            kind: MouseEventKind::Up(crossterm::event::MouseButton::Left),
            column: 12,
            row: 7,
            modifiers: KeyModifiers::NONE,
        });
        assert!(model.expanded_events.is_empty());
        model.mouse(MouseEvent {
            kind: MouseEventKind::Up(crossterm::event::MouseButton::Left),
            column: 1,
            row: 5,
            modifiers: KeyModifiers::NONE,
        });
        assert!(
            model
                .expanded_events
                .contains(&(model.focused_id.clone(), SemanticId::transcript(5)))
        );
    }

    #[test]
    fn thinking_block_becomes_inactive_after_text() {
        let mut items = Vec::new();
        fold_event(&mut items, &AgentEvent::Thinking("plan".into()));
        fold_event(&mut items, &AgentEvent::Text("answer".into()));
        match &items[0] {
            Item::Thinking { text } => assert_eq!(text, "plan"),
            other => panic!("expected thinking, got {other:?}"),
        }
    }

    #[test]
    fn thinking_after_tool_work_is_a_distinct_block() {
        let mut items = Vec::new();
        fold_event(&mut items, &AgentEvent::Thinking("one".into()));
        fold_event(
            &mut items,
            &AgentEvent::ToolCallDelta {
                index: 0,
                id: "t1".into(),
                name_delta: "read".into(),
                args_delta: "{}".into(),
            },
        );
        // The tool intervened, so new reasoning is a separate block rather
        // than appending to the pre-tool block.
        fold_event(&mut items, &AgentEvent::Thinking(" two".into()));
        assert_eq!(items.len(), 3);
        match &items[0] {
            Item::Thinking { text } => {
                assert_eq!(text, "one");
            }
            other => panic!("expected thinking, got {other:?}"),
        }
        match &items[2] {
            Item::Thinking { text } => {
                assert_eq!(text, " two");
            }
            other => panic!("expected thinking, got {other:?}"),
        }
    }

    #[test]
    fn idle_submit_records_turn_intent_that_snapshot_reconciler_respects() {
        let agent_id = "agent-1";
        let mut model = snapshot_test_model();
        model.focused_id = agent_id.into();
        // The user submitted while the client believed the agent idle. Until
        // TurnAccepted arrives, a racing snapshot must not flip the session
        // back to idle (the second-prompt regression).
        model.local_turn_intent.insert(agent_id.into());
        model.busy = false;
        model.replace_remote_snapshot(firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 0,
            primary_agent_id: agent_id.into(),
            agents: vec![firmius_protocol::AgentSnapshot {
                record: snapshot_agent_record(agent_id, Vec::new()),
                usage: Default::default(),
                total_usage: Default::default(),
                busy: false,
                processes: Vec::new(),
                todo: None,
            }],
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns: Default::default(),
            active_delegates: 0,
            live_events: Vec::new(),
        });
        assert!(model.busy, "pending submission keeps the session busy");
    }

    #[test]
    fn settled_turn_clears_the_local_submission_intent() {
        let agent_id = "agent-1";
        let mut model = snapshot_test_model();
        model.focused_id = agent_id.into();
        model.local_turn_intent.insert(agent_id.into());
        model.busy = true;
        model.update(AppEvent::RemoteTurnDone {
            agent_id: agent_id.into(),
            turn_id: uuid::Uuid::new_v4(),
            result: Ok(()),
        });
        assert!(
            !model.local_turn_intent.contains(agent_id),
            "terminal event retires the submission intent"
        );
    }

    #[test]
    fn two_identical_submissions_stay_two_messages() {
        // Deliberately identical text is two submissions; identity comes
        // from the submission boundary, never text equality.
        let mut items = Vec::new();
        fold_event(&mut items, &AgentEvent::UserMessage("do it".into()));
        fold_event(&mut items, &AgentEvent::UserMessage("do it".into()));
        assert_eq!(items.len(), 2);
    }

    #[test]
    fn failed_remote_submit_removes_the_echoed_row() {
        let agent_id = "agent-1";
        let mut model = snapshot_test_model();
        model.focused_id = agent_id.into();
        model
            .transcripts
            .entry(agent_id.into())
            .or_default()
            .push(Item::User("hello".into()));
        model.local_turn_intent.insert(agent_id.into());
        model.draft_before_remote_submit = Some("hello".into());
        // Mirror the event loop's failure path: remove the phantom row and
        // restore the draft so the user can retry without retyping.
        if let Some(items) = model.transcripts.get_mut(agent_id)
            && items
                .last()
                .is_some_and(|item| matches!(item, Item::User(_)))
        {
            items.pop();
        }
        model.local_turn_intent.remove(agent_id);
        if let Some(draft) = model.draft_before_remote_submit.take() {
            model.composer.replace_text(&draft);
        }
        assert!(model.transcripts[agent_id].is_empty());
        assert!(!model.local_turn_intent.contains(agent_id));
        assert_eq!(model.composer.text(&model.pastes), "hello");
    }

    fn snapshot_test_model() -> Model {
        let (settings_path, settings) = temp_settings("snapshot-reconcile");
        let model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "text-only".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings,
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        model
    }

    fn snapshot_agent_record(
        id: &str,
        history: firmius_core::Context,
    ) -> firmius_core::AgentRecord {
        firmius_core::AgentRecord {
            id: id.into(),
            provider_id: "test-provider".into(),
            model: "text-only".into(),
            effort: None,
            system_prompt: None,
            persona: None,
            temperature: None,
            max_tokens: None,
            workdir: std::env::temp_dir(),
            label: None,
            metadata: Default::default(),
            history,
            mailbox: Vec::new(),
            active_goal_id: None,
            todo: Default::default(),
            compaction: None,
        }
    }

    fn todo_projection(
        agent_id: &str,
        revision: u64,
        title: &str,
    ) -> firmius_protocol::TodoProjectionDto {
        firmius_protocol::TodoProjectionDto {
            version: firmius_protocol::TODO_DTO_VERSION,
            agent_id: agent_id.into(),
            revision,
            pending: 1,
            in_progress: 0,
            blocked: 0,
            completed: 0,
            items: vec![firmius_protocol::TodoItemDto {
                id: format!("{agent_id}-item"),
                title: title.into(),
                status: firmius_protocol::TodoStatusDto::Pending,
                evidence_count: 0,
                evidence_required: false,
                waiting_reason: None,
            }],
            completion: firmius_protocol::TodoCompletionDto::Waiting {
                unfinished: 1,
                blocked: 0,
                evidence_deficits: 0,
            },
        }
    }

    fn snapshot_agent(
        id: &str,
        todo: Option<firmius_protocol::TodoProjectionDto>,
    ) -> firmius_protocol::AgentSnapshot {
        firmius_protocol::AgentSnapshot {
            record: snapshot_agent_record(id, Vec::new()),
            usage: Default::default(),
            total_usage: Default::default(),
            busy: false,
            processes: Vec::new(),
            todo,
        }
    }

    fn todo_snapshot(revision: u64, title: &str) -> firmius_protocol::SessionSnapshot {
        firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 1,
            primary_agent_id: "parent".into(),
            agents: vec![
                snapshot_agent("parent", None),
                snapshot_agent("child", Some(todo_projection("child", revision, title))),
            ],
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns: Default::default(),
            active_delegates: 1,
            live_events: Vec::new(),
        }
    }

    #[test]
    fn todo_rail_for_resolves_a_nonfocused_remote_agent() {
        let mut model = snapshot_test_model();
        model.replace_remote_snapshot(todo_snapshot(4, "child-only item"));
        model.focused_id = "parent".into();

        let rail = model.todo_rail_for("child");
        assert_eq!(rail.revision, 4);
        assert_eq!(rail.rows()[0].title, "child-only item");
    }

    #[test]
    fn stale_todo_events_do_not_regress_a_child_projection() {
        let mut model = snapshot_test_model();
        model.replace_remote_snapshot(todo_snapshot(5, "new"));
        model.update(AppEvent::Todo(firmius_protocol::TodoEvent {
            version: firmius_protocol::TODO_DTO_VERSION,
            session_id: "session-1".into(),
            agent_id: "child".into(),
            projection: todo_projection("child", 4, "stale"),
        }));
        assert_eq!(model.todo_rail_for("child").rows()[0].title, "new");

        model.update(AppEvent::Todo(firmius_protocol::TodoEvent {
            version: firmius_protocol::TODO_DTO_VERSION,
            session_id: "session-1".into(),
            agent_id: "child".into(),
            projection: todo_projection("child", 6, "newest"),
        }));
        assert_eq!(model.todo_rail_for("child").rows()[0].title, "newest");
    }

    fn delegate_history() -> firmius_core::Context {
        vec![
            Message::text(MessageRole::User, "investigate the failure"),
            Message::with_parts(
                MessageRole::Assistant,
                [
                    MessagePart::ToolCall {
                        id: "call-delegate".into(),
                        name: "delegate".into(),
                        args: r#"{"prompt":"look into it"}"#.into(),
                    },
                    MessagePart::ToolCall {
                        id: "call-lint".into(),
                        name: "bash".into(),
                        args: "{}".into(),
                    },
                ],
            ),
            // A sibling result landed, but the delegate call has no result
            // yet: exactly what a snapshot taken mid-tool-execution shows.
            Message::tool_results([MessagePart::ToolResult {
                id: "call-lint".into(),
                content: "ok".into(),
                ok: true,
            }]),
        ]
    }

    #[test]
    fn interrupted_delegate_call_with_result_never_arrived_stays_running_while_child_works() {
        let parent_id = "agent-parent";
        let child_id = "agent-child";
        let mut active_turns = std::collections::HashMap::new();
        active_turns.insert(child_id.to_string(), uuid::Uuid::nil());
        let mut hierarchy = std::collections::HashMap::new();
        hierarchy.insert(
            child_id.to_string(),
            firmius_protocol::HierarchySnapshot {
                parent_id: Some(parent_id.into()),
                spawned_via_tool_call_id: Some("call-delegate".into()),
                label: None,
            },
        );
        let snapshot = firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 10,
            primary_agent_id: parent_id.into(),
            agents: vec![
                firmius_protocol::AgentSnapshot {
                    record: snapshot_agent_record(parent_id, delegate_history()),
                    usage: firmius_core::Usage::default(),
                    total_usage: firmius_core::Usage::default(),
                    busy: true,
                    processes: Vec::new(),
                    todo: None,
                },
                firmius_protocol::AgentSnapshot {
                    record: snapshot_agent_record(
                        child_id,
                        vec![Message::text(MessageRole::User, "child work")],
                    ),
                    usage: firmius_core::Usage::default(),
                    total_usage: firmius_core::Usage::default(),
                    busy: true,
                    processes: Vec::new(),
                    todo: None,
                },
            ],
            hierarchy,
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns,
            active_delegates: 1,
            live_events: Vec::new(),
        };

        let mut model = snapshot_test_model();
        model.replace_remote_snapshot(snapshot);

        let transcript = &model.transcripts[parent_id];
        let delegate = transcript
            .iter()
            .find_map(|item| match item {
                Item::ToolCall {
                    stream_id: Some(id),
                    state,
                    ..
                } if id == "call-delegate" => Some(state),
                _ => None,
            })
            .expect("delegate call item");
        // The child presenter is still live, so the parent's delegate
        // presenter must not reconcile to an errored/interrupted state.
        assert!(
            matches!(delegate, ToolState::Running(_)),
            "delegate presenter drifted to {delegate:?}"
        );
        // The sibling call still pairs its result by id.
        assert!(matches!(
            transcript.iter().find_map(|item| match item {
                Item::ToolCall {
                    stream_id: Some(id),
                    state,
                    ..
                } if id == "call-lint" => Some(state),
                _ => None,
            }),
            Some(ToolState::Done { ok: true, .. })
        ));
    }

    #[test]
    fn snapshot_rebuild_finalizes_pending_calls_when_no_turn_is_active() {
        let parent_id = "agent-parent";
        let child_id = "agent-child";
        let mut hierarchy = std::collections::HashMap::new();
        hierarchy.insert(
            child_id.to_string(),
            firmius_protocol::HierarchySnapshot {
                parent_id: Some(parent_id.into()),
                spawned_via_tool_call_id: Some("call-delegate".into()),
                label: None,
            },
        );
        let snapshot = firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 10,
            primary_agent_id: parent_id.into(),
            agents: vec![firmius_protocol::AgentSnapshot {
                record: snapshot_agent_record(parent_id, delegate_history()),
                usage: firmius_core::Usage::default(),
                total_usage: firmius_core::Usage::default(),
                busy: false,
                processes: Vec::new(),
                todo: None,
            }],
            hierarchy,
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns: std::collections::HashMap::new(),
            active_delegates: 0,
            live_events: Vec::new(),
        };

        let mut model = snapshot_test_model();
        model.replace_remote_snapshot(snapshot);

        let transcript = &model.transcripts[parent_id];
        // With no active turn and no live child, the unmatched call was cut.
        assert!(matches!(
            transcript.iter().find_map(|item| match item {
                Item::ToolCall {
                    stream_id: Some(id),
                    state,
                    ..
                } if id == "call-delegate" => Some(state),
                _ => None,
            }),
            Some(ToolState::Interrupted)
        ));
    }

    #[test]
    fn running_snapshot_call_without_result_finalizes_when_no_turn_is_active() {
        let agent_id = "agent-running".to_string();
        let call_id = "call-running".to_string();
        let mut transcripts = std::collections::HashMap::from([(
            agent_id.clone(),
            vec![Item::ToolCall {
                stream_id: Some(call_id.clone()),
                stream_index: 0,
                name: "bash".into(),
                args: "{}".into(),
                result: None,
                state: ToolState::Running(Instant::now()),
            }],
        )]);

        reconcile_snapshot_transcripts(
            &mut transcripts,
            &std::collections::HashMap::new(),
            &std::collections::HashMap::new(),
        );

        assert!(matches!(
            transcripts[&agent_id].as_slice(),
            [Item::ToolCall {
                stream_id: Some(id),
                result: None,
                state: ToolState::Interrupted,
                ..
            }] if id == &call_id
        ));
    }

    #[test]
    fn retry_events_are_folded_into_transcript_notes() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::RetryScheduled {
                account_id: "anthropic-user-2".into(),
                attempt: 1,
                delay_ms: 1500,
                switched: true,
                class: firmius_core::FailureClass::RateLimited,
            },
        );

        assert!(matches!(
            items.as_slice(),
            [Item::Note(note)]
                if note.contains("retry:")
                    && note.contains("switching to anthropic-user-2")
                    && note.contains("rate limited")
                    && note.contains("1.50s")
        ));
    }

    #[test]
    fn daemon_initiated_stream_marks_busy_so_live_phrase_is_not_idle() {
        let mut model = snapshot_test_model();
        model.focused_id = "agent-1".into();
        model.primary_id = "agent-1".into();
        model.session_event_sequence = 0;
        let action = model.update(AppEvent::Bus(SessionEvent {
            session_id: "session-1".into(),
            sequence: 1,
            at: chrono::Utc::now(),
            payload: firmius_core::SessionEventPayload::Agent {
                agent_id: "agent-1".into(),
                event: AgentEvent::Text("working".into()),
            },
        }));
        assert!(matches!(action, Action::Continue));
        assert!(model.busy, "streaming daemon turn left the model idle");
        assert_ne!(
            model.desired_activity_phrase(),
            "idle",
            "live phrase stayed idle during a streaming turn"
        );
    }

    #[test]
    fn snapshot_agent_busy_marks_session_busy_without_tracked_turn() {
        let agent_id = "agent-1";
        let mut model = snapshot_test_model();
        model.focused_id = agent_id.into();
        model.replace_remote_snapshot(firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 0,
            primary_agent_id: agent_id.into(),
            agents: vec![firmius_protocol::AgentSnapshot {
                record: snapshot_agent_record(agent_id, Vec::new()),
                usage: Default::default(),
                total_usage: Default::default(),
                busy: true,
                processes: Vec::new(),
                todo: None,
            }],
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns: Default::default(),
            active_delegates: 0,
            live_events: Vec::new(),
        });

        assert!(model.busy);
        assert_eq!(model.active_agent_id.as_deref(), Some(agent_id));
    }

    #[test]
    fn snapshot_gap_recovery_replays_deltas_before_local_watermark() {
        let agent_id = "agent-1";
        let mut model = snapshot_test_model();
        model.session_event_sequence = 12;
        model.remote_snapshot = Some(firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 10,
            primary_agent_id: agent_id.into(),
            agents: Vec::new(),
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns: Default::default(),
            active_delegates: 0,
            live_events: Vec::new(),
        });
        let live = |sequence, event| SessionEvent {
            session_id: "session-1".into(),
            sequence,
            at: chrono::Utc::now(),
            payload: firmius_core::SessionEventPayload::Agent {
                agent_id: agent_id.into(),
                event,
            },
        };
        let snapshot = firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 13,
            primary_agent_id: agent_id.into(),
            agents: vec![firmius_protocol::AgentSnapshot {
                record: snapshot_agent_record(
                    agent_id,
                    vec![Message::text(MessageRole::User, "question")],
                ),
                usage: Default::default(),
                total_usage: Default::default(),
                busy: true,
                processes: Vec::new(),
                todo: None,
            }],
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns: std::collections::HashMap::from([(agent_id.into(), uuid::Uuid::nil())]),
            active_delegates: 0,
            live_events: vec![
                live(11, AgentEvent::Thinking("reason".into())),
                live(12, AgentEvent::Text("hello ".into())),
                live(13, AgentEvent::Text("world".into())),
            ],
        };

        model.replace_remote_snapshot(snapshot);

        assert!(matches!(
            &model.transcripts[agent_id][1],
            Item::Thinking { text, .. } if text == "reason"
        ));
        assert!(
            matches!(&model.transcripts[agent_id][2], Item::Text(text) if text == "hello world")
        );
    }

    #[test]
    fn queued_remote_user_message_is_visible_immediately_and_not_duplicated() {
        let agent_id = "agent-1";
        let mut model = snapshot_test_model();
        model.focused_id = agent_id.into();
        model.busy = true;
        // submit() echoes queued input immediately, then the later
        // UserMessage event must not duplicate it.
        model
            .pending_remote_user_echoes
            .entry(agent_id.into())
            .or_default()
            .push_back("queued follow-up".into());
        let action = model.update(AppEvent::Bus(SessionEvent {
            session_id: "session-1".into(),
            sequence: 1,
            at: chrono::Utc::now(),
            payload: firmius_core::SessionEventPayload::Agent {
                agent_id: agent_id.into(),
                event: AgentEvent::UserMessage("queued follow-up".into()),
            },
        }));
        assert!(matches!(action, Action::Continue));
        let items = &model.transcripts[agent_id];
        assert_eq!(items.len(), 1);
        assert!(matches!(&items[0], Item::User(text) if text == "queued follow-up"));
    }

    #[test]
    fn bus_lag_requests_a_non_preserving_transcript_rebuild() {
        let mut model = snapshot_test_model();
        let action = model.update(AppEvent::BusLagged(3));
        assert!(matches!(action, Action::RebuildTranscripts));
    }

    #[test]
    fn first_submit_snapshot_keeps_local_user_echo_and_busy() {
        let agent_id = "agent-1";
        let mut model = snapshot_test_model();
        model.focused_id = agent_id.into();
        model.busy = true;
        model.turn_started = Some(Instant::now());
        model
            .transcripts
            .entry(agent_id.into())
            .or_default()
            .push(Item::User("hello".into()));
        model
            .pending_remote_user_echoes
            .entry(agent_id.into())
            .or_default()
            .push_back("hello".into());
        model.replace_remote_snapshot(firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 0,
            primary_agent_id: agent_id.into(),
            agents: vec![firmius_protocol::AgentSnapshot {
                record: snapshot_agent_record(agent_id, Vec::new()),
                usage: Default::default(),
                total_usage: Default::default(),
                busy: false,
                processes: Vec::new(),
                todo: None,
            }],
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns: Default::default(),
            active_delegates: 0,
            live_events: Vec::new(),
        });
        assert!(model.busy);
        assert!(matches!(&model.transcripts[agent_id][0], Item::User(text) if text == "hello"));
    }

    #[test]
    fn later_live_delta_is_not_dropped_as_stale_after_unjournaled_snapshot() {
        let mut model = snapshot_test_model();
        model.focused_id = "agent-1".into();
        model.session_event_sequence = 10;
        model.work_snapshot = Some(firmius_core::WorkSnapshot::new(
            "session-1",
            0,
            firmius_core::WorkState::default(),
        ));
        let action = model.update(AppEvent::Bus(SessionEvent {
            session_id: "session-1".into(),
            sequence: 11,
            at: chrono::Utc::now(),
            payload: firmius_core::SessionEventPayload::Agent {
                agent_id: "agent-1".into(),
                event: AgentEvent::Text("hello".into()),
            },
        }));
        assert!(matches!(action, Action::Continue));
        assert!(matches!(&model.transcripts["agent-1"][0], Item::Text(text) if text == "hello"));
    }

    #[test]
    fn sibling_agent_busy_does_not_mark_focused_composer_busy() {
        let mut model = snapshot_test_model();
        model.focused_id = "parent".into();
        model.replace_remote_snapshot(firmius_protocol::SessionSnapshot {
            session_id: "session-1".into(),
            title: None,
            sequence: 0,
            primary_agent_id: "parent".into(),
            agents: vec![
                firmius_protocol::AgentSnapshot {
                    record: snapshot_agent_record("parent", Vec::new()),
                    usage: Default::default(),
                    total_usage: Default::default(),
                    busy: false,
                    processes: Vec::new(),
                    todo: None,
                },
                firmius_protocol::AgentSnapshot {
                    record: snapshot_agent_record("child", Vec::new()),
                    usage: Default::default(),
                    total_usage: Default::default(),
                    busy: true,
                    processes: Vec::new(),
                    todo: None,
                },
            ],
            hierarchy: Default::default(),
            work: firmius_core::WorkSnapshot::new(
                "session-1",
                0,
                firmius_core::WorkState::default(),
            ),
            active_turns: std::collections::HashMap::from([("child".into(), uuid::Uuid::nil())]),
            active_delegates: 1,
            live_events: Vec::new(),
        });
        assert!(!model.busy);
        assert_eq!(model.pending_user_messages(), Vec::<String>::new());
    }

    #[test]
    fn fold_event_does_not_append_thinking_or_text_onto_a_mismatched_item() {
        let mut items = Vec::new();
        fold_event(&mut items, &AgentEvent::Thinking("plan".into()));
        fold_event(&mut items, &AgentEvent::Text("answer".into()));
        fold_event(&mut items, &AgentEvent::Thinking(" more".into()));
        fold_event(&mut items, &AgentEvent::Text(" more".into()));
        assert!(matches!(
            items.as_slice(),
            [Item::Thinking { text: thinking, .. }, Item::Text(text), Item::Thinking { text: later, .. }, Item::Text(more)]
                if thinking == "plan"
                    && text == "answer"
                    && later == " more"
                    && more == " more"
        ));
    }

    #[test]
    fn fold_event_web_search_started_is_a_search_item_not_a_tool_call() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::WebSearchStarted { id: "ws-1".into() },
        );
        assert!(matches!(
            items.as_slice(),
            [Item::WebSearch {
                id,
                state: SearchState::Preparing(_),
                ..
            }] if id == "ws-1"
        ));
        assert!(
            !items
                .iter()
                .any(|item| matches!(item, Item::ToolCall { .. }))
        );
    }

    #[test]
    fn fold_event_web_search_finished_merges_and_keeps_query_as_subject() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::WebSearchStarted { id: "ws-1".into() },
        );
        fold_event(
            &mut items,
            &AgentEvent::WebSearchFinished {
                id: "ws-1".into(),
                action: WebSearchAction::Search {
                    query: Some("rust async".into()),
                    queries: None,
                },
            },
        );
        assert_eq!(items.len(), 1);
        assert!(matches!(
            &items[0],
            Item::WebSearch {
                id,
                action: WebSearchAction::Search { query: Some(q), .. },
                state: SearchState::Done,
            } if id == "ws-1" && q == "rust async"
        ));
        assert!(
            !items
                .iter()
                .any(|item| matches!(item, Item::ToolCall { .. }))
        );
    }

    #[test]
    fn fold_event_web_search_finished_without_started_still_renders() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::WebSearchFinished {
                id: "ws-2".into(),
                action: WebSearchAction::OpenPage {
                    url: Some("https://example.com".into()),
                },
            },
        );
        assert!(matches!(
            &items[0],
            Item::WebSearch {
                id,
                action: WebSearchAction::OpenPage { url: Some(url) },
                state: SearchState::Done,
            } if id == "ws-2" && url == "https://example.com"
        ));
    }

    #[test]
    fn tool_call_start_reconciles_streaming_placeholder_without_duplicate() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::ToolCallDelta {
                index: 0,
                id: "call-1".into(),
                name_delta: "read".into(),
                args_delta: r#"{"path":"src/lib.rs"}"#.into(),
            },
        );
        fold_event(
            &mut items,
            &AgentEvent::ToolCallStarted {
                index: 0,
                id: "call-1".into(),
                name: "read".into(),
                args: r#"{"path":"src/lib.rs"}"#.into(),
            },
        );

        assert_eq!(items.len(), 1);
        assert!(matches!(items[0], Item::ToolCall { .. }));
    }

    #[test]
    fn tool_call_start_reconciles_when_final_id_or_args_are_normalized() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::ToolCallDelta {
                index: 0,
                id: "call-1".into(),
                name_delta: "read".into(),
                args_delta: r#"{"path":"Cargo.toml"}"#.into(),
            },
        );
        fold_event(
            &mut items,
            &AgentEvent::ToolCallStarted {
                index: 0,
                id: String::new(),
                name: "read".into(),
                args: r#"{"path":"Cargo.toml","start_line":1}"#.into(),
            },
        );

        assert_eq!(items.len(), 1);
        assert!(matches!(
            &items[0],
            Item::ToolCall {
                args,
                state: ToolState::Running(_),
                ..
            } if args.contains("start_line")
        ));
    }

    #[test]
    fn parallel_tool_deltas_correlate_by_id_not_index() {
        let mut items = Vec::new();
        // Same generation index for every chunk, distinct stable ids: the
        // exact failure shape from OpenAI-compatible endpoints that omit
        // `index` on intermediate tool-call chunks.
        for (id, name, args) in [
            ("call-a", "bash", r#"{"command":"pwd && find"}"#),
            ("call-b", "mcp__ast-grep__ast_grep_version", "{}"),
            (
                "call-c",
                "mcp__ast-grep__ast_grep_scan",
                r#"{"rule":"id: s"}"#,
            ),
        ] {
            fold_event(
                &mut items,
                &AgentEvent::ToolCallDelta {
                    index: 0,
                    id: id.into(),
                    name_delta: name.into(),
                    args_delta: args.into(),
                },
            );
        }

        assert_eq!(items.len(), 3);
        assert!(items.iter().any(|item| matches!(
            item,
            Item::ToolCall { name, args, .. }
                if name == "bash" && args == r#"{"command":"pwd && find"}"#
        )));
        assert!(items.iter().any(|item| matches!(
            item,
            Item::ToolCall { name, args, .. }
                if name == "mcp__ast-grep__ast_grep_version" && args == "{}"
        )));
        assert!(items.iter().any(|item| matches!(
            item,
            Item::ToolCall { name, args, .. }
                if name == "mcp__ast-grep__ast_grep_scan" && args == r#"{"rule":"id: s"}"#
        )));
    }

    #[test]
    fn parallel_start_events_reconcile_by_name_when_ids_are_absent() {
        // Responses-style backends (grok/codex) stream every delta at index
        // 0; the agent then finalizes with positional indexes 0,1,2... If the
        // backend also omits the id on the finalized event, the old code
        // compared `stream_index == index` across those two spaces, failed on
        // the second call, and left a ◌ placeholder orphaned.
        let mut items = Vec::new();
        for (id, name, args) in [
            ("call-a", "bash", r#"{"command":"ls"}"#),
            ("call-b", "read", r#"{"path":"src/lib.rs"}"#),
        ] {
            fold_event(
                &mut items,
                &AgentEvent::ToolCallDelta {
                    index: 0,
                    id: id.into(),
                    name_delta: name.into(),
                    args_delta: args.into(),
                },
            );
        }
        // Finalized events omit the id and use positional indexes.
        for (index, name) in ["bash", "read"].into_iter().enumerate() {
            fold_event(
                &mut items,
                &AgentEvent::ToolCallStarted {
                    index: index as u32,
                    id: String::new(),
                    name: name.into(),
                    args: "{}".into(),
                },
            );
        }

        assert_eq!(items.len(), 2);
        assert!(items.iter().all(|item| matches!(
            item,
            Item::ToolCall {
                state: ToolState::Running(_),
                ..
            }
        )));
    }

    #[test]
    fn result_event_reconciles_orphaned_preparing_placeholder_by_name() {
        let mut items = Vec::new();
        fold_event(
            &mut items,
            &AgentEvent::ToolCallDelta {
                index: 0,
                id: "call-1".into(),
                name_delta: "bash".into(),
                args_delta: r#"{"command":"ls"}"#.into(),
            },
        );
        // Result arrives with a positional index and no id; the name is the
        // only link back to the ◌ placeholder.
        fold_event(
            &mut items,
            &AgentEvent::ToolResult {
                index: 1,
                id: String::new(),
                name: "bash".into(),
                ok: true,
                content: "proc_id=1\n".into(),
            },
        );

        assert_eq!(items.len(), 1);
        assert!(matches!(
            &items[0],
            Item::ToolCall {
                result: Some(content),
                state: ToolState::Done { ok: true, .. },
                ..
            } if content == "proc_id=1\n"
        ));
    }

    #[test]
    fn viewport_scrolls_up_and_back_down_from_follow_position() {
        let mut viewport = Viewport {
            offset: 0,
            max_offset: std::cell::Cell::new(6),
            follow: true,
            anchor_top: std::cell::Cell::new(None),
            anchor_key: std::cell::RefCell::new(None),
        };
        viewport.scroll(-6);
        assert_eq!(viewport.offset, 6);
        assert!(!viewport.follow);
        viewport.scroll(3);
        assert_eq!(viewport.offset, 3);
        assert!(!viewport.follow);
        viewport.scroll(3);
        assert_eq!(viewport.offset, 0);
        assert!(viewport.follow);
        viewport.scroll(3);
        assert_eq!(viewport.offset, 0);
        assert!(viewport.follow);
    }

    #[test]
    fn viewport_clamps_wheel_bursts_and_reverses_immediately() {
        let mut viewport = Viewport {
            offset: 0,
            max_offset: std::cell::Cell::new(4),
            follow: true,
            anchor_top: std::cell::Cell::new(None),
            anchor_key: std::cell::RefCell::new(None),
        };
        viewport.scroll(-99);
        assert_eq!(viewport.offset, 4);
        viewport.scroll(1);
        assert_eq!(
            viewport.offset, 3,
            "reverse movement must not pay hidden debt"
        );
    }

    #[test]
    fn viewport_clamps_stale_offset_when_content_contracts() {
        let mut viewport = Viewport {
            offset: 8,
            max_offset: std::cell::Cell::new(8),
            follow: false,
            anchor_top: std::cell::Cell::new(None),
            anchor_key: std::cell::RefCell::new(None),
        };
        viewport.set_max_offset(2);
        assert_eq!(viewport.offset, 2);
        viewport.scroll(1);
        assert_eq!(viewport.offset, 1);
    }

    #[test]
    fn viewport_contraction_invalidates_cached_absolute_anchor() {
        let mut viewport = Viewport {
            offset: 8,
            max_offset: std::cell::Cell::new(8),
            follow: false,
            anchor_top: std::cell::Cell::new(Some(42)),
            anchor_key: std::cell::RefCell::new(Some(("agent".into(), 80))),
        };
        viewport.set_max_offset(2);
        assert_eq!(viewport.offset, 2);
        assert_eq!(viewport.anchor_top.get(), None);
        assert!(viewport.anchor_key.borrow().is_none());
    }

    #[test]
    fn viewport_contraction_invalidates_anchor_when_relative_offset_still_fits() {
        let mut viewport = Viewport {
            offset: 1,
            max_offset: std::cell::Cell::new(10),
            follow: false,
            anchor_top: std::cell::Cell::new(Some(9)),
            anchor_key: std::cell::RefCell::new(Some(("agent".into(), 80))),
        };
        viewport.set_max_offset(1);
        assert_eq!(viewport.offset, 1);
        assert_eq!(viewport.anchor_top.get(), None);
        assert!(viewport.anchor_key.borrow().is_none());
    }

    #[test]
    fn welcome_model_accepts_theoretical_model_selection() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let (settings_path, settings) = temp_settings("welcome-default");
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );

        assert!(matches!(
            model.run_command("/model test-provider/sonnet-4"),
            Action::Continue
        ));
        assert_eq!(model.model, "sonnet-4");
        assert_eq!(model.provider_id, "test-provider");
        assert!(!model.has_agent());
        assert_eq!(model.focus_label(), "welcome");
        assert_eq!(
            settings
                .lock()
                .unwrap()
                .preferred_default_model()
                .unwrap()
                .model,
            "sonnet-4"
        );
        assert!(settings_path.exists());
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
    }

    #[test]
    fn welcome_model_selection_with_a_persona_saves_that_personas_preference() {
        let directory = std::env::temp_dir().join(format!(
            "firmius-tui-persona-model-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("lead.md"),
            "---\nname: Lead\ntool_scopes: [fs_read]\nbackground: false\n---\nLead prompt.",
        )
        .unwrap();
        let personas = Arc::new(PersonaManager::load_from(directory.clone()).unwrap());
        let (settings_path, settings) = temp_settings("welcome-persona");
        let mut model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(ProviderManager::new())),
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            personas,
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.pending_persona = Some("lead".into());

        assert!(matches!(
            model.run_command("/model test-provider/lead-model"),
            Action::Continue
        ));
        let settings = settings.lock().unwrap();
        assert!(settings.preferred_default_model().is_none());
        assert_eq!(
            settings.preferred_model("lead").unwrap().model,
            "lead-model"
        );
        drop(settings);
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn live_main_persona_model_selection_updates_its_preference() {
        let directory = std::env::temp_dir().join(format!(
            "firmius-tui-live-persona-model-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("lead.md"),
            "---\nname: Lead\ntool_scopes: [fs_read]\nbackground: false\n---\nLead prompt.",
        )
        .unwrap();
        let personas = Arc::new(PersonaManager::load_from(directory.clone()).unwrap());
        let provider_manager = test_provider_manager();
        let provider = provider_manager.build("test-provider").unwrap();
        let tools = Arc::new(ToolRegistry::default());
        let agent = Arc::new(Agent::new_with_personas(
            provider,
            tools.clone(),
            AgentConfig {
                provider_id: "test-provider".into(),
                model: "old-model".into(),
                persona: Some("lead".into()),
                ..Default::default()
            },
            "session",
            personas.clone(),
        ));
        let (settings_path, settings) = temp_settings("live-persona");
        let mut model = Model::new(
            None,
            Some(agent.clone()),
            "test-provider".into(),
            Arc::new(std::sync::Mutex::new(provider_manager)),
            "old-model".into(),
            tools,
            personas,
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );

        assert!(matches!(
            model.run_command("/model test-provider/new-model"),
            Action::Continue
        ));
        assert_eq!(agent.config().model, "new-model");
        assert_eq!(
            settings
                .lock()
                .unwrap()
                .preferred_model("lead")
                .unwrap()
                .model,
            "new-model"
        );
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn welcome_uses_the_saved_default_model_when_its_provider_is_available() {
        let (settings_path, settings) = temp_settings("restore-default");
        {
            let mut settings = settings.lock().unwrap();
            settings.set_preferred_default("test-provider", "saved-model", Some("high".into()));
            settings.save().unwrap();
        }
        let model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "fallback-model".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );

        assert_eq!(model.provider_id, "test-provider");
        assert_eq!(model.model, "saved-model");
        assert_eq!(
            model.effort.as_ref().map(|effort| effort.name.as_str()),
            Some("high")
        );
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
    }

    #[test]
    fn main_persona_without_an_override_uses_the_saved_default_model() {
        let directory = std::env::temp_dir().join(format!(
            "firmius-tui-persona-default-fallback-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("lead.md"),
            "---\nname: Lead\ntool_scopes: [fs_read]\nbackground: false\n---\nLead prompt.",
        )
        .unwrap();
        let (settings_path, settings) = temp_settings("persona-default-fallback");
        settings.lock().unwrap().set_preferred_default(
            "test-provider",
            "default-model",
            Some("medium".into()),
        );
        let mut model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "fallback-model".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::load_from(directory.clone()).unwrap()),
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );

        model.apply_persona(Some("lead".into())).unwrap();
        assert_eq!(model.provider_id, "test-provider");
        assert_eq!(model.model, "default-model");
        assert_eq!(
            model.effort.as_ref().map(|effort| effort.name.as_str()),
            Some("medium")
        );

        settings.lock().unwrap().set_preferred_model_and_effort(
            "lead",
            "test-provider",
            "persona-model",
            Some("high".into()),
        );
        model.apply_persona(Some("lead".into())).unwrap();
        assert_eq!(model.model, "persona-model");
        assert_eq!(
            model.effort.as_ref().map(|effort| effort.name.as_str()),
            Some("high")
        );
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn backtab_cycles_welcome_personas_and_excludes_delegate_only_entries() {
        let directory = std::env::temp_dir().join(format!(
            "firmius-tui-personas-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        std::fs::write(
            directory.join("general.md"),
            "---\nname: General\ntool_scopes: [fs_read]\nbackground: false\n---\nGeneral prompt.",
        )
        .unwrap();
        std::fs::write(
            directory.join("coder.md"),
            "---\nname: Coder\ntool_scopes: [fs_write]\nbackground: true\n---\nCoder prompt.",
        )
        .unwrap();
        let personas = Arc::new(PersonaManager::load_from(directory.clone()).unwrap());
        let mut model = Model::new(
            None,
            None,
            String::new(),
            Arc::new(std::sync::Mutex::new(ProviderManager::new())),
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            personas,
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );

        for expected in [Some("general"), None] {
            assert!(matches!(
                model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
                    KeyCode::BackTab,
                    KeyModifiers::SHIFT,
                )))),
                Action::Continue
            ));
            assert_eq!(model.pending_persona.as_deref(), expected);
        }

        std::fs::remove_dir_all(directory).ok();
    }

    #[test]
    fn enter_accepts_completion_without_reopening_the_menu() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.composer.insert_str("/model ");
        model.completion = Some(CompletionState {
            items: vec![CompletionItem {
                insert: "/model codex/gpt-5.6-luna".into(),
                label: "codex/gpt-5.6-luna".into(),
                detail: "model · provider codex".into(),
            }],
            selected: 0,
        });

        assert!(matches!(
            model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
                KeyCode::Enter,
                KeyModifiers::NONE,
            )))),
            Action::Continue
        ));
        assert_eq!(
            model.composer.text(&model.pastes),
            "/model codex/gpt-5.6-luna"
        );
        assert!(model.completion.is_none());
    }

    #[test]
    fn enter_accepting_command_completion_opens_empty_argument_suggestions() {
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let mut model = Model::new(
            None,
            None,
            "codex".into(),
            Arc::new(std::sync::Mutex::new(manager)),
            "gpt-5.6-luna".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.composer.insert_str("/mo");
        model.completion = Some(CompletionState {
            items: vec![CompletionItem {
                insert: "/model ".into(),
                label: "/model".into(),
                detail: "select a model".into(),
            }],
            selected: 0,
        });

        assert!(matches!(
            model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
                KeyCode::Enter,
                KeyModifiers::NONE,
            )))),
            Action::Continue
        ));
        assert_eq!(model.composer.text(&model.pastes), "/model ");
        assert!(
            model
                .completion
                .as_ref()
                .is_some_and(|completion| !completion.items.is_empty())
        );
    }

    #[test]
    fn effort_completion_uses_models_dev_values_for_the_selected_model() {
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let manager = Arc::new(std::sync::Mutex::new(manager));
        let (settings_path, settings) = temp_settings("welcome-effort-model-change");
        let mut model = Model::new(
            None,
            None,
            "codex".into(),
            manager,
            "gpt-5.6-sol".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings.clone(),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.composer.insert_str("/effort ");
        model.refresh_completion();
        let labels: Vec<_> = model
            .completion
            .as_ref()
            .expect("effort suggestions")
            .items
            .iter()
            .map(|item| item.label.as_str())
            .collect();
        assert_eq!(labels, ["low", "medium", "high", "xhigh", "max", "ultra"]);
        assert_eq!(
            model.focused_model_status(),
            ("codex".into(), "gpt-5.6-sol".into(), "default".into())
        );

        model.composer.replace_text("/effort");
        model.refresh_completion();
        assert!(model.completion.as_ref().is_some_and(|completion| {
            completion.items.iter().any(|item| item.label == "xhigh")
        }));

        assert!(matches!(
            model.run_command("/effort ultra"),
            Action::Continue
        ));
        assert_eq!(
            model
                .effort
                .as_ref()
                .and_then(|effort| effort.reasoning_effort.as_deref()),
            Some("ultra")
        );
        assert_eq!(model.focused_model_status().2, "ultra");
        assert_eq!(
            settings
                .lock()
                .unwrap()
                .preferred_default_model()
                .unwrap()
                .effort
                .as_deref(),
            Some("ultra")
        );

        assert!(matches!(
            model.run_command("/model codex/gpt-5.5"),
            Action::Continue
        ));
        assert!(model.effort.is_none());
        assert_eq!(model.focused_model_status().2, "default");
        assert!(
            settings
                .lock()
                .unwrap()
                .preferred_default_model()
                .unwrap()
                .effort
                .is_none()
        );
        model.composer.replace_text("/effort ");
        model.refresh_completion();
        assert!(model.completion.as_ref().is_some_and(|completion| {
            completion.items.iter().all(|item| item.label != "ultra")
        }));
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
    }

    #[test]
    fn welcome_effort_completion_falls_back_to_selected_model_metadata() {
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let mut model = Model::new(
            None,
            None,
            "account-alias".into(),
            Arc::new(std::sync::Mutex::new(manager)),
            "gpt-5.6-sol".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );

        model.composer.insert_str("/effort ");
        model.refresh_completion();
        let labels = model
            .completion
            .as_ref()
            .expect("effort suggestions")
            .items
            .iter()
            .map(|item| item.label.as_str())
            .collect::<Vec<_>>();
        assert_eq!(labels, ["low", "medium", "high", "xhigh", "max", "ultra"]);
    }

    #[test]
    fn accounts_completion_lists_kinds_not_account_ids() {
        let mut manager = ProviderManager::new();
        manager.register_kind(Arc::new(CodexKind));
        manager.register_account(AccountRecord {
            id: "codex-9d8sq8dh-ew7dya-s7wbw".into(),
            kind: "codex".into(),
            schema: firmius_core::kinds::codex::schema_template("codex-9d8sq8dh-ew7dya-s7wbw"),
            credentials: serde_json::json!({}),
        });
        let mut model = Model::new(
            None,
            None,
            "codex-9d8sq8dh-ew7dya-s7wbw".into(),
            Arc::new(std::sync::Mutex::new(manager)),
            "gpt-5.6-luna".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );

        model.composer.insert_str("/accounts ");
        model.refresh_completion();
        let labels: Vec<_> = model
            .completion
            .as_ref()
            .expect("account kind suggestions")
            .items
            .iter()
            .map(|item| item.label.as_str())
            .collect();

        assert!(labels.contains(&"codex"));
        assert!(!labels.contains(&"codex-9d8sq8dh-ew7dya-s7wbw"));
    }

    #[test]
    fn effort_completion_uses_the_focused_agents_modes() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "unknown".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.focused_id = "child".into();
        model.agent_efforts.insert(
            "child".into(),
            vec![EffortMode {
                name: "focused-only".into(),
                thinking_budget_tokens: None,
                reasoning_effort: Some("focused-only".into()),
            }],
        );
        model.composer.insert_str("/effort ");
        model.refresh_completion();
        assert_eq!(
            model.completion.as_ref().unwrap().items[0].label,
            "focused-only"
        );
    }

    #[test]
    fn effort_command_updates_the_focused_agent_and_status() {
        let provider_manager = test_provider_manager();
        let provider = provider_manager.build("test-provider").unwrap();
        let tools = Arc::new(ToolRegistry::default());
        let primary = Arc::new(Agent::new(
            provider.clone(),
            tools.clone(),
            AgentConfig {
                provider_id: "test-provider".into(),
                model: "primary-model".into(),
                ..Default::default()
            },
            "session",
        ));
        let child = Arc::new(Agent::new(
            provider,
            tools.clone(),
            AgentConfig {
                provider_id: "test-provider".into(),
                model: "child-model".into(),
                ..Default::default()
            },
            "session",
        ));
        let mut model = Model::new(
            None,
            Some(primary.clone()),
            "test-provider".into(),
            Arc::new(std::sync::Mutex::new(provider_manager)),
            "primary-model".into(),
            tools,
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.agents.insert(child.id.clone(), child.clone());
        model.focused_id = child.id.clone();
        model.agent_efforts.insert(
            child.id.clone(),
            vec![EffortMode {
                name: "focused-only".into(),
                thinking_budget_tokens: None,
                reasoning_effort: Some("focused-only".into()),
            }],
        );

        assert!(matches!(
            model.run_command("/effort focused-only"),
            Action::Continue
        ));
        assert!(primary.config().effort.is_none());
        assert_eq!(child.config().effort.unwrap().name, "focused-only");
        assert_eq!(
            model.focused_model_status(),
            (
                "test-provider".into(),
                "child-model".into(),
                "focused-only".into()
            )
        );
    }

    #[test]
    fn argument_completion_opens_with_an_empty_partial() {
        let mut manager = ProviderManager::new();
        manager.register_schema(firmius_core::kinds::codex::schema_template("codex"));
        let mut model = Model::new(
            None,
            None,
            "codex".into(),
            Arc::new(std::sync::Mutex::new(manager)),
            "gpt-5.6-luna".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.composer.insert_str("/model ");
        model.refresh_completion();
        assert!(
            model
                .completion
                .as_ref()
                .is_some_and(|completion| { !completion.items.is_empty() })
        );
    }

    #[test]
    fn welcome_first_message_stays_local_without_a_provider() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "gpt-4o-mini".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.composer = Composer::new();
        model.composer.insert_str("hello");

        assert!(matches!(model.submit(), Action::Continue));
        assert!(!model.has_agent());
        assert!(
            model
                .note
                .as_ref()
                .is_some_and(|(note, _)| note.contains("no provider configured"))
        );
    }

    #[test]
    fn ctrl_p_returns_to_parent_agent_when_available() {
        let manager = Arc::new(std::sync::Mutex::new(ProviderManager::new()));
        let mut model = Model::new(
            None,
            None,
            String::new(),
            manager,
            "model".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.focused_id = "child".into();
        model
            .parent_by_agent
            .insert("child".into(), "parent".into());
        model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
            KeyCode::Char('p'),
            KeyModifiers::CONTROL,
        ))));
        assert_eq!(model.focused_id, "parent");
    }

    #[test]
    fn image_submission_is_blocked_when_model_lacks_image_capability() {
        let mut model = Model::new(
            None,
            None,
            "test-provider".into(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "text-only".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.composer.insert_paste_block(1);
        model.pastes.push(StoredPaste::Image(PastedImage {
            media_type: "image/png".into(),
            data_base64: "Zm9v".into(),
            width: 2,
            height: 3,
            bytes: 16,
        }));

        assert!(matches!(model.submit(), Action::Continue));
        assert!(
            model
                .note
                .as_ref()
                .is_some_and(|(note, _)| note.contains("does not support image inputs"))
        );
        assert!(matches!(
            model.composer.submission(&model.pastes),
            Some(ComposerSubmission::Message(_))
        ));
    }

    #[test]
    fn image_submission_returns_message_action_for_image_capable_model() {
        let mut model = Model::new(
            None,
            None,
            "test-provider".into(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "vision".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            Arc::new(std::sync::Mutex::new(UserSettings::default())),
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        model.composer.insert_str("look");
        model.pastes.push(StoredPaste::Image(PastedImage {
            media_type: "image/png".into(),
            data_base64: "Zm9v".into(),
            width: 2,
            height: 3,
            bytes: 16,
        }));
        model.composer.insert_paste_block(1);

        let action = model.submit();
        let Action::Submit { message, .. } = action else {
            panic!("expected submit action")
        };
        assert_eq!(message.role, MessageRole::User);
        assert_eq!(
            message,
            Message::with_parts(
                MessageRole::User,
                [
                    MessagePart::Text("look".into()),
                    MessagePart::Image(firmius_core::ImagePart::from_base64("image/png", "Zm9v")),
                ],
            )
        );
    }

    fn welcome_model(name: &str) -> (PathBuf, Model) {
        let (path, settings) = temp_settings(name);
        let model = Model::new(
            None,
            None,
            "test-provider".into(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "text-only".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings,
            Arc::new(std::sync::Mutex::new(FirmiusConfig::default())),
            Arc::new(McpManager::default()),
        );
        (path, model)
    }

    fn press(model: &mut Model, code: KeyCode) -> Action {
        press_with_modifiers(model, code, KeyModifiers::NONE)
    }

    fn press_with_modifiers(model: &mut Model, code: KeyCode, modifiers: KeyModifiers) -> Action {
        model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
            code, modifiers,
        ))))
    }

    #[test]
    fn alt_tab_cycles_permission_modes_without_inserting_tab() {
        let mut model = snapshot_test_model();
        model.permission_policy = Some(firmius_core::PermissionPolicy::default());
        model.permission_policy.as_mut().unwrap().yolo_confirmed = true;

        assert!(matches!(
            press_with_modifiers(&mut model, KeyCode::Tab, KeyModifiers::ALT),
            Action::SetPermissionPolicy { policy, .. }
                if matches!(policy.mode, firmius_core::PermissionMode::Auto)
        ));
        model.permission_policy.as_mut().unwrap().mode = firmius_core::PermissionMode::Auto;
        assert!(matches!(
            press_with_modifiers(&mut model, KeyCode::Tab, KeyModifiers::ALT),
            Action::SetPermissionPolicy { policy, .. }
                if matches!(policy.mode, firmius_core::PermissionMode::Yolo)
        ));
        assert_eq!(model.composer.text(&model.pastes), "");
    }

    #[test]
    fn alt_b_and_f_move_by_word_without_inserting_characters() {
        let (path, mut model) = welcome_model("alt-word-navigation");
        model.composer.insert_str("one two");

        press_with_modifiers(&mut model, KeyCode::Char('b'), KeyModifiers::ALT);
        assert_eq!(model.composer.text(&model.pastes), "one two");
        assert_eq!(model.composer.cursor_pos(&model.pastes), (0, 4));

        press_with_modifiers(&mut model, KeyCode::Char('b'), KeyModifiers::ALT);
        assert_eq!(model.composer.cursor_pos(&model.pastes), (0, 0));

        press_with_modifiers(&mut model, KeyCode::Char('f'), KeyModifiers::ALT);
        assert_eq!(model.composer.cursor_pos(&model.pastes), (0, 3));
        press_with_modifiers(&mut model, KeyCode::Char('f'), KeyModifiers::ALT);
        assert_eq!(model.composer.cursor_pos(&model.pastes), (0, 4));
        press_with_modifiers(&mut model, KeyCode::Char('f'), KeyModifiers::ALT);
        assert_eq!(model.composer.cursor_pos(&model.pastes), (0, 7));
        assert_eq!(model.composer.text(&model.pastes), "one two");

        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn ctrl_a_and_e_move_to_the_current_composer_line_edges() {
        let (path, mut model) = welcome_model("ctrl-line-edges");
        model.composer.insert_str("first\nsecond");
        // Key dispatch refreshes the logical line layout before applying the
        // readline shortcut, so this also covers a key arriving before the
        // first render frame.
        press_with_modifiers(&mut model, KeyCode::Char('a'), KeyModifiers::CONTROL);
        assert_eq!(model.composer.cursor_pos(&model.pastes), (1, 0));
        press(&mut model, KeyCode::Up);
        press_with_modifiers(&mut model, KeyCode::Char('e'), KeyModifiers::CONTROL);
        assert_eq!(model.composer.cursor_pos(&model.pastes), (0, 5));
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn shifted_control_navigation_keys_are_not_inserted_into_composer() {
        let (path, mut model) = welcome_model("shifted-control-navigation");
        model.roster = vec![
            (model.focused_id.clone(), "main".into()),
            ("child".into(), "child".into()),
        ];
        press_with_modifiers(
            &mut model,
            KeyCode::Char('N'),
            KeyModifiers::CONTROL | KeyModifiers::SHIFT,
        );
        assert_eq!(model.focused_id, "child");
        assert!(model.composer.is_empty());

        press_with_modifiers(
            &mut model,
            KeyCode::Char('B'),
            KeyModifiers::CONTROL | KeyModifiers::SHIFT,
        );
        assert_eq!(model.focused_id, "welcome");
        assert!(model.composer.is_empty());
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn e_inserts_composer_text_and_enter_does_not_toggle_disclosure() {
        let (path, mut model) = welcome_model("composer-disclosure-keys");
        press(&mut model, KeyCode::Char('e'));
        assert_eq!(model.composer.text(&model.pastes), "e");
        model
            .transcript_hits
            .borrow_mut()
            .push(TranscriptHitRegion {
                event_id: SemanticId::transcript(8),
                subtarget: HitSubtarget::ThinkingHeader,
                left: 0,
                right: 3,
                top: 4,
                bottom: 4,
            });
        press(&mut model, KeyCode::Enter);
        assert!(model.expanded_events.is_empty());
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn up_arrow_recalls_prompt_history_and_down_restores_draft() {
        let (path, mut model) = welcome_model("history-recall");
        model.prompt_history = vec!["first".into(), "second".into()];
        model.composer.insert_str("draft");
        press(&mut model, KeyCode::Up);
        assert_eq!(model.composer.text(&model.pastes), "second");
        press(&mut model, KeyCode::Up);
        assert_eq!(model.composer.text(&model.pastes), "first");
        press(&mut model, KeyCode::Down);
        assert_eq!(model.composer.text(&model.pastes), "second");
        press(&mut model, KeyCode::Down);
        assert_eq!(model.composer.text(&model.pastes), "draft");
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn copy_last_emits_copy_text_action() {
        let (path, mut model) = welcome_model("copy-last");
        model.transcripts.insert(
            model.focused_id.clone(),
            vec![Item::User("hi".into()), Item::Text("hello there".into())],
        );
        let action = model.run_command("/copy");
        assert!(matches!(action, Action::CopyText(text) if text == "hello there"));
        let action = model.run_command("/copy all");
        let Action::CopyText(text) = action else {
            panic!("expected copy all");
        };
        assert!(text.contains("You: hi"));
        assert!(text.contains("hello there"));
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    #[test]
    fn new_emits_action() {
        let (path, mut model) = welcome_model("session-actions");
        assert!(matches!(model.run_command("/new"), Action::NewSession));
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }

    fn welcome_model_with_config(name: &str) -> (PathBuf, PathBuf, Model) {
        let (settings_path, settings) = temp_settings(name);
        let config_dir = std::env::temp_dir().join(format!(
            "firmius-tui-config-{name}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&config_dir).unwrap();
        let config_path = config_dir.join("config.json");
        let config = FirmiusConfig::load_from_path(&config_path).unwrap();
        let model = Model::new(
            None,
            None,
            "test-provider".into(),
            Arc::new(std::sync::Mutex::new(test_provider_manager())),
            "text-only".into(),
            Arc::new(ToolRegistry::default()),
            Arc::new(PersonaManager::default()),
            settings,
            Arc::new(std::sync::Mutex::new(config)),
            Arc::new(McpManager::default()),
        );
        (settings_path, config_path, model)
    }

    #[test]
    fn search_command_lists_and_persists_off_by_default() {
        let (settings_path, config_path, mut model) = welcome_model_with_config("search-off");
        assert!(model.config.lock().unwrap().general.web_search.is_none());
        assert!(matches!(model.run_command("/search"), Action::Continue));
        assert!(
            model
                .note
                .as_ref()
                .is_some_and(|(note, _)| note.contains("off"))
        );
        assert!(matches!(model.run_command("/search off"), Action::Continue));
        assert!(model.config.lock().unwrap().general.web_search.is_none());
        let reloaded = FirmiusConfig::load_from_path(&config_path).unwrap();
        assert_eq!(reloaded.general.web_search, None);
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(config_path.parent().unwrap()).ok();
    }

    #[test]
    fn search_live_is_rejected_when_provider_has_no_capability() {
        let (settings_path, config_path, mut model) = welcome_model_with_config("search-live");
        assert!(matches!(
            model.run_command("/search live"),
            Action::Continue
        ));
        assert!(
            model.note.as_ref().is_some_and(|(note, _)| note
                .contains("this provider does not support web search"))
        );
        assert!(model.config.lock().unwrap().general.web_search.is_none());
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(config_path.parent().unwrap()).ok();
    }

    struct SearchCapableProvider;

    #[async_trait::async_trait]
    impl firmius_core::Provider for SearchCapableProvider {
        fn id(&self) -> &str {
            "search-capable"
        }
        fn capabilities(&self) -> firmius_core::ProviderCapabilities {
            firmius_core::ProviderCapabilities {
                web_search: Some(firmius_core::LLMWebSearch {
                    modes: &[
                        firmius_core::WebSearchMode::Cached,
                        firmius_core::WebSearchMode::Live,
                    ],
                    default_mode: firmius_core::WebSearchMode::Cached,
                    content: firmius_core::WebSearchContent::Text,
                    supports_filters: false,
                    supports_location: false,
                }),
            }
        }
        async fn stream(
            &self,
            _request: firmius_core::ProviderRequest,
        ) -> Result<
            futures::stream::BoxStream<
                'static,
                Result<firmius_core::ProviderEvent, firmius_core::ProviderError>,
            >,
            firmius_core::ProviderError,
        > {
            Ok(futures::stream::empty().boxed())
        }
    }

    #[test]
    fn search_live_persists_when_provider_advertises_the_mode() {
        let (settings_path, config_path, mut model) = welcome_model_with_config("search-persist");
        let tools = Arc::new(ToolRegistry::default());
        let agent = Arc::new(Agent::new(
            std::sync::Arc::new(SearchCapableProvider),
            tools.clone(),
            AgentConfig {
                provider_id: "search-capable".into(),
                model: "m".into(),
                ..Default::default()
            },
            "session",
        ));
        model.primary = Some(agent.clone());
        model.primary_id = agent.id.clone();
        model.focused_id = agent.id.clone();
        model.agents.insert(agent.id.clone(), agent);

        assert!(matches!(
            model.run_command("/search live"),
            Action::Continue
        ));
        assert_eq!(
            model.config.lock().unwrap().general.web_search.as_deref(),
            Some("live")
        );
        let reloaded = FirmiusConfig::load_from_path(&config_path).unwrap();
        assert_eq!(reloaded.general.web_search.as_deref(), Some("live"));
        assert!(matches!(
            model.run_command("/search indexed"),
            Action::Continue
        ));
        assert!(
            model
                .note
                .as_ref()
                .is_some_and(|(note, _)| note.contains("does not support indexed search"))
        );
        assert_eq!(
            model.config.lock().unwrap().general.web_search.as_deref(),
            Some("live")
        );
        std::fs::remove_dir_all(settings_path.parent().unwrap()).ok();
        std::fs::remove_dir_all(config_path.parent().unwrap()).ok();
    }

    #[test]
    fn ctrl_home_jumps_to_top_and_ctrl_end_follows() {
        let (path, mut model) = welcome_model("jump");
        let action = model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
            KeyCode::Home,
            KeyModifiers::CONTROL,
        ))));
        assert!(matches!(action, Action::Continue));
        assert!(!model.viewport.follow);
        let action = model.update(AppEvent::Term(TermEvent::Key(KeyEvent::new(
            KeyCode::End,
            KeyModifiers::CONTROL,
        ))));
        assert!(matches!(action, Action::Continue));
        assert!(model.viewport.follow);
        std::fs::remove_dir_all(path.parent().unwrap()).ok();
    }
}
