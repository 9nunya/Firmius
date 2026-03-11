#include "workflow/WorkflowLoader.hpp"
#include "utils/StringUtil.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <cctype>

namespace firmius::core {

/**
 * Parse a YAML args array from frontmatter lines.
 * Expects format:
 * args:
 *   - name: arg_name
 *     type: string|number|filepath|agent_id|thread_id
 *     description: Some description
 *     optional: true|false
 */
static std::vector<WorkflowArg> parseYamlArgs(const std::vector<std::string> &frontmatterLines) {
  std::vector<WorkflowArg> args;
  bool inArgsArray = false;
  WorkflowArg currentArg;
  bool hasCurrentArg = false;

  for (const auto &line : frontmatterLines) {
    // Skip empty lines
    if (line.empty()) continue;

    // Check for args: key
    if (line.substr(0, 5) == "args:") {
      inArgsArray = true;
      continue;
    }

    if (!inArgsArray) continue;

    // Check if we've left the args section (new top-level key)
    if (line[0] != ' ' && line[0] != '\t' && line.find(':') != std::string::npos) {
      // Save current arg if any
      if (hasCurrentArg) {
        args.push_back(currentArg);
        hasCurrentArg = false;
      }
      inArgsArray = false;
      continue;
    }

    // Parse array item start
    if (line.substr(0, 2) == "- ") {
      // Save previous arg if any
      if (hasCurrentArg) {
        args.push_back(currentArg);
      }
      hasCurrentArg = true;
      currentArg = WorkflowArg{};
      currentArg.optional = false;

      // Check if there's inline content after "- "
      std::string inlineContent = firmius::shared::StringUtil::trim(line.substr(2));
      if (inlineContent.substr(0, 5) == "name:") {
        currentArg.name = firmius::shared::StringUtil::trim(inlineContent.substr(5));
      }
      continue;
    }

    // Parse arg properties
    if (hasCurrentArg) {
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;

      std::string key = firmius::shared::StringUtil::trim(line.substr(0, colon));
      std::string value = firmius::shared::StringUtil::trim(line.substr(colon + 1));

      if (key == "name") {
        currentArg.name = value;
      } else if (key == "type") {
        if (value == "string") currentArg.type = WorkflowArgType::String;
        else if (value == "number") currentArg.type = WorkflowArgType::Number;
        else if (value == "filepath") currentArg.type = WorkflowArgType::Filepath;
        else if (value == "agent_id") currentArg.type = WorkflowArgType::AgentId;
        else if (value == "thread_id") currentArg.type = WorkflowArgType::ThreadId;
        else currentArg.type = WorkflowArgType::String; // default
      } else if (key == "description") {
        currentArg.description = value;
      } else if (key == "optional") {
        currentArg.optional = (value == "true" || value == "yes" || value == "1");
      }
    }
  }

  // Don't forget the last arg
  if (hasCurrentArg) {
    args.push_back(currentArg);
  }

  return args;
}

WorkflowLoader &WorkflowLoader::instance() {
  static WorkflowLoader inst;
  return inst;
}

void WorkflowLoader::init() {
  std::string dir = getWorkflowsDir();

  if (!std::filesystem::exists(dir)) {
    return;
  }

  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".md") {
      auto workflow = loadWorkflow(entry.path().string());
      if (workflow) {
        workflows_[workflow->id] = *workflow;
      }
    }
  }
}

const Workflow *WorkflowLoader::getWorkflow(const std::string &id) const {
  auto it = workflows_.find(id);
  if (it != workflows_.end()) {
    return &it->second;
  }
  return nullptr;
}

std::vector<Workflow> WorkflowLoader::getAllWorkflows() const {
  std::vector<Workflow> result;
  result.reserve(workflows_.size());
  for (const auto &[id, workflow] : workflows_) {
    result.push_back(workflow);
  }
  return result;
}

std::vector<std::string> WorkflowLoader::getWorkflowIds() const {
  std::vector<std::string> result;
  result.reserve(workflows_.size());
  for (const auto &[id, workflow] : workflows_) {
    result.push_back(id);
  }
  return result;
}

