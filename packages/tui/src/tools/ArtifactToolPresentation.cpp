#include "tools/ArtifactToolPresentation.hpp"

#include "components/FileEditDiff.hpp"
#include "utils/ErrorCleaner.hpp"

#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui {

namespace {

using firmius::shared::ToolCallView;
using firmius::shared::ToolPhase;

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

std::string StringMember(const rapidjson::Value &value, const char *key) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsString()) {
    return value[key].GetString();
  }
  return "";
}

std::string ExtractAction(const std::string &args) {
  rapidjson::Document doc;
  doc.Parse(args.c_str());
  if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("action") &&
      doc["action"].IsString()) {
    return doc["action"].GetString();
  }
  return "";
}

bool BoolMember(const rapidjson::Value &value, const char *key, bool fallback = false) {
  if (value.IsObject() && value.HasMember(key) && value[key].IsBool()) {
    return value[key].GetBool();
  }
  return fallback;
}

std::vector<std::string> SplitLines(const std::string &text, size_t max_lines = 0) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
    if (max_lines > 0 && lines.size() >= max_lines) {
      break;
    }
  }
  return lines;
}

std::vector<ToolPresentationDiffLine>
BuildArtifactCreateLines(const std::vector<std::string> &lines) {
  std::vector<ToolPresentationDiffLine> diff_lines;
  diff_lines.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    ToolPresentationDiffLine line;
    line.type = '+';
    line.new_line = static_cast<int>(i) + 1;
    line.content = lines[i];
    line.highlight_background = true;
    diff_lines.push_back(std::move(line));
  }
  return diff_lines;
}

std::vector<ToolPresentationDiffLine>
BuildArtifactUpdateLines(const std::string &previous_content,
                         const std::string &content) {
  std::vector<ToolPresentationDiffLine> diff_lines;
  const auto hunks = BuildDiffHunks(previous_content, content);
  for (const auto &hunk : hunks) {
    for (const auto &line : hunk.lines) {
      ToolPresentationDiffLine rendered;
      rendered.type = line.type;
      rendered.old_line = line.oldLine;
      rendered.new_line = line.newLine;
      rendered.content = line.content;
      diff_lines.push_back(std::move(rendered));
    }
  }
  return diff_lines;
}

void ApplyError(ToolPresentation &presentation, const ToolCallView &view,
                const std::string &failed_title) {
  presentation.lifecycle = ToolPresentationLifecycle::Error;
  presentation.title = failed_title;
  presentation.subtitle = view.name;
  presentation.error_text = firmius::shared::ErrorCleaner::clean(view.result);
}

std::string ArtifactOwner(const rapidjson::Value &artifact) {
  std::string owner = StringMember(artifact, "owner_friendly_name");
  if (owner.empty()) {
    owner = StringMember(artifact, "owner_agent_id");
  }
  return owner;
}

