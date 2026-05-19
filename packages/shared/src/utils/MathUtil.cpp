#include "utils/MathUtil.hpp"

#include <cmath>

namespace firmius::shared {

float cosineSimilarity(const std::vector<float>& a,
                       const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0f;
    }
    float dot = 0.0f;
    float normA = 0.0f;
    float normB = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    const float denom = std::sqrt(normA) * std::sqrt(normB);
    return denom > 0.0f ? dot / denom : 0.0f;
}

} // namespace firmius::shared
