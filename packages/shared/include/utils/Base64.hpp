#ifndef FIRMIUS_SHARED_UTILS_BASE64_HPP
#define FIRMIUS_SHARED_UTILS_BASE64_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace firmius::shared {

/**
 * @brief Standard base64 encode (RFC 4648 alphabet, '=' padding).
 */
std::string base64Encode(const unsigned char* data, std::size_t size);
std::string base64Encode(std::string_view data);
std::string base64Encode(const std::vector<std::uint8_t>& data);

/**
 * @brief Standard base64 decode. Invalid characters are skipped; '='
 * pads are honored. Returns empty on totally-malformed input.
 */
std::vector<std::uint8_t> base64Decode(std::string_view input);

/**
 * @brief URL-safe base64 encode (uses '-' and '_' in place of '+' and '/',
 * strips '=' padding).
 */
std::string base64UrlEncode(const unsigned char* data, std::size_t size);
std::string base64UrlEncode(std::string_view data);
std::string base64UrlEncode(const std::vector<std::uint8_t>& data);

/**
 * @brief URL-safe base64 decode. Tolerates missing '=' padding.
 */
std::vector<std::uint8_t> base64UrlDecode(std::string_view input);

} // namespace firmius::shared

#endif
