#ifndef FIRMIUS_SHARED_MATHUTIL_HPP
#define FIRMIUS_SHARED_MATHUTIL_HPP

#include <cstddef>
#include <vector>

namespace firmius::shared {

/**
 * @brief Cosine similarity between two equal-length float vectors.
 *
 * Returns a value in [-1, 1] where 1 means identical direction, 0 means
 * orthogonal, -1 means opposite direction. Returns 0 when either vector
 * is empty, the lengths mismatch, or either norm is zero.
 *
 * Single-precision throughout: input vectors are float; intermediate
 * dot-product / norms are accumulated as float; result is float. Callers
 * that want a double can `static_cast<double>(cosineSimilarity(...))`.
 */
float cosineSimilarity(const std::vector<float>& a,
                       const std::vector<float>& b);

} // namespace firmius::shared

#endif
