#ifndef FIRMIUS_SHARED_PANIC_HPP
#define FIRMIUS_SHARED_PANIC_HPP

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

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
     * @brief Type alias for dynamic debug info callback.
     */
    using DebugInfoCallback = std::function<std::string()>;

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

    /**
     * @brief Sets a callback to be executed before the process aborts.
     * Useful for restoring terminal state in TUI applications.
     */
    static void setPrePanicCallback(std::function<void()> callback);

    /**
     * @brief Registers static debug information to be printed on panic.
     * @param name Unique identifier for this debug info entry.
     * @param content Static string content to display.
     */
    static void addExtraInfo(const std::string& name, const std::string& content);

    /**
     * @brief Registers dynamic debug information callback to be called on panic.
     * @param name Unique identifier for this debug info entry.
     * @param callback Function that returns a string to display.
     */
    static void addExtraInfo(const std::string& name, DebugInfoCallback callback);

    /**
     * @brief Removes previously registered debug information.
     * @param name The identifier of the debug info entry to remove.
     */
    static void removeExtraInfo(const std::string& name);

private:
    struct DebugInfoEntry {
        std::string staticContent;
        DebugInfoCallback dynamicCallback;
        bool isDynamic;

        explicit DebugInfoEntry(const std::string& content)
            : staticContent(content), isDynamic(false) {}

        explicit DebugInfoEntry(DebugInfoCallback callback)
            : dynamicCallback(std::move(callback)), isDynamic(true) {}
    };

    static std::mutex registryMutex;
    static std::unordered_map<std::string, DebugInfoEntry> extraInfoRegistry;

public:
    static void printExtraInfo();
};

/**
 * @brief Macro to trigger a panic with current location information.
 */
#define FIRMIUS_PANIC(msg) firmius::shared::Panic::trigger(msg, __FILE__, __LINE__)

}

#endif
