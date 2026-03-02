#ifndef FIRMIUS_SHARED_TERMINAL_UTIL_HPP
#define FIRMIUS_SHARED_TERMINAL_UTIL_HPP

#include <string>
#include <map>
#include <regex>
#include <cctype>

namespace firmius::shared {

/**
 * @brief Advanced terminal sequence translator for virtual keyboard support.
 */
class TerminalUtil {
public:
    /**
     * @brief Translates human-readable tags into terminal escape sequences.
     * Supports:
     * - {Ctrl+X} where X is A-Z
     * - {Alt+X} where X is any char
     * - {F1}-{F12}
     * - Standard keys: {Enter}, {Tab}, {Esc}, {Backspace}, {Delete}, {Up}, {Down}, {Left}, {Right}, {Home}, {End}
     */
    static std::string translate(const std::string& input) {
        std::string result = input;
        
        // 1. Static mappings for common non-printable keys
        static const std::map<std::string, std::string> staticMap = {
            {"{Enter}", "\n"}, {"{Tab}", "\t"}, {"{Esc}", "\x1b"},
            {"{Backspace}", "\x7f"}, {"{Delete}", "\x1b[3~"},
            {"{Up}", "\x1b[A"}, {"{Down}", "\x1b[B"}, {"{Right}", "\x1b[C"}, {"{Left}", "\x1b[D"},
            {"{Home}", "\x1b[H"}, {"{End}", "\x1b[F"}, {"{PageUp}", "\x1b[5~"}, {"{PageDown}", "\x1b[6~"},
            {"{Ctrl+Alt+Delete}", "\x1b[3;7~"}
        };

        for (const auto& [tag, code] : staticMap) {
            size_t pos = 0;
            while ((pos = result.find(tag, pos)) != std::string::npos) {
                result.replace(pos, tag.length(), code);
                pos += code.length();
            }
        }

        // 2. Heuristic: {Ctrl+X} -> ASCII Control Characters (1-26)
        static std::regex ctrlRegex(R"(\{Ctrl\+([A-Za-z])\})");
        std::smatch match;
        while (std::regex_search(result, match, ctrlRegex)) {
            char c = std::toupper(static_cast<unsigned char>(match[1].str()[0]));
            char ctrlChar = static_cast<char>(c - 'A' + 1);
            result.replace(match.position(), match.length(), std::string(1, ctrlChar));
        }

        // 3. Heuristic: {Alt+X} -> ESC + char
        static std::regex altRegex(R"(\{Alt\+([^\}])\})");
        while (std::regex_search(result, match, altRegex)) {
            std::string seq = "\x1b";
            seq += match[1].str();
            result.replace(match.position(), match.length(), seq);
        }

        // 4. Heuristic: {FX} -> Function keys
        static std::regex fRegex(R"(\{F([1-9]|1[0-2])\})");
        while (std::regex_search(result, match, fRegex)) {
            int fNum = std::stoi(match[1].str());
            std::string seq;
            if (fNum <= 4) seq = "\x1bOR" + std::to_string(fNum + 1); // Not quite right for all terms but standard
            else if (fNum <= 5) seq = "\x1b[15~";
            else if (fNum <= 6) seq = "\x1b[17~";
            else if (fNum <= 8) seq = "\x1b[18~";
            else if (fNum <= 9) seq = "\x1b[20~";
            else if (fNum <= 10) seq = "\x1b[21~";
            else if (fNum <= 11) seq = "\x1b[23~";
            else if (fNum <= 12) seq = "\x1b[24~";
            result.replace(match.position(), match.length(), seq);
        }

        return result;
    }
};

}

#endif
