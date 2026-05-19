#ifndef FIRMIUS_SHARED_CREW_HPP
#define FIRMIUS_SHARED_CREW_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace firmius::shared {

// ─── Lifecycle states ───────────────────────────────────────────────────────

/// Lifecycle state of a Crew (the swarm container itself).
enum class CrewStatus : std::uint8_t {
  Forming,    ///< Just formed, no members or tasks yet.
  Active,     ///< Has members and at least one task in flight.
  Idle,       ///< All tasks finished, awaiting more or disband.
  Paused,     ///< Frozen by user; no advancement, state preserved.
  Disbanded,  ///< Coordinator called Disband; immutable after this.
};

/// Lifecycle state of a single CrewTask within a Crew.
enum class CrewTaskStatus : std::uint8_t {
  Pending,    ///< Created, not yet assigned to anyone.
  Assigned,   ///< Has an owner; awaiting first heartbeat or completion.
  InProgress, ///< Owner is actively working (heartbeats received).
  ReadyForReview, ///< Owner reported done; awaiting reviewer accept/reject.
  Accepted,   ///< Reviewer accepted the work.
  Rejected,   ///< Reviewer rejected; may be Reassigned or Salvaged.
  Salvaged,   ///< Pulled away from owner mid-flight (e.g. stale, abandoned).
  Cancelled,  ///< Task explicitly cancelled by coordinator before completion.
};

/// Role a CrewMember plays inside the Crew. Maps onto persona/purpose lanes.
enum class CrewRole : std::uint8_t {
  Coordinator,     ///< The top-level Orchestrator. Talks to user. One per crew.
  SubOrchestrator, ///< Owns a single epic. Reserved; v2 hierarchy.
  Architect,       ///< Designs the master task tree + crew-level quality gates.
  Lead,            ///< Decomposes a sub-area into Builder tasks; reviews own Builders.
  Builder,         ///< Implementer; the only role permitted to write files.
  Reviewer,        ///< Reviews ReadyForReview tasks against done_when criteria.
  Scout,           ///< Investigates, reports findings; never owns implementation.
  Unknown,         ///< Persona did not map to any of the above. Treated as Builder.
};

/// Type of CrewEvent for the per-crew event stream.
enum class CrewEventKind : std::uint8_t {
  CrewFormed,
  CrewDisbanded,
  CrewPaused,
  CrewResumed,
  MemberEnlisted,
  MemberDischarged,
  TaskCreated,
  TaskAssigned,
  TaskReassigned,
  TaskHeartbeat,
  TaskReadyForReview,
  TaskAccepted,
  TaskRejected,
  TaskSalvaged,
  TaskCancelled,
  ChannelOpened,
  ChannelClosed,
  MailSent,
  MailAcked,
  CoordinatorChanged,
  GateAdded,
  GateUpdated,
  GatePassed,
  GateFailed,
  FlagRaised,
  FlagResolved,
  NudgeSent,
};

// ─── Core data types ────────────────────────────────────────────────────────

struct CrewMember {
  std::string memberId;       ///< Stable per-crew identifier (m-XXX).
  std::string agentId;        ///< Engine agent id, if member is a live agent.
  std::string persona;        ///< Persona name (aster, forge, witness, ...).
  CrewRole role = CrewRole::Unknown;
  std::uint64_t enlistedAtMs = 0;
  std::uint64_t dischargedAtMs = 0; ///< 0 means still enlisted.
  std::string note;           ///< Optional human-readable note.

  bool operator==(const CrewMember &) const = default;
};

struct CrewTask {
  std::string taskId;         ///< Stable per-crew identifier (t-XXX).
  std::string title;          ///< Short imperative title.
  std::string brief;          ///< Longer description of what's wanted.
  std::vector<std::string> doneWhen; ///< Acceptance criteria, free-form.
  CrewTaskStatus status = CrewTaskStatus::Pending;
  std::string ownerMemberId;  ///< CrewMember.memberId currently owning, or "".
  std::string reviewerMemberId; ///< Who reviews this task. Optional.
  std::uint64_t createdAtMs = 0;
  std::uint64_t assignedAtMs = 0;
  std::uint64_t lastHeartbeatMs = 0; ///< Last heartbeat received; 0 if none.
  std::uint64_t completedAtMs = 0;
  std::uint64_t staleSinceMs = 0;    ///< Set by watchdog only.
  std::string lastVerdict;    ///< "accept" / "reject" / "" — set by reviewer.
  std::string lastSuggestion; ///< Reviewer's reject reason, or accept note.
  std::string evidenceJson;   ///< Owner's claim payload at ready-for-review.
  std::string parentTaskId;   ///< Optional ancestor task; "" if root.
  std::string parentEpicId;   ///< For builder tasks, the epic they roll up to.
  int reassignCount = 0;      ///< Number of times reassigned.
  std::vector<std::string> tags;
  std::vector<std::string> dependsOnTaskIds;     ///< Tasks that must complete first.
  std::string integrationContractJson;           ///< Architect's interface spec.
  std::vector<std::string> declaredFileScope;    ///< Paths/globs the task may touch.

  bool operator==(const CrewTask &) const = default;
};

