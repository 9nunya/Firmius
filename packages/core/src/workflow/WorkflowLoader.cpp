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

namespace firmius::core {

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
  std::string frontmatter;
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
      frontmatter += line + "\n";
    else
      body += line + "\n";
  }

  Workflow workflow;

  // Extract ID from filename
  std::filesystem::path fsPath(path);
  workflow.id = fsPath.stem().string();

  // Parse frontmatter
  std::stringstream ss_fm(frontmatter);
  while (std::getline(ss_fm, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;

    std::string key = firmius::shared::StringUtil::trim(line.substr(0, colon));
    std::string value =
        firmius::shared::StringUtil::trim(line.substr(colon + 1));

    if (key == "name")
      workflow.name = value;
    else if (key == "description")
      workflow.description = value;
  }

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

  // Count argument placeholders ($1, $2, etc.)
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
