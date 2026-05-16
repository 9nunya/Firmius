#include "WorkflowCommand.hpp"
#include "ActionDispatcher.hpp"
#include "workflow/WorkflowLoader.hpp"

namespace firmius::tui2 {

WorkflowCommand::WorkflowCommand(const firmius::core::Workflow &workflow,
                                 ActionDispatcher &dispatcher)
    : workflow_(workflow), dispatcher_(dispatcher) {}

std::string WorkflowCommand::name() const {
  if (workflow_.slashCommand.has_value() && !workflow_.slashCommand->empty()) {
    std::string cmd = *workflow_.slashCommand;
    if (!cmd.empty() && cmd.front() == '/') {
      cmd.erase(cmd.begin());
    }
    return cmd.empty() ? workflow_.id : cmd;
  }
  return workflow_.id;
}

std::string WorkflowCommand::description() const {
  return workflow_.description;
}

std::vector<CommandArg> WorkflowCommand::args() const {
  std::vector<CommandArg> result;
  for (const auto &wfArg : workflow_.args) {
    CommandArg arg;
    arg.name = wfArg.name;
    arg.description = wfArg.description;
    arg.optional = wfArg.optional;

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

    result.push_back(std::move(arg));
  }
  return result;
}

void WorkflowCommand::execute(const std::vector<ParsedArg> &args) {
  std::vector<std::string> strArgs;
  strArgs.reserve(args.size());
  for (const auto &arg : args) {
    strArgs.push_back(arg.rawValue);
  }
  dispatcher_.executeWorkflow(workflow_.id, strArgs);
}

bool WorkflowCommand::takesRawRemainder() const {
  return workflow_.rawRemainder;
}

void registerWorkflowCommands(CommandManager &manager,
                              ActionDispatcher &dispatcher) {
  auto &loader = firmius::core::WorkflowLoader::instance();
  auto workflows = loader.getAllWorkflows();

  for (const auto &workflow : workflows) {
    // Skip event-triggered hooks — they're not user-invocable slash commands.
    if (workflow.isHook()) continue;

    auto cmd = std::make_shared<WorkflowCommand>(workflow, dispatcher);
    if (!manager.getCommand(cmd->name())) {
      manager.registerCommand(cmd);
    }
  }
}

} // namespace firmius::tui2
