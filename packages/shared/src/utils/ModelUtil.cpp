#include "utils/ModelUtil.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace firmius::shared {

namespace {

std::string capitalize(const std::string &s) {
  if (s.empty())
    return s;

  // Check if it's "gpt" (special case)
  std::string lower = s;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower == "gpt")
    return "GPT";

  // Check if it's a version number (starts with digit or has dots/digits only)
  bool is_version = true;
  bool has_digit = false;
  for (char c : s) {
    if (!std::isdigit(c) && c != '.') {
      is_version = false;
      break;
    }
    if (std::isdigit(c))
      has_digit = true;
  }
  if (is_version && has_digit)
    return s;

  std::string result = s;
  result[0] = std::toupper(static_cast<unsigned char>(result[0]));
  return result;
}

} // namespace

std::string PrettifyModelName(const std::string &model_slug) {
  if (model_slug.empty())
    return "";

  std::string name = model_slug;

  // 1. Strip organization prefix
  size_t slash_pos = name.find('/');
  if (slash_pos != std::string::npos) {
    name = name.substr(slash_pos + 1);
  }

  // 2. Split by hyphens
  std::vector<std::string> parts;
  std::stringstream ss(name);
  std::string item;
  while (std::getline(ss, item, '-')) {
    if (!item.empty()) {
      parts.push_back(item);
    }
  }

  // 3. Process and Join
  std::string pretty;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0)
      pretty += " ";
    pretty += capitalize(parts[i]);
  }

  return pretty;
}

} // namespace firmius::shared
