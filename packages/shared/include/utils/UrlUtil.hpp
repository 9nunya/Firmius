#ifndef FIRMIUS_SHARED_URLUTIL_HPP
#define FIRMIUS_SHARED_URLUTIL_HPP

#include <map>
#include <string>
#include <string_view>

namespace firmius::shared {

std::string urlEncode(std::string_view value);
std::string objectToUrlEncoded(const std::map<std::string, std::string> &data);

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_URLUTIL_HPP
