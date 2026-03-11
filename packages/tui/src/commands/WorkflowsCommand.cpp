#include "commands/WorkflowsCommand.hpp"
#include "commands/CommandManager.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <iostream>
#include <sstream>

namespace firmius::tui {

WorkflowCommand::WorkflowCommand(const firmius::core::Workflow &workflow)
    : workflow_(workflow) {}

std::vector<CommandArg> WorkflowCommand::args() const {
  std::vector<CommandArg> args;

  for (const auto &wfArg : workflow_.args) {
    CommandArg arg;
    arg.name = wfArg.name;
    arg.description = wfArg.description;
    arg.optional = wfArg.optional;

    // Map WorkflowArgType to CommandArg ArgType
    switch (wfArg.type) {
      case firmius::core::WorkflowArgType::String:
        arg.type = ArgType::String;
        break;
      case firmius::core::WorkflowArgType::Number:
        arg.type = ArgType::Number;
        break;
      case firmius::core::WorkflowArgType::Filepath:
        arg.type = ArgType::Filepath;
        break;
      case firmius::core::WorkflowArgType::AgentId:
        arg.type = ArgType::AgentId;
        break;
      case firmius::core::WorkflowArgType::ThreadId:
        arg.type = ArgType::ThreadId;
        break;
    }

    args.push_back(arg);
  }

  return args;
}

void WorkflowCommand::execute(CommandCtx &ctx,
                              const std::vector<ParsedArg> &parsedArgs) {
  (void)ctx; // Mark as unused for now

  std::vector<std::string> args;
  args.reserve(parsedArgs.size());

  for (const auto &arg : parsedArgs) {
    args.push_back(arg.raw_value);
  }

  auto &harness = firmius::core::Harness::instance();
  bool success = harness.executeWorkflow(workflow_.id, args);

  if (!success) {
    std::stringstream ss;
    ss << "## Workflow Error\n\n";
    ss << "Failed to execute workflow '" << workflow_.id << "'.\n";
    std::cout << ss.str() << std::endl;
  }
}

void registerWorkflowCommands() {
  auto &loader = firmius::core::WorkflowLoader::instance();
  auto &commandManager = CommandManager::instance();

  auto workflows = loader.getAllWorkflows();
  for (const auto &workflow : workflows) {
    commandManager.registerCommand(
        std::make_shared<WorkflowCommand>(workflow));
  }
}

} // namespace firmius::tui