void WorkflowLoader::bootstrapDefaults(const std::string &builtinWorkflowsDir) {
  const char *home = std::getenv("HOME");
  if (!home)
    return;

  std::string userDir = std::string(home) + "/.firmius/workflows";
  if (std::filesystem::exists(userDir))
    return;

  if (!std::filesystem::exists(builtinWorkflowsDir))
    return;

  std::filesystem::create_directories(userDir);
  for (const auto &entry :
       std::filesystem::directory_iterator(builtinWorkflowsDir)) {
    if (entry.is_regular_file()) {
      std::filesystem::copy_file(
          entry.path(),
          std::string(userDir) + "/" + entry.path().filename().string(),
          std::filesystem::copy_options::overwrite_existing);
    }
  }
}

std::optional<Workflow> WorkflowLoader::loadWorkflow(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Warning: Could not open workflow file: " << path << std::endl;
    return std::nullopt;
  }

  std::string line;
  std::vector<std::string> frontmatterLines;
  std::string body;
  bool inFrontmatter = false;
  int dashCount = 0;

  while (std::getline(file, line)) {
    if (line == "---") {
      dashCount++;
      if (dashCount == 1)
        inFrontmatter = true;
      else if (dashCount == 2)
        inFrontmatter = false;
      continue;
    }

    if (inFrontmatter)
      frontmatterLines.push_back(line);
    else
      body += line + "\n";
  }

  Workflow workflow;

  // Extract ID from filename
  std::filesystem::path fsPath(path);
  workflow.id = fsPath.stem().string();

  // Parse frontmatter
  for (const auto &fmLine : frontmatterLines) {
    auto colon = fmLine.find(':');
    if (colon == std::string::npos)
      continue;

    std::string key = firmius::shared::StringUtil::trim(fmLine.substr(0, colon));
    std::string value =
        firmius::shared::StringUtil::trim(fmLine.substr(colon + 1));

    if (key == "name")
      workflow.name = value;
    else if (key == "description")
      workflow.description = value;
  }

  // Parse YAML args array if present
  workflow.args = parseYamlArgs(frontmatterLines);

  // Set defaults if not provided
  if (workflow.name.empty()) {
    // Convert snake_case to Title Case
    std::string name = workflow.id;
    std::replace(name.begin(), name.end(), '_', ' ');
    for (auto &c : name) {
      if (c == ' ' || c == '_') {
        continue;
      }
      c = std::toupper(c);
    }
    // Capitalize first letter of each word
    bool newWord = true;
    for (auto &c : name) {
      if (c == ' ') {
        newWord = true;
      } else if (newWord) {
        c = std::toupper(c);
        newWord = false;
      } else {
        c = std::tolower(c);
      }
    }
    workflow.name = name;
  }

  if (workflow.description.empty()) {
    workflow.description = "Execute workflow: " + workflow.name;
  }

  workflow.body = firmius::shared::StringUtil::trim(body);

  // Count argument placeholders ($1, $2, etc.) for legacy support
  std::regex argPattern(R"(\$([0-9]+))");
  auto begin = std::sregex_iterator(workflow.body.begin(), workflow.body.end(),
                                    argPattern);
  auto end = std::sregex_iterator();

  size_t maxArg = 0;
  for (auto it = begin; it != end; ++it) {
    std::smatch match = *it;
    size_t argNum = std::stoul(match[1].str());
    maxArg = std::max(maxArg, argNum);
  }
  workflow.argCount = maxArg;

  // If no YAML args defined, fall back to legacy mode (argCount from $N placeholders)
  // If YAML args defined, use those and ignore argCount
  if (workflow.args.empty() && workflow.argCount > 0) {
    // Legacy mode: create generic string args based on $N placeholders
    for (size_t i = 0; i < workflow.argCount; ++i) {
      WorkflowArg arg;
      arg.name = "arg" + std::to_string(i + 1);
      arg.type = WorkflowArgType::String;
      arg.description = "Argument " + std::to_string(i + 1);
      arg.optional = false;
      workflow.args.push_back(arg);
    }
  }

  return workflow;
}

std::string WorkflowLoader::getWorkflowsDir() const {
  const char *envDir = std::getenv("FIRMIUS_WORKFLOWS_DIR");
  if (envDir && std::filesystem::exists(envDir)) {
    std::string dir = envDir;
    if (dir.back() != '/')
      dir += '/';
    return dir;
  }

  const char *home = std::getenv("HOME");
  if (home) {
    std::string userDir = std::string(home) + "/.firmius/workflows/";
    if (std::filesystem::exists(userDir)) {
      return userDir;
    }
  }

  return "workflows/";
}

} // namespace firmius::core
