#ifndef FIRMIUS_TUI_QUOTA_PRESENTER_HPP
#define FIRMIUS_TUI_QUOTA_PRESENTER_HPP

#include <string>
#include <vector>
#include <memory>

namespace firmius::provider {
    class IProvider;
}
namespace firmius::shared {
    struct QuotaBucket;
}

namespace firmius::tui::quota {

/**
 * @brief Formats a human-readable quota status string for the status bar.
 *
 * The presenter is provider-aware and tailors the output:
 *   - Codex: shows both 5h and weekly quotas (e.g., "󱑂 85% · 󰃭 92%")
 *   - Antigravity and others: shows the bucket matching the current model (e.g., "󰆧 73%")
 *
 * @param provider The LLM provider instance.
 * @param modelId The model identifier currently in use.
 * @param buckets Quota buckets for the active account.
 * @return A formatted string suitable for display in the status bar.
 */
std::string format(const std::shared_ptr<firmius::provider::IProvider>& provider,
                   const std::string& modelId,
                   const std::vector<firmius::shared::QuotaBucket>& buckets);

} // namespace firmius::tui::quota

#endif // FIRMIUS_TUI_QUOTA_PRESENTER_HPP
