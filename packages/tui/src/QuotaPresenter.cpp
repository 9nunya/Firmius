#include "tui/QuotaPresenter.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

#include "IProvider.hpp"
#include "Enums.hpp"

namespace firmius::tui::quota {

namespace {

// Helper: convert string to lowercase
std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Icon selection based on bucket name keywords
std::string iconForBucket(const std::string& name) {
    std::string lower = toLowerCopy(name);
    if (lower.find("5h") != std::string::npos ||
        lower.find("hour") != std::string::npos) {
        return "󱑂"; // hourglass
    }
    if (lower.find("weekly") != std::string::npos ||
        lower.find("week") != std::string::npos) {
        return "󰃭"; // calendar week
    }
    if (lower.find("monthly") != std::string::npos ||
        lower.find("month") != std::string::npos) {
        return "󰃮"; // calendar month
    }
    if (lower.find("annual") != std::string::npos ||
        lower.find("year") != std::string::npos) {
        return "󰸗"; // year
    }
    if (lower.find("credit") != std::string::npos ||
        lower.find("balance") != std::string::npos) {
        return "󰆧"; // credit card
    }
    if (lower.find("qwen") != std::string::npos) {
        return "󰘦"; // qwen icon
    }
    // Default generic quota icon
    return "󰆧";
}

// Format a single bucket as "icon XX%"
std::string formatSingleBucket(const firmius::shared::QuotaBucket& bucket) {
    std::ostringstream oss;
    oss << iconForBucket(bucket.name) << " "
        << std::fixed << std::setprecision(0)
        << (bucket.remainingFraction * 100.0f) << "%";
    return oss.str();
}

// Normalize model/key strings for matching
std::string normalizeKey(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(value.begin(), value.end(), '_', '-');
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char c) { return std::isspace(c); }),
                value.end());
    return value;
}

// Default presenter: pick the bucket that best matches the modelId
std::string presentDefault(const std::vector<firmius::shared::QuotaBucket>& buckets,
                           const std::string& modelId) {
    if (buckets.empty()) {
        return "";
    }

    const std::string normalizedModel = normalizeKey(modelId);
    if (!normalizedModel.empty()) {
        // Exact match
        for (const auto& bucket : buckets) {
            if (normalizeKey(bucket.name) == normalizedModel) {
                return formatSingleBucket(bucket);
            }
        }
        // Partial match
        for (const auto& bucket : buckets) {
            std::string normalizedBucket = normalizeKey(bucket.name);
            if (normalizedBucket.empty()) continue;
            if (normalizedBucket.find(normalizedModel) != std::string::npos ||
                normalizedModel.find(normalizedBucket) != std::string::npos) {
                return formatSingleBucket(bucket);
            }
        }
    }

    // Fallback: first bucket
    return formatSingleBucket(buckets.front());
}

// Codex presenter: show both 5h and weekly quotas
std::string presentCodex(const std::vector<firmius::shared::QuotaBucket>& buckets) {
    std::string result;
    auto tryAddBucket = [&](const std::string& keyword,
                            const std::string& icon) {
        for (const auto& bucket : buckets) {
            std::string lower = toLowerCopy(bucket.name);
            if (lower.find(keyword) != std::string::npos) {
                if (!result.empty()) {
                    result += " · ";
                }
                result += icon + " " +
                          std::to_string(static_cast<int>(
                              bucket.remainingFraction * 100.0f)) + "%";
                break; // only one per keyword
            }
        }
    };

    tryAddBucket("5h", "󱑂");
    tryAddBucket("weekly", "󰃭");

    if (!result.empty()) {
        return result;
    }

    // Fallback: use first bucket with generic icon
    return formatSingleBucket(buckets.front());
}

} // namespace

std::string format(const std::shared_ptr<firmius::provider::IProvider>& provider,
                   const std::string& modelId,
                   const std::vector<firmius::shared::QuotaBucket>& buckets) {
    if (!provider) {
        return "";
    }
    const std::string id = provider->getId();
    if (id == "codex") {
        return presentCodex(buckets);
    }
    // For antigravity and all other providers, use default presenter
    return presentDefault(buckets, modelId);
}

} // namespace firmius::tui::quota
