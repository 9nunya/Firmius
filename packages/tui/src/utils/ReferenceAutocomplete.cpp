#include "utils/ReferenceAutocomplete.hpp"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace firmius::tui {

namespace {

std::string toLowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool fuzzyMatchIgnoreCase(const std::string &text, const std::string &query) {
  if (query.empty()) {
    return true;
  }
  auto comp = [](char lhs, char rhs) {
    return std::tolower(static_cast<unsigned char>(lhs)) ==
           std::tolower(static_cast<unsigned char>(rhs));
  };
  return std::search(text.begin(), text.end(), query.begin(), query.end(),
                     comp) != text.end();
}

void sortWithPrefixBias(std::vector<std::string> &items,
                        const std::string &query) {
  std::sort(items.begin(), items.end(),
            [&query](const std::string &lhs, const std::string &rhs) {
              const std::string lowerLhs = toLowerAscii(lhs);
              const std::string lowerRhs = toLowerAscii(rhs);
              const bool lhsPrefix =
                  !query.empty() && lowerLhs.rfind(query, 0) == 0;
              const bool rhsPrefix =
                  !query.empty() && lowerRhs.rfind(query, 0) == 0;
              if (lhsPrefix != rhsPrefix) {
                return lhsPrefix;
              }
              return lhs < rhs;
            });
}

} // namespace

std::vector<std::string>
BuildFileReferenceSuggestions(const std::vector<std::string> &relativePaths,
                              const std::string &query, std::size_t limit) {
  std::vector<std::string> matches;
  matches.reserve(relativePaths.size());
  const std::string lowerQuery = toLowerAscii(query);
  for (const auto &path : relativePaths) {
    if (fuzzyMatchIgnoreCase(path, lowerQuery)) {
      matches.push_back(path);
    }
  }
  sortWithPrefixBias(matches, lowerQuery);
  matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
  if (matches.size() > limit) {
    matches.resize(limit);
  }
  return matches;
}

std::vector<std::string> BuildArtifactReferenceSuggestions(
    const std::vector<shared::ThreadArtifactMetadata> &artifacts,
    const std::string &query, std::size_t limit) {
  std::vector<std::string> matches;
  std::unordered_map<std::string, int> filenameCounts;
  filenameCounts.reserve(artifacts.size());
  for (const auto &artifact : artifacts) {
    filenameCounts[artifact.filename]++;
  }

  const std::string lowerQuery = toLowerAscii(query);
  for (const auto &artifact : artifacts) {
    const std::string owner = artifact.ownerFriendlyName.empty()
                                  ? artifact.ownerAgentId
                                  : artifact.ownerFriendlyName;
    const bool ambiguous = filenameCounts[artifact.filename] > 1;
    const std::string display =
        ambiguous ? (owner + "/" + artifact.filename) : artifact.filename;
    if (fuzzyMatchIgnoreCase(display, lowerQuery)) {
      matches.push_back(display);
    }
  }

  sortWithPrefixBias(matches, lowerQuery);
  matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
  if (matches.size() > limit) {
    matches.resize(limit);
  }
  return matches;
}

} // namespace firmius::tui