struct CrewChannel {
  std::string channelId;      ///< Stable per-crew identifier (c-XXX).
  std::string name;           ///< Human-readable name (e.g. "main", "scouts").
  std::vector<std::string> memberIds; ///< Subscribers.
  bool open = true;           ///< Once closed, no more mail.
  std::uint64_t openedAtMs = 0;
  std::uint64_t closedAtMs = 0;

  bool operator==(const CrewChannel &) const = default;
};

struct CrewMail {
  std::string mailId;         ///< Stable per-crew identifier (mail-XXX).
  std::string channelId;      ///< Channel this mail belongs to.
  std::string fromMemberId;   ///< Sender.
  std::vector<std::string> toMemberIds; ///< Direct recipients (may be empty: broadcast).
  std::string subject;
  std::string body;
  std::string payloadJson;    ///< Optional structured payload.
  std::uint64_t sentAtMs = 0;
  std::vector<std::string> ackedByMemberIds; ///< Who has Ack'd this mail.

  bool operator==(const CrewMail &) const = default;
};

struct CrewEvent {
  std::uint64_t sequence = 0; ///< Monotonic per-crew event sequence.
  CrewEventKind kind = CrewEventKind::CrewFormed;
  std::uint64_t timestampMs = 0;
  std::string actorMemberId;  ///< Who caused this event ("" for system).
  std::string targetMemberId; ///< Subject of action (e.g. enlisted member).
  std::string targetTaskId;
  std::string targetChannelId;
  std::string targetMailId;
  std::string note;
  std::string detailJson;     ///< Free-form detail blob.

  bool operator==(const CrewEvent &) const = default;
};

struct Crew {
  std::string crewId;         ///< Globally unique within thread (crew-XXX).
  std::string threadId;       ///< Owning thread.
  std::string title;          ///< Short imperative title.
  std::string brief;          ///< Longer description of mission.
  CrewStatus status = CrewStatus::Forming;
  std::string coordinatorMemberId;
  std::string architectMemberId;          ///< Pinned Architect; "" if not enlisted.
  std::uint64_t formedAtMs = 0;
  std::uint64_t pausedAtMs = 0;
  std::uint64_t resumedAtMs = 0;
  std::uint64_t disbandedAtMs = 0;
  std::uint64_t schemaVersion = 1;
  std::uint64_t lastEventSequence = 0;    ///< Highest sequence in events.jsonl.
  std::uint64_t lastFlagSequence = 0;     ///< Highest sequence in flags.jsonl.
  std::map<std::string, std::string> meta; ///< Free-form key/value tags.

  bool operator==(const Crew &) const = default;
};

/// A crew-level quality gate; the Architect designs them, the Watchdog
/// evaluates them, and the Orchestrator cannot Disband cleanly while any
/// gate is failing.
struct CrewQualityGate {
  std::string gateId;             ///< Stable per-crew identifier (gate-XXX).
  std::string description;        ///< Human-readable gate name.
  std::string evaluatorJson;      ///< Structured spec, e.g. {"kind":"command",...}.
  bool passing = false;
  std::uint64_t lastEvaluatedAtMs = 0;
  std::string lastFailureReason;
  std::vector<std::string> dependentTaskIds; ///< Tasks blocked while gate fails.

  bool operator==(const CrewQualityGate &) const = default;
};

/// A watchdog flag raised against a member. Stored append-only in flags.jsonl.
struct CrewFlag {
  std::uint64_t sequence = 0;     ///< Monotonic per-crew flag sequence.
  std::string flagId;             ///< Stable per-crew identifier (flag-XXX).
  std::string memberId;           ///< Who triggered the rule.
  std::string ruleId;             ///< Which rule fired ("stalled", "tool_violation", ...).
  std::string severity;           ///< "info" | "warn" | "error".
  std::uint64_t raisedAtMs = 0;
  std::uint64_t resolvedAtMs = 0; ///< 0 if still raised.
  std::string resolvedByMemberId;
  std::string detail;             ///< Short human-readable.
  std::string detailJson;         ///< Full structured detail.

  bool operator==(const CrewFlag &) const = default;
};

/// Snapshot of a single crew's complete state (manifest + counts only,
/// never the full task/mail list — those are fetched on demand).
struct CrewSummary {
  Crew manifest;
  std::vector<CrewQualityGate> gates;
  std::size_t memberCount = 0;
  std::size_t taskCount = 0;
  std::size_t pendingTaskCount = 0;
  std::size_t inProgressTaskCount = 0;
  std::size_t readyForReviewCount = 0;
  std::size_t completedTaskCount = 0;
  std::size_t staleTaskCount = 0;
  std::size_t openMailCount = 0;
  std::size_t activeFlagCount = 0;
  std::size_t passingGateCount = 0;
  std::size_t failingGateCount = 0;
  std::uint64_t lastEventSequence = 0;
  std::uint64_t lastEventTimestampMs = 0;

  bool operator==(const CrewSummary &) const = default;
};

// ─── String mapping helpers ─────────────────────────────────────────────────

std::string crewStatusToString(CrewStatus value);
CrewStatus crewStatusFromString(const std::string &value);

std::string crewTaskStatusToString(CrewTaskStatus value);
CrewTaskStatus crewTaskStatusFromString(const std::string &value);

std::string crewRoleToString(CrewRole value);
CrewRole crewRoleFromString(const std::string &value);

std::string crewEventKindToString(CrewEventKind value);
CrewEventKind crewEventKindFromString(const std::string &value);

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_CREW_HPP
