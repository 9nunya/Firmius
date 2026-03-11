#include "workflow/Workflow.hpp"
#include <algorithm>
#include <regex>
#include <stdexcept>

namespace firmius::core {

std::string Workflow::build(const std::vector<std::string> &args) const {
  // Validate required arguments
  for (size_t i = 0; i < args.size() && i < this->args.size(); ++i) {
    if (!this->args[i].optional && args[i].empty()) {
      throw std::runtime_error("Missing required argument: " + this->args[i].name);
    }
  }

  // Check that all required args are provided
  for (size_t i = args.size(); i < this->args.size(); ++i) {
    if (!this->args[i].optional) {
      throw std::runtime_error("Missing required argument: " + this->args[i].name);
    }
  }

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
