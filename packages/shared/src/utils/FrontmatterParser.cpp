#include "utils/FrontmatterParser.hpp"

#include "utils/StringUtil.hpp"

#include <cctype>
#include <sstream>
#include <stdexcept>

namespace firmius::shared {

namespace {

std::vector<std::string> splitLines(std::string_view text) {
  std::vector<std::string> lines;
  std::string current;
  for (char ch : text) {
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  lines.push_back(current);
  return lines;
}

std::string normalizeScalar(std::string value) {
  value = StringUtil::trim(value);
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return StringUtil::trim(value);
}

bool parseBoolToken(const std::string &token, bool &value) {
  const std::string lowered = StringUtil::toLower(StringUtil::trim(token));
  if (lowered == "true" || lowered == "yes" || lowered == "1") {
    value = true;
    return true;
  }
  if (lowered == "false" || lowered == "no" || lowered == "0") {
    value = false;
    return true;
  }
  return false;
}

FrontmatterValue parseScalarValue(const std::string &rawValue) {
  const std::string trimmed = StringUtil::trim(rawValue);
  if (trimmed.empty()) {
    return FrontmatterValue{std::string{}};
  }

  if (trimmed.size() >= 2 &&
      ((trimmed.front() == '"' && trimmed.back() == '"') ||
       (trimmed.front() == '\'' && trimmed.back() == '\''))) {
    return FrontmatterValue{normalizeScalar(trimmed)};
  }

  bool boolValue = false;
  if (parseBoolToken(trimmed, boolValue)) {
    return FrontmatterValue{boolValue};
  }

  return FrontmatterValue{trimmed};
}

std::string frontmatterError(const std::string &source, std::size_t lineNumber,
                            const std::string &message) {
  std::ostringstream out;
  if (!source.empty()) {
    out << source << ": ";
  }
  if (lineNumber > 0) {
    out << "line " << lineNumber << ": ";
  }
  out << message;
  return out.str();
}

FrontmatterValue parseArrayValue(const std::string &rawValue,
                                const std::string &source,
                                std::size_t lineNumber) {
  const std::string trimmed = StringUtil::trim(rawValue);
  if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
    throw std::runtime_error(frontmatterError(
        source, lineNumber, "Expected a bracketed string array"));
  }

  std::vector<std::string> values;
  const std::string inner = trimmed.substr(1, trimmed.size() - 2);
  std::size_t i = 0;
  while (i < inner.size()) {
    while (i < inner.size() &&
           std::isspace(static_cast<unsigned char>(inner[i]))) {
      ++i;
    }
    if (i >= inner.size()) {
      break;
    }

    std::string item;
    if (inner[i] == '"' || inner[i] == '\'') {
      const char quote = inner[i++];
      bool escaped = false;
      while (i < inner.size()) {
        const char ch = inner[i++];
        if (escaped) {
          item.push_back(ch);
          escaped = false;
          continue;
        }
        if (ch == '\\') {
          escaped = true;
          continue;
        }
        if (ch == quote) {
          break;
        }
        item.push_back(ch);
      }
      if (i > inner.size() || inner[i - 1] != quote) {
        throw std::runtime_error(frontmatterError(
            source, lineNumber, "Unterminated quoted array element"));
      }
    } else {
      const std::size_t start = i;
      while (i < inner.size() && inner[i] != ',') {
        ++i;
      }
      item = StringUtil::trim(inner.substr(start, i - start));
    }

    item = StringUtil::trim(item);
    if (item.empty()) {
      throw std::runtime_error(frontmatterError(
          source, lineNumber, "Array elements must not be empty"));
    }
    values.push_back(item);

    while (i < inner.size() &&
           std::isspace(static_cast<unsigned char>(inner[i]))) {
      ++i;
    }
    if (i >= inner.size()) {
      break;
    }
    if (inner[i] != ',') {
      throw std::runtime_error(frontmatterError(
          source, lineNumber, "Expected ',' between array elements"));
    }
    ++i;
    while (i < inner.size() &&
           std::isspace(static_cast<unsigned char>(inner[i]))) {
      ++i;
    }
    if (i >= inner.size()) {
      throw std::runtime_error(frontmatterError(
          source, lineNumber, "Trailing commas are not allowed in arrays"));
    }
  }

  return FrontmatterValue{values};
}

} // namespace

FrontmatterDocument FrontmatterParser::parseMarkdown(std::string_view markdown,
                                                     const std::string &source) {
  FrontmatterDocument document;
  const auto lines = splitLines(markdown);

  std::size_t lineIndex = 0;
  while (lineIndex < lines.size() && StringUtil::trim(lines[lineIndex]).empty()) {
    ++lineIndex;
  }

  if (lineIndex >= lines.size() || StringUtil::trim(lines[lineIndex]) != "---") {
    document.body = std::string(markdown);
    return document;
  }

  ++lineIndex;
  std::string frontmatter;
  bool closed = false;
  for (; lineIndex < lines.size(); ++lineIndex) {
    const std::string &line = lines[lineIndex];
    if (StringUtil::trim(line) == "---") {
      closed = true;
      ++lineIndex;
      break;
    }
    frontmatter += line;
    frontmatter.push_back('\n');
  }

  if (!closed) {
    throw std::runtime_error(
        frontmatterError(source, 0, "Unterminated frontmatter block"));
  }

  document.values = parse(frontmatter, source);

  std::ostringstream body;
  for (std::size_t i = lineIndex; i < lines.size(); ++i) {
    body << lines[i];
    if (i + 1 < lines.size()) {
      body << '\n';
    }
  }
  document.body = body.str();
  return document;
}

std::map<std::string, FrontmatterValue>
FrontmatterParser::parse(std::string_view frontmatter, const std::string &source) {
  std::map<std::string, FrontmatterValue> values;
  const auto lines = splitLines(frontmatter);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string line = StringUtil::trim(lines[i]);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      throw std::runtime_error(frontmatterError(
          source, i + 1, "Expected 'key: value' metadata line"));
    }

