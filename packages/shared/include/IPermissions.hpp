#ifndef FIRMIUS_SHARED_IPERMISSIONS_HPP
#define FIRMIUS_SHARED_IPERMISSIONS_HPP

#include "ICommandIntent.hpp"
#include "Enums.hpp"
#include <functional>
#include <memory>

namespace firmius::shared {

/**
 * @brief Types of file/directory access.
 */
enum class AccessMode {
    READ,
    WRITE,
    EXECUTE
};

/**
 * @brief Interface for managing agent permissions and approval flows.
 */
class IPermissions {
public:
    virtual ~IPermissions() = default;

    /**
     * @brief Requests approval for executing a command.
     * @param command The command to execute.
     * @param intent The analyzed command intent.
     * @return User's permission response.
     */
    virtual PermissionResponse requestCommandApproval(
        const std::string& command,
        const CommandIntent& intent,
        const std::string& toolName = "") = 0;

    /**
     * @brief Requests approval for reading a file or directory.
     * @param absolutePath The file or directory path to read.
     * @return User's permission response.
     */
    virtual PermissionResponse requestReadApproval(
        const std::string& absolutePath) = 0;

    /**
     * @brief Requests approval for editing/writing a file.
     * @param absolutePath The file path to edit.
     * @return User's permission response.
     */
    virtual PermissionResponse requestEditApproval(
        const std::string& absolutePath) = 0;

    /**
     * @brief Checks if path access is allowed without prompting.
     * @param absolutePath The path to check.
     * @param mode The access mode.
     * @return True if access is allowed.
     */
    virtual bool checkPathAccess(
        const std::string& absolutePath,
        AccessMode mode) const = 0;

    /**
     * @brief Validates path access, throwing if denied.
     * @param absolutePath The path to validate.
     * @param mode The access mode.
     * @throws std::runtime_error if access is denied.
     */
    virtual void validatePathAccess(
        const std::string& absolutePath,
        AccessMode mode) const = 0;

    /**
     * @brief Checks if a command can execute without prompting.
     * @param intent The command intent.
     * @return True if allowed without approval.
     */
    virtual bool isCommandAllowed(const CommandIntent& intent) const = 0;

    /**
     * @brief Adds a command pattern to always allow.
     * @param pattern The command pattern.
     */
    virtual void allowCommandAlways(const std::string& pattern) = 0;

    /**
     * @brief Adds a command pattern to always deny.
     * @param pattern The command pattern.
     */
    virtual void denyCommandAlways(const std::string& pattern) = 0;

    /**
     * @brief Gets the intent analyzer for this permissions instance.
     * @return Reference to the intent analyzer.
     */
    virtual const ICommandIntentAnalyzer& getIntentAnalyzer() const = 0;

    /**
     * @brief Sets the thread permission mode for this permissions context.
     * @param mode The permission mode.
     */
    virtual void setApprovalMode(ThreadPermissionMode mode) = 0;
};

} // namespace firmius::shared

#endif // FIRMIUS_SHARED_IPERMISSIONS_HPP
