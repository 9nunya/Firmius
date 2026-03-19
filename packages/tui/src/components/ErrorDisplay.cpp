#include "components/ErrorDisplay.hpp"

#include "components/Markdown.hpp"
#include <algorithm>
#include <cctype>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <sstream>
#include <vector>

namespace firmius::tui {
namespace {

std::string trimCopy(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

bool startsWith(const std::string& str, const std::string& prefix) {
  if (prefix.length() > str.length()) return false;
  return str.compare(0, prefix.length(), prefix) == 0;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
}

bool looksLikeJsonStart(char ch) { return ch == '{' || ch == '['; }

bool looksLikeMetadataLabel(const std::string& line, std::string& out_label, std::string& out_content) {
  static const std::vector<std::string> known_labels = {
    "Provider:",
    "Model:",
    "HTTP",
    "Status:",
    "Code:",
    "Error:",
    "Type:",
  };
  
  std::string trimmed = trimCopy(line);
  if (trimmed.empty()) return false;
  
  for (const auto& label : known_labels) {
    if (startsWith(trimmed, label)) {
      out_label = label.substr(0, label.length() - (label.back() == ':' ? 1 : 0));
      out_content = trimCopy(trimmed.substr(label.length()));
      return true;
    }
  }
  return false;
}

bool looksLikeRawBodyLabel(const std::string& line, std::string& out_label) {
  static const std::vector<std::string> body_labels = {
    "Raw provider body:",
    "Raw body:",
    "Response body:",
    "Body:",
    "Raw response:",
    "Payload:",
  };
  
  std::string trimmed = trimCopy(line);
  for (const auto& label : body_labels) {
    if (startsWith(trimmed, label)) {
      out_label = label;
      return true;
    }
  }
  return false;
}

size_t findJsonBlockEnd(const std::vector<std::string>& lines, size_t start_idx) {
  if (start_idx >= lines.size()) return start_idx;
  
  std::string first_line = trimCopy(lines[start_idx]);
  if (first_line.empty() || !looksLikeJsonStart(first_line[0])) {
    return start_idx;
  }
  
  int brace_depth = 0;
  int bracket_depth = 0;
  bool in_string = false;
  bool escaped = false;
  
  for (size_t i = start_idx; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    for (char c : line) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (c == '"' && !in_string) {
        in_string = true;
        continue;
      }
      if (c == '"' && in_string) {
        in_string = false;
        continue;
      }
      if (in_string) continue;
      
      if (c == '{') brace_depth++;
      else if (c == '}') brace_depth--;
      else if (c == '[') bracket_depth++;
      else if (c == ']') bracket_depth--;
    }
    
    if (brace_depth == 0 && bracket_depth == 0 && 
        (first_line[0] == '{' || first_line[0] == '[')) {
      return i + 1;
    }
  }
  
  return lines.size();
}

std::string joinLines(const std::vector<std::string>& lines, size_t start, size_t end) {
  std::string result;
  for (size_t i = start; i < end && i < lines.size(); ++i) {
    if (i > start) result += "\n";
    result += lines[i];
  }
  return result;
}

bool tryParseAndPrettyPrint(const std::string& json_text, std::string& out_pretty) {
  rapidjson::Document document;
  if (document.Parse(json_text.c_str()).HasParseError()) {
    return false;
  }
  
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  writer.SetIndent(' ', 2);
  document.Accept(writer);
  
  out_pretty = buffer.GetString();
  return true;
}

ftxui::Element renderValueSpan(const Theme &theme, const std::string &value) {
  auto trimmed = trimCopy(value);
  ftxui::Color color = theme.modals.fg;
  if (!trimmed.empty() && trimmed.front() == '"') {
    color = theme.syntax.string;
  } else if (trimmed == "true" || trimmed == "false" || trimmed == "null") {
    color = theme.syntax.keyword;
  } else if (!trimmed.empty() &&
             (std::isdigit(static_cast<unsigned char>(trimmed.front())) ||
              trimmed.front() == '-')) {
    color = theme.syntax.number;
  } else if (!trimmed.empty() &&
             (trimmed.front() == '{' || trimmed.front() == '[')) {
    color = theme.syntax.type;
  }
  return ftxui::paragraph(value) | ftxui::color(color);
}

ftxui::Element renderJsonLine(const Theme &theme, const std::string &line) {
  size_t indent_end = 0;
  while (indent_end < line.size() &&
         std::isspace(static_cast<unsigned char>(line[indent_end]))) {
    ++indent_end;
  }

  const std::string indent = line.substr(0, indent_end);
  const std::string rest = line.substr(indent_end);

  if (rest.empty()) {
    return ftxui::text(line);
  }

  if (rest.front() != '"') {
    return ftxui::hbox({
        ftxui::text(indent),
        renderValueSpan(theme, rest),
    });
  }

  const size_t key_end = rest.find('"', 1);
  if (key_end == std::string::npos) {
    return ftxui::text(line);
  }
  const size_t colon = rest.find(':', key_end + 1);
  if (colon == std::string::npos) {
    return ftxui::text(line);
  }

  const std::string key = rest.substr(0, colon + 1);
  const std::string after_colon = rest.substr(colon + 1);
  return ftxui::hbox({
      ftxui::text(indent),
      ftxui::text(key) | ftxui::bold | ftxui::color(theme.base.highlight),
      renderValueSpan(theme, after_colon),
  });
}

ftxui::Element renderJsonBlock(const Theme &theme, const std::string &json_text) {
  std::stringstream ss(json_text);
  std::string line;
  ftxui::Elements lines;
  while (std::getline(ss, line)) {
    lines.push_back(renderJsonLine(theme, line));
  }
  return ftxui::vbox(std::move(lines));
}

ftxui::Element metadataRow(const Theme &theme, const std::string &label, const std::string &content) {
  return ftxui::hbox({
      ftxui::text(label + ": ") | ftxui::color(theme.base.dim),
      ftxui::paragraph(content) | ftxui::color(theme.base.fg),
  });
}

} // namespace

ParsedErrorDetails ParseErrorDetails(const std::string &details) {
  ParsedErrorDetails parsed;
  if (details.empty()) return parsed;
  
  std::vector<std::string> lines = splitLines(details);
  if (lines.empty()) {
    parsed.headline = details;
    return parsed;
  }
  
  size_t idx = 0;
  
  // First non-empty line is the headline
  while (idx < lines.size() && trimCopy(lines[idx]).empty()) {
    ++idx;
  }
  if (idx < lines.size()) {
    parsed.headline = trimCopy(lines[idx]);
    ++idx;
  }
  
  // Parse metadata rows and look for raw body label
  bool found_raw_body_label = false;
  while (idx < lines.size()) {
    std::string line = lines[idx];
    std::string trimmed = trimCopy(line);
    
    if (trimmed.empty()) {
      ++idx;
      continue;
    }
    
    // Check for raw body label
    std::string body_label;
    if (looksLikeRawBodyLabel(line, body_label)) {
      parsed.raw_body_label = body_label;
      found_raw_body_label = true;
      ++idx;
      break;
    }
    
    // Check for metadata label
    std::string label, content;
    if (looksLikeMetadataLabel(line, label, content)) {
      parsed.metadata.push_back({label, content, true});
      ++idx;
    } else {
      // If it's not a label and not empty, it might be continuation of headline
      // or extra context before the raw body
      if (parsed.headline.length() < 200) {
        parsed.headline += " " + trimmed;
      }
      ++idx;
    }
  }
  
  // Parse raw body content (JSON or text)
  if (found_raw_body_label && idx < lines.size()) {
    // Check if the next line starts JSON
    std::string trimmed = trimCopy(lines[idx]);
    if (!trimmed.empty() && looksLikeJsonStart(trimmed[0])) {
      // Find the end of the JSON block
      size_t json_end = findJsonBlockEnd(lines, idx);
      parsed.raw_body_content = joinLines(lines, idx, json_end);
      
      // Try to parse and pretty-print the JSON
      if (tryParseAndPrettyPrint(parsed.raw_body_content, parsed.pretty_json)) {
        parsed.has_json = true;
      }
      
      idx = json_end;
    } else {
      // Collect lines until next metadata-like line or end
      size_t body_start = idx;
      while (idx < lines.size()) {
        std::string lbl, cont;
        std::string body_line = lines[idx];
        if (looksLikeMetadataLabel(body_line, lbl, cont) || 
            looksLikeRawBodyLabel(body_line, lbl)) {
          break;
        }
        ++idx;
      }
      parsed.raw_body_content = joinLines(lines, body_start, idx);
    }
  }
  
  // Remaining lines are trailing details
  if (idx < lines.size()) {
    parsed.trailing_details = trimCopy(joinLines(lines, idx, lines.size()));
  }
  
  // If no headline was found, use the whole details
  if (parsed.headline.empty()) {
    parsed.headline = trimCopy(details);
  }
  
  return parsed;
}

ftxui::Element RenderErrorDisplay(const Theme &theme,
                                   const firmius::shared::ErrorContent &error) {
  const auto parsed = ParseErrorDetails(error.details);
  ftxui::Elements body;

  // Error title/header
  body.push_back(ftxui::hbox({
                     ftxui::text(" ! ") | ftxui::bold |
                         ftxui::color(theme.status_bar.error.normal.fg),
                     ftxui::text(error.errorName) | ftxui::bold |
                         ftxui::color(theme.status_bar.error.normal.fg),
                 }) |
                 ftxui::xflex);

  if (!error.description.empty()) {
    body.push_back(ftxui::paragraph(error.description) |
                   ftxui::color(theme.base.fg));
  }

  if (!parsed.headline.empty() && parsed.headline != error.description) {
    body.push_back(ftxui::paragraph(parsed.headline) |
                   ftxui::color(theme.status_bar.error.normal.fg));
  }

  // Metadata rows
  if (!parsed.metadata.empty()) {
    ftxui::Elements meta_rows;
    for (const auto& meta : parsed.metadata) {
      meta_rows.push_back(metadataRow(theme, meta.label, meta.content));
    }
    body.push_back(ftxui::vbox(std::move(meta_rows)) | 
                   ftxui::bgcolor(theme.chat.markdown.code_bg) |
                   ftxui::color(theme.chat.markdown.code_fg));
  }

  // Raw body section
  if (!parsed.raw_body_content.empty()) {
    ftxui::Elements raw_body_elements;
    
    // Label
    if (!parsed.raw_body_label.empty()) {
      raw_body_elements.push_back(
          ftxui::text(parsed.raw_body_label) | ftxui::bold |
          ftxui::color(theme.base.highlight));
    }
    
    // Content - either pretty JSON or raw text
    if (parsed.has_json && !parsed.pretty_json.empty()) {
      raw_body_elements.push_back(renderJsonBlock(theme, parsed.pretty_json));
    } else {
      raw_body_elements.push_back(
          ftxui::paragraph(parsed.raw_body_content) | 
          ftxui::color(theme.chat.markdown.code_fg));
    }
    
    body.push_back(
        ftxui::vbox(std::move(raw_body_elements)) | 
        ftxui::bgcolor(theme.chat.markdown.code_bg) |
        ftxui::color(theme.chat.markdown.code_fg) |
        ftxui::xflex);
  }
  
  if (!parsed.trailing_details.empty()) {
    body.push_back(
        ftxui::paragraph(parsed.trailing_details) | 
        ftxui::color(theme.base.dim));
  }

  return ftxui::vbox(std::move(body)) | 
         ftxui::color(theme.status_bar.error.normal.fg) |
         ftxui::bgcolor(theme.status_bar.error.normal.bg) | 
         ftxui::xflex;
}

} // namespace firmius::tui
