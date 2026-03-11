#include "commands/WorkflowsCommand.hpp"
#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <iostream>
#include <sstream>

namespace firmius::tui {

std::vector<CommandArg> WorkflowsCommand::args() const {
  std::vector<CommandArg> args;

  // First arg: workflow ID (required)
  CommandArg workflowArg;
  workflowArg.name = "workflow_id";
  workflowArg.type = ArgType::String;
  workflowArg.description = "Workflow ID to execute";
  workflowArg.optional = true; // Optional to allow listing
  args.push_back(workflowArg);

  // Additional args are dynamic based on workflow, but we'll accept strings
  for (int i = 0; i < 10; ++i) {
    CommandArg arg;
    arg.name = "arg" + std::to_string(i + 1);
    arg.type = ArgType::String;
    arg.description = "Argument " + std::to_string(i + 1);
    arg.optional = true;
    args.push_back(arg);
  }

  return args;
}

void WorkflowsCommand::execute(CommandCtx &ctx,
                               const std::vector<ParsedArg> &parsedArgs) {
  (void)ctx; // Mark as unused for now
  auto &loader = firmius::core::WorkflowLoader::instance();
  auto &harness = firmius::core::Harness::instance();

  // If no workflow ID provided, list all workflows
  if (parsedArgs.empty() || parsedArgs[0].raw_value.empty()) {
    auto workflows = loader.getAllWorkflows();

    std::stringstream ss;
    ss << "## Available Workflows\n\n";
    ss << "Use `/workflows <id> [arg1] [arg2] ...` to execute a workflow.\n\n";

    for (const auto &wf : workflows) {
      ss << "### " << wf.name << " (`" << wf.id << "`)\n\n";
      ss << wf.description << "\n\n";
      if (wf.argCount > 0) {
        ss << "**Arguments:** " << wf.argCount << "\n\n";
      }
    }

    // Display in TUI log/output
    std::cout << ss.str() << std::endl;
    return;
  }

  // Execute the specified workflow
  std::string workflowId = parsedArgs[0].raw_value;
  std::vector<std::string> args;

  // Collect arguments (skip the first one which is the workflow ID)
  for (size_t i = 1; i < parsedArgs.size(); ++i) {
    if (!parsedArgs[i].raw_value.empty()) {
      args.push_back(parsedArgs[i].raw_value);
    }
  }

  bool success = harness.executeWorkflow(workflowId, args);

  if (!success) {
    std::stringstream ss;
    ss << "## Workflow Error\n\n";
    ss << "Workflow '" << workflowId << "' not found.\n\n";
    ss << "Use `/workflows` to list available workflows.\n";
    std::cout << ss.str() << std::endl;
  }
}

} // namespace firmius::tui
