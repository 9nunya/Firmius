#include "utils/ToolSummaries.hpp"
#include <rapidjson/document.h>
#include <sstream>

namespace firmius::shared {

std::vector<std::string> TailLines(const std::string &text, int maxLines) {
  std::vector<std::string> all;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    all.push_back(line);
  }
  if (static_cast<int>(all.size()) <= maxLines) {
    return all;
  }
  return std::vector<std::string>(all.end() - maxLines, all.end());
}

std::string baseName(const std::string &path) {
  auto pos = path.find_last_of('/');
  if (pos != std::string::npos && pos + 1 < path.size()) {
    return path.substr(pos + 1);
  }
  return path;
}

std::string firstWords(const std::string &s, int n) {
  std::istringstream ss(s);
  std::string word;
  std::string result;
  int count = 0;
  while (ss >> word && count < n) {
    if (!result.empty())
      result += " ";
    result += word;
    count++;
  }
  if (ss >> word) {
    result += "...";
  }
  return result;
}

static bool isMatch(const std::string &actual, const std::string &expected) {
  if (actual.empty() || expected.empty()) return false;
  return actual.find(expected) != std::string::npos;
}

std::string SummarizeToolCall(const std::string &name, const std::string &args, ToolPhase phase) {
  if (phase == ToolPhase::Preparing) {
    if (isMatch(name, "file_edit")) return "Preparing edit...";
    if (isMatch(name, "file_read")) return "Preparing read...";
    if (isMatch(name, "process_execute") || isMatch(name, "process_spawn")) return "Writing command...";
    if (isMatch(name, "summon_subagent")) return "Summoning subagent...";
    if (isMatch(name, "subagent_wait")) return "Awaiting subagent...";
    if (isMatch(name, "grep")) return "Preparing grep...";
    if (isMatch(name, "glob")) return "Preparing glob...";
    return "Preparing " + name + "...";
  }

  rapidjson::Document doc;
  doc.Parse(args.c_str());
  bool valid = !doc.HasParseError() && doc.IsObject();

  if (isMatch(name, "list_directory")) {
    std::string path = "";
    if (valid && doc.HasMember("path") && doc["path"].IsString())
      path = doc["path"].GetString();
    return "List " + (path.empty() ? "." : path);
  }
  if (isMatch(name, "file_read")) {
    std::string path = "";
    int start = -1, end = -1;
    if (valid) {
      if (doc.HasMember("path") && doc["path"].IsString())
        path = doc["path"].GetString();
      if (doc.HasMember("start_line") && doc["start_line"].IsInt())
        start = doc["start_line"].GetInt();
      if (doc.HasMember("end_line") && doc["end_line"].IsInt())
        end = doc["end_line"].GetInt();
    }
    std::string s = "Read " + path;
    if (start >= 0 && end >= 0)
      s += "[" + std::to_string(start) + ":" + std::to_string(end) + "]";
    return s;
  }
  if (isMatch(name, "file_edit")) {
    std::string path = "";
    if (valid && doc.HasMember("path") && doc["path"].IsString())
      path = doc["path"].GetString();
    return "Edit " + path;
  }
  if (isMatch(name, "process_execute") || isMatch(name, "process_spawn")) {
    std::string cmd = "";
    if (valid && doc.HasMember("command") && doc["command"].IsString())
      cmd = doc["command"].GetString();
    return "$ " + firstWords(cmd, 3);
  }
  if (isMatch(name, "grep")) {
    std::string pattern = "";
    if (valid && doc.HasMember("pattern") && doc["pattern"].IsString())
      pattern = doc["pattern"].GetString();
    return "Grep \"" + firstWords(pattern, 2) + "\"";
  }
  if (isMatch(name, "glob")) {
    std::string pattern = "";
    if (valid && doc.HasMember("pattern") && doc["pattern"].IsString())
      pattern = doc["pattern"].GetString();
    return "Glob \"" + pattern + "\"";
  }
  if (isMatch(name, "python_execute")) {
    return "Python exec";
  }
  if (isMatch(name, "web_fetch")) {
    std::string url = "";
    if (valid && doc.HasMember("url") && doc["url"].IsString())
      url = doc["url"].GetString();
    auto pos = url.find("://");
    if (pos != std::string::npos) {
      url = url.substr(pos + 3);
    }
    auto slash = url.find('/');
    if (slash != std::string::npos) {
      url = url.substr(0, slash);
    }
    return "Fetch " + url;
  }
  if (isMatch(name, "summon_subagent")) {
    std::string title = "";
    if (valid && doc.HasMember("title") && doc["title"].IsString())
      title = doc["title"].GetString();
    if (title.empty() && valid && doc.HasMember("name") &&
        doc["name"].IsString())
      title = doc["name"].GetString();
    return "Subagent \"" + firstWords(title, 3) + "\"";
  }
  if (isMatch(name, "subagent_wait")) {
    std::string title = "";
    if (valid && doc.HasMember("title") && doc["title"].IsString())
      title = doc["title"].GetString();
    if (title.empty() && valid && doc.HasMember("name") && doc["name"].IsString())
      title = doc["name"].GetString();
    
    if (!title.empty()) return "Wait for " + title;
    return "Await subagent";
  }
  if (isMatch(name, "subagent_terminate")) {
    return "Kill subagent";
  }
  if (isMatch(name, "process_status")) {
    return "Process status";
  }
  if (isMatch(name, "process_input")) {
    return "Process input";
  }
  if (isMatch(name, "process_wait")) {
    return "Process wait";
  }

  return name;
}

} // namespace firmius::shared
