#include "GlintRenderer.hpp"
#include "Terminal.hpp"

#include <algorithm>
#include <cmath>

namespace firmius::tui {

namespace {

struct Utf8Chunk {
  std::string text;
};

std::vector<Utf8Chunk> splitUtf8(const std::string& text) {
  std::vector<Utf8Chunk> chunks;
  for (std::size_t i = 0; i < text.size();) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    std::size_t len = 1;
    if ((ch & 0xE0) == 0xC0 && i + 1 < text.size()) len = 2;
    else if ((ch & 0xF0) == 0xE0 && i + 2 < text.size()) len = 3;
    else if ((ch & 0xF8) == 0xF0 && i + 3 < text.size()) len = 4;
    chunks.push_back({text.substr(i, len)});
    i += len;
  }
  return chunks;
}

GlintColorStop interpolate(const std::vector<GlintColorStop>& gradient, float t) {
  if (gradient.empty()) return {};
  if (gradient.size() == 1) return gradient.front();
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  const float scaled = clamped * static_cast<float>(gradient.size() - 1);
  const auto idx = static_cast<std::size_t>(scaled);
  if (idx >= gradient.size() - 1) return gradient.back();
  const float frac = scaled - static_cast<float>(idx);
  const auto& left = gradient[idx];
  const auto& right = gradient[idx + 1];
  return {
      static_cast<int>(std::lround(left.r + (right.r - left.r) * frac)),
      static_cast<int>(std::lround(left.g + (right.g - left.g) * frac)),
      static_cast<int>(std::lround(left.b + (right.b - left.b) * frac)),
  };
}

} // namespace

std::string GlintRenderer::apply(const std::string& text,
                                 const GlintConfig& config,
                                 std::uint64_t nowMs,
                                 bool active) {
  if (!active || text.empty() || config.glintSize <= 0 ||
      config.durationMs == 0) {
    return text;
  }

  const auto glyphs = splitUtf8(text);
  if (glyphs.empty()) {
    return text;
  }

  const std::uint64_t cycleMs = config.durationMs + config.intervalMs;
  const std::uint64_t cyclePos = cycleMs == 0 ? 0 : nowMs % cycleMs;
  if (cyclePos > config.durationMs) {
    return text;
  }

  const float progress =
      static_cast<float>(cyclePos) / static_cast<float>(config.durationMs);
  const float span = static_cast<float>(
      std::min(config.glintSize, static_cast<int>(glyphs.size())));
  const float center =
      -span + (static_cast<float>(glyphs.size()) + span * 2.0f) * progress;

  std::string out;
  out.reserve(text.size() * 8);
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    const float distance = std::abs(center - static_cast<float>(i));
    const float strength = 1.0f - std::min(distance / std::max(1.0f, span / 2.0f), 1.0f);
    if (strength <= 0.0f || ansi::strip(glyphs[i].text) == " ") {
      out += glyphs[i].text;
      continue;
    }
    const auto color = interpolate(config.gradient, strength);
    out += ansi::fgRgb(color.r, color.g, color.b, glyphs[i].text);
  }
  return out;
}

} // namespace firmius::tui
