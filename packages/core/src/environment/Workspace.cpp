#include "environment/Workspace.hpp"
#include "utils/FSUtil.hpp"
#include <algorithm>

namespace firmius::core {

Workspace::Workspace(std::string cwd)
    : cwd_(std::move(cwd))
{
}

std::string Workspace::resolvePath(const std::string& path) const {
    return FSUtil::resolvePath(path, cwd_);
}

bool Workspace::hasReadFile(const std::string& path) const {
    std::lock_guard<std::mutex> lock(fileMutex_);
    return readFiles_.count(path) > 0;
}

void Workspace::markFileAsRead(const std::string& path) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    readFiles_.insert(path);
}

bool Workspace::hasFullyReadFile(const std::string& path) const {
    std::lock_guard<std::mutex> lock(fileMutex_);
    return fullyReadFiles_.count(path) > 0;
}

void Workspace::markFileAsFullyRead(const std::string& path) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    readFiles_.insert(path);
    fullyReadFiles_.insert(path);
}

std::string Workspace::getCurrentWorkingDirectory() const {
    return cwd_;
}

void Workspace::setCurrentWorkingDirectory(const std::string& cwd) {
    cwd_ = cwd;
}

} // namespace firmius::core