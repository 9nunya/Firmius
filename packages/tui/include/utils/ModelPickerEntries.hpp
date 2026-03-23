#ifndef FIRMIUS_TUI_MODEL_PICKER_ENTRIES_HPP
#define FIRMIUS_TUI_MODEL_PICKER_ENTRIES_HPP

#include "IProvider.hpp"
#include "utils/ModelUtil.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::tui {

struct ModelPickerEntry {
  std::string provider_id;
  std::string model_id;
  std::string variant_name;
  std::string label;
  std::string title;
  std::string provider_label;
  std::string meta_label;
  std::string search_text;
};

inline std::string NormalizeModelSearchText(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char ch : value) {
    if (std::isalnum(ch)) {
      out.push_back(static_cast<char>(std::tolower(ch)));
    } else {
      out.push_back(' ');
    }
  }
  return out;
}

inline std::string JoinModelParts(const std::vector<std::string> &parts,
                                  const std::string &separator) {
  std::string out;
  for (const auto &part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += separator;
    }
    out += part;
  }
  return out;
}

inline std::string PrettifyVariantNameForPicker(const std::string &variant) {
  if (variant.empty()) {
    return "Default";
  }
  std::string out = variant;
  bool nextTitle = true;
  for (char &ch : out) {
    if (ch == '_' || ch == '-') {
      ch = ' ';
      nextTitle = true;
      continue;
    }
    ch = nextTitle ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))
                   : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    nextTitle = false;
  }
  return out;
}

inline std::string FormatContextWindowForPicker(std::uint32_t contextWindow) {
  if (contextWindow == 0) {
    return "";
  }
  if (contextWindow >= 1000000 && contextWindow % 1000000 == 0) {
    return std::to_string(contextWindow / 1000000) + "M ctx";
  }
  if (contextWindow >= 1000 && contextWindow % 1000 == 0) {
    return std::to_string(contextWindow / 1000) + "K ctx";
  }
  if (contextWindow >= 1000000) {
    std::ostringstream oss;
    oss << (contextWindow / 1000000.0) << "M ctx";
    return oss.str();
  }
  if (contextWindow >= 1000) {
    std::ostringstream oss;
    oss << (contextWindow / 1000.0) << "K ctx";
    return oss.str();
  }
  return std::to_string(contextWindow) + " ctx";
}

inline bool ModelSupportsVision(const firmius::shared::ModelInfo &model) {
  for (const auto &modality : model.modalities) {
    std::string lowered = modality;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
    if (lowered == "image" || lowered == "vision") {
      return true;
    }
  }
  return false;
}

inline std::vector<ModelPickerEntry>
BuildModelPickerEntries(const std::vector<firmius::shared::ModelInfo> &models,
                        bool includeVariantRows = true) {
  std::vector<ModelPickerEntry> entries;
  for (const auto &model : models) {
    const std::string prettyName = firmius::shared::PrettifyModelName(model.id);
    const auto makeEntry = [&](const std::string &variantName) {
      std::vector<std::string> metaParts;
      const std::string contextLabel = FormatContextWindowForPicker(model.contextWindow);
      if (!contextLabel.empty()) {
        metaParts.push_back(contextLabel);
      }
      metaParts.push_back(PrettifyVariantNameForPicker(variantName) + " variant");
      if (ModelSupportsVision(model)) {
        metaParts.push_back("Vision");
      }
      if (model.supportsReasoning) {
        metaParts.push_back("Thinking");
      }
      const std::string metaLabel = JoinModelParts(metaParts, "  ");
      const std::string lineLabel =
          prettyName + " [" + model.provider + "] " + metaLabel;
      const std::string searchText = NormalizeModelSearchText(
          JoinModelParts({prettyName, model.provider, model.id, variantName,
                          PrettifyVariantNameForPicker(variantName), metaLabel},
                         " "));
      entries.push_back({model.provider,
                         model.id,
                         variantName,
                         lineLabel,
                         prettyName,
                         model.provider,
                         metaLabel,
                         searchText});
    };

    makeEntry("");

    if (!includeVariantRows) {
      continue;
    }

    for (const auto &variant : model.variants) {
      if (variant.variantName.empty()) {
        continue;
      }
      makeEntry(variant.variantName);
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const ModelPickerEntry &lhs, const ModelPickerEntry &rhs) {
              if (lhs.title != rhs.title) {
                return lhs.title < rhs.title;
              }
              if (lhs.provider_label != rhs.provider_label) {
                return lhs.provider_label < rhs.provider_label;
              }
              if (lhs.variant_name.empty() != rhs.variant_name.empty()) {
                return lhs.variant_name.empty();
              }
              return lhs.variant_name < rhs.variant_name;
            });
  return entries;
}

inline bool FuzzyTokenMatches(const std::string &haystack,
                              const std::string &needle) {
  if (needle.empty()) {
    return true;
  }
  if (haystack.find(needle) != std::string::npos) {
    return true;
  }
  std::size_t cursor = 0;
  for (char ch : needle) {
    cursor = haystack.find(ch, cursor);
    if (cursor == std::string::npos) {
      return false;
    }
    ++cursor;
  }
  return true;
}

inline std::vector<int>
FilterModelPickerEntries(const std::vector<ModelPickerEntry> &entries,
                         const std::string &rawFilter) {
  const std::string filter = NormalizeModelSearchText(rawFilter);

  std::vector<std::string> tokens;
  size_t start = 0;
  while (start < filter.size()) {
    size_t end = filter.find(' ', start);
    if (end == std::string::npos) {
      end = filter.size();
    }
    if (end > start) {
      tokens.push_back(filter.substr(start, end - start));
    }
    start = end + 1;
  }

  std::vector<std::pair<int, int>> scored;
  for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
    bool allTokensMatch = true;
    int score = 0;
    for (const auto &token : tokens) {
      if (!FuzzyTokenMatches(entries[index].search_text, token)) {
        allTokensMatch = false;
        break;
      }
      if (entries[index].search_text.find(token) != std::string::npos) {
        score += 20;
      } else {
        score += 5;
      }
      if (NormalizeModelSearchText(entries[index].title).find(token) == 0 ||
          NormalizeModelSearchText(entries[index].model_id).find(token) == 0) {
        score += 10;
      }
    }
    if (allTokensMatch) {
      scored.push_back({-score, index});
    }
  }

  std::stable_sort(scored.begin(), scored.end());
  std::vector<int> filtered;
  filtered.reserve(scored.size());
  for (const auto &[_, index] : scored) {
    filtered.push_back(index);
  }
  return filtered;
}

} // namespace firmius::tui

#endif
