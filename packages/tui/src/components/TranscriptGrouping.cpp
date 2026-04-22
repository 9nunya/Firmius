#include "components/TranscriptGrouping.hpp"

#include <filesystem>
#include <rapidjson/document.h>
#include <sstream>
#include <unordered_set>

namespace firmius::tui {

namespace {

std::string shorten(const std::string &text, size_t limit = 512) {
  if (text.size() <= limit) {
    return text;
  }
  if (limit < 4) {
    return text.substr(0, limit);
  }
  return text.substr(0, limit - 1) + "…";
}

std::string relativePath(const std::string &path) {
  if (path.empty()) {
    return path;
  }

  try {
    std::filesystem::path fs_path(path);
    if (fs_path.is_absolute()) {
      for (auto base = std::filesystem::current_path(); !base.empty();
           base = base.parent_path()) {
        auto rel = fs_path.lexically_relative(base);
        auto rel_string = rel.generic_string();
        if (!rel_string.empty() && rel_string.rfind("..", 0) != 0) {
          return rel_string;
        }
        if (base == base.root_path()) {
          break;
        }
      }
    }
    return fs_path.generic_string();
  } catch (...) {
    return path;
  }
}

std::string stringArg(const rapidjson::Document &doc, const char *key) {
  if (doc.IsObject() && doc.HasMember(key) && doc[key].IsString()) {
    return doc[key].GetString();
  }
  return "";
}

int intArg(const rapidjson::Document &doc, const char *key, int fallback = -1) {
  if (doc.IsObject() && doc.HasMember(key) && doc[key].IsInt()) {
    return doc[key].GetInt();
  }
  return fallback;
}

std::string lineRangeSuffix(int start_line, int end_line) {
  if (start_line < 0 || end_line < 0) {
    return "";
  }
  return ":" + std::to_string(start_line) + "-" + std::to_string(end_line);
}

std::string joinTargets(const std::vector<std::string> &targets) {
  std::ostringstream out;
  for (size_t i = 0; i < targets.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << targets[i];
  }
  return out.str();
}

} // namespace

QuickToolCategory QuickToolCategoryForName(const std::string &name) {
  if (name == "Files") {
    return QuickToolCategory::List;
  }
  if (name == "Search") {
    return QuickToolCategory::Search;
  }
  return QuickToolCategory::None;
}

bool IsQuickToolCategory(QuickToolCategory category) {
  return category != QuickToolCategory::None;
}

bool IsQuickInspectionTool(const std::string &name) {
  return IsQuickToolCategory(QuickToolCategoryForName(name));
}

QuickToolDescriptor DescribeQuickToolCall(const shared::ToolCallView &view) {
  QuickToolDescriptor descriptor;
  if (!shared::ToolCallHasRenderableIdentity(view)) {
    return descriptor;
  }
  rapidjson::Document doc;
  doc.Parse(view.args.c_str());
  const std::string action = stringArg(doc, "action");

  descriptor.category = QuickToolCategoryForName(view.name);
  if (view.name == "Files" && action == "Read") {
    descriptor.category = QuickToolCategory::Read;
  } else if (view.name == "Files" &&
             (action == "Grep" || action == "Glob")) {
    descriptor.category = QuickToolCategory::Search;
  }
  if (!IsQuickToolCategory(descriptor.category)) {
    return descriptor;
  }

  if (descriptor.category == QuickToolCategory::Read) {
    auto path = relativePath(stringArg(doc, "path"));
    auto start_line = intArg(doc, "start_line");
    auto end_line = intArg(doc, "end_line");
    descriptor.target = shorten(path + lineRangeSuffix(start_line, end_line));
    return descriptor;
  }

  if (descriptor.category == QuickToolCategory::List) {
    auto path = relativePath(stringArg(doc, "path"));
    descriptor.target = shorten(path.empty() ? "." : path);
    return descriptor;
  }

  if (descriptor.category == QuickToolCategory::Search) {
    auto pattern = stringArg(doc, "pattern");
    if (pattern.empty()) {
      pattern = stringArg(doc, "query");
    }
    auto path = relativePath(stringArg(doc, "path"));
    auto target = "\"" + shorten(pattern.empty() ? "…" : pattern, 512) + "\"";
    if (!path.empty()) {
      target += " in " + shorten(path);
    }
    descriptor.target = target;
    return descriptor;
  }

  return descriptor;
}

std::string QuickToolGroupLabel(const QuickToolGroupSummary &summary) {
  std::string prefix;
  switch (summary.category) {
  case QuickToolCategory::Read:
    prefix = summary.has_error ? "Failed Reading " : "Read ";
    break;
  case QuickToolCategory::List:
    prefix = summary.has_error ? "Failed Listing " : "Listed ";
    break;
  case QuickToolCategory::Search:
    prefix = summary.has_error ? "Failed Search " : "Search ";
    break;
  case QuickToolCategory::None:
    return "";
  }

  auto deduped = DedupeQuickToolTargets(summary.targets);

  return prefix + joinTargets(deduped);
}

std::vector<std::string>
DedupeQuickToolTargets(const std::vector<std::string> &targets) {
  std::vector<std::string> deduped;
  std::unordered_set<std::string> seen;
  deduped.reserve(targets.size());
  for (const auto &target : targets) {
    if (target.empty() || seen.count(target) > 0) {
      continue;
    }
    seen.insert(target);
    deduped.push_back(target);
  }
  return deduped;
}

} // namespace firmius::tui
