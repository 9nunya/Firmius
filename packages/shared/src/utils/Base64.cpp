#include "utils/Base64.hpp"

namespace firmius::shared {

namespace {

constexpr const char kStandardAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Standard base64 encoder, table-driven, three input bytes → four output
// characters. Pads the trailing partial group with '=' so the output
// length is always a multiple of 4.
std::string encodeStandard(const unsigned char* data, std::size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= size) {
        out.push_back(kStandardAlphabet[(data[i] >> 2) & 0x3F]);
        out.push_back(kStandardAlphabet[((data[i] << 4) | (data[i + 1] >> 4)) & 0x3F]);
        out.push_back(kStandardAlphabet[((data[i + 1] << 2) | (data[i + 2] >> 6)) & 0x3F]);
        out.push_back(kStandardAlphabet[data[i + 2] & 0x3F]);
        i += 3;
    }
    if (i < size) {
        out.push_back(kStandardAlphabet[(data[i] >> 2) & 0x3F]);
        if (i + 1 < size) {
            out.push_back(kStandardAlphabet[((data[i] << 4) | (data[i + 1] >> 4)) & 0x3F]);
            out.push_back(kStandardAlphabet[(data[i + 1] << 2) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back(kStandardAlphabet[(data[i] << 4) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

// Lookup helper for the standard base64 alphabet. Returns -1 for any
// character that isn't a valid base64 digit (caller skips those).
int decodeChar(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

// ── Standard base64 ─────────────────────────────────────────────────────

std::string base64Encode(const unsigned char* data, std::size_t size) {
    return encodeStandard(data, size);
}

std::string base64Encode(std::string_view data) {
    return encodeStandard(reinterpret_cast<const unsigned char*>(data.data()),
                          data.size());
}

std::string base64Encode(const std::vector<std::uint8_t>& data) {
    return encodeStandard(data.data(), data.size());
}

std::vector<std::uint8_t> base64Decode(std::string_view input) {
    std::vector<std::uint8_t> out;
    out.reserve((input.size() / 4) * 3);

    int val = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        int d = decodeChar(c);
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ── URL-safe base64 ─────────────────────────────────────────────────────

std::string base64UrlEncode(const unsigned char* data, std::size_t size) {
    std::string b64 = encodeStandard(data, size);
    for (char& c : b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    return b64;
}

std::string base64UrlEncode(std::string_view data) {
    return base64UrlEncode(
        reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

std::string base64UrlEncode(const std::vector<std::uint8_t>& data) {
    return base64UrlEncode(data.data(), data.size());
}

std::vector<std::uint8_t> base64UrlDecode(std::string_view input) {
    std::string b64(input);
    for (char& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (b64.size() % 4 != 0) b64.push_back('=');
    return base64Decode(b64);
}

} // namespace firmius::shared
