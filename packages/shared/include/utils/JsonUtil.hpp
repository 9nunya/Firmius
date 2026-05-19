#ifndef FIRMIUS_SHARED_JSONUTIL_HPP
#define FIRMIUS_SHARED_JSONUTIL_HPP

#include <rapidjson/fwd.h>
#include <string>

namespace firmius::shared {

/**
 * @brief Serialize any rapidjson Value/Document to a compact JSON string.
 *
 * Equivalent to constructing a StringBuffer + Writer<StringBuffer> +
 * `value.Accept(writer)`. Provided here so the same one-liner doesn't
 * get re-implemented in every caller.
 */
std::string toJsonString(const rapidjson::Value& value);

} // namespace firmius::shared

#endif
