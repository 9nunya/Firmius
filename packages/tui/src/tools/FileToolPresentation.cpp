#include "tools/FileToolPresentation.hpp"

#include "components/FileEditDiff.hpp"
#include "utils/ErrorCleaner.hpp"

#include <algorithm>
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
  lines.reserve(64);  // Reserve for common case
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

std::vector<std::string> ParseStringArray(const rapidjson::Value &value) {
  std::vector<std::string> lines;
  if (!value.IsArray()) {
    return lines;
  }
  lines.reserve(value.GetArray().Size());
  for (const auto &entry : value.GetArray()) {
    if (entry.IsString()) {
      lines.emplace_back(entry.GetString());
    }
  }
  return lines;
}

std::string JoinLines(const std::vector<std::string> &lines) {
  std::string joined;
  for (const auto &line : lines) {
    joined += line;
    joined.push_back('\n');
  }
  return joined;
}

std::string HumanizeEditOperation(std::string op) {
  if (op.empty()) {
    return "edit";
  }
  if (op == "overwrite_file_content") {
    return "create file";
  }
  std::replace(op.begin(), op.end(), '_', ' ');
  return op;
}

bool IsAnchorHeavyDescription(const std::string &description) {
  return description.find("lineNumber") != std::string::npos ||
         description.find("...") != std::string::npos;
}

std::string JoinDisplayParts(const std::vector<std::string> &parts) {
  std::string joined;
  for (const auto &part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined += "  •  ";
    }
    joined += part;
  }
  return joined;
}

struct FileEditPreview {
  std::string path;
  std::string op;
  std::string description;
  std::string error;
  int start_line = 0;
  int end_line = 0;
  bool relocated = false;
  std::vector<std::string> old_lines;
  std::vector<std::string> new_lines;
  int patch_line = 0;
  bool highlight_full_addition = false;
};

struct FileEditTargetArgs {
  std::string path;
  bool has_content_overwrite = false;
  std::string overwrite_content;
  bool has_patch = false;
  std::string patch;
};

std::string BuildPreviewTitle(const FileEditPreview &preview) {
  if (!preview.description.empty() &&
      !IsAnchorHeavyDescription(preview.description)) {
    return preview.description;
  }
  if (!preview.path.empty()) {
    return BaseName(preview.path) + ": " + HumanizeEditOperation(preview.op);
  }
  return HumanizeEditOperation(preview.op);
}

std::string BuildPreviewMeta(const FileEditPreview &preview) {
  std::vector<std::string> parts;
  if (preview.start_line > 0) {
    std::string range = "line " + std::to_string(preview.start_line);
    if (preview.end_line > 0 && preview.end_line != preview.start_line) {
      range += "-" + std::to_string(preview.end_line);
    }
    parts.push_back(range);
  }
  if (!preview.old_lines.empty()) {
    parts.push_back("-" + std::to_string(preview.old_lines.size()));
  }
  if (!preview.new_lines.empty()) {
    parts.push_back("+" + std::to_string(preview.new_lines.size()));
  }
  if (preview.relocated) {
    parts.push_back("relocated");
  }
  if (preview.patch_line > 0) {
    parts.push_back("patch line " + std::to_string(preview.patch_line));
  }
  if (!preview.path.empty()) {
    parts.insert(parts.begin(), BaseName(preview.path));
  }
  return JoinDisplayParts(parts);
}

