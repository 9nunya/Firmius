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

  try {
    std::size_t pos = 0;
    long long val = std::stoll(trimmed, &pos);
    if (pos == trimmed.size()) {
      return FrontmatterValue{static_cast<int64_t>(val)};
    }
  } catch (...) {
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

  FrontmatterValue::Array values;
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
    values.push_back(FrontmatterValue{item});

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
    const std::string line = lines[i];
    const std::string trimmedLine = StringUtil::trim(line);
    if (trimmedLine.empty() || trimmedLine.front() == '#') {
      continue;
    }
    const auto colon = trimmedLine.find(':');
    if (colon == std::string::npos) {
      continue;
    }

    const std::string key = StringUtil::trim(trimmedLine.substr(0, colon));
    const std::string rawValue = trimmedLine.substr(colon + 1);
    if (key.empty()) {
      continue;
    }

    const std::string trimmedValue = StringUtil::trim(rawValue);
    if (!trimmedValue.empty()) {
      if (trimmedValue.front() == '[') {
        values[key] = parseArrayValue(trimmedValue, source, i + 1);
      } else {
        values[key] = parseScalarValue(rawValue);
      }
    } else {
      // Potential block structure (list of maps for WorkflowLoader)
      if (i + 1 < lines.size()) {
        std::size_t j = i + 1;
        while (j < lines.size() && StringUtil::trim(lines[j]).empty()) {
          j++;
        }
        if (j < lines.size()) {
          const std::string &nextLine = lines[j];
          // Check indentation: block must be indented
          bool hasIndent = false;
          for (char c : nextLine) {
            if (c == ' ' || c == '\t') {
              hasIndent = true;
              break;
            }
            if (!std::isspace(static_cast<unsigned char>(c))) break;
          }
          
          if (hasIndent && StringUtil::trim(nextLine)[0] == '-') {
            FrontmatterValue::Array array;
            std::optional<FrontmatterValue::Map> currentMap;
            while (j < lines.size()) {
              std::string l = lines[j];
              std::string lt = StringUtil::trim(l);
              if (lt.empty()) {
                j++;
                continue;
              }
              
              bool lineHasIndent = false;
              for (char c : l) {
                if (c == ' ' || c == '\t') {
                  lineHasIndent = true;
                  break;
                }
                if (!std::isspace(static_cast<unsigned char>(c))) break;
              }
              if (!lineHasIndent) break;

              if (lt[0] == '-') {
                if (currentMap) array.push_back(FrontmatterValue{*currentMap});
                currentMap = FrontmatterValue::Map{};
                lt = StringUtil::trim(lt.substr(1));
              }
              auto cPos = lt.find(':');
              if (cPos != std::string::npos) {
                if (!currentMap) currentMap = FrontmatterValue::Map{};
                std::string k = StringUtil::trim(lt.substr(0, cPos));
                std::string v = StringUtil::trim(lt.substr(cPos + 1));
                (*currentMap)[k] = parseScalarValue(v);
              }
              j++;
            }
            if (currentMap) array.push_back(FrontmatterValue{*currentMap});
            if (!array.empty()) {
              values[key] = FrontmatterValue{array};
              i = j - 1;
            }
          }
        }
      }
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
    std::vector<std::string> result;
    for (const auto &item : *arrayValue) {
      if (const auto *s = std::get_if<std::string>(&item.value)) {
        result.push_back(*s);
      }
    }
    return result;
  }
  throw std::runtime_error("Frontmatter key '" + key +
                           "' must be a string array");
}

std::optional<int64_t>
FrontmatterParser::getInt(const FrontmatterDocument &document,
                           const std::string &key) {
  const auto *value = find(document, key);
  if (!value) {
    return std::nullopt;
  }
  if (const auto *intValue = std::get_if<int64_t>(&value->value)) {
    return *intValue;
  }
  if (const auto *stringValue = std::get_if<std::string>(&value->value)) {
    try {
      return std::stoll(*stringValue);
    } catch (...) {
    }
  }
  throw std::runtime_error("Frontmatter key '" + key + "' must be an integer");
}

std::optional<FrontmatterValue::Map>
FrontmatterParser::getMap(const FrontmatterDocument &document,
                           const std::string &key) {
  const auto *value = find(document, key);
  if (!value) {
    return std::nullopt;
  }
  if (const auto *mapValue = std::get_if<FrontmatterValue::Map>(&value->value)) {
    return *mapValue;
  }
  throw std::runtime_error("Frontmatter key '" + key + "' must be a map");
}

std::optional<FrontmatterValue::Array>
FrontmatterParser::getArray(const FrontmatterDocument &document,
                             const std::string &key) {
  const auto *value = find(document, key);
  if (!value) {
    return std::nullopt;
  }
  if (const auto *arrayValue = std::get_if<FrontmatterValue::Array>(&value->value)) {
    return *arrayValue;
  }
  throw std::runtime_error("Frontmatter key '" + key + "' must be an array");
}

} // namespace firmius::shared
