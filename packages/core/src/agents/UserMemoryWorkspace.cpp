#include "agents/UserMemoryWorkspace.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

namespace firmius::core {

namespace {

bool ensureWritableDirectory(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec || !std::filesystem::exists(dir) ||
      !std::filesystem::is_directory(dir)) {
    return false;
  }

  const auto probe = dir / ".write_probe";
  std::ofstream out(probe);
  if (!out.is_open()) {
    return false;
  }
  out << "ok";
  out.close();
  std::filesystem::remove(probe, ec);
  return true;
}

std::string firmiusHome() {
  if (const char *home = std::getenv("HOME")) {
    const std::filesystem::path userHome =
        std::filesystem::path(home) / ".firmius";
    if (ensureWritableDirectory(userHome)) {
      return userHome.string();
    }
  }
  const std::filesystem::path localHome =
      std::filesystem::current_path() / ".firmius";
  if (ensureWritableDirectory(localHome)) {
    return localHome.string();
  }
  const std::filesystem::path tempHome =
      std::filesystem::temp_directory_path() / "firmius";
  ensureWritableDirectory(tempHome);
  return tempHome.string();
}

std::string sanitizePathFragment(std::string value) {
  for (char &ch : value) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' ||
          ch == '_')) {
      ch = '-';
    }
  }
  while (!value.empty() && value.front() == '-') {
    value.erase(value.begin());
  }
  while (!value.empty() && value.back() == '-') {
    value.pop_back();
  }
  return value.empty() ? "workspace" : value;
}

std::string workspaceIdForCwd(const std::string &cwd) {
  std::filesystem::path path(cwd.empty() ? "." : cwd);
  const std::string basename =
      sanitizePathFragment(path.filename().string().empty() ? "workspace"
                                                            : path.filename().string());
  const std::size_t hashValue = std::hash<std::string>{}(path.lexically_normal().string());
  std::ostringstream out;
  out << basename << "-" << std::hex << (hashValue & 0xffffffffULL);
  return out.str();
}

void ensureFileWithDefault(const std::filesystem::path &path,
                           const std::string &content) {
  if (std::filesystem::exists(path)) {
    return;
  }
  std::ofstream out(path);
  out << content;
}

std::string readTrimmed(const std::filesystem::path &path,
                        std::size_t maxBytes = 1800) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string content = buffer.str();
  if (content.size() > maxBytes) {
    content.resize(maxBytes);
    content += "\n... [trimmed]";
  }
  return content;
}

} // namespace

UserMemoryWorkspace ensureUserMemoryWorkspace(const std::string &cwd) {
  UserMemoryWorkspace workspace;
  workspace.rootDir = (std::filesystem::path(firmiusHome()) / "user").string();
  workspace.workspaceId = workspaceIdForCwd(cwd);
  workspace.userFile =
      (std::filesystem::path(workspace.rootDir) / "USER.md").string();
  workspace.behaviorFile =
      (std::filesystem::path(workspace.rootDir) / "BEHAVIOR.md").string();
  workspace.projectDir =
      (std::filesystem::path(workspace.rootDir) / "projects" /
       workspace.workspaceId)
          .string();
  workspace.fixesDir =
      (std::filesystem::path(workspace.projectDir) / "fixes").string();

  std::filesystem::create_directories(workspace.fixesDir);

  ensureFileWithDefault(
      workspace.userFile,
      "# USER\n\n- Durable user facts, preferences, and personal invariants.\n");
  ensureFileWithDefault(
      workspace.behaviorFile,
      "# BEHAVIOR\n\n- Learned workflow, testing, review, and interaction preferences.\n");

  const auto projectReadme =
      std::filesystem::path(workspace.projectDir) / "README.md";
  ensureFileWithDefault(
      projectReadme,
      "# Project Dream Log\n\n- Workspace-specific fix notes and durable project learnings.\n");

  return workspace;
}

std::string buildUserMemoryOverlay(const std::string &cwd) {
  const auto workspace = ensureUserMemoryWorkspace(cwd);
  const std::string user = readTrimmed(workspace.userFile);
  const std::string behavior = readTrimmed(workspace.behaviorFile);

  std::vector<std::filesystem::directory_entry> fixEntries;
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(workspace.fixesDir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    fixEntries.push_back(entry);
  }
  std::sort(fixEntries.begin(), fixEntries.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.path().filename().string() >
                     rhs.path().filename().string();
            });

  std::ostringstream out;
  out << "## USER MEMORY\n";
  out << "Workspace: " << workspace.workspaceId << "\n";
  out << "User file: " << workspace.userFile << "\n";
  out << "Behavior file: " << workspace.behaviorFile << "\n\n";
  if (!user.empty()) {
    out << user << "\n\n";
  }
  if (!behavior.empty()) {
    out << behavior << "\n\n";
  }
  out << "## PROJECT FIX LOGS\n";
  if (fixEntries.empty()) {
    out << "- No fix notes yet.\n";
  } else {
    const std::size_t limit = std::min<std::size_t>(3, fixEntries.size());
    for (std::size_t i = 0; i < limit; ++i) {
      out << "- " << fixEntries[i].path().filename().string() << "\n";
    }
  }
  return out.str();
}

} // namespace firmius::core
