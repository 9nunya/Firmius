#include "utils/StringUtil.hpp"
#include <algorithm>
#include <numeric>
#include <regex>
#include <sstream>
#include <uuid/uuid.h>
#include <vector>

namespace firmius::shared {

std::string StringUtil::trim(const std::string &s) {
  auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return "";
  auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, (last - first + 1));
}

std::string StringUtil::trim(std::string_view s) {
  auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return "";
  auto last = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(first, (last - first + 1)));
}

std::vector<std::string> StringUtil::split(const std::string &s,
                                           char delimiter) {
  std::vector<std::string> tokens;
  tokens.reserve(16);  // Reserve space for common case
  std::string token;
  std::istringstream tokenStream(s);
  while (std::getline(tokenStream, token, delimiter)) {
    tokens.push_back(trim(token));
  }
  return tokens;
}

std::string StringUtil::concat(const std::vector<std::string> &arr,
                               const std::string &str) {
  if (arr.empty())
    return "";
  
  // Pre-calculate total size to avoid reallocations
  size_t total_size = arr.size() * str.size();
  for (const auto &s : arr) {
    total_size += s.size();
  }
  
  std::string result;
  result.reserve(total_size);
  result = arr[0];
  for (size_t i = 1; i < arr.size(); ++i) {
    result += str;
    result += arr[i];
  }
  return result;
}

bool StringUtil::startsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool StringUtil::endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string StringUtil::toLower(const std::string &s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

std::string StringUtil::generateUuid() {
  uuid_t binuuid;
  uuid_generate_random(binuuid);
  char uuid[37];
  uuid_unparse_lower(binuuid, uuid);
  return std::string(uuid);
}

std::string StringUtil::shellEscape(std::string_view s) {
  std::string result;
  result.reserve(s.size() + 2);  // Reserve for worst case + quotes
  result = "'";
  for (char c : s) {
    if (c == '\'')
      result += "'\\''";
    else
      result += c;
  }
  result += "'";
  return result;
}

size_t StringUtil::levenshteinDistance(std::string_view s1,
                                       std::string_view s2) {
  if (s1.empty())
    return s2.size();
  if (s2.empty())
    return s1.size();

  std::vector<size_t> v0(s2.size() + 1);
  std::vector<size_t> v1(s2.size() + 1);

  for (size_t i = 0; i <= s2.size(); ++i)
    v0[i] = i;

  for (size_t i = 0; i < s1.size(); ++i) {
    v1[0] = i + 1;
    for (size_t j = 0; j < s2.size(); ++j) {
      size_t cost = (s1[i] == s2[j]) ? 0 : 1;
      v1[j + 1] = std::min({v1[j] + 1, v0[j + 1] + 1, v0[j] + cost});
    }
    v0 = v1;
  }
  return v0[s2.size()];
}

std::vector<size_t> StringUtil::findFuzzy(std::string_view text,
                                          std::string_view pattern,
                                          float threshold) {
  std::vector<size_t> matches;
  if (pattern.empty() || threshold > 1.0f)
    return matches;
  if (threshold <= 0.0f) {
    for (size_t i = 0; i <= text.size(); ++i)
      matches.push_back(i);
    return matches;
  }

  size_t n = pattern.length();
  if (text.length() < n)
    return matches;

  for (size_t i = 0; i <= text.length() - n; ++i) {
    std::string_view window = text.substr(i, n);
    size_t dist = levenshteinDistance(window, pattern);
    float similarity =
        1.0f - (static_cast<float>(dist) / static_cast<float>(n));
    if (similarity >= threshold) {
      matches.push_back(i);
    }
  }
  return matches;
}

std::string StringUtil::htmlToMarkdown(const std::string &html) {
  std::string md = html;

  // Headers
  md = std::regex_replace(
      md, std::regex("<h1[^>]*>(.*?)</h1>", std::regex::icase), "# $1\n");
  md = std::regex_replace(
      md, std::regex("<h2[^>]*>(.*?)</h2>", std::regex::icase), "## $1\n");
  md = std::regex_replace(
      md, std::regex("<h3[^>]*>(.*?)</h3>", std::regex::icase), "### $1\n");
  md = std::regex_replace(
      md, std::regex("<h4[^>]*>(.*?)</h4>", std::regex::icase), "#### $1\n");
  md = std::regex_replace(
      md, std::regex("<h5[^>]*>(.*?)</h5>", std::regex::icase), "##### $1\n");
  md = std::regex_replace(
      md, std::regex("<h6[^>]*>(.*?)</h6>", std::regex::icase), "###### $1\n");

  // Bold/Strong
  md = std::regex_replace(
      md, std::regex("<(b|strong)[^>]*>(.*?)</\\1>", std::regex::icase),
      "**$2**");

  // Italic/Em
  md = std::regex_replace(
      md, std::regex("<(i|em)[^>]*>(.*?)</\\1>", std::regex::icase), "*$2*");

  // Links
  md = std::regex_replace(
      md,
      std::regex("<a[^>]*href=\"([^\"]*)\"[^>]*>(.*?)</a>", std::regex::icase),
      "[$2]($1)");

  // Lists
  md = std::regex_replace(
      md, std::regex("<li[^>]*>(.*?)</li>", std::regex::icase), "- $1\n");
  md = std::regex_replace(
      md, std::regex("<ul[^>]*>(.*?)</ul>", std::regex::icase), "$1");

  // Paragraphs and Breaks
  md = std::regex_replace(
      md, std::regex("<p[^>]*>(.*?)</p>", std::regex::icase), "$1\n\n");
  md = std::regex_replace(md, std::regex("<br[^>]*>", std::regex::icase), "\n");

  // Strip all other tags
  md = std::regex_replace(md, std::regex("<[^>]*>"), "");

  return trim(md);
}

} // namespace firmius::shared
