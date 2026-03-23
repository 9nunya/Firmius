#include "tools/FileToolPresentation.hpp"

#include "utils/ErrorCleaner.hpp"
#include "utils/StringUtil.hpp"

#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

namespace {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

bool IsMatch(const std::string &actual, const std::string &expected) {
  return !actual.empty() && !expected.empty() &&
         actual.find(expected) != std::string::npos;
}

ToolPresentationLifecycle LifecycleFromPhase(const ToolCallView &view) {
  if (view.phase == ToolPhase::Preparing) {
    return ToolPresentationLifecycle::Preparing;
  }
  if (view.phase == ToolPhase::Called || view.phase == ToolPhase::BackgroundRunning) {
    return ToolPresentationLifecycle::Running;
  }
  if (view.phase == ToolPhase::Error ||
      (view.phase == ToolPhase::Finished && !view.success)) {
    return ToolPresentationLifecycle::Error;
  }
  return ToolPresentationLifecycle::Success;
}

bool ParseObject(const std::string &json, rapidjson::Document &doc) {
  doc.Parse(json.c_str());
  return !doc.HasParseError() && doc.IsObject();
}

bool ParseArray(const std::string &json, rapidjson::Document &doc) {
  doc.Parse(json.c_str());
  return !doc.HasParseError() && doc.IsArray();
}

std::string StringMember(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsString()) {
    return value[key].GetString();
  }
  return "";
}

int IntMember(const rapidjson::Value &value, const char *key, int fallback = 0) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsInt()) {
    return value[key].GetInt();
  }
  return fallback;
}

void ApplyError(ToolPresentation &presentation, const ToolCallView &view,
                const std::string &title) {
  presentation.lifecycle = ToolPresentationLifecycle::Error;
  presentation.title = title;
  presentation.subtitle = view.name;
  presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
}

std::vector<std::string> SplitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::string BaseName(const std::string &path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos || pos + 1 >= path.size()) {
    return path;
  }
  return path.substr(pos + 1);
}

ToolPresentation BuildFileReadPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string path = has_args ? StringMember(args_doc, "path") : "";
  const int start_line = has_args ? IntMember(args_doc, "start_line", -1) : -1;
  const int end_line = has_args ? IntMember(args_doc, "end_line", -1) : -1;

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare file read";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "reading file";
  } else {
    presentation.title = "file read";
  }
  if (!path.empty()) {
    presentation.footer_badges.push_back(path);
    if (presentation.lifecycle != ToolPresentationLifecycle::Preparing &&
        presentation.lifecycle != ToolPresentationLifecycle::Running) {
      presentation.compact_summary = BaseName(path);
    }
  }
  if (start_line >= 0 && end_line >= 0) {
    presentation.facts.push_back(
        {"Range", std::to_string(start_line) + "-" + std::to_string(end_line)});
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "file read failed");
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  std::string content = view.result;
  int line_start = start_line;
  int line_end = end_line;
  int lines_read = 0;
  if (ParseObject(view.result, result_doc)) {
    const std::string parsed_content = StringMember(result_doc, "content");
    if (!parsed_content.empty()) {
      content = parsed_content;
    }
    line_start = IntMember(result_doc, "line_start", line_start);
    line_end = IntMember(result_doc, "line_end", line_end);
    lines_read = IntMember(result_doc, "lines_read", 0);
  }

  if (line_start >= 0 && line_end >= line_start) {
    presentation.footer_badges.push_back("lines " + std::to_string(line_start) + "-" +
                                         std::to_string(line_end));
  }
  if (lines_read > 0) {
    presentation.footer_badges.push_back(std::to_string(lines_read) + " lines");
  }

  auto lines = SplitLines(content);
  if (!lines.empty()) {
    constexpr size_t kPreviewLines = 12;
    ToolPresentationSection preview;
    preview.title = "Preview";
    const size_t shown = std::min(kPreviewLines, lines.size());
    preview.lines.assign(lines.begin(), lines.begin() + static_cast<long>(shown));
    presentation.body_lines = preview.lines;
    presentation.sections.push_back(std::move(preview));
    if (lines.size() > shown) {
      presentation.expandable = true;
      presentation.expanded = view.show_result;
      presentation.notices.push_back(
          {ToolPresentationNoticeKind::Info,
           "Showing first " + std::to_string(shown) + " of " +
               std::to_string(lines.size()) + " lines"});
      if (presentation.expanded) {
        ToolPresentationSection full;
        full.title = "Content";
        full.lines = std::move(lines);
        presentation.sections.push_back(std::move(full));
      }
    }
  }

  return presentation;
}