ToolPresentation BuildArtifactWritePresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::DiffPreview;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string name = has_args ? StringMember(args_doc, "name") : "";
  const std::string kind = has_args ? StringMember(args_doc, "kind") : "";
  const std::string description = has_args ? StringMember(args_doc, "description") : "";
  const std::string content = has_args ? StringMember(args_doc, "content") : "";

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare artifact write";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "writing artifact";
  } else {
    presentation.title = "artifact write";
  }
  if (!name.empty()) {
    presentation.compact_summary = name;
    presentation.footer_badges.push_back(name);
    presentation.diff_source_name = name;
  }
  if (!kind.empty()) {
    presentation.footer_badges.push_back(kind);
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "artifact write failed");
    if (!content.empty()) {
      ToolPresentationDiffSection section;
      section.title = name.empty() ? "artifact content" : name;
      section.lines = BuildArtifactCreateLines(SplitLines(content));
      presentation.diff_sections.push_back(std::move(section));
    }
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  const bool has_result = ParseObject(view.result, result_doc);
  const bool created = has_result && BoolMember(result_doc, "created");
  const bool updated = has_result && BoolMember(result_doc, "updated");
  const std::string reference = has_result ? StringMember(result_doc, "reference") : "";
  const rapidjson::Value &artifact =
      has_result && result_doc.HasMember("artifact") ? result_doc["artifact"] : result_doc;
  const std::string owner = ArtifactOwner(artifact);
  const std::string filename = StringMember(artifact, "filename");
  const std::string resolved_kind = StringMember(artifact, "kind");
  const std::string resolved_description = StringMember(artifact, "description");
  const std::string previous_content =
      has_result ? StringMember(result_doc, "previous_content") : "";

  if (created) {
    presentation.title = "artifact created";
  } else if (updated) {
    presentation.title = "artifact updated";
  } else {
    presentation.title = "artifact write complete";
  }
  presentation.footer_badges.push_back(created ? "created" : (updated ? "updated" : "written"));
  if (!filename.empty()) {
    presentation.footer_badges.push_back(filename);
    presentation.diff_source_name = filename;
  }
  if (!owner.empty()) {
    presentation.footer_badges.push_back(owner);
  }
  if (!reference.empty()) {
    presentation.footer_badges.push_back(reference);
    presentation.facts.push_back({"Reference", reference});
  }
  if (!resolved_kind.empty()) {
    presentation.facts.push_back({"Kind", resolved_kind});
  }
  if (!resolved_description.empty()) {
    presentation.facts.push_back({"Description", resolved_description});
  } else if (!description.empty()) {
    presentation.facts.push_back({"Description", description});
  }

  ToolPresentationDiffSection section;
  section.title =
      filename.empty() ? (name.empty() ? "artifact content" : name) : filename;
  if (updated && !previous_content.empty()) {
    section.meta = "updated artifact";
    section.lines = BuildArtifactUpdateLines(previous_content, content);
  } else {
    section.meta = created ? "new artifact" : "artifact content";
    section.lines = BuildArtifactCreateLines(SplitLines(content));
  }
  if (!section.lines.empty()) {
    int added_lines = 0;
    int removed_lines = 0;
    for (const auto &line : section.lines) {
      if (line.type == '+') {
        added_lines++;
      } else if (line.type == '-') {
        removed_lines++;
      }
    }
    presentation.diff_sections.push_back(std::move(section));
    if (added_lines > 0) {
      presentation.footer_badges.push_back("+" + std::to_string(added_lines));
      presentation.facts.push_back({"Added lines", std::to_string(added_lines)});
    }
    if (removed_lines > 0) {
      presentation.footer_badges.push_back("-" + std::to_string(removed_lines));
      presentation.facts.push_back({"Removed lines", std::to_string(removed_lines)});
    }
  }

  return presentation;
}

ToolPresentation BuildArtifactReadPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::CompactFactCard;
  presentation.density = ToolPresentationDensity::CompactSummaryCard;
  presentation.subtitle = view.name;

  rapidjson::Document args_doc;
  const bool has_args = ParseObject(view.args, args_doc);
  const std::string reference = has_args ? StringMember(args_doc, "reference") : "";
  const std::string name = has_args ? StringMember(args_doc, "name") : "";

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare artifact read";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "reading artifact";
  } else {
    presentation.title = "loaded";
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    if (!reference.empty()) {
      presentation.footer_badges.push_back(reference);
    } else if (!name.empty()) {
      presentation.footer_badges.push_back(name);
    }
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "artifact read failed");
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  const bool has_result = ParseObject(view.result, result_doc);
  const std::string resolved_reference = has_result ? StringMember(result_doc, "reference") : "";
  const rapidjson::Value &artifact =
      has_result && result_doc.HasMember("artifact") ? result_doc["artifact"] : result_doc;
  const std::string owner = ArtifactOwner(artifact);
  const std::string filename = StringMember(artifact, "filename");
  const std::string title_label =
      !resolved_reference.empty()
          ? resolved_reference
          : (!reference.empty() ? reference
                                : (!name.empty() ? name : filename));

  presentation.title =
      title_label.empty() ? "loaded artifact" : ("loaded " + title_label);

  if (!owner.empty()) {
    presentation.footer_badges.push_back(owner);
  }
  if (!filename.empty()) {
    presentation.footer_badges.push_back(filename);
  }
  if (!resolved_reference.empty() && resolved_reference != title_label) {
    presentation.footer_badges.push_back(resolved_reference);
  }

  return presentation;
}

