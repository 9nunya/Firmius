#ifndef FIRMIUS_SHARED_ERRORCLEANER_HPP
#define FIRMIUS_SHARED_ERRORCLEANER_HPP

#include <string>

namespace firmius::shared {

/**
 * @brief Utility to clean up raw JSON or infrastructure noise from error
 * messages.
 */
class ErrorCleaner {
public:
  static std::string clean(const std::string &error);
};

} // namespace firmius::shared

#endif
