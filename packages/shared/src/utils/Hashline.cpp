#include "utils/Hashline.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace firmius::shared::utils {

namespace {
/**
 * @brief FNV-1a hash implementation for 32-bit results.
 */
constexpr uint32_t fnv1a_32(std::string_view data) noexcept {
    uint32_t hash = 0x811c9dc5;
    for (char c : data) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x01000193;
    }
    return hash;
}
} // namespace

std::string Hashline::computeHash(std::string_view content) noexcept {
    const uint32_t hash = fnv1a_32(content);
    // Take lower 16 bits and format as 4 hex chars
    std::stringstream ss;
    ss << std::hex << std::setw(4) << std::setfill('0') << (hash & 0xFFFF);
    return ss.str();
}

std::string Hashline::formatAnchor(int lineNum, std::string_view content) {
    std::string result;
    const std::string hash = computeHash(content);
    result.reserve(hash.size() + 16);
    result += std::to_string(lineNum);
    result += '#';
    result += hash;
    return result;
}

std::string Hashline::formatLine(int lineNum, std::string_view content) {
    std::string result;
    const std::string anchor = formatAnchor(lineNum, content);
    result.reserve(anchor.size() + content.size() + 1);
    result += anchor;
    result += '|';
    result += content;
    return result;
}

std::optional<HashlineAnchor> Hashline::parseAnchor(std::string_view anchor) noexcept {
    const size_t hashPos = anchor.find('#');
    if (hashPos == std::string_view::npos || hashPos == 0 ||
        hashPos + 1 >= anchor.size()) {
        return std::nullopt;
    }

    int lineNumber = 0;
    for (size_t i = 0; i < hashPos; ++i) {
        const unsigned char ch = static_cast<unsigned char>(anchor[i]);
        if (!std::isdigit(ch)) {
            return std::nullopt;
        }
        lineNumber = (lineNumber * 10) + (anchor[i] - '0');
    }
    if (lineNumber <= 0) {
        return std::nullopt;
    }

    HashlineAnchor parsed;
    parsed.lineNumber = lineNumber;
    parsed.hash = std::string(anchor.substr(hashPos + 1));
    if (parsed.hash.empty()) {
        return std::nullopt;
    }
    if (!std::all_of(parsed.hash.begin(), parsed.hash.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        })) {
        return std::nullopt;
    }
    std::transform(parsed.hash.begin(), parsed.hash.end(), parsed.hash.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return parsed;
}

bool Hashline::verifyAnchor(std::string_view expectedHash, std::string_view actualContent) noexcept {
    return computeHash(actualContent) == expectedHash;
}

std::string HashlineReadEnhancer::enhance(std::string_view content) {
    std::string result;
    result.reserve(content.size() * 1.2); // Rough estimate

    int lineNum = 1;
    size_t start = 0;
    size_t end = content.find('\n');

    while (end != std::string_view::npos) {
        std::string_view line = content.substr(start, end - start);
        result += Hashline::formatLine(lineNum++, line);
        result += '\n';
        start = end + 1;
        end = content.find('\n', start);
    }

    // Handle last line if it doesn't end with \n
    if (start < content.size()) {
        std::string_view line = content.substr(start);
        result += Hashline::formatLine(lineNum, line);
    }

    return result;
}

std::string HashlineTrimmer::trimLine(std::string_view line) {
    // Find the '|' separator that marks the end of the hashline prefix
    size_t separatorPos = line.find('|');
    if (separatorPos == std::string_view::npos) {
        // Not a hashline-formatted line, return as-is
        return std::string(line);
    }
    
    // Verify it looks like a hashline prefix (has # before |)
    size_t hashPos = line.find('#');
    if (hashPos == std::string_view::npos || hashPos > separatorPos) {
        // Doesn't look like a hashline, return as-is
        return std::string(line);
    }
    
    // Return everything after the '|'
    return std::string(line.substr(separatorPos + 1));
}

std::string HashlineTrimmer::trimAll(std::string_view content) {
    std::string result;
    result.reserve(content.size()); // Reserve at least the same size
    
    size_t start = 0;
    size_t end = content.find('\n');
    
    while (end != std::string_view::npos) {
        std::string_view line = content.substr(start, end - start);
        result += trimLine(line);
        result += '\n';
        start = end + 1;
        end = content.find('\n', start);
    }
    
    // Handle last line if it doesn't end with \n
    if (start < content.size()) {
        std::string_view line = content.substr(start);
        result += trimLine(line);
    }
    
    return result;
}

} // namespace firmius::shared::utils
