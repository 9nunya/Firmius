#include "commands/EditHistoryCommand.hpp"

#include "harness/Harness.hpp"
#include "persistence/ThreadManager.hpp"
#include "Serialization.hpp"

#include <iostream>
#include <algorithm>

namespace {

bool isUserTurn(const firmius::shared::AgentTurn &turn) {
  for (const auto &msg : turn.messages) {
    if (msg.role == firmius::shared::Role::User) {
      return true;
    }
  }
  return false;
}

bool isCompactionTurn(const firmius::shared::AgentTurn &turn) {
  return turn.turnId.rfind("compaction-start-", 0) == 0 ||
         turn.turnId.rfind("compaction-summary-", 0) == 0 ||
         turn.turnId.rfind("compaction-end-", 0) == 0;
}

int defaultUndoTurnCount(firmius::core::Harness &harness) {
  const std::string agentId = harness.focusedAgentId();
  if (agentId.empty()) {
    return 1;
  }
  auto history = harness.getAgentHistoryPtr(agentId);
  if (!history || history->turns.size() <= 2) {
    return 1;
  }
  for (std::size_t i = history->turns.size(); i-- > 2;) {
    if (isUserTurn(history->turns[i])) {
      return std::max(1, static_cast<int>(history->turns.size() - i));
    }
  }
  int compactionTail = 0;
  for (std::size_t i = history->turns.size(); i-- > 2;) {
    if (!isCompactionTurn(history->turns[i])) {
      break;
    }
    ++compactionTail;
  }
  return std::max(1, compactionTail);
}

} // namespace

namespace firmius::tui {

void EditsCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  if (!ctx.state) {
    std::cout << "No TUI state available\n";
    return;
  }
  (void)args;
  auto &harness = firmius::core::Harness::instance();
  const std::string threadId = harness.currentThreadId();
  std::cout << "# Edit History\n\n";
  if (!threadId.empty()) {
    std::cout << "Thread: " << threadId << "\n\n";
  }

  auto edits = harness.listEditBatches();
  if (edits.empty()) {
    std::cout << "No persisted edits found.\n";
    return;
  }

  // Newest-first for a more useful rollback console.
  std::sort(edits.begin(), edits.end(),
            [](const auto &a, const auto &b) { return a.createdAt > b.createdAt; });

  for (const auto &edit : edits) {
    std::cout << "- " << edit.editBatchId << "  [" << edit.toolName << "]";
    if (!edit.turnId.empty()) std::cout << "  turn=" << edit.turnId;
    if (edit.createdAt > 0) std::cout << "  at=" << edit.createdAt;
    std::cout << "\n";
    std::cout << "    status=" << firmius::shared::editBatchStatusToString(edit.status)
              << "  added=" << edit.addedLines
              << "  removed=" << edit.removedLines << "\n";
    if (!edit.summaryText.empty()) {
      std::cout << "    summary: " << edit.summaryText << "\n";
    }

    auto eligibility = harness.evaluateEditBatchUndo(edit.editBatchId);
    std::cout << "    undoable: " << (eligibility.undoable ? "yes" : "no");
    if (!eligibility.reason.empty()) std::cout << "  reason=" << eligibility.reason;
    std::cout << "\n";
    if (!eligibility.blockingEditBatchIds.empty()) {
      std::cout << "    blocked_by: ";
      for (size_t i = 0; i < eligibility.blockingEditBatchIds.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << eligibility.blockingEditBatchIds[i];
      }
      std::cout << "\n";
    }
  }

  std::cout << "\nRollback:\n";
  std::cout << "- /undo_edit <edit_batch_id>\n";
  std::cout << "- /redo_edit <edit_undo_action_id>\n";
  std::cout << "\nDetails:\n";
  std::cout << "- /undo_edit <id> will print an undo_action_id (persisted)\n";
  std::cout << "- /redo_edit <undo_action_id> replays that undo\n";
}

void UndoTranscriptCommand::execute(CommandCtx &ctx,
                                    const std::vector<ParsedArg> &args) {
  (void)ctx;
  auto &harness = firmius::core::Harness::instance();
  int count = args.empty() ? defaultUndoTurnCount(harness) : 1;
  if (!args.empty()) {
    try {
      count = args.front().asInt();
    } catch (...) {
      count = 1;
    }
  }
  auto result = harness.undoTurnsWithRedo(std::max(1, count));
  std::cout << (result ? ("Transcript undo action: " + result->undoActionId + "\n") : "Transcript undo was not applied\n");
}

void UndoEditCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)ctx;
  if (args.empty() || args.front().raw_value.empty()) {
    std::cout << "undo_edit requires an edit batch id\n";
    return;
  }
  auto result = firmius::core::Harness::instance().undoEditBatch(args.front().raw_value);
  if (!result.has_value()) {
    std::cout << "Edit undo was not applied\n";
    return;
  }
  std::cout << "Undo edit result: " << result->undoActionId << "\n";
}

void RedoTranscriptCommand::execute(CommandCtx &ctx,
                                    const std::vector<ParsedArg> &args) {
  (void)ctx;
  if (args.empty() || args.front().raw_value.empty()) {
    std::cout << "redo_transcript requires a transcript undo action id\n";
    return;
  }
  auto &harness = firmius::core::Harness::instance();
  auto eligibility = harness.evaluateTranscriptRedo(args.front().raw_value);
  if (!eligibility.redoable) {
    std::cout << "Transcript redo unavailable: " << eligibility.reason << "\n";
    return;
  }
  auto result = harness.redoTranscriptUndoAction(args.front().raw_value);
  std::cout << (result ? ("Transcript redo action: " + result->redoActionId + "\n")
                      : "Transcript redo was not applied\n");
}

void RedoEditCommand::execute(CommandCtx &ctx,
                              const std::vector<ParsedArg> &args) {
  (void)ctx;
  if (args.empty() || args.front().raw_value.empty()) {
    std::cout << "redo_edit requires an edit undo action id\n";
    return;
  }
  auto &harness = firmius::core::Harness::instance();
  auto eligibility = harness.evaluateEditBatchRedo(args.front().raw_value);
  if (!eligibility.redoable) {
    std::cout << "Edit redo unavailable: " << eligibility.reason << "\n";
    return;
  }
  auto result = harness.redoEditUndoAction(args.front().raw_value);
  std::cout << (result ? ("Edit redo action: " + result->redoActionId + "\n")
                       : "Edit redo was not applied\n");
}

} // namespace firmius::tui
