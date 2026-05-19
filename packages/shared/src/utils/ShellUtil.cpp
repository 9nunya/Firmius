#include "utils/ShellUtil.hpp"

namespace firmius::shared {

std::string shellQuoteSingle(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string escapeForCmd(std::string_view command) {
    std::string escaped;
    escaped.reserve(command.size() + 8);
    for (char ch : command) {
        if (ch == '"') {
            escaped += '\\';
        }
        escaped += ch;
    }
    return escaped;
}

} // namespace firmius::shared