std::vector<FileEditTargetArgs> ParseFileEditArgs(const rapidjson::Document &args_doc) {
  std::vector<FileEditTargetArgs> targets;
  if (!args_doc.IsObject()) {
    return targets;
  }

  if (args_doc.HasMember("files") && args_doc["files"].IsArray()) {
    for (const auto &entry : args_doc["files"].GetArray()) {
      if (!entry.IsObject()) {
        continue;
      }
      FileEditTargetArgs target;
      target.path = StringMember(entry, "path");
      target.has_content_overwrite =
          entry.HasMember("content") && entry["content"].IsString();
      if (target.has_content_overwrite) {
        target.overwrite_content = entry["content"].GetString();
      }
      target.has_patch =
          entry.HasMember("patch") && entry["patch"].IsString();
      if (target.has_patch) {
        target.patch = entry["patch"].GetString();
      }
      targets.push_back(std::move(target));
    }
    return targets;
  }

  FileEditTargetArgs target;
  target.path = StringMember(args_doc, "path");
  target.has_content_overwrite =
      args_doc.HasMember("content") && args_doc["content"].IsString();
  if (target.has_content_overwrite) {
    target.overwrite_content = args_doc["content"].GetString();
  }
  target.has_patch =
      args_doc.HasMember("patch") && args_doc["patch"].IsString();
  if (target.has_patch) {
    target.patch = args_doc["patch"].GetString();
  }
  if (!target.path.empty() || target.has_content_overwrite ||
      args_doc.HasMember("edits")) {
    targets.push_back(std::move(target));
  }
  return targets;
}

std::vector<ToolPresentationDiffLine>
BuildAddedDiffLines(const std::vector<std::string> &lines, int start_line,
                    bool highlight_background) {
  std::vector<ToolPresentationDiffLine> diff_lines;
  diff_lines.reserve(lines.size());
  const int first_line = start_line > 0 ? start_line : 1;
  for (size_t i = 0; i < lines.size(); ++i) {
    ToolPresentationDiffLine line;
    line.type = '+';
    line.new_line = first_line + static_cast<int>(i);
    line.content = lines[i];
    line.highlight_background = highlight_background;
    diff_lines.push_back(std::move(line));
  }
  return diff_lines;
}

struct PostEditSlice {
  int start_line = 1;
  std::vector<std::string> lines;
  bool valid = false;
};

std::string StripAnchorPrefix(const std::string &line) {
  const auto pipe = line.find('|');
  if (pipe == std::string::npos) {
    return line;
  }
  return line.substr(pipe + 1);
}

PostEditSlice ParsePostEditSlice(const rapidjson::Value &value) {
  PostEditSlice slice;
  if (!value.IsObject() || !value.HasMember("lines") || !value["lines"].IsArray()) {
    return slice;
  }
  slice.start_line = IntMember(value, "start_line", 1);
  for (const auto &entry : value["lines"].GetArray()) {
    if (entry.IsString()) {
      slice.lines.push_back(StripAnchorPrefix(entry.GetString()));
    }
  }
  slice.valid = true;
  return slice;
}

bool ApplyReversePreview(const FileEditPreview &preview, int slice_start_line,
                         std::vector<std::string> &lines, int cumulative_delta) {
  const int final_start_line = preview.start_line + cumulative_delta;
  const int start_index = final_start_line - slice_start_line;
  const int erase_count = static_cast<int>(preview.new_lines.size());
  if (start_index < 0 || start_index > static_cast<int>(lines.size())) {
    return false;
  }
  if (start_index + erase_count > static_cast<int>(lines.size())) {
    return false;
  }
  lines.erase(lines.begin() + start_index,
              lines.begin() + start_index + erase_count);
  lines.insert(lines.begin() + start_index, preview.old_lines.begin(),
               preview.old_lines.end());
  return true;
}

std::vector<ToolPresentationDiffLine>
BuildUnifiedDiffLines(const std::vector<std::string> &original_lines,
                      const std::vector<std::string> &final_lines,
                      int start_line) {
  std::vector<ToolPresentationDiffLine> diff_lines;
  const auto hunks =
      BuildDiffHunks(JoinLines(original_lines), JoinLines(final_lines));
  for (const auto &hunk : hunks) {
    for (const auto &line : hunk.lines) {
      ToolPresentationDiffLine rendered;
      rendered.type = line.type;
      rendered.content = line.content;
      if (line.oldLine > 0) {
        rendered.old_line = start_line + line.oldLine - 1;
      }
      if (line.newLine > 0) {
        rendered.new_line = start_line + line.newLine - 1;
      }
      diff_lines.push_back(std::move(rendered));
    }
  }
  return diff_lines;
}

