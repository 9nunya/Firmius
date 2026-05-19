#ifndef FIRMIUS_SHARED_HEURISTICTOKENIZER_HPP
#define FIRMIUS_SHARED_HEURISTICTOKENIZER_HPP

#include "ITokenizer.hpp"

namespace firmius::shared {

/**
 * @brief Fallback tokenizer using bytes/4 approximation.
 *
 * This is the same heuristic the codebase already uses — exposed behind
 * the ITokenizer interface so callers can be swapped to real tokenizers
 * later without changing call signatures.
 */
class HeuristicTokenizer : public ITokenizer {
public:
  [[nodiscard]] uint32_t count(std::string_view text) const override;
  [[nodiscard]] std::string id() const override;
};

} // namespace firmius::shared

#endif
