#pragma once

#include "ICommand.hpp"
#include "workflow/Workflow.hpp"

namespace firmius::tui {

/**
 * Command wrapper for executing a specific workflow.
 * Each workflow .md file gets registered as its own top-level command.
 * Example: /parallel_exploration executes parallel_exploration.md
 */
class WorkflowCommand : public ICommand {
public:
  explicit WorkflowCommand(const firmius::core::Workflow &workflow);

  std::string name() const override { return workflow_.id; }
  std::string description() const override { return workflow_.description; }
  std::vector<CommandArg> args() const override;
  void execute(CommandCtx &ctx,
               const std::vector<ParsedArg> &args) override;
  bool isWorkflow() const override { return true; }

private:
  firmius::core::Workflow workflow_;
};

/**
 * Registers all workflows as individual commands in the CommandManager.
 * Call this during TUI initialization after WorkflowLoader::init().
 */
void registerWorkflowCommands();

} // namespace firmius::tui
