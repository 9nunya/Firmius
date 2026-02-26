#ifndef FIRMIUS_SHARED_PANIC_HPP
#define FIRMIUS_SHARED_PANIC_HPP

#include <string>

/**
 * @brief Global crash and exception handling utility.
 */
namespace firmius::shared {

/**
 * @brief Manages engine panics and provides symbolicated backtraces.
 */
class Panic {
public:
    /**
     * @brief Initializes the global terminate handler for unhandled exceptions.
     */
    static void init();

    /**
     * @brief Manually triggers a panic with file and line information.
     * @param message The panic message.
     * @param file The source file where the panic occurred.
     * @param line The line number.
     */
    static void trigger(const std::string& message, const char* file, int line);
};

/**
 * @brief Macro to trigger a panic with current location information.
 */
#define FIRMIUS_PANIC(msg) firmius::shared::Panic::trigger(msg, __FILE__, __LINE__)

}

#endif
