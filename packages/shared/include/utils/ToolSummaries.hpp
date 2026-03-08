#ifndef FIRMIUS_SHARED_UTILS_TOOL_SUMMARIES_HPP
#define FIRMIUS_SHARED_UTILS_TOOL_SUMMARIES_HPP

#include "utils/ToolView.hpp"
#include <string>
#include <vector>

namespace firmius::shared {

std::string baseName(const std::string &path);
std::string firstWords(const std::string &s, int n);
std::vector<std::string> TailLines(const std::string &text, int maxLines);
std::string SummarizeToolCall(const std::string &name, const std::string &args, ToolPhase phase);

} // namespace firmius::shared

#endif
