#include "workflow/WorkflowLoader.hpp"
#include "utils/FrontmatterParser.hpp"
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
#include <unistd.h>

namespace firmius::core {

namespace {

bool ensureWritableDirectory(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec || !std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
    return false;
  }

  const auto probe = dir / (".write_probe_" + firmius::shared::StringUtil::generateUuid());
  std::ofstream out(probe);
  if (!out.is_open()) {
    return false;
  }
  out << "ok";
  out.close();
  std::filesystem::remove(probe, ec);
  return true;
}

bool isUsableWorkflowDir(const std::filesystem::path &dir) {
  try {
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
      return false;
    }
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".md") {
        continue;
      }
      std::ifstream file(entry.path());
      if (file.good()) {
        return true;
      }
    }
    return false;
  } catch (...) {
    return false;
  }
}

std::filesystem::path resolveWritableFirmiusHome() {
  if (const char *home = std::getenv("HOME")) {
    const std::filesystem::path userHome = std::filesystem::path(home) / ".firmius";
    if (ensureWritableDirectory(userHome)) {
      return userHome;
    }
  }

  const std::filesystem::path localHome =
      std::filesystem::current_path() / ".firmius";
  if (ensureWritableDirectory(localHome)) {
    return localHome;
  }

  const std::filesystem::path tempHome =
      std::filesystem::temp_directory_path() /
      ("firmius-" + std::to_string(static_cast<long long>(getuid())));
  if (ensureWritableDirectory(tempHome)) {
    return tempHome;
  }

  return std::filesystem::temp_directory_path() / "firmius";
}

std::string ensureTrailingSlash(std::string dir) {
  if (!dir.empty() && dir.back() != '/') {
    dir += '/';
  }
  return dir;
}

} // namespace


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
  if (!std::filesystem::exists(builtinWorkflowsDir))
    return;

  const std::filesystem::path userDir = resolveWritableFirmiusHome() / "workflows";
  if (isUsableWorkflowDir(userDir)) {
    return;
  }

  try {
    std::filesystem::create_directories(userDir);
  } catch (const std::filesystem::filesystem_error &) {
    return;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(builtinWorkflowsDir)) {
    if (entry.is_regular_file()) {
      try {
        std::filesystem::copy_file(
            entry.path(), userDir / entry.path().filename(),
            std::filesystem::copy_options::overwrite_existing);
      } catch (const std::filesystem::filesystem_error &) {
      }
    }
  }
}

std::optional<Workflow> WorkflowLoader::loadWorkflow(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Warning: Could not open workflow file: " << path << std::endl;
    return std::nullopt;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  firmius::shared::FrontmatterDocument doc;
  try {
    doc = firmius::shared::FrontmatterParser::parseMarkdown(content, path);
  } catch (const std::exception &e) {
    std::cerr << "Error: Failed to parse workflow frontmatter in " << path << ": " << e.what() << std::endl;
    return std::nullopt;
  }

  Workflow workflow;
  std::filesystem::path fsPath(path);
  workflow.id = fsPath.stem().string();
  workflow.name = firmius::shared::FrontmatterParser::getString(doc, "name").value_or("");
  workflow.description = firmius::shared::FrontmatterParser::getString(doc, "description").value_or("");
  workflow.body = firmius::shared::StringUtil::trim(doc.body);

  if (auto argsArray = firmius::shared::FrontmatterParser::getArray(doc, "args")) {
    for (const auto &item : *argsArray) {
      if (const auto *argMap = std::get_if<firmius::shared::FrontmatterValue::Map>(&item.value)) {
        WorkflowArg arg;
        arg.optional = false;
        
        auto it = argMap->find("name");
        if (it != argMap->end()) {
          if (const auto *s = std::get_if<std::string>(&it->second.value)) {
            arg.name = *s;
          }
        }
        
        it = argMap->find("type");
        if (it != argMap->end()) {
          if (const auto *s = std::get_if<std::string>(&it->second.value)) {
            std::string typeStr = *s;
            if (typeStr == "string") arg.type = WorkflowArgType::String;
            else if (typeStr == "number") arg.type = WorkflowArgType::Number;
            else if (typeStr == "filepath") arg.type = WorkflowArgType::Filepath;
            else if (typeStr == "agent_id") arg.type = WorkflowArgType::AgentId;
            else if (typeStr == "thread_id") arg.type = WorkflowArgType::ThreadId;
            else arg.type = WorkflowArgType::String;
          }
        }
        
        it = argMap->find("description");
        if (it != argMap->end()) {
          if (const auto *s = std::get_if<std::string>(&it->second.value)) {
            arg.description = *s;
          }
        }
        
        it = argMap->find("optional");
        if (it != argMap->end()) {
          if (const auto *b = std::get_if<bool>(&it->second.value)) {
            arg.optional = *b;
          } else if (const auto *s = std::get_if<std::string>(&it->second.value)) {
            arg.optional = (*s == "true" || *s == "yes" || *s == "1");
          } else if (const auto *i = std::get_if<int64_t>(&it->second.value)) {
            arg.optional = (*i != 0);
          }
        }
        if (!arg.name.empty()) {
          workflow.args.push_back(arg);
        }
      }
    }
  }

  // Set defaults if not provided
  if (workflow.name.empty()) {
    std::string name = workflow.id;
    std::replace(name.begin(), name.end(), '_', ' ');
    bool newWord = true;
    for (auto &c : name) {
      if (c == ' ') newWord = true;
      else if (newWord) { c = std::toupper(c); newWord = false; }
      else c = std::tolower(c);
    }
    workflow.name = name;
  }

  if (workflow.description.empty()) {
    workflow.description = "Execute workflow: " + workflow.name;
  }

  // Count argument placeholders ($1, $2, etc.) for legacy support
  std::regex argPattern(R"(\$([0-9]+))");
  auto begin = std::sregex_iterator(workflow.body.begin(), workflow.body.end(), argPattern);
  auto end = std::sregex_iterator();
  size_t maxArg = 0;
  for (auto it = begin; it != end; ++it) {
    std::smatch match = *it;
    maxArg = std::max(maxArg, (size_t)std::stoul(match[1].str()));
  }
  workflow.argCount = maxArg;

  if (workflow.args.empty() && workflow.argCount > 0) {
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
  if (envDir) {
    const std::filesystem::path dir(envDir);
    if (isUsableWorkflowDir(dir)) {
      return ensureTrailingSlash(dir.string());
    }
  }

  const std::filesystem::path userDir = resolveWritableFirmiusHome() / "workflows";
  if (isUsableWorkflowDir(userDir)) {
    return ensureTrailingSlash(userDir.string());
  }

  return ensureTrailingSlash("workflows");
}

} // namespace firmius::core
