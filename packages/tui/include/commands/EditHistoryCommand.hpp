#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class EditsCommand : public ICommand {
public:
  std::string name() const override { return "edits"; }
  std::string description() const override { return "List persisted edit batches"; }
  std::vector<CommandArg> args() const override { return {}; }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

class UndoEditCommand : public ICommand {
public:
  std::string name() const override { return "undo_edit"; }
  std::string description() const override { return "Undo a persisted edit batch by id"; }
  std::vector<CommandArg> args() const override {
    return {{"edit_batch_id", ArgType::String, "Edit batch id", false}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

class UndoTranscriptCommand : public ICommand {
public:
  std::string name() const override { return "undo_transcript"; }
  std::string description() const override { return "Undo transcript turns with persisted redo capture"; }
  std::vector<CommandArg> args() const override {
    return {{"count", ArgType::Number, "Number of turns to undo", true}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

class RedoTranscriptCommand : public ICommand {
public:
  std::string name() const override { return "redo_transcript"; }
  std::string description() const override { return "Replay a persisted transcript undo action by id"; }
  std::vector<CommandArg> args() const override {
    return {{"undo_action_id", ArgType::String, "Transcript undo action id", false}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

class RedoEditCommand : public ICommand {
public:
  std::string name() const override { return "redo_edit"; }
  std::string description() const override { return "Replay a persisted edit undo action by id"; }
  std::vector<CommandArg> args() const override {
    return {{"undo_action_id", ArgType::String, "Edit undo action id", false}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui
