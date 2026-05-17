#ifndef FIRMIUS_SHARED_ITOKENIZER_HPP
#define FIRMIUS_SHARED_ITOKENIZER_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace firmius::shared {

/**
 * @brief Interface for token counting.
 *
 * Implementations provide model-specific tokenization. The HeuristicTokenizer
 * fallback uses bytes/4 — accurate enough for budget math until a real
 * tokenizer (tiktoken-cpp, anthropic-tokenizer) is wired per provider.
 */
class ITokenizer {
public:
  virtual ~ITokenizer() = default;

  /// Count tokens in a raw text string.
  [[nodiscard]] virtual uint32_t count(std::string_view text) const = 0;

  /// Identifier for this tokenizer (e.g. "heuristic", "cl100k_base", "claude-v3").
  [[nodiscard]] virtual std::string id() const = 0;
};

} // namespace firmius::shared

#endif
