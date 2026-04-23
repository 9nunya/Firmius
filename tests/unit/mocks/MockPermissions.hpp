#ifndef FIRMIUS_TEST_MOCK_PERMISSIONS_HPP
#define FIRMIUS_TEST_MOCK_PERMISSIONS_HPP

#include "IPermissions.hpp"
#include "environment/CommandIntentAnalyzer.hpp"
#include <vector>
#include <string>

namespace firmius::test {

using namespace firmius::shared;

/**
 * @brief Mock implementation of IPermissions for unit testing.
 */
class MockPermissions : public IPermissions {
public:
    virtual ~MockPermissions() = default;
    std::vector<std::string> allowedPaths_;
    bool allowOutsideCwd_ = true;
    std::string cwd_ = "/tmp/work";
    PermissionResponse commandApprovalResponse_ = PermissionResponse::AllowAlways;
    PermissionResponse editApprovalResponse_ = PermissionResponse::AllowAlways;
    mutable std::vector<std::string> requestedCommands_;
    mutable std::vector<std::string> requestedEditPaths_;
    firmius::core::CommandIntentAnalyzer analyzer_;

    PermissionResponse requestCommandApproval(const std::string& command,
                                           const CommandIntent& /*intent*/,
                                           const std::string& toolName = "") override {
        requestedCommands_.push_back(command);
        if (!toolName.empty()) requestedCommands_.push_back("tool:" + toolName);
        return commandApprovalResponse_;
    }

    PermissionResponse requestReadApproval(const std::string& absolutePath) override {
        requestedEditPaths_.push_back(absolutePath);
        return editApprovalResponse_;
    }

    PermissionResponse requestEditApproval(const std::string& absolutePath) override {
        requestedEditPaths_.push_back(absolutePath);
        return editApprovalResponse_;
    }

    bool checkPathAccess(const std::string& absolutePath,
                        AccessMode /*mode*/) const override {
        if (allowOutsideCwd_) {
            return true;
        }

        for (const auto& allowed : allowedPaths_) {
            if (absolutePath.starts_with(allowed)) {
                return true;
            }
        }

        if (absolutePath.starts_with(cwd_)) {
            return true;
        }

        return false;
    }

    void validatePathAccess(const std::string& absolutePath,
                          AccessMode mode) const override {
        if (checkPathAccess(absolutePath, AccessMode::READ)) {
            if (mode == AccessMode::WRITE) {
                requestedEditPaths_.push_back(absolutePath);
                if (editApprovalResponse_ == PermissionResponse::Deny) {
                    throw std::runtime_error("Write access denied: " + absolutePath);
                }
            }
            return;
        }

        if (mode == AccessMode::READ || mode == AccessMode::EXECUTE) {
            requestedEditPaths_.push_back(absolutePath);
            if (editApprovalResponse_ == PermissionResponse::Deny) {
                throw std::runtime_error("Read access denied: " + absolutePath);
            }
            return;
        }

        requestedEditPaths_.push_back(absolutePath);
        if (editApprovalResponse_ == PermissionResponse::Deny) {
            throw std::runtime_error("Write access denied: " + absolutePath);
        }
    }

    bool isCommandAllowed(const CommandIntent& /*intent*/) const override {
        return true;
    }

    void allowCommandAlways(const std::string& /*pattern*/) override {}
    void denyCommandAlways(const std::string& /*pattern*/) override {}

    const ICommandIntentAnalyzer& getIntentAnalyzer() const override {
        return analyzer_;
    }

    void setApprovalMode(ThreadPermissionMode /*mode*/) override {}
};

} // namespace firmius::test

#endif // FIRMIUS_TEST_MOCK_PERMISSIONS_HPP
