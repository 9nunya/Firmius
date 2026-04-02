#include "utils/LineRange.hpp"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace firmius::shared::utils {

namespace {

bool isAllDigits(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

bool parseLinePrefix(std::string_view line, size_t& prefixLength) noexcept {
    const size_t pipePos = line.find('|');
    if (pipePos == std::string_view::npos || pipePos == 0) {
        return false;
    }
    if (!isAllDigits(line.substr(0, pipePos))) {
        return false;
    }
    prefixLength = pipePos + 1;
    return true;
}

} // namespace

std::string LineRange::formatAnchor(int lineNum) {
    return std::to_string(lineNum);
}

std::string LineRange::formatLine(int lineNum, std::string_view content) {
    std::string result = std::to_string(lineNum);
    result += '|';
    result += content;
    return result;
}

AnchorResult LineRange::resolveAnchor(const std::vector<std::string>& lines,
                                      const std::string& anchorText) {
    std::string numericPart;
    size_t i = 0;
    while (i < anchorText.size() && std::isdigit(static_cast<unsigned char>(anchorText[i]))) {
        numericPart += anchorText[i];
        ++i;
    }

    if (numericPart.empty()) {
        return {AnchorResult::Status::NOT_NUMERIC, -1, false, "Anchor must start with a line number; got: " + anchorText};
    }

    try {
        int lineNum = std::stoi(numericPart);
        return resolveLineNumber(lines, lineNum);
    } catch (...) {
        return {AnchorResult::Status::NOT_NUMERIC, -1, false, "Anchor contains an invalid line number: " + numericPart};
    }
}

AnchorResult LineRange::resolveLineNumber(const std::vector<std::string>& lines,
                                            int lineNum) {
    if (lineNum <= 0) {
        return {AnchorResult::Status::OUT_OF_RANGE, -1, false, "Line number must be positive: " + std::to_string(lineNum)};
    }
    if (lineNum > static_cast<int>(lines.size())) {
        return {AnchorResult::Status::OUT_OF_RANGE, -1, false, "Line number " + std::to_string(lineNum) + " is out of range for file with " + std::to_string(lines.size()) + " lines."};
    }
    return {AnchorResult::Status::SUCCESS, lineNum - 1, false, ""};
}

std::string LineRangeReadEnhancer::enhance(std::string_view content) {
    std::string result;
    result.reserve(content.size() * 1.1);

    size_t start = 0;
    size_t end = content.find('\n');
    int lineNum = 1;

    while (end != std::string_view::npos) {
        result += std::to_string(lineNum++);
        result += '|';
        result += content.substr(start, end - start);
        result += '\n';
        start = end + 1;
        end = content.find('\n', start);
    }

    if (start < content.size()) {
        result += std::to_string(lineNum);
        result += '|';
        result += content.substr(start);
    }

    return result;
}

std::string LineRangeTrimmer::trimLine(std::string_view line) {
    return sanitizeContent(line);
}

std::string LineRangeTrimmer::sanitizeContent(std::string_view content, SanitationResult* outResult) {
    std::string result;
    result.reserve(content.size());
    size_t start = 0;
    size_t end = content.find('\n');

    auto processLine = [&](std::string_view line) {
        std::string_view remainder = line;
        size_t prefixLength = 0;

        if (parseLinePrefix(remainder, prefixLength)) {
            remainder.remove_prefix(prefixLength);
            if (outResult) {
                outResult->lineRangePrefixesStripped++;
            }
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

std::string LineRangeTrimmer::trimAll(std::string_view content) {
    return sanitizeContent(content);
}

std::string LineRangeTrimmer::sanitizeDiffMarkers(std::string_view line) {
    if (line.starts_with("+ ") || line.starts_with("- ")) return std::string(line.substr(2));
    if (line == "+" || line == "-") return "";
    return std::string(line);
}

bool LineRangeTrimmer::startsWithSuspiciousMetadata(std::string_view line) noexcept {
    size_t prefixLength = 0;
    return parseLinePrefix(line, prefixLength);
}

bool LineRangeTrimmer::startsWithSuspiciousDiffJunk(std::string_view line) noexcept {
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
