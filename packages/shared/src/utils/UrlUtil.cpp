#include "utils/UrlUtil.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace firmius::shared {

namespace {

bool isUnreservedChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

} // namespace

std::string urlEncode(std::string_view value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;
  for (unsigned char c : value) {
    if (isUnreservedChar(static_cast<char>(c))) {
      escaped << c;
    } else {
      escaped << '%' << std::uppercase << std::setw(2) << static_cast<int>(c)
              << std::nouppercase;
    }
  }
  return escaped.str();
}

std::string objectToUrlEncoded(const std::map<std::string, std::string> &data) {
  std::string result;
  bool first = true;
  for (const auto &[key, value] : data) {
    if (!first)
      result += "&";
    first = false;
    result += urlEncode(key) + "=" + urlEncode(value);
  }
  return result;
}

} // namespace firmius::shared