ToolPresentation BuildArtifactListPresentation(const ToolCallView &view) {
  ToolPresentation presentation;
  presentation.lifecycle = LifecycleFromPhase(view);
  presentation.layout = ToolPresentationLayoutKind::ResultsList;
  presentation.density = ToolPresentationDensity::DetailHeavy;
  presentation.subtitle = view.name;

  if (presentation.lifecycle == ToolPresentationLifecycle::Preparing) {
    presentation.title = "prepare artifact listing";
  } else if (presentation.lifecycle == ToolPresentationLifecycle::Running) {
    presentation.title = "listing artifacts";
  } else {
    presentation.title = "artifacts";
  }

  if (presentation.lifecycle == ToolPresentationLifecycle::Error) {
    ApplyError(presentation, view, "artifact list failed");
    return presentation;
  }
  if (presentation.lifecycle != ToolPresentationLifecycle::Success) {
    return presentation;
  }

  rapidjson::Document result_doc;
  if (!ParseObject(view.result, result_doc) || !result_doc.HasMember("artifacts") ||
      !result_doc["artifacts"].IsArray()) {
    return presentation;
  }

  const auto &artifacts = result_doc["artifacts"].GetArray();
  presentation.footer_badges.push_back(std::to_string(artifacts.Size()) + " artifacts");
  presentation.facts.push_back({"Count", std::to_string(artifacts.Size())});
  presentation.title = "listed " + std::to_string(artifacts.Size()) + " artifacts";

  ToolPresentationSection section;
  section.title = "Artifacts";
  const rapidjson::SizeType max_rows =
      view.show_result ? artifacts.Size() : static_cast<rapidjson::SizeType>(18);
  for (rapidjson::SizeType i = 0; i < artifacts.Size() && i < max_rows; ++i) {
    const auto &item = artifacts[i];
    std::string row = StringMember(item, "reference");
    const std::string display = StringMember(item, "display");
    if (!display.empty()) {
      row = display + " (" + row + ")";
    }
    if (row.empty()) {
      continue;
    }
    if (item.HasMember("ambiguous_filename") && item["ambiguous_filename"].IsBool() &&
        item["ambiguous_filename"].GetBool()) {
      row += " [ambiguous name]";
    }
    presentation.body_lines.push_back(row);
    section.lines.push_back(std::move(row));
  }
  if (!section.lines.empty()) {
    presentation.sections.push_back(std::move(section));
  }

  if (artifacts.Size() > max_rows) {
    presentation.expandable = true;
    presentation.expanded = view.show_result;
    ToolPresentationNotice notice;
    notice.kind = ToolPresentationNoticeKind::Info;
    notice.text = "Show to open full artifact list";
    presentation.notices.push_back(std::move(notice));
  }

  return presentation;
}

} // namespace

bool IsArtifactFamilyTool(const std::string &tool_name) {
  return tool_name == "Artifacts" || tool_name == "artifact_write" || tool_name == "artifact_read" || tool_name == "artifact_list";
}

ToolPresentation BuildArtifactToolPresentation(const ToolCallView &view) {
  std::string action = ExtractAction(view.args);
  if (action.empty()) {
    if (view.name == "artifact_write") {
      action = "Write";
    } else if (view.name == "artifact_read") {
      action = "Read";
    }
  }
  if (action == "Write") {
    return BuildArtifactWritePresentation(view);
  }
  if (action == "Read") {
    return BuildArtifactReadPresentation(view);
  }
  return BuildArtifactListPresentation(view);
}

} // namespace firmius::tui
