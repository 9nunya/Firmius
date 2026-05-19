#include "workflow/Workflow.hpp"
#include <algorithm>
#include <regex>
#include <stdexcept>
#include <unordered_map>

namespace firmius::core {

namespace {
const std::unordered_map<std::string, WorkflowEventKind> &eventKindMap() {
  // Map every accepted spelling (snake_case + CamelCase) to a kind. Keep
  // the table in sync with WorkflowEventKind and the dispatcher fire-sites.
  static const std::unordered_map<std::string, WorkflowEventKind> kMap = {
      {"pre_tool_use", WorkflowEventKind::PreToolUse},
      {"PreToolUse", WorkflowEventKind::PreToolUse},
      {"post_tool_use", WorkflowEventKind::PostToolUse},
      {"PostToolUse", WorkflowEventKind::PostToolUse},
      {"user_message", WorkflowEventKind::UserMessage},
      {"UserMessage", WorkflowEventKind::UserMessage},
      {"agent_stop", WorkflowEventKind::AgentStop},
      {"AgentStop", WorkflowEventKind::AgentStop},
      {"thread_start", WorkflowEventKind::ThreadStart},
      {"ThreadStart", WorkflowEventKind::ThreadStart},
      {"thread_resume", WorkflowEventKind::ThreadResume},
      {"ThreadResume", WorkflowEventKind::ThreadResume},
      {"mode_entered", WorkflowEventKind::ModeEntered},
      {"ModeEntered", WorkflowEventKind::ModeEntered},
      {"mode_exited", WorkflowEventKind::ModeExited},
      {"ModeExited", WorkflowEventKind::ModeExited},
      {"subagent_return", WorkflowEventKind::SubagentReturn},
      {"SubagentReturn", WorkflowEventKind::SubagentReturn},
      {"workflow_complete", WorkflowEventKind::WorkflowComplete},
      {"WorkflowComplete", WorkflowEventKind::WorkflowComplete},
  };
  return kMap;
}
} // namespace

WorkflowEventKind workflowEventKindFromString(const std::string &s) {
  const auto &m = eventKindMap();
  const auto it = m.find(s);
  return it == m.end() ? WorkflowEventKind::Unknown : it->second;
}

std::string workflowEventKindToString(WorkflowEventKind kind) {
  switch (kind) {
  case WorkflowEventKind::PreToolUse:
    return "pre_tool_use";
  case WorkflowEventKind::PostToolUse:
    return "post_tool_use";
  case WorkflowEventKind::UserMessage:
    return "user_message";
  case WorkflowEventKind::AgentStop:
    return "agent_stop";
  case WorkflowEventKind::ThreadStart:
    return "thread_start";
  case WorkflowEventKind::ThreadResume:
    return "thread_resume";
  case WorkflowEventKind::ModeEntered:
    return "mode_entered";
  case WorkflowEventKind::ModeExited:
    return "mode_exited";
  case WorkflowEventKind::SubagentReturn:
    return "subagent_return";
  case WorkflowEventKind::WorkflowComplete:
    return "workflow_complete";
  case WorkflowEventKind::Unknown:
    break;
  }
  return "unknown";
}

bool workflowEventIsBlockable(WorkflowEventKind kind) {
  switch (kind) {
  case WorkflowEventKind::PreToolUse:
  case WorkflowEventKind::UserMessage:
  case WorkflowEventKind::ThreadStart:
  case WorkflowEventKind::ThreadResume:
  case WorkflowEventKind::AgentStop:
    return true;
  default:
    return false;
  }
}

WorkflowActionKind workflowActionKindFromString(const std::string &s) {
  if (s == "prompt" || s == "Prompt")
    return WorkflowActionKind::Prompt;
  if (s == "shell" || s == "Shell")
    return WorkflowActionKind::Shell;
  if (s == "workflow" || s == "Workflow")
    return WorkflowActionKind::Workflow;
  if (s == "agent" || s == "Agent")
    return WorkflowActionKind::Agent;
  if (s == "tool_intercept" || s == "ToolIntercept")
    return WorkflowActionKind::ToolIntercept;
  if (s == "script" || s == "Script")
    return WorkflowActionKind::Script;
  if (s == "state" || s == "State")
    return WorkflowActionKind::State;
  if (s == "compose" || s == "Compose")
    return WorkflowActionKind::Compose;
  if (s == "tool" || s == "Tool")
    return WorkflowActionKind::Tool;
  return WorkflowActionKind::Prompt;
}

std::string workflowActionKindToString(WorkflowActionKind kind) {
  switch (kind) {
  case WorkflowActionKind::Prompt:        return "prompt";
  case WorkflowActionKind::Shell:         return "shell";
  case WorkflowActionKind::Workflow:      return "workflow";
  case WorkflowActionKind::Agent:         return "agent";
  case WorkflowActionKind::ToolIntercept: return "tool_intercept";
  case WorkflowActionKind::Script:        return "script";
  case WorkflowActionKind::State:         return "state";
  case WorkflowActionKind::Compose:       return "compose";
  case WorkflowActionKind::Tool:          return "tool";
  }
  return "prompt";
}

std::string Workflow::build(const std::vector<std::string> &args) const {
  // Validate required arguments
  for (size_t i = 0; i < args.size() && i < this->args.size(); ++i) {
    if (!this->args[i].optional && args[i].empty()) {
      throw std::runtime_error("Missing required argument: " + this->args[i].name);
    }
  }

  // Check that all required args are provided
  for (size_t i = args.size(); i < this->args.size(); ++i) {
    if (!this->args[i].optional) {
      throw std::runtime_error("Missing required argument: " + this->args[i].name);
    }
  }

  std::string result = body;

  for (size_t i = 0; i < args.size(); ++i) {
    std::string placeholder = "$" + std::to_string(i + 1);
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
      result.replace(pos, placeholder.length(), args[i]);
      pos += args[i].length();
    }
  }

  // Remove any remaining unmatched placeholders
  std::regex unmatched(R"(\$[0-9]+)");
  result = std::regex_replace(result, unmatched, "");

  return result;
}

} // namespace firmius::core
