#include "workflow/Workflow.hpp"
#include <algorithm>
#include <regex>

namespace firmius::core {

std::string Workflow::build(const std::vector<std::string> &args) const {
  std::string result = body;

  for (size_t i = 0; i < args.size(); ++i) {
    std::string placeholder = "$" + std::to_string(i + 1);
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
      result.replace(pos, placeholder.length(), args[i]);
      pos += args[i].length();
    }
  }

  // Remove any remaining unmatched placeholders
  std::regex unmatched(R"(\$[0-9]+)");
  result = std::regex_replace(result, unmatched, "");

  return result;
}

} // namespace firmius::core
