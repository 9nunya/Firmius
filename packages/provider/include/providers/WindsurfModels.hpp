#ifndef FIRMIUS_PROVIDER_WINDSURF_MODELS_HPP
#define FIRMIUS_PROVIDER_WINDSURF_MODELS_HPP

#include "Enums.hpp"

#include <optional>
#include <string>
#include <vector>

namespace firmius::provider::windsurf {

/**
 * @brief A resolved Windsurf model: canonical id + protobuf enum + optional
 * variant key (e.g. "thinking", "high").
 *
 * The wire request needs:
 *   - chat_model (int32 enum) — the variant-aware enum
 *   - chat_model_name (string)  — "<canonical>:<variant>" or just "<canonical>"
 */
struct ResolvedModel {
  std::string canonicalId;             ///< e.g. "claude-4.5-sonnet"
  std::string variant;                 ///< e.g. "thinking" or "" (none)
  int enumValue = 0;                   ///< Windsurf protobuf Model enum value
  std::string wireModelName;           ///< canonicalId or "canonicalId:variant"
  bool supportsThinkingVariant = false;
};

/**
 * @brief Resolve a user-supplied model id (with optional ":variant" suffix or
 * "-variant" suffix) to a Windsurf enum + canonical id. Returns std::nullopt if
 * the model is not recognized.
 *
 * Recognized forms:
 *   - "claude-4.5-sonnet"          → default variant
 *   - "claude-4.5-sonnet:thinking" → thinking variant
 *   - "claude-4.5-sonnet-thinking" → thinking variant
 *   - "gemini-3.0-pro:high"        → high reasoning effort variant
 *
 * @param modelId      User-supplied model identifier (case-insensitive).
 * @param variantHint  Optional explicit variant override (takes precedence).
 */
std::optional<ResolvedModel> resolveModel(
    const std::string &modelId,
    const std::optional<std::string> &variantHint = std::nullopt);

/**
 * @brief Returns ModelInfo for a canonical model id, with all variants
 * surfaced as ModelVariant entries. Returns std::nullopt for unknown ids.
 */
std::optional<firmius::shared::ModelInfo> getModelInfo(
    const std::string &canonicalId);

/**
 * @brief Lists every canonical Windsurf model (no variant duplicates).
 * Variants are nested in `ModelInfo::variants`.
 */
std::vector<firmius::shared::ModelInfo> listAllModels();

/**
 * @brief Default canonical model used when no model id is provided.
 */
std::string getDefaultModel();

/**
 * @brief Reverse lookup: protobuf enum → canonical model id (or empty).
 */
std::string canonicalIdFromEnum(int enumValue);

} // namespace firmius::provider::windsurf

#endif