ToolPresentation BuildListDirectoryPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::ResultsList;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string path = has_args ? StringMember(args_doc, "path") : "";

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare directory listing";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "listing directory";
  } else {
    presentation.title = "directory listing";
  }
  if (!path.empty()) {
    presentation.footer_badges.push_back(path);
    presentation.compact_summary = BaseName(path);
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "directory listing failed");
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  if (!ParseArray(view.result, result_doc)) {
    return presentation;
  }
  presentation.facts.push_back({"Entries", std::to_string(result_doc.Size())});
  presentation.footer_badges.push_back(std::to_string(result_doc.Size()) + " entries");
  ToolPresentationSection section;
  section.title = "Entries";
  const rapidjson::SizeType max_rows =
      view.show_result ? result_doc.Size() : static_cast<rapidjson::SizeType>(10);
  for (rapidjson::SizeType i = 0; i < result_doc.Size() && i < max_rows; ++i) {
    const auto &entry = result_doc[i];
    std::string name = StringMember(entry, "name");
    if (name.empty()) {
      continue;
    }
    const bool is_dir =
        entry.IsObject() && entry.HasMember("is_directory") && entry["is_directory"].IsBool() &&
        entry["is_directory"].GetBool();
    section.lines.push_back(std::string(is_dir ? "[dir] " : "[file] ") + name);
    presentation.body_lines.push_back(std::string(is_dir ? "[dir] " : "[file] ") + name);
  }
  if (!section.lines.empty()) {
    presentation.sections.push_back(std::move(section));
  }
  if (result_doc.Size() > max_rows) {
    presentation.expandable = true;
    presentation.expanded = view.show_result;
    presentation.notices.push_back(
        {ToolPresentationNoticeKind::Info, "Show to open full listing"});
  }

  return presentation;
}

ToolPresentation BuildFileEditPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::ResultsList;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string path = has_args ? StringMember(args_doc, "path") : "";
  const bool has_content_overwrite =
      has_args && args_doc.HasMember("content") && args_doc["content"].IsString();

  int edit_count = 0;
  if (has_args && args_doc.HasMember("edits") && args_doc["edits"].IsArray()) {
    edit_count = static_cast<int>(args_doc["edits"].Size());
  } else if (has_content_overwrite) {
    edit_count = 1;
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare file edit";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "editing file";
  } else {
    presentation.title = "file edit";
  }
  if (!path.empty()) {
    presentation.footer_badges.push_back(path);
    presentation.compact_summary = BaseName(path);
  }
  if (edit_count > 0) {
    presentation.footer_badges.push_back(std::to_string(edit_count) + " requested edit(s)");
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "file edit failed");
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  ParseObject(view.result, result_doc);
  int operation_count = 0;
  int added_lines = 0;
  int removed_lines = 0;
  bool overwrite_edit = false;
  ToolPresentationSection changes;
  changes.title = "Changes";
  auto append_overwrite_preview = [&](const std::string &content) {
    auto new_lines = SplitLines(content);
    if (new_lines.empty()) {
      changes.lines.push_back("(empty file)");
      return;
    }
    constexpr size_t kPreviewLines = 8;
    changes.lines.push_back("new content:");
    const size_t shown = std::min(kPreviewLines, new_lines.size());
    for (size_t i = 0; i < shown; ++i) {
      changes.lines.push_back("  " + new_lines[i]);
    }
    if (new_lines.size() > shown) {
      changes.lines.push_back("  ... +" +
                              std::to_string(new_lines.size() - shown) +
                              " more line(s)");
    }
  };

  if (result_doc.IsObject() && result_doc.HasMember("operations") &&
      result_doc["operations"].IsArray()) {
    const auto &ops = result_doc["operations"].GetArray();
    operation_count = static_cast<int>(ops.Size());
    for (const auto &op : ops) {
      if (!op.IsObject()) {
        continue;
      }
      std::string op_name = StringMember(op, "op");
      std::string desc = StringMember(op, "description");
      if (desc.empty()) {
        desc = op_name;
      }
      if (!desc.empty()) {
        changes.lines.push_back(desc);
      }
      if (!op_name.empty() &&
          op_name.find("overwrite") != std::string::npos) {
        overwrite_edit = true;
      }
      if (op.HasMember("new_lines") && op["new_lines"].IsArray()) {
        added_lines += static_cast<int>(op["new_lines"].Size());
      }
      if (op.HasMember("old_lines") && op["old_lines"].IsArray()) {
        removed_lines += static_cast<int>(op["old_lines"].Size());
      }
    }
    if (overwrite_edit && has_content_overwrite && args_doc["content"].IsString()) {
      append_overwrite_preview(args_doc["content"].GetString());
    }
  } else if (has_content_overwrite && args_doc["content"].IsString()) {
    auto new_lines = SplitLines(args_doc["content"].GetString());
    operation_count = 1;
    added_lines = static_cast<int>(new_lines.size());
    changes.lines.push_back("overwrite file content");
    append_overwrite_preview(args_doc["content"].GetString());
  }

  if (operation_count > 0) {
    presentation.footer_badges.push_back(std::to_string(operation_count) + " operation(s)");
    presentation.facts.push_back({"Operations", std::to_string(operation_count)});
  }
  if (added_lines > 0) {
    presentation.footer_badges.push_back("+" + std::to_string(added_lines));
    presentation.facts.push_back({"Added lines", std::to_string(added_lines)});
  }
  if (removed_lines > 0) {
    presentation.footer_badges.push_back("-" + std::to_string(removed_lines));
    presentation.facts.push_back({"Removed lines", std::to_string(removed_lines)});
  }
  if (!changes.lines.empty()) {
    presentation.body_lines = changes.lines;
    const size_t max_rows = view.show_result ? changes.lines.size() : 12;
    if (changes.lines.size() > max_rows) {
      changes.lines.resize(max_rows);
      presentation.expandable = true;
      presentation.expanded = view.show_result;
      presentation.notices.push_back(
          {ToolPresentationNoticeKind::Info, "Show to open full change list"});
    }
    presentation.sections.push_back(std::move(changes));
  }

  return presentation;
}

