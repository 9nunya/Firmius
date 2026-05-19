#include "utils/Hashline.hpp"
#include "utils/HashUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace firmius::shared::utils {

namespace {

std::string hex64(uint64_t value, int width = 12) {
    std::stringstream ss;
    ss << std::hex << std::setw(width) << std::setfill('0')
       << (value & ((width >= 16) ? UINT64_MAX : ((1ULL << (width * 4)) - 1ULL)));
    return ss.str();
}

std::string buildContextFingerprint(const std::vector<std::string>& lines,
                                    std::size_t index, std::size_t radius) {
    std::string fingerprint;
    for (std::ptrdiff_t offset = -static_cast<std::ptrdiff_t>(radius);
         offset <= static_cast<std::ptrdiff_t>(radius); ++offset) {
        const std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(index) + offset;
        if (pos < 0) {
            fingerprint += "\x01BOF\x1f";
            continue;
        }
        if (pos >= static_cast<std::ptrdiff_t>(lines.size())) {
            fingerprint += "\x01EOF\x1f";
            continue;
        }
        fingerprint += lines[static_cast<std::size_t>(pos)];
        fingerprint += '\x1f';
    }
    return fingerprint;
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
    if (pipePos == std::string_view::npos || pipePos < 4 || pipePos > 16) {
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
    uint32_t hash = firmius::shared::fnv1a32(content);
    // Incorporate length for extra entropy on similar short strings
    hash ^= static_cast<uint32_t>(content.length());
    hash *= 0x01000193;
    // Take lower 16 bits and format as 4 hex chars
    std::stringstream ss;
    ss << std::hex << std::setw(4) << std::setfill('0') << (hash & 0xFFFF);
    return ss.str();
}

std::vector<std::string> Hashline::computeLineHashes(
    const std::vector<std::string>& lines) noexcept {
    std::vector<std::string> hashes(lines.size());
    if (lines.empty()) {
        return hashes;
    }

    std::vector<std::size_t> unresolved(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        unresolved[i] = i;
    }

    const std::size_t MAX_RADIUS = 3;
    for (std::size_t radius = 0; radius <= MAX_RADIUS && !unresolved.empty();
         ++radius) {
        std::unordered_map<std::string, std::vector<std::size_t>> groups;
        groups.reserve(unresolved.size());

        for (std::size_t index : unresolved) {
            const std::string fingerprint =
                buildContextFingerprint(lines, index, radius);
            groups[hex64(firmius::shared::fnv1a64(fingerprint))].push_back(index);
        }

        std::vector<std::size_t> nextUnresolved;
        for (const auto& [hash, indices] : groups) {
            if (indices.size() == 1) {
                hashes[indices.front()] = hash;
                continue;
            }
            nextUnresolved.insert(nextUnresolved.end(), indices.begin(),
                                  indices.end());
        }
        unresolved = std::move(nextUnresolved);
    }

    for (std::size_t index : unresolved) {
        std::string fingerprint = buildContextFingerprint(lines, index, lines.size());
        fingerprint += "\x1eline:";
        fingerprint += std::to_string(index + 1);
        hashes[index] = hex64(firmius::shared::fnv1a64(fingerprint));
    }

    return hashes;
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

std::string Hashline::formatAnchor(const std::vector<std::string>& lines,
                                   int lineNum) {
    if (lineNum <= 0 || lineNum > static_cast<int>(lines.size())) {
        return std::to_string(lineNum) + "#";
    }
    const auto hashes = computeLineHashes(lines);
    std::string result;
    result.reserve(hashes[lineNum - 1].size() + 16);
    result += std::to_string(lineNum);
    result += '#';
    result += hashes[lineNum - 1];
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

std::string Hashline::formatLine(const std::vector<std::string>& lines,
                                 int lineNum) {
    if (lineNum <= 0 || lineNum > static_cast<int>(lines.size())) {
        return std::to_string(lineNum) + "#|";
    }
    const std::string anchor = formatAnchor(lines, lineNum);
    std::string result;
    result.reserve(anchor.size() + lines[lineNum - 1].size() + 1);
    result += anchor;
    result += '|';
    result += lines[lineNum - 1];
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

AnchorResult Hashline::resolveAnchor(const std::vector<std::string>& lines,
                                      const std::string& anchorText,
                                      int /*searchWindow*/) {
    std::string numericPart;
    size_t i = 0;
    while (i < anchorText.size() && std::isdigit(static_cast<unsigned char>(anchorText[i]))) {
        numericPart += anchorText[i];
        ++i;
    }

    if (numericPart.empty()) {
        return {AnchorResult::Status::NOT_NUMERIC, -1, false, "Anchor must start with a line number; got: " + anchorText, "", ""};
    }

    try {
        int lineNum = std::stoi(numericPart);
        return resolveLineNumber(lines, lineNum);
    } catch (...) {
        return {AnchorResult::Status::NOT_NUMERIC, -1, false, "Anchor contains an invalid line number: " + numericPart, "", ""};
    }
}

AnchorResult Hashline::resolveLineNumber(const std::vector<std::string>& lines,
                                            int lineNum) {
    if (lineNum <= 0) {
        return {AnchorResult::Status::OUT_OF_RANGE, -1, false, "Line number must be positive: " + std::to_string(lineNum), "", ""};
    }
    if (lineNum > static_cast<int>(lines.size())) {
        return {AnchorResult::Status::OUT_OF_RANGE, -1, false, "Line number " + std::to_string(lineNum) + " is out of range for file with " + std::to_string(lines.size()) + " lines.", "", ""};
    }
    return {AnchorResult::Status::SUCCESS, lineNum - 1, false, "", "", ""};
}

std::string HashlineReadEnhancer::enhance(std::string_view content) {
    std::string result;
    result.reserve(content.size() * 1.2); // Rough estimate

    size_t start = 0;
    size_t end = content.find('\n');
    std::vector<std::string> lines;

    while (end != std::string_view::npos) {
        lines.emplace_back(content.substr(start, end - start));
        start = end + 1;
        end = content.find('\n', start);
    }

    // Handle last line if it doesn't end with \n
    if (start < content.size()) {
        lines.emplace_back(content.substr(start));
    }

    for (std::size_t i = 0; i < lines.size(); ++i) {
        result += Hashline::formatLine(lines, static_cast<int>(i + 1));
        if (i + 1 < lines.size()) {
            result += '\n';
        }
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
