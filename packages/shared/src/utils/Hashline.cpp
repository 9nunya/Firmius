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

bool isAllDigits(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

bool isAllHex(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

bool parseLineHashPrefix(std::string_view line, size_t& prefixLength) noexcept {
    const size_t hashPos = line.find('#');
    const size_t pipePos = line.find('|');
    if (hashPos == std::string_view::npos || pipePos == std::string_view::npos ||
        hashPos == 0 || hashPos + 1 >= pipePos) {
        return false;
    }
    if (!isAllDigits(line.substr(0, hashPos)) ||
        !isAllHex(line.substr(hashPos + 1, pipePos - hashPos - 1))) {
        return false;
    }
    prefixLength = pipePos + 1;
    return true;
}

bool parseBareHashFragment(std::string_view line, size_t& prefixLength) noexcept {
    const size_t pipePos = line.find('|');
    if (pipePos == std::string_view::npos || pipePos != 4) {
        return false;
    }
    if (!isAllHex(line.substr(0, pipePos))) {
        return false;
    }
    prefixLength = pipePos + 1;
    return true;
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
    const size_t pipePos = anchor.find('|');
    if (pipePos != std::string_view::npos) {
        anchor = anchor.substr(0, pipePos);
    }

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
    return sanitizeContent(line);
}


std::string HashlineTrimmer::sanitizeContent(std::string_view content, SanitationResult* outResult) {
    std::string result;
    result.reserve(content.size());
    size_t start = 0;
    size_t end = content.find('\n');

    auto processLine = [&](std::string_view line) {
        std::string_view remainder = line;
        size_t prefixLength = 0;
        bool strippedHashlinePrefix = false;

        if (parseLineHashPrefix(remainder, prefixLength)) {
            remainder.remove_prefix(prefixLength);
            strippedHashlinePrefix = true;
            if (outResult) {
                outResult->hashlinePrefixesStripped++;
            }
        }
        while (strippedHashlinePrefix) {
            if (parseLineHashPrefix(remainder, prefixLength)) {
                remainder.remove_prefix(prefixLength);
                if (outResult) {
                    outResult->hashlinePrefixesStripped++;
                }
                continue;
            }
            if (parseBareHashFragment(remainder, prefixLength)) {
                remainder.remove_prefix(prefixLength);
                if (outResult) {
                    outResult->malformedHashFragmentsStripped++;
                }
                continue;
            }
            break;
        }

        std::string res(remainder);
        res = sanitizeDiffMarkers(res);
        if (outResult && res.size() != remainder.size()) {
            outResult->diffMarkersStripped++;
        }
        return res;
    };

    while (end != std::string::npos) {
        result += processLine(content.substr(start, end - start));
        result += '\n';
        start = end + 1;
        end = content.find('\n', start);
    }
    if (start < content.size()) result += processLine(content.substr(start));
    return result;
}

std::string HashlineTrimmer::trimAll(std::string_view content) {
    return sanitizeContent(content);
}

std::string HashlineTrimmer::sanitizeDiffMarkers(std::string_view line) {
    if (line.starts_with("+ ") || line.starts_with("- ")) return std::string(line.substr(2));
    if (line == "+" || line == "-") return "";
    return std::string(line);
}

bool HashlineTrimmer::startsWithSuspiciousMetadata(std::string_view line) noexcept {
    size_t prefixLength = 0;
    return parseLineHashPrefix(line, prefixLength) || parseBareHashFragment(line, prefixLength);
}

bool HashlineTrimmer::startsWithSuspiciousDiffJunk(std::string_view line) noexcept {
    if (line.empty()) {
        return false;
    }

    if (line.starts_with("+++ ") || line.starts_with("--- ") ||
        line.starts_with("@@") || line.starts_with("diff --git ") ||
        line.starts_with("index ")) {
        return true;
    }

    if (line.size() >= 2 && (line[0] == '+' || line[0] == '-')) {
        const unsigned char second = static_cast<unsigned char>(line[1]);
        if (std::isspace(second) != 0 || std::isalpha(second) != 0 ||
            line[1] == '"' || line[1] == '\'' || line[1] == '/' ||
            line[1] == '#') {
            return true;
        }
    }

    return false;
}

} // namespace firmius::shared::utils