ToolPresentation BuildWebFetchPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::BodyFirstPreview;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string url = has_args ? StringMember(args_doc, "url") : "";

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare URL fetch";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "fetching URL";
  } else {
    presentation.title = "web fetch";
  }
  if (!url.empty()) {
    presentation.footer_badges.push_back(url);
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "web fetch failed");
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  if (!ParseObject(view.result, result_doc)) {
    return presentation;
  }
  if (result_doc.HasMember("size") && result_doc["size"].IsUint64()) {
    presentation.footer_badges.push_back(
        std::to_string(result_doc["size"].GetUint64()) + " bytes");
    presentation.facts.push_back(
        {"Size", std::to_string(result_doc["size"].GetUint64()) + " bytes"});
  }
  const std::string redirected = StringMember(result_doc, "redirected_to");
  if (!redirected.empty()) {
    presentation.notices.push_back(
        {ToolPresentationNoticeKind::Info, "Large response saved to " + redirected});
  }
  const std::string instruction = StringMember(result_doc, "instruction");
  if (!instruction.empty()) {
    presentation.notices.push_back(
        {ToolPresentationNoticeKind::Info, "Follow-up: " + instruction});
    ToolPresentationSection followup;
    followup.title = "Follow-up";
    followup.lines.push_back(instruction);
    presentation.sections.push_back(std::move(followup));
    presentation.expandable = true;
    presentation.expanded = view.show_result;
  }
  const std::string content = StringMember(result_doc, "content");
  auto lines = SplitLines(content);
  if (!lines.empty()) {
    ToolPresentationSection section;
    section.title = "Excerpt";
    const size_t shown = std::min<size_t>(8, lines.size());
    section.lines.assign(lines.begin(), lines.begin() + static_cast<long>(shown));
    presentation.body_lines = section.lines;
    presentation.sections.push_back(std::move(section));
    if (lines.size() > shown) {
      presentation.expandable = true;
      presentation.expanded = view.show_result;
      presentation.notices.push_back(
          {ToolPresentationNoticeKind::Info,
           "Showing first " + std::to_string(shown) + " lines"});
    }
  }
  return presentation;
}

ToolPresentation BuildSubagentTerminatePresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::CompactFactCard;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  std::string target = has_args ? StringMember(args_doc, "agent_id") : "";
  if (target.empty()) {
    target = has_args ? StringMember(args_doc, "subagent_id") : "";
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare subagent termination";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "terminating subagent";
  } else {
    presentation.title = "subagent terminated";
  }
  if (!target.empty()) {
    presentation.footer_badges.push_back(target);
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "subagent termination failed");
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  if (ParseObject(view.result, result_doc)) {
    const std::string result_target = StringMember(result_doc, "agent_id");
    if (!result_target.empty()) {
      presentation.footer_badges.push_back(result_target);
      presentation.facts.push_back({"Subagent", result_target});
    }
    const std::string status = StringMember(result_doc, "status");
    if (!status.empty()) {
      presentation.footer_badges.push_back(status);
      presentation.facts.push_back({"Status", status});
    }
  }
  return presentation;
}

} // namespace

bool IsFileFamilyTool(const std::string &tool_name) {
  return IsMatch(tool_name, "file_read") || IsMatch(tool_name, "file_edit") ||
         IsMatch(tool_name, "list_directory") || IsMatch(tool_name, "web_fetch") ||
         IsMatch(tool_name, "subagent_terminate") || IsMatch(tool_name, "terminate_subagent");
}

ToolPresentation BuildFileToolPresentation(const ToolCallView &view) {
  if (IsMatch(view.name, "file_read")) {
    return BuildFileReadPresentation(view);
  }
  if (IsMatch(view.name, "file_edit")) {
    return BuildFileEditPresentation(view);
  }
  if (IsMatch(view.name, "list_directory")) {
    return BuildListDirectoryPresentation(view);
  }
  if (IsMatch(view.name, "web_fetch")) {
    return BuildWebFetchPresentation(view);
  }
  return BuildSubagentTerminatePresentation(view);
}

} // namespace firmius::tui
