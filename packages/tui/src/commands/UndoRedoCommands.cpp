#include "commands/UndoRedoCommands.hpp"

#include "harness/Harness.hpp"

#include <iostream>

namespace firmius::tui {

void UndoTurnCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)args;
  auto &harness = firmius::core::Harness::instance();
  auto result = harness.undoTurnsWithRedo(1);
  if (!result) {
    std::cout << "Transcript undo was not applied\n";
    return;
  }

  if (ctx.state) {
    ctx.state->last_transcript_undo_action_ = result;
    ctx.state->last_transcript_redo_action_.reset();
    ctx.state->refreshFocusedHistory();
    ctx.state->notifyChatTranscriptChanged();
  }

  std::cout << "Transcript undo action: " << result->undoActionId << "\n";
}

void RedoCommand::execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) {
  (void)args;

  auto &harness = firmius::core::Harness::instance();

  if (!ctx.state || !ctx.state->last_transcript_undo_action_) {
    std::cout << "No transcript undo action is available to redo\n";
    return;
  }

  const std::string undoActionId = ctx.state->last_transcript_undo_action_->undoActionId;
  auto eligibility = harness.evaluateTranscriptRedo(undoActionId);
  if (!eligibility.redoable) {
    std::cout << "Transcript redo unavailable: " << eligibility.reason << "\n";
    return;
  }

  auto result = harness.redoTranscriptUndoAction(undoActionId);
  if (!result) {
    std::cout << "Transcript redo was not applied\n";
    return;
  }

  ctx.state->last_transcript_redo_action_ = result;
  ctx.state->last_transcript_undo_action_.reset();
  ctx.state->refreshFocusedHistory();
  ctx.state->notifyChatTranscriptChanged();

  std::cout << "Transcript redo action: " << result->redoActionId << "\n";
}

} // namespace firmius::tui
