#include "utils/Hashline.hpp"
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

std::string Hashline::formatLine(int lineNum, std::string_view content) {
    const std::string hash = computeHash(content);
    std::string result;
    result.reserve(hash.size() + content.size() + 16);
    result += std::to_string(lineNum);
    result += '#';
    result += hash;
    result += '|';
    result += content;
    return result;
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

} // namespace firmius::shared::utils
