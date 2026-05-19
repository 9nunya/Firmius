#ifndef FIRMIUS_SHARED_CRYPTOUTIL_HPP
#define FIRMIUS_SHARED_CRYPTOUTIL_HPP

#include <cstdint>
#include <string_view>
#include <vector>

namespace firmius::shared {

std::vector<uint8_t> sha256(std::string_view input);

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_CRYPTOUTIL_HPP
