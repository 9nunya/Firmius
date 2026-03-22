#ifndef FIRMIUS_TUI_MODEL_PICKER_ENTRIES_HPP
#define FIRMIUS_TUI_MODEL_PICKER_ENTRIES_HPP

#include "IProvider.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace firmius::tui {

struct ModelPickerEntry {
  std::string provider_id;
  std::string model_id;
  std::string variant_name;
  std::string label;
};

inline std::vector<ModelPickerEntry>
BuildModelPickerEntries(const std::vector<firmius::shared::ModelInfo> &models,
                        bool includeVariantRows = true) {
  std::vector<ModelPickerEntry> entries;
  for (const auto &model : models) {
    const std::string baseLabel = model.provider + "/" + model.id;
    entries.push_back(
        {model.provider, model.id, "", baseLabel + " (default variant)"});

    if (!includeVariantRows) {
      continue;
    }

    for (const auto &variant : model.variants) {
      if (variant.variantName.empty()) {
        continue;
      }
      entries.push_back({model.provider, model.id, variant.variantName,
                         baseLabel + " (" + variant.variantName + ")"});
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const ModelPickerEntry &lhs, const ModelPickerEntry &rhs) {
              return lhs.label < rhs.label;
            });
  return entries;
}

inline std::vector<int>
FilterModelPickerEntries(const std::vector<ModelPickerEntry> &entries,
                         const std::string &rawFilter) {
  std::string filter = rawFilter;
  std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

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

  std::vector<int> filtered;
  for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
    std::string lowered = entries[index].label;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);

    bool allTokensMatch = true;
    for (const auto &token : tokens) {
      if (lowered.find(token) == std::string::npos) {
        allTokensMatch = false;
        break;
      }
    }
    if (allTokensMatch) {
      filtered.push_back(index);
    }
  }

  return filtered;
}

} // namespace firmius::tui

#endif
