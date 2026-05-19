#pragma once

#include "CommandManager.hpp"
#include "workflow/Workflow.hpp"

#include <vector>

namespace firmius::tui {

class ActionDispatcher;

/// Wraps a core::Workflow as an ICommand for tui.
class WorkflowCommand : public ICommand {
public:
  WorkflowCommand(const firmius::core::Workflow &workflow,
                  ActionDispatcher &dispatcher);

  std::string name() const override;
  std::string description() const override;
  std::vector<CommandArg> args() const override;
  void execute(const std::vector<ParsedArg> &args) override;
  bool isWorkflow() const override { return true; }
  bool takesRawRemainder() const override;

private:
  firmius::core::Workflow workflow_;
  ActionDispatcher &dispatcher_;
};

/// Load all workflows from WorkflowLoader and register them as commands.
void registerWorkflowCommands(CommandManager &manager,
                              ActionDispatcher &dispatcher);

} // namespace firmius::tui
