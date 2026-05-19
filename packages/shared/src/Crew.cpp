#include "Crew.hpp"

#include <stdexcept>

namespace firmius::shared {

std::string crewStatusToString(CrewStatus value) {
  switch (value) {
  case CrewStatus::Forming:
    return "Forming";
  case CrewStatus::Active:
    return "Active";
  case CrewStatus::Idle:
    return "Idle";
  case CrewStatus::Paused:
    return "Paused";
  case CrewStatus::Disbanded:
    return "Disbanded";
  }
  return "Forming";
}

CrewStatus crewStatusFromString(const std::string &value) {
  if (value == "Forming")
    return CrewStatus::Forming;
  if (value == "Active")
    return CrewStatus::Active;
  if (value == "Idle")
    return CrewStatus::Idle;
  if (value == "Paused")
    return CrewStatus::Paused;
  if (value == "Disbanded")
    return CrewStatus::Disbanded;
  throw std::runtime_error("Unknown CrewStatus: " + value);
}

std::string crewTaskStatusToString(CrewTaskStatus value) {
  switch (value) {
  case CrewTaskStatus::Pending:
    return "Pending";
  case CrewTaskStatus::Assigned:
    return "Assigned";
  case CrewTaskStatus::InProgress:
    return "InProgress";
  case CrewTaskStatus::ReadyForReview:
    return "ReadyForReview";
  case CrewTaskStatus::Accepted:
    return "Accepted";
  case CrewTaskStatus::Rejected:
    return "Rejected";
  case CrewTaskStatus::Salvaged:
    return "Salvaged";
  case CrewTaskStatus::Cancelled:
    return "Cancelled";
  }
  return "Pending";
}

CrewTaskStatus crewTaskStatusFromString(const std::string &value) {
  if (value == "Pending")
    return CrewTaskStatus::Pending;
  if (value == "Assigned")
    return CrewTaskStatus::Assigned;
  if (value == "InProgress")
    return CrewTaskStatus::InProgress;
  if (value == "ReadyForReview")
    return CrewTaskStatus::ReadyForReview;
  if (value == "Accepted")
    return CrewTaskStatus::Accepted;
  if (value == "Rejected")
    return CrewTaskStatus::Rejected;
  if (value == "Salvaged")
    return CrewTaskStatus::Salvaged;
  if (value == "Cancelled")
    return CrewTaskStatus::Cancelled;
  throw std::runtime_error("Unknown CrewTaskStatus: " + value);
}

std::string crewRoleToString(CrewRole value) {
  switch (value) {
  case CrewRole::Coordinator:
    return "Coordinator";
  case CrewRole::SubOrchestrator:
    return "SubOrchestrator";
  case CrewRole::Architect:
    return "Architect";
  case CrewRole::Lead:
    return "Lead";
  case CrewRole::Builder:
    return "Builder";
  case CrewRole::Reviewer:
    return "Reviewer";
  case CrewRole::Scout:
    return "Scout";
  case CrewRole::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

CrewRole crewRoleFromString(const std::string &value) {
  if (value == "Coordinator")
    return CrewRole::Coordinator;
  if (value == "SubOrchestrator")
    return CrewRole::SubOrchestrator;
  if (value == "Architect")
    return CrewRole::Architect;
  if (value == "Lead")
    return CrewRole::Lead;
  if (value == "Builder")
    return CrewRole::Builder;
  if (value == "Reviewer")
    return CrewRole::Reviewer;
  if (value == "Scout")
    return CrewRole::Scout;
  if (value == "Unknown")
    return CrewRole::Unknown;
  // Legacy aliases from earlier prototype, accepted for back-compat:
  if (value == "Executor" || value == "Worker")
    return CrewRole::Builder;
  if (value == "Observer")
    return CrewRole::Scout;
  throw std::runtime_error("Unknown CrewRole: " + value);
}

std::string crewEventKindToString(CrewEventKind value) {
  switch (value) {
  case CrewEventKind::CrewFormed:
    return "CrewFormed";
  case CrewEventKind::CrewDisbanded:
    return "CrewDisbanded";
  case CrewEventKind::CrewPaused:
    return "CrewPaused";
  case CrewEventKind::CrewResumed:
    return "CrewResumed";
  case CrewEventKind::MemberEnlisted:
    return "MemberEnlisted";
  case CrewEventKind::MemberDischarged:
    return "MemberDischarged";
  case CrewEventKind::TaskCreated:
    return "TaskCreated";
  case CrewEventKind::TaskAssigned:
    return "TaskAssigned";
  case CrewEventKind::TaskReassigned:
    return "TaskReassigned";
  case CrewEventKind::TaskHeartbeat:
    return "TaskHeartbeat";
  case CrewEventKind::TaskReadyForReview:
    return "TaskReadyForReview";
  case CrewEventKind::TaskAccepted:
    return "TaskAccepted";
  case CrewEventKind::TaskRejected:
    return "TaskRejected";
  case CrewEventKind::TaskSalvaged:
    return "TaskSalvaged";
  case CrewEventKind::TaskCancelled:
    return "TaskCancelled";
  case CrewEventKind::ChannelOpened:
    return "ChannelOpened";
  case CrewEventKind::ChannelClosed:
    return "ChannelClosed";
  case CrewEventKind::MailSent:
    return "MailSent";
  case CrewEventKind::MailAcked:
    return "MailAcked";
  case CrewEventKind::CoordinatorChanged:
    return "CoordinatorChanged";
  case CrewEventKind::GateAdded:
    return "GateAdded";
  case CrewEventKind::GateUpdated:
    return "GateUpdated";
  case CrewEventKind::GatePassed:
    return "GatePassed";
  case CrewEventKind::GateFailed:
    return "GateFailed";
  case CrewEventKind::FlagRaised:
    return "FlagRaised";
  case CrewEventKind::FlagResolved:
    return "FlagResolved";
  case CrewEventKind::NudgeSent:
    return "NudgeSent";
  }
  return "CrewFormed";
}

CrewEventKind crewEventKindFromString(const std::string &value) {
  if (value == "CrewFormed")
    return CrewEventKind::CrewFormed;
  if (value == "CrewDisbanded")
    return CrewEventKind::CrewDisbanded;
  if (value == "CrewPaused")
    return CrewEventKind::CrewPaused;
  if (value == "CrewResumed")
    return CrewEventKind::CrewResumed;
  if (value == "MemberEnlisted")
    return CrewEventKind::MemberEnlisted;
  if (value == "MemberDischarged")
    return CrewEventKind::MemberDischarged;
  if (value == "TaskCreated")
    return CrewEventKind::TaskCreated;
  if (value == "TaskAssigned")
    return CrewEventKind::TaskAssigned;
  if (value == "TaskReassigned")
    return CrewEventKind::TaskReassigned;
  if (value == "TaskHeartbeat")
    return CrewEventKind::TaskHeartbeat;
  if (value == "TaskReadyForReview")
    return CrewEventKind::TaskReadyForReview;
  if (value == "TaskAccepted")
    return CrewEventKind::TaskAccepted;
  if (value == "TaskRejected")
    return CrewEventKind::TaskRejected;
  if (value == "TaskSalvaged")
    return CrewEventKind::TaskSalvaged;
  if (value == "TaskCancelled")
    return CrewEventKind::TaskCancelled;
  if (value == "ChannelOpened")
    return CrewEventKind::ChannelOpened;
  if (value == "ChannelClosed")
    return CrewEventKind::ChannelClosed;
  if (value == "MailSent")
    return CrewEventKind::MailSent;
  if (value == "MailAcked")
    return CrewEventKind::MailAcked;
  if (value == "CoordinatorChanged")
    return CrewEventKind::CoordinatorChanged;
  if (value == "GateAdded")
    return CrewEventKind::GateAdded;
  if (value == "GateUpdated")
    return CrewEventKind::GateUpdated;
  if (value == "GatePassed")
    return CrewEventKind::GatePassed;
  if (value == "GateFailed")
    return CrewEventKind::GateFailed;
  if (value == "FlagRaised")
    return CrewEventKind::FlagRaised;
  if (value == "FlagResolved")
    return CrewEventKind::FlagResolved;
  if (value == "NudgeSent")
    return CrewEventKind::NudgeSent;
  throw std::runtime_error("Unknown CrewEventKind: " + value);
}

} // namespace firmius::shared
