#include "environment/Workspace.hpp"
#include "utils/FSUtil.hpp"
#include <algorithm>

namespace firmius::core {

using namespace firmius::shared;

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

void Workspace::mergeRange(std::vector<std::pair<int, int>>& ranges,
                           int startLine, int endLine) {
    if (startLine <= 0 || endLine < startLine) {
        return;
    }

    ranges.push_back({startLine, endLine});
    std::sort(ranges.begin(), ranges.end());

    std::vector<std::pair<int, int>> merged;
    for (const auto& range : ranges) {
        if (merged.empty() || range.first > merged.back().second + 1) {
            merged.push_back(range);
            continue;
        }
        merged.back().second = std::max(merged.back().second, range.second);
    }
    ranges = std::move(merged);
}

bool Workspace::isFullyCovered(const ReadCoverage& coverage) {
    if (!coverage.terminalLine.has_value() || coverage.ranges.empty()) {
        return false;
    }

    return coverage.ranges.size() == 1 && coverage.ranges.front().first <= 1 &&
           coverage.ranges.front().second >= *coverage.terminalLine;
}

void Workspace::recordFileRead(const std::string& path, int startLine,
                               int endLine, bool reachedEnd) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    readFiles_.insert(path);

    if (fullyReadFiles_.count(path) > 0) {
        return;
    }

    auto& coverage = readCoverage_[path];
    mergeRange(coverage.ranges, startLine, endLine);
    if (reachedEnd && endLine >= startLine) {
        coverage.terminalLine = endLine;
    }

    if (isFullyCovered(coverage)) {
        fullyReadFiles_.insert(path);
    }
}

bool Workspace::hasFullyReadFile(const std::string& path) const {
    std::lock_guard<std::mutex> lock(fileMutex_);
    return fullyReadFiles_.count(path) > 0;
}

void Workspace::markFileAsFullyRead(const std::string& path) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    readFiles_.insert(path);
    fullyReadFiles_.insert(path);
    readCoverage_.erase(path);

}

void Workspace::recordFileEdit(const std::string& path) {
    std::lock_guard<std::mutex> lock(fileMutex_);
    readFiles_.erase(path);
    fullyReadFiles_.erase(path);
    readCoverage_.erase(path);
}

bool Workspace::isLineRead(const std::string& path, int line) const {
    std::lock_guard<std::mutex> lock(fileMutex_);
    if (fullyReadFiles_.count(path)) {
        return true;
    }
    auto coverage = readCoverage_.get(path);
    if (!coverage) {
        return false;
    }
    for (const auto& range : coverage->ranges) {
        if (line >= range.first && line <= range.second) {
            return true;
        }
    }
    return false;
}

std::string Workspace::getCurrentWorkingDirectory() const {
    return cwd_;
}

void Workspace::setCurrentWorkingDirectory(const std::string& cwd) {
    cwd_ = cwd;
}

} // namespace firmius::core
