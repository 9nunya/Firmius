#include "tools/LspPresenter.hpp"
#include "tools/ToolArgsParser.hpp"
#include "items/ToolCallItem.hpp"
#include "Terminal.hpp"
#include "ThemeAnsi.hpp"

#include <rapidjson/document.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace firmius::tui2 {

bool LspPresenter::matches(const std::string& toolName) const {
  return toolName == "Lsp";
}

namespace {

std::string formatDuration(std::chrono::milliseconds ms) {
  double secs = static_cast<double>(ms.count()) / 1000.0;
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1) << secs << "s";
  return oss.str();
}

} // namespace

std::vector<std::string> LspPresenter::render(const ToolCallItem& item, const ToolRenderContext& /*ctx*/, int /*width*/) const {
  if (item.phase() == ToolPhase::Preparing) {
    return {theme_ansi::warning("  \xe2\x9a\x99 Lsp")};
  }

  std::string action;
  std::string operation;
  std::string path;
  int line = 0;
  int character = 0;
  if (!item.args().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.args().c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      if (doc.HasMember("action") && doc["action"].IsString()) action = doc["action"].GetString();
      if (doc.HasMember("operation") && doc["operation"].IsString()) operation = doc["operation"].GetString();
      if (doc.HasMember("path") && doc["path"].IsString()) path = doc["path"].GetString();
      if (doc.HasMember("line") && doc["line"].IsInt()) line = doc["line"].GetInt();
      if (doc.HasMember("character") && doc["character"].IsInt()) character = doc["character"].GetInt();
    }
  }

  if (item.phase() == ToolPhase::Called) {
    std::string op = operation.empty() ? action : operation;
    std::string text = "  \xe2\x9a\x99 Lsp." + op;
    if (!path.empty()) {
      text += " " + path + ":" + std::to_string(line) + ":" + std::to_string(character);
    }
    return {theme_ansi::warning(text),
            theme_ansi::dim("  " + formatDuration(item.elapsed()))};
  }

  // Finished
  bool success = item.success();
  auto color = [&](const std::string& s) {
    return success ? theme_ansi::success(s) : theme_ansi::error(s);
  };
  std::string icon = success ? "\xe2\x9c\x93" : "\xe2\x9c\x97";
  std::string op = operation.empty() ? action : operation;

  if (!item.result().empty()) {
    rapidjson::Document doc;
    doc.Parse(item.result().c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      // Diagnostics
      if (op == "Diagnostics" || action == "Diagnostics") {
        int errors = 0, warnings = 0;
        if (doc.HasMember("errors") && doc["errors"].IsInt()) errors = doc["errors"].GetInt();
        if (doc.HasMember("warnings") && doc["warnings"].IsInt()) warnings = doc["warnings"].GetInt();
        if (doc.HasMember("diagnostics") && doc["diagnostics"].IsArray()) {
          for (const auto& d : doc["diagnostics"].GetArray()) {
            if (d.IsObject() && d.HasMember("severity") && d["severity"].IsInt()) {
              int sev = d["severity"].GetInt();
              if (sev == 1) errors++;
              else if (sev == 2) warnings++;
            }
          }
        }
        std::string text = "  " + icon + " Diagnostics \xe2\x80\x94 " +
                           std::to_string(errors) + " errors, " +
                           std::to_string(warnings) + " warnings";
        return {color(text)};
      }

      // Query results
      int resultCount = 0;
      if (doc.HasMember("results") && doc["results"].IsArray()) resultCount = doc["results"].Size();
      if (doc.HasMember("locations") && doc["locations"].IsArray()) resultCount = doc["locations"].Size();
      if (resultCount > 0) {
        return {color("  " + icon + " Lsp." + op + " \xe2\x80\x94 " + std::to_string(resultCount) + " results")};
      }

      // Hover — show type info preview
      if (op == "hover" || op == "Hover") {
        std::string value;
        if (doc.HasMember("value") && doc["value"].IsString()) value = doc["value"].GetString();
        if (doc.HasMember("contents") && doc["contents"].IsString()) value = doc["contents"].GetString();
        if (!value.empty()) {
          // Truncate to first line
          auto nl = value.find('\n');
          if (nl != std::string::npos) value = value.substr(0, nl);
          if (value.size() > 60) value = value.substr(0, 57) + "...";
          return {color("  " + icon + " Lsp.hover \xe2\x80\x94 " + value)};
        }
      }
    }
  }

  return {color("  " + icon + " Lsp." + op)};
}

} // namespace firmius::tui2
