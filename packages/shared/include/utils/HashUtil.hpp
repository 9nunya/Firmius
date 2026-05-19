#ifndef FIRMIUS_SHARED_HASHUTIL_HPP
#define FIRMIUS_SHARED_HASHUTIL_HPP

#include <cstdint>
#include <string_view>

namespace firmius::shared {

/**
 * @brief 32-bit FNV-1a hash. Standard algorithm, suitable for content
 * fingerprinting, cache keys, and other non-cryptographic uses.
 */
constexpr std::uint32_t fnv1a32(std::string_view data) noexcept {
    std::uint32_t hash = 0x811c9dc5u;
    for (char c : data) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x01000193u;
    }
    return hash;
}

/**
 * @brief 64-bit FNV-1a hash. Same algorithm as fnv1a32 with the
 * canonical 64-bit offset basis and prime.
 */
constexpr std::uint64_t fnv1a64(std::string_view data) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : data) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/**
 * @brief splitmix64 from xxHash / Sebastiano Vigna. Mixes a 64-bit
 * value through three rounds of multiplication-and-shift. Useful as
 * a finalizer on top of accumulating hashes (FNV, etc.) or as a
 * deterministic byte-distributor.
 */
inline constexpr std::uint64_t splitMix64(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

} // namespace firmius::shared

#endif
