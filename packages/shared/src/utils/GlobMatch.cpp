#include "utils/GlobMatch.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace firmius::shared::utils {

namespace {

std::string normalizeSlashes(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  return value;
}

bool isRegexMeta(char ch) {
  switch (ch) {
  case '.': case '^': case '$': case '|': case '(': case ')':
  case '[': case ']': case '{': case '}': case '+': case '?':
  case '*': case '\\': return true;
  default: return false;
  }
}

std::vector<std::string> splitBraceAlternatives(const std::string &body) {
  std::vector<std::string> parts;
  std::string current;
  int depth = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    const char ch = body[i];
    if (ch == '\\' && i + 1 < body.size()) {
      current.push_back(ch);
      current.push_back(body[++i]);
      continue;
    }
    if (ch == '{') { ++depth; current.push_back(ch); continue; }
    if (ch == '}') { --depth; current.push_back(ch); continue; }
    if (ch == ',' && depth == 0) {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  parts.push_back(current);
  return parts;
}

std::string globPatternToRegexBody(const std::string &pattern);

std::optional<std::pair<size_t, std::string>> parseBraceExpression(
    const std::string &pattern, size_t start) {
  int depth = 0;
  std::string body;
  for (size_t i = start; i < pattern.size(); ++i) {
    const char ch = pattern[i];
    if (ch == '\\' && i + 1 < pattern.size()) {
      body.push_back(ch);
      body.push_back(pattern[++i]);
      continue;
    }
    if (ch == '{') {
      ++depth;
      if (depth > 1) body.push_back(ch);
      continue;
    }
    if (ch == '}') {
      --depth;
      if (depth == 0) return std::make_pair(i, body);
      if (depth < 0) return std::nullopt;
      body.push_back(ch);
      continue;
    }
    body.push_back(ch);
  }
  return std::nullopt;
}

std::string globPatternToRegexBody(const std::string &pattern) {
  std::string regex;
  for (size_t i = 0; i < pattern.size(); ++i) {
    const char ch = pattern[i];
    if (ch == '\\' && i + 1 < pattern.size()) {
      const char literal = pattern[++i];
      if (isRegexMeta(literal)) regex.push_back('\\');
      regex.push_back(literal);
      continue;
    }
    if (ch == '*') {
      const bool doubleStar = (i + 1 < pattern.size() && pattern[i + 1] == '*');
      if (doubleStar) {
        while (i + 1 < pattern.size() && pattern[i + 1] == '*') ++i;
        if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
          ++i;
          regex += "(?:.*/)?";
        } else {
          regex += ".*";
        }
      } else {
        regex += "[^/]*";
      }
      continue;
    }
    if (ch == '?') { regex += "[^/]"; continue; }
    if (ch == '[') {
      size_t close = i + 1;
      while (close < pattern.size() && pattern[close] != ']') {
        if (pattern[close] == '\\' && close + 1 < pattern.size()) close += 2;
        else ++close;
      }
      if (close >= pattern.size()) { regex += "\\["; continue; }
      std::string charClass = pattern.substr(i + 1, close - i - 1);
      if (!charClass.empty() &&
          (charClass.front() == '!' || charClass.front() == '^')) {
        charClass.front() = '^';
      }
      regex += "["; regex += charClass; regex += "]";
      i = close;
      continue;
    }
    if (ch == '{') {
      auto brace = parseBraceExpression(pattern, i);
      if (brace.has_value()) {
        const auto alternatives = splitBraceAlternatives(brace->second);
        regex += "(?:";
        for (size_t idx = 0; idx < alternatives.size(); ++idx) {
          if (idx > 0) regex += "|";
          regex += globPatternToRegexBody(alternatives[idx]);
        }
        regex += ")";
        i = brace->first;
        continue;
      }
    }
    if (isRegexMeta(ch)) regex.push_back('\\');
    regex.push_back(ch);
  }
  return regex;
}

} // namespace

std::regex compileGlobRegex(const std::string &pattern) {
  return std::regex("^" + globPatternToRegexBody(normalizeSlashes(pattern)) + "$");
}

bool globMatches(const std::string &pattern, const std::string &value) {
  try {
    return std::regex_match(normalizeSlashes(value), compileGlobRegex(pattern));
  } catch (const std::regex_error&) {
    return false;
  }
}

} // namespace firmius::shared::utils