    const std::string key = StringUtil::trim(line.substr(0, colon));
    const std::string rawValue = line.substr(colon + 1);
    if (key.empty()) {
      throw std::runtime_error(
          frontmatterError(source, i + 1, "Frontmatter key must not be empty"));
    }

    const std::string trimmedValue = StringUtil::trim(rawValue);
    if (!trimmedValue.empty() && trimmedValue.front() == '[') {
      values[key] = parseArrayValue(trimmedValue, source, i + 1);
    } else {
      values[key] = parseScalarValue(rawValue);
    }
  }
  return values;
}

const FrontmatterValue *
FrontmatterParser::find(const FrontmatterDocument &document,
                        const std::string &key) {
  auto it = document.values.find(key);
  if (it == document.values.end()) {
    return nullptr;
  }
  return &it->second;
}

std::optional<std::string>
FrontmatterParser::getString(const FrontmatterDocument &document,
                             const std::string &key) {
  const auto *value = find(document, key);
  if (!value) {
    return std::nullopt;
  }
  if (const auto *stringValue = std::get_if<std::string>(&value->value)) {
    return *stringValue;
  }
  throw std::runtime_error("Frontmatter key '" + key +
                           "' must be a string value");
}

std::optional<bool>
FrontmatterParser::getBool(const FrontmatterDocument &document,
                           const std::string &key) {
  const auto *value = find(document, key);
  if (!value) {
    return std::nullopt;
  }
  if (const auto *boolValue = std::get_if<bool>(&value->value)) {
    return *boolValue;
  }
  if (const auto *stringValue = std::get_if<std::string>(&value->value)) {
    bool parsed = false;
    if (parseBoolToken(*stringValue, parsed)) {
      return parsed;
    }
  }
  throw std::runtime_error("Frontmatter key '" + key +
                           "' must be a boolean value");
}

std::vector<std::string>
FrontmatterParser::getStringArray(const FrontmatterDocument &document,
                                 const std::string &key) {
  const auto *value = find(document, key);
  if (!value) {
    return {};
  }
  if (const auto *arrayValue = std::get_if<FrontmatterValue::Array>(&value->value)) {
    return *arrayValue;
  }
  throw std::runtime_error("Frontmatter key '" + key +
                           "' must be a string array");
}

} // namespace firmius::shared
