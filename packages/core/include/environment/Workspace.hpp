#ifndef FIRMIUS_CORE_WORKSPACE_HPP
#define FIRMIUS_CORE_WORKSPACE_HPP

#include "IEnvironment.hpp"
#include "utils/FastHash.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
    void recordFileRead(const std::string& path, int startLine, int endLine,
                        bool reachedEnd) override;
    bool hasFullyReadFile(const std::string& path) const override;
    void markFileAsFullyRead(const std::string& path) override;
    void recordFileEdit(const std::string& path) override;
    bool isLineRead(const std::string& path, int line) const override;
    std::string getCurrentWorkingDirectory() const override;

    /**
     * @brief Changes the current working directory.
     * @param cwd The new directory.
     */
    void setCurrentWorkingDirectory(const std::string& cwd);

private:
    struct ReadCoverage {
        std::vector<std::pair<int, int>> ranges;
        std::optional<int> terminalLine;
    };

    static void mergeRange(std::vector<std::pair<int, int>>& ranges,
                           int startLine, int endLine);
    static bool isFullyCovered(const ReadCoverage& coverage);

    std::string cwd_;
    mutable std::mutex fileMutex_;
    std::set<std::string> readFiles_;
    std::set<std::string> fullyReadFiles_;
    firmius::shared::utils::FastHash<std::string, ReadCoverage> readCoverage_;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_WORKSPACE_HPP