bool TryBuildUnifiedSuccessDiff(const rapidjson::Document &result_doc,
                                const std::vector<FileEditPreview> &previews,
                                ToolPresentationDiffSection &section) {
  if (!result_doc.IsObject() || !result_doc.HasMember("post_edit_slice")) {
    return false;
  }
  const auto post_edit_slice = ParsePostEditSlice(result_doc["post_edit_slice"]);
  if (!post_edit_slice.valid) {
    return false;
  }

  std::vector<std::string> original_lines = post_edit_slice.lines;
  int cumulative_delta = 0;
  std::vector<int> deltas;
  deltas.reserve(previews.size());
  for (const auto &preview : previews) {
    deltas.push_back(cumulative_delta);
    cumulative_delta += static_cast<int>(preview.new_lines.size()) -
                        static_cast<int>(preview.old_lines.size());
  }

  for (size_t i = previews.size(); i-- > 0;) {
    if (!ApplyReversePreview(previews[i], post_edit_slice.start_line,
                             original_lines, deltas[i])) {
      return false;
    }
  }

  section.lines = BuildUnifiedDiffLines(original_lines, post_edit_slice.lines,
                                        post_edit_slice.start_line);
  return true;
}

std::vector<ToolPresentationDiffLine>
BuildRemovedDiffLines(const std::vector<std::string> &lines, int start_line) {
  std::vector<ToolPresentationDiffLine> diff_lines;
  diff_lines.reserve(lines.size());
  const int first_line = start_line > 0 ? start_line : 1;
  for (size_t i = 0; i < lines.size(); ++i) {
    ToolPresentationDiffLine line;
    line.type = '-';
    line.old_line = first_line + static_cast<int>(i);
    line.content = lines[i];
    diff_lines.push_back(std::move(line));
  }
  return diff_lines;
}

std::vector<ToolPresentationDiffLine>
BuildPreviewDiffLines(const FileEditPreview &preview) {
  if (preview.old_lines.empty()) {
    return BuildAddedDiffLines(preview.new_lines, preview.start_line,
                               preview.highlight_full_addition);
  }
  if (preview.new_lines.empty()) {
    return BuildRemovedDiffLines(preview.old_lines, preview.start_line);
  }

  std::vector<ToolPresentationDiffLine> diff_lines;
  const auto hunks =
      BuildDiffHunks(JoinLines(preview.old_lines), JoinLines(preview.new_lines));
  for (const auto &hunk : hunks) {
    for (const auto &line : hunk.lines) {
      ToolPresentationDiffLine rendered;
      rendered.type = line.type;
      rendered.content = line.content;
      if (line.oldLine > 0) {
        rendered.old_line = preview.start_line + line.oldLine - 1;
      }
      if (line.newLine > 0) {
        rendered.new_line = preview.start_line + line.newLine - 1;
      }
      diff_lines.push_back(std::move(rendered));
    }
  }
  return diff_lines;
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
  std::string content;
  int line_start = start_line;
  int line_end = end_line;
  int lines_read = 0;
  std::string watch_scope;
  std::string watch_state;
  if (ParseObject(view.result, result_doc)) {
    const std::string parsed_content = StringMember(result_doc, "content");
    if (!parsed_content.empty()) {
      content = parsed_content;
    }
    line_start = IntMember(result_doc, "line_start", line_start);
    line_end = IntMember(result_doc, "line_end", line_end);
    lines_read = IntMember(result_doc, "lines_read", 0);
    watch_scope = StringMember(result_doc, "watch_scope");
    watch_state = StringMember(result_doc, "watch_state");
  }

  if (line_start >= 0 && line_end >= line_start) {
    presentation.footer_badges.push_back("lines " + std::to_string(line_start) + "-" +
                                         std::to_string(line_end));
  }
  if (lines_read > 0) {
    presentation.footer_badges.push_back(std::to_string(lines_read) + " lines");
  }
  if (!watch_scope.empty()) {
    presentation.footer_badges.push_back(
        watch_scope == "full" ? "watch full" : "watch range");
  }
  if (!watch_state.empty()) {
    presentation.facts.push_back({"Watch state", watch_state});
  }
  if (!watch_scope.empty()) {
    presentation.facts.push_back(
        {"Watch scope", watch_scope == "full" ? "full file" : "range"});
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

void AppendLspDetailsFromObject(const rapidjson::Value &lsp,
                                const std::string &path,
                                ToolPresentation &presentation) {
  if (!lsp.IsObject()) {
    return;
  }

  const bool checked =
      lsp.HasMember("checked") && lsp["checked"].IsBool() && lsp["checked"].GetBool();
  const bool available =
      lsp.HasMember("available") && lsp["available"].IsBool() && lsp["available"].GetBool();
  const std::string pathLabel = path.empty() ? "file" : BaseName(path);

  if (checked) {
    presentation.facts.push_back({"LSP", available ? "checked" : "unavailable"});
  }
  const std::string serverId = StringMember(lsp, "server_id");
  if (!serverId.empty()) {
    presentation.facts.push_back({"LSP server", serverId});
  }

  const int errors = IntMember(lsp, "errors", 0);
  const int warnings = IntMember(lsp, "warnings", 0);
  const int newErrors = IntMember(lsp, "new_error_count", 0);
  const int newWarnings = IntMember(lsp, "new_warning_count", 0);

  if (errors > 0) {
    presentation.footer_badges.push_back("lsp:E" + std::to_string(errors));
    presentation.facts.push_back({"LSP errors", std::to_string(errors)});
  }
  if (warnings > 0) {
    presentation.footer_badges.push_back("lsp:W" + std::to_string(warnings));
    presentation.facts.push_back({"LSP warnings", std::to_string(warnings)});
  }
  if (newErrors > 0) {
    presentation.footer_badges.push_back("new:E" + std::to_string(newErrors));
    presentation.facts.push_back({"New LSP errors", std::to_string(newErrors)});
  }
  if (newWarnings > 0) {
    presentation.footer_badges.push_back("new:W" + std::to_string(newWarnings));
    presentation.facts.push_back(
        {"New LSP warnings", std::to_string(newWarnings)});
  }

  const std::string error = StringMember(lsp, "error");
  if (!available && !error.empty()) {
    presentation.notices.push_back(
        {ToolPresentationNoticeKind::Info,
         "LSP unavailable for " + pathLabel + ": " + error});
  }

  const auto newErrorIssues =
      lsp.HasMember("new_errors") ? ParseStringArray(lsp["new_errors"])
                                  : std::vector<std::string>{};
  const auto newWarningIssues =
      lsp.HasMember("new_warnings") ? ParseStringArray(lsp["new_warnings"])
                                    : std::vector<std::string>{};
  const auto issues = lsp.HasMember("issues") ? ParseStringArray(lsp["issues"])
                                               : std::vector<std::string>{};
  const auto warningIssues =
      lsp.HasMember("warning_issues")
          ? ParseStringArray(lsp["warning_issues"])
          : std::vector<std::string>{};

  ToolPresentationSection section;
  if (!newErrorIssues.empty() || !newWarningIssues.empty()) {
    section.title = "New LSP issues: " + pathLabel;
    section.lines.insert(section.lines.end(), newErrorIssues.begin(),
                         newErrorIssues.end());
    section.lines.insert(section.lines.end(), newWarningIssues.begin(),
                         newWarningIssues.end());
    presentation.notices.push_back(
        {ToolPresentationNoticeKind::Warning,
         "New LSP issues detected in " + pathLabel});
  } else if (!issues.empty() || !warningIssues.empty()) {
    section.title = "LSP issues: " + pathLabel;
    section.lines.insert(section.lines.end(), issues.begin(), issues.end());
    section.lines.insert(section.lines.end(), warningIssues.begin(),
                         warningIssues.end());
  }

  if (!section.lines.empty()) {
    presentation.sections.push_back(std::move(section));
  }
}

ToolPresentation BuildFileEditPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::DiffPreview;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const auto target_args = has_args ? ParseFileEditArgs(args_doc)
                                    : std::vector<FileEditTargetArgs>{};
  const bool multi_file_args = target_args.size() > 1;
  const std::string path =
      !target_args.empty() ? target_args.front().path : "";
  const bool has_content_overwrite =
      !target_args.empty() && target_args.front().has_content_overwrite;
  const std::string overwrite_content =
      !target_args.empty() ? target_args.front().overwrite_content : "";
  const bool has_patch =
      !target_args.empty() && target_args.front().has_patch;
  const std::string patch =
      !target_args.empty() ? target_args.front().patch : "";

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
    presentation.diff_source_name = BaseName(path);
  } else if (has_patch) {
    presentation.title = "file patch";
    presentation.compact_summary = "patch " + BaseName(path);
  } else if (multi_file_args) {
    presentation.footer_badges.push_back(
        std::to_string(target_args.size()) + " files");
    presentation.compact_summary =
        std::to_string(target_args.size()) + " files";
  }
  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing ||
      presentation.lifecycle == ToolPresentationLifecycle::Running) {
    return presentation;
  }

  rapidjson::Document result_doc;
  const bool has_result = ParseObject(view.result, result_doc);

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    presentation.title = "file edit failed";
    if (has_result) {
      const std::string top_level_error = StringMember(result_doc, "error");
      if (!top_level_error.empty()) {
        presentation.error_text = top_level_error;
      } else {
        presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
      }
    } else {
      presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
    }
  }

  std::vector<FileEditPreview> previews;
  auto appendOperations =
      [&](const rapidjson::Value &operations, const std::string &op_path,
          bool op_has_content_overwrite, const std::string &op_overwrite_content) {
    if (!operations.IsArray()) {
      return;
    }
    for (const auto &op : operations.GetArray()) {
      if (!op.IsObject()) {
        continue;
      }
      FileEditPreview preview;
      preview.path = op_path;
      preview.op = StringMember(op, "op");
      preview.description = StringMember(op, "description");
      preview.error = StringMember(op, "error");
      preview.start_line = IntMember(op, "start_line", 1);
      preview.end_line = IntMember(op, "end_line", preview.start_line);
      preview.patch_line = IntMember(op, "patch_line", 0);
      preview.relocated =
          op.HasMember("relocated") && op["relocated"].IsBool() &&
          op["relocated"].GetBool();
      if (op.HasMember("old_lines")) {
        preview.old_lines = ParseStringArray(op["old_lines"]);
      }
      if (op.HasMember("new_lines")) {
        preview.new_lines = ParseStringArray(op["new_lines"]);
      }
      if (preview.op == "overwrite_file_content" && op_has_content_overwrite) {
        preview.new_lines = SplitLines(op_overwrite_content);
        preview.highlight_full_addition = true;
        preview.start_line = 1;
        preview.end_line = static_cast<int>(preview.new_lines.size());
      }
      previews.push_back(std::move(preview));
    }
  };

  if (has_result && result_doc.HasMember("files") && result_doc["files"].IsArray()) {
    for (rapidjson::SizeType i = 0; i < result_doc["files"].Size(); ++i) {
      const auto &file = result_doc["files"][i];
      if (!file.IsObject()) {
        continue;
      }
      const std::string file_path = StringMember(file, "path");
      bool file_has_content_overwrite = false;
      std::string file_overwrite_content;
      if (i < target_args.size()) {
        file_has_content_overwrite = target_args[i].has_content_overwrite;
        file_overwrite_content = target_args[i].overwrite_content;
      }

      if (file.HasMember("operations")) {
        appendOperations(file["operations"], file_path, file_has_content_overwrite,
                         file_overwrite_content);
      } else if (file_has_content_overwrite) {
        FileEditPreview preview;
        preview.path = file_path;
        preview.op = "overwrite_file_content";
        preview.description = "create file";
        preview.new_lines = SplitLines(file_overwrite_content);
        preview.highlight_full_addition = true;
        preview.start_line = 1;
        preview.end_line = static_cast<int>(preview.new_lines.size());
        previews.push_back(std::move(preview));
      }
    }
  } else if (has_result && result_doc.HasMember("operations") &&
             result_doc["operations"].IsArray()) {
    appendOperations(result_doc["operations"], path, has_content_overwrite,
                     overwrite_content);
  }

  if (previews.empty() && has_content_overwrite) {
    FileEditPreview preview;
    preview.path = path;
    preview.op = "overwrite_file_content";
    preview.description = "create file";
    preview.new_lines = SplitLines(overwrite_content);
    preview.highlight_full_addition = true;
    preview.start_line = 1;
    preview.end_line = static_cast<int>(preview.new_lines.size());
    previews.push_back(std::move(preview));
  }

  int added_lines = 0;
  int removed_lines = 0;
  if (presentation.lifecycle == ToolPresentationLifecycle::Success) {
    ToolPresentationDiffSection unified_section;
    if (has_patch && !multi_file_args) {
        unified_section.title = "Patch applied";
        if (previews.empty()) {
            presentation.body_lines = SplitLines(patch);
        }
    }
    if (has_content_overwrite && !multi_file_args) {
      unified_section.lines =
          BuildAddedDiffLines(SplitLines(overwrite_content), 1, true);
    } else if (!previews.empty() &&
               !multi_file_args &&
               !TryBuildUnifiedSuccessDiff(result_doc, previews, unified_section)) {
      for (const auto &preview : previews) {
        auto lines = BuildPreviewDiffLines(preview);
        unified_section.lines.insert(unified_section.lines.end(), lines.begin(),
                                     lines.end());
      }
    }
    if (!unified_section.lines.empty()) {
      for (const auto &line : unified_section.lines) {
        if (line.type == '+') {
          added_lines++;
        } else if (line.type == '-') {
          removed_lines++;
        }
      }
      presentation.diff_sections.push_back(std::move(unified_section));
    } else if (multi_file_args) {
      for (const auto &preview : previews) {
        ToolPresentationDiffSection section;
        section.title = BuildPreviewTitle(preview);
        section.meta = BuildPreviewMeta(preview);
        section.lines = BuildPreviewDiffLines(preview);
        for (const auto &line : section.lines) {
          if (line.type == '+') {
            added_lines++;
          } else if (line.type == '-') {
            removed_lines++;
          }
        }
        presentation.diff_sections.push_back(std::move(section));
      }
    }
  } else {
    for (const auto &preview : previews) {
      ToolPresentationDiffSection section;
      section.title = BuildPreviewTitle(preview);
      section.meta = BuildPreviewMeta(preview);
      if (!preview.error.empty()) {
        section.error_text = preview.error;
      }
      section.lines = BuildPreviewDiffLines(preview);
      for (const auto &line : section.lines) {
        if (line.type == '+') {
          added_lines++;
        } else if (line.type == '-') {
          removed_lines++;
        }
      }
      presentation.diff_sections.push_back(std::move(section));
    }
  }
  if (added_lines > 0) {
    presentation.footer_badges.push_back("+" + std::to_string(added_lines));
    presentation.facts.push_back({"Added lines", std::to_string(added_lines)});
  }
  if (removed_lines > 0) {
    presentation.footer_badges.push_back("-" + std::to_string(removed_lines));
    presentation.facts.push_back({"Removed lines", std::to_string(removed_lines)});
  }

  if (has_result && result_doc.HasMember("lsp")) {
    AppendLspDetailsFromObject(result_doc["lsp"], path, presentation);
  }
  if (has_result && result_doc.HasMember("files") && result_doc["files"].IsArray()) {
    for (const auto &file : result_doc["files"].GetArray()) {
      if (!file.IsObject() || !file.HasMember("lsp")) {
        continue;
      }
      AppendLspDetailsFromObject(file["lsp"], StringMember(file, "path"),
                                 presentation);
    }
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
