#ifndef FIRMIUS_SHARED_MODELUTIL_HPP
#define FIRMIUS_SHARED_MODELUTIL_HPP

#include <string>

namespace firmius::shared {

/**
 * @brief Converts a model slug-name (e.g. gemini-3-flash,
 * stepfun-ai/step-3.5-flash) into a human-readable pretty name (e.g. Gemini 3
 * Flash, Step 3.5 Flash).
 *
 * Rules:
 * 1. Remove organization prefix (everything before '/')
 * 2. Split by hyphens
 * 3. Capitalize words (GPT is special-cased to uppercase)
 * 4. Keep version numbers and dots as-is
 */
std::string PrettifyModelName(const std::string &model_slug);

} // namespace firmius::shared

#endif
