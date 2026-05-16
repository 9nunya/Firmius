#include "utils/ToolSummaries.hpp"
#include <rapidjson/document.h>
#include <regex>
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

static std::string stringArg(const rapidjson::Document &doc, const char *key) {
  if (doc.IsObject() && doc.HasMember(key) && doc[key].IsString()) {
    return doc[key].GetString();
  }
  return "";
}

static std::string partialStringArg(const std::string &args, const char *key) {
  if (args.empty() || !key || !*key) {
    return "";
  }
  try {
    const std::string pattern =
        std::string("\"") + key + "\"\\s*:\\s*\"([^\"]*)";
    std::regex re(pattern);
    std::smatch match;
    if (std::regex_search(args, match, re) && match.size() >= 2) {
      return match[1].str();
    }
  } catch (...) {
  }
  return "";
}

static std::string quotedLabel(const std::string &value, int words = 4) {
  if (value.empty()) {
    return "";
  }
  return " \"" + firstWords(value, words) + "\"";
}

static int countNonEmptyLines(const std::string &text) {
  std::istringstream ss(text);
  std::string line;
  int count = 0;
  while (std::getline(ss, line)) {
    if (!line.empty()) {
      count++;
    }
  }
  return count;
}

static std::string firstNonEmptyLine(const std::string &text) {
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty()) {
      return line;
    }
  }
  return "";
}

std::string SummarizeToolCall(const std::string &name, const std::string &args, ToolPhase phase) {
  if (phase == ToolPhase::Preparing) {
    if (name == "Edit") return "Preparing edit...";
    if (name == "Files") return "Preparing filesystem query...";
    if (name == "Process")
      return "Preparing process operation...";
    if (name == "Delegate")
      return "Preparing delegation...";
    if (name == "Todo" || name == "Fleet")
      return "Preparing work update...";
    if (name == "Artifacts")
      return "Preparing artifact operation...";
    if (name == "Lsp")
      return "Preparing language-server query...";
    if (name == "Web")
      return "Preparing web operation...";
    if (name == "Skill")
      return "Preparing skill load...";
    return "Preparing " + name + "...";
  }

  rapidjson::Document doc;
  doc.Parse(args.c_str());
  bool valid = !doc.HasParseError() && doc.IsObject();

  auto bestStringArg = [&](const char *key) -> std::string {
    std::string value = valid ? stringArg(doc, key) : "";
    if (value.empty()) {
      value = partialStringArg(args, key);
    }
    return value;
  };

  if (name == "Todo") {
    return "Update todo list";
  }
  if (name == "Artifacts") {
    const std::string action = bestStringArg("action");
    if (action == "Read") {
      std::string reference = bestStringArg("reference");
      if (reference.empty()) {
        reference = bestStringArg("name");
      }
      return reference.empty() ? "Read artifact" : "Read " + reference;
    }
    if (action == "List") {
      return "List artifacts";
    }
    return "Write artifact";
  }

  if (name == "Files") {
    const std::string action = bestStringArg("action");
    if (action == "Read") {
      std::string path = bestStringArg("path");
      int start = -1, end = -1;
      if (valid) {
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
    if (action == "Grep") {
      std::string pattern = bestStringArg("pattern");
      return "Search \"" + pattern + "\"";
    }
    if (action == "Glob") {
      std::string pattern = bestStringArg("pattern");
      if (pattern.empty()) pattern = bestStringArg("glob");
      return pattern.empty() ? "Find files" : "Find \"" + pattern + "\"";
    }
    std::string path = bestStringArg("path");
    if (path.empty()) path = ".";
    return "List " + path;
  }
  if (name == "Edit") {
    std::string path = bestStringArg("path");
    if (path.empty()) {
      path = bestStringArg("filename");
    }

    // Prefer to differentiate edit lanes when payload is valid JSON.
    if (valid) {
      if (doc.HasMember("edits") && doc["edits"].IsArray()) {
        return "Edit " + path + " (" + std::to_string(doc["edits"].Size()) + " ops)";
      }
      if (doc.HasMember("content") && doc["content"].IsString()) {
        return "Overwrite " + path;
      }
      if (doc.HasMember("patch") && doc["patch"].IsString()) {
        return "Apply patch";
      }
      if (doc.HasMember("files") && doc["files"].IsArray()) {
        return "Apply patch";
      }
    }

    return "Apply patch";
  }
  if (name == "EditWrite") {
    std::string path = bestStringArg("path");
    return "Write " + path;
  }
  if (name == "EditReplace") {
    std::string path = bestStringArg("path");
    size_t replacementCount = 0;
    if (valid && doc.HasMember("replacements") && doc["replacements"].IsArray()) {
      replacementCount = doc["replacements"].Size();
    }
    return replacementCount > 0
               ? "Replace in " + path + " (" + std::to_string(replacementCount) + " ops)"
               : "Replace in " + path;
  }
  if (name == "EditRange") {
    std::string path = bestStringArg("path");
    size_t editCount = 0;
    if (valid && doc.HasMember("operations") && doc["operations"].IsArray())
      editCount = doc["operations"].Size();
    if (editCount > 0)
      return "Range edit " + path + " (" + std::to_string(editCount) + " ops)";
    return "Range edit " + path;
  }
  if (name == "Process") {
    const std::string action = bestStringArg("action");
    if (action == "Status") return "Inspect process";
    if (action == "Input") return "Send process input";
    if (action == "Wait") return "Wait for process";
    std::string cmd = bestStringArg("command");
    return "$ " + cmd;
  }
  if (name == "Delegate") {
    const std::string action = bestStringArg("action");
    if (action == "Wait") {
      std::string title = bestStringArg("title");
      if (title.empty()) title = bestStringArg("name");
      return !title.empty() ? "Wait for " + title : "Await subagent";
    }
    if (action == "Stop") {
      return "Stop subagent";
    }
    std::string title = bestStringArg("title");
    if (title.empty()) title = bestStringArg("name");
    return title.empty() ? "Delegate subagent" : "Delegate \"" + firstWords(title, 3) + "\"";
  }
  if (name == "Memory") {
    return "Recall memory";
  }
  if (name == "Lsp") {
    std::string op = bestStringArg("operation");
    return op.empty() ? "LSP query" : "LSP " + op;
  }
  if (name == "Web") {
    const std::string action = bestStringArg("action");
    if (action == "Search") {
      std::string query = bestStringArg("query");
      return query.empty() ? "Web search"
                           : "Search the web for \"" + firstWords(query, 6) + "\"";
    }
    std::string url = bestStringArg("url");
    auto pos = url.find("://");
    if (pos != std::string::npos) url = url.substr(pos + 3);
    auto slash = url.find('/');
    if (slash != std::string::npos) url = url.substr(0, slash);
    return "Fetch " + url;
  }
  if (name == "Skill") {
    std::string what = bestStringArg("what");
    return what.empty() ? "Load skill" : "Load skill \"" + what + "\"";
  }
  if (name == "Python" || isMatch(name, "python_execute")) {
    std::string code = valid ? stringArg(doc, "code") : "";
    std::string firstLine = firstNonEmptyLine(code);
    if (!firstLine.empty()) {
      return "Python" + quotedLabel(firstLine, 5);
    }
    int lineCount = countNonEmptyLines(code);
    if (lineCount > 0) {
      return "Python " + std::to_string(lineCount) + " lines";
    }
    return "Python exec";
  }

  return name;
}

} // namespace firmius::shared
