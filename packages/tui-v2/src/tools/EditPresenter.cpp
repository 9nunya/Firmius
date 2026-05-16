#include "tools/EditPresenter.hpp"
#include "items/ToolCallItem.hpp"
#include "DiffRenderer.hpp"
#include "Terminal.hpp"

#include "utils/ToolView.hpp"

#include <rapidjson/document.h>
#include <sstream>

namespace firmius::tui2 {

bool EditPresenter::matches(const std::string& toolName) const {
  return toolName == "Edit" || toolName == "EditWrite" ||
         toolName == "EditReplace" || toolName == "EditRange";
}

namespace {

struct EditArgs {
  std::string path;
  std::string patch;
};

EditArgs parseArgs(const std::string& json) {
  EditArgs a;
  if (json.empty()) return a;
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return a;
  if (doc.HasMember("path") && doc["path"].IsString()) a.path = doc["path"].GetString();
  if (doc.HasMember("patch") && doc["patch"].IsString()) a.patch = doc["patch"].GetString();
  return a;
}

std::string displayToolName(const std::string& name) {
  if (name == "EditWrite") return "EditWrite";
  if (name == "EditReplace") return "EditReplace";
  if (name == "EditRange") return "EditRange";
  return "Edit";
}

} // namespace

std::vector<std::string> EditPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int width) const {
  std::string dname = displayToolName(item.toolName());

  if (item.phase() == ToolPhase::Preparing) {
    return {ansi::fgRgb(220, 180, 80, "  \xe2\x9a\x99 " + dname)};
  }

  auto args = parseArgs(item.args());

  // Called phase
  if (item.phase() == ToolPhase::Called) {
    std::vector<std::string> result;
    std::string header = "  \xe2\x9a\x99 " + dname;
    if (!args.path.empty()) header += " " + args.path;
    result.push_back(ansi::fgRgb(220, 180, 80, header));

    // Check if diffs already arrived
    if (item.diffEdits().empty()) {
      result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  Loading diff...")));
    } else {
      // Render diffs
      for (const auto& edit : item.diffEdits()) {
        auto diffLines = DiffRenderer::render(edit.diffPreview, width);
        result.insert(result.end(), diffLines.begin(), diffLines.end());
      }
    }
    return result;
  }

  // Finished phase
  std::vector<std::string> result;
  int totalAdded = 0;
  int totalRemoved = 0;
  for (const auto& edit : item.diffEdits()) {
    totalAdded += edit.addedLines;
    totalRemoved += edit.removedLines;
  }

  // Header
  if (item.success()) {
    std::string header = "  \xe2\x9c\x93 " + dname;
    if (item.diffEdits().size() > 1) {
      header += " \xe2\x80\x94 " + std::to_string(item.diffEdits().size()) + " files changed";
    } else if (!args.path.empty()) {
      header += " " + args.path;
    }
    if (totalAdded > 0 || totalRemoved > 0) {
      header += " ";
      if (totalAdded > 0) header += "+" + std::to_string(totalAdded);
      if (totalAdded > 0 && totalRemoved > 0) header += " ";
      if (totalRemoved > 0) header += "-" + std::to_string(totalRemoved);
    }
    result.push_back(ansi::fgRgb(100, 200, 120, header));
  } else {
    // On error: show tool name + path (from args or from first diff edit)
    std::string filePath = args.path;
    if (filePath.empty() && !item.diffEdits().empty()) {
      filePath = item.diffEdits()[0].path;
    }
    std::string header = "  \xe2\x9c\x97 " + dname;
    if (!filePath.empty()) header += " " + filePath;
    header += " failed";
    result.push_back(ansi::fgRgb(220, 80, 80, header));
  }

  // Render diffs
  if (item.diffEdits().empty()) {
    result.push_back(ansi::dim(ansi::fgRgb(120, 120, 140, "  (no diff available)")));
  } else {
    for (size_t i = 0; i < item.diffEdits().size(); ++i) {
      if (i > 0) result.push_back("");  // blank line between files
      auto diffLines = DiffRenderer::render(item.diffEdits()[i].diffPreview, width);
      result.insert(result.end(), diffLines.begin(), diffLines.end());
    }
  }

  // Error text on failure
  if (!item.success() && !item.result().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.result().c_str());
    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("error") && doc["error"].IsString()) {
      result.push_back(ansi::fgRgb(220, 80, 80, "  " + std::string(doc["error"].GetString())));
    }
  }

  return result;
}

} // namespace firmius::tui2
