#ifndef FIRMIUS_CORE_WORKSPACE_HPP
#define FIRMIUS_CORE_WORKSPACE_HPP

#include "IEnvironment.hpp"
#include <set>
#include <mutex>
#include <string>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @brief Manages workspace operations and file tracking.
 */
class Workspace : public IWorkspace {
public:
    /**
     * @brief Constructs a Workspace.
     * @param cwd The current working directory.
     */
    explicit Workspace(std::string cwd);

    // IWorkspace implementation
    std::string resolvePath(const std::string& path) const override;
    bool hasReadFile(const std::string& path) const override;
    void markFileAsRead(const std::string& path) override;
    bool hasFullyReadFile(const std::string& path) const override;
    void markFileAsFullyRead(const std::string& path) override;
    std::string getCurrentWorkingDirectory() const override;

    /**
     * @brief Changes the current working directory.
     * @param cwd The new directory.
     */
    void setCurrentWorkingDirectory(const std::string& cwd);

private:
    std::string cwd_;
    mutable std::mutex fileMutex_;
    std::set<std::string> readFiles_;
    std::set<std::string> fullyReadFiles_;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_WORKSPACE_HPP