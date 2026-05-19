#ifndef FIRMIUS_SHARED_UTILS_SHELLUTIL_HPP
#define FIRMIUS_SHARED_UTILS_SHELLUTIL_HPP

#include <string>
#include <string_view>

namespace firmius::shared {

/**
 * @brief Wrap a value in single quotes for safe POSIX-shell quoting.
 *
 * Produces `'value'` with embedded single quotes converted to the
 * standard `'\''` sequence. Resulting string is always valid shell
 * input even when the value contains spaces, $-expansions, or
 * single quotes.
 */
std::string shellQuoteSingle(std::string_view value);

/**
 * @brief Escape `"` for inclusion inside a Windows cmd.exe quoted
 * command line. The caller is responsible for wrapping the returned
 * value in `"..."`.
 *
 * Intended use:
 *     `"cmd.exe /S /C \"" + escapeForCmd(command) + "\""`.
 */
std::string escapeForCmd(std::string_view command);

} // namespace firmius::shared

#endif
