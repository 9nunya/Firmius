#ifndef FIRMIUS_TUI_UTILS_REFERENCE_AUTOCOMPLETE_HPP
#define FIRMIUS_TUI_UTILS_REFERENCE_AUTOCOMPLETE_HPP

#include "Context.hpp"
#include <string>
#include <vector>

namespace firmius::tui {

std::vector<std::string>
BuildFileReferenceSuggestions(const std::vector<std::string> &relativePaths,
                              const std::string &query,
                              std::size_t limit = 8);

std::vector<std::string> BuildArtifactReferenceSuggestions(
    const std::vector<shared::ThreadArtifactMetadata> &artifacts,
    const std::string &query, std::size_t limit = 8);

} // namespace firmius::tui

#endif
