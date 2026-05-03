#include "components/LiveStatusRow.hpp"

#include "ClaudexPhrases.hpp"
#include "ThemeManager.hpp"
#include "components/GlintEffect.hpp"
#include <cmath>
#include <string_view>

#include <algorithm>
#include <array>
#include <chrono>

namespace firmius::tui {

namespace {

std::string liveRowSpinnerGlyph(uint64_t now_ms) {
  static constexpr std::array<const char *, 3> kFrames = {"✦", "✧", "⋆"};
  const size_t index = static_cast<size_t>((now_ms / 350ULL) % kFrames.size());
  return kFrames[index];
}

std::string truncateText(const std::string &text, int max_len) {
  if (max_len <= 0)
    return "";
  if (static_cast<int>(text.size()) <= max_len)
    return text;
  if (max_len <= 3)
    return text.substr(0, max_len);
  return text.substr(0, max_len - 3) + "...";
}

std::string statusLabel(firmius::shared::WorkChunkStatus status) {
  switch (status) {
  case firmius::shared::WorkChunkStatus::InProgress:
    return "In Progress";
  case firmius::shared::WorkChunkStatus::Verifying:
    return "Verifying";
  case firmius::shared::WorkChunkStatus::Ready:
    return "Ready";
  case firmius::shared::WorkChunkStatus::Implemented:
    return "Implemented";
  case firmius::shared::WorkChunkStatus::Done:
    return "Done";
  case firmius::shared::WorkChunkStatus::Blocked:
    return "Blocked";
  case firmius::shared::WorkChunkStatus::Failed:
    return "Failed";
  case firmius::shared::WorkChunkStatus::Cancelled:
    return "Cancelled";
  }
  return "Ready";
}

static ftxui::Color interpolatePhraseColor(const Theme &theme, float emphasis) {
  const float clamped = std::clamp(emphasis, 0.0f, 1.0f);
  return ftxui::Color::Interpolate(clamped, theme.base.bg,
                                   theme.base.highlight);
}

float smoothstep(float x) {
  const float t = std::clamp(x, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// 0..1 -> per-char alpha where 0 means fully background-colored.
// We do this by interpolating between bg and highlight; the effect is
// that characters "disappear" into the background.
ftxui::Color charFadeToBg(const Theme &theme, float alpha) {
  const float t = std::clamp(alpha, 0.0f, 1.0f);
  return ftxui::Color::Interpolate(t, theme.base.bg, theme.base.highlight);
}

// Build a left-to-right wipe fade.
// - out: characters go from highlight -> bg, staggered left-to-right
// - in:  characters go from bg -> highlight, staggered left-to-right
ftxui::Element renderPhraseWipe(const Theme &theme, std::string_view text,
                                float global_t, bool fade_in) {
  ftxui::Elements parts;
  parts.reserve(text.size());

  // Timing knobs (in 0..1 global_t space)
  // Tune these to adjust wipe feel.
  constexpr float kCharStagger = 0.055f;   // per character delay
  constexpr float kCharFadeWindow = 0.28f; // how long a char takes to fade

  const int n = static_cast<int>(text.size());
  const float total_span =
      n <= 1 ? kCharFadeWindow
             : (kCharFadeWindow + kCharStagger * static_cast<float>(n - 1));
  const float scaled_t = std::clamp(global_t, 0.0f, 1.0f) * total_span;
  for (int i = 0; i < n; ++i) {
    const float start = i * kCharStagger;
    const float end = start + kCharFadeWindow;
    float local = (scaled_t - start) / (end - start);
    local = smoothstep(local);

    const float alpha = fade_in ? local : (1.0f - local);
    const std::string ch(1, text[static_cast<size_t>(i)]);
    parts.push_back(ftxui::text(ch) | ftxui::color(charFadeToBg(theme, alpha)));
  }

  return ftxui::hbox(std::move(parts));
}

ftxui::Element renderSpinnerPersistentPhrase(ftxui::Element spinner,
                                             ftxui::Element phrase_body) {
  return ftxui::hbox(ftxui::Elements{
      std::move(spinner),
      std::move(phrase_body),
  });
}

ftxui::Element renderInvisiblePhrase(const Theme &theme,
                                     std::string_view text) {
  return ftxui::text(std::string(text)) | ftxui::color(theme.base.bg);
}

struct PhraseTransitionSlices {
  float fade_out_end;
  float hold_end;
};

PhraseTransitionSlices phraseTransitionSlices(std::size_t previous_len,
                                              std::size_t next_len) {
  const float max_len = static_cast<float>(
      std::max<std::size_t>(std::max(previous_len, next_len), 1));
  const float fade_out_portion =
      std::clamp(0.42f + max_len * 0.005f, 0.46f, 0.62f);
  const float hold_portion =
      std::clamp(0.08f + max_len * 0.0015f, 0.10f, 0.18f);
  return {.fade_out_end = fade_out_portion,
          .hold_end = std::min(0.82f, fade_out_portion + hold_portion)};
}

} // namespace

ftxui::Element RenderLiveStatusRow(const LiveStatusRowModel &model) {

  if (!model.skin.show_persistent_live_row || model.focused_agent_id.empty()) {
    return ftxui::emptyElement();
  }
  if (!model.busy && model.skin.live_row_busy_only) {
    return ftxui::emptyElement();
  }

  const auto nowMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());

  // NOTE: phrases/phrase are still computed for the non-transition case below.
  const auto &phrases = claudexLivePhrasesForMode(model.phrase_mode);
  const auto cycle_ms =
      static_cast<uint64_t>(std::max(1, model.skin.live_row_cycle_seconds)) *
      1000ULL;
  const auto index =
      phrases.empty()
          ? 0u
          : static_cast<size_t>((nowMs / cycle_ms) % phrases.size());
  // Only used for the non-transition case. During transitions we render
  // phrase_prev/phrase_next with the wipe effect.
  const std::string phrase =
      !model.phrase.empty()
          ? model.phrase
          : (phrases.empty() ? std::string("Standing by.") : phrases[index]);

  const std::string right =
      model.activity.empty() ? model.elapsed : model.activity;
  const auto &theme = ThemeManager::instance().getCurrentTheme();

  const std::string glyph = liveRowSpinnerGlyph(nowMs);
  const auto spinner_visible =
      ftxui::text(" " + glyph + " ") | ftxui::bold |
      ftxui::color(interpolatePhraseColor(theme, 1.0f));

  // When a phrase transition is active, render the left segment as a
  // per-character wipe fade-out -> invisible -> fade-in.
  ftxui::Element left;
  if (model.phrase_transition_active) {
    const float t = std::clamp(model.phrase_transition_t, 0.0f, 1.0f);
    const std::string prev_phrase = model.phrase_prev;
    const std::string next_phrase = model.phrase_next;

    const auto slices =
        phraseTransitionSlices(prev_phrase.size(), next_phrase.size());
    const float kFadeOutEnd = slices.fade_out_end;
    const float kInvisibleEnd = slices.hold_end;
    const float kFadeInSpan = std::max(0.001f, 1.0f - kInvisibleEnd);

    if (t < kFadeOutEnd) {
      left = renderSpinnerPersistentPhrase(
                 spinner_visible,
                 renderPhraseWipe(theme, prev_phrase, t / kFadeOutEnd,
                                  /*fade_in=*/false)) |
             ftxui::bold;
    } else if (t < kInvisibleEnd) {
      left = renderSpinnerPersistentPhrase(
                 spinner_visible, renderInvisiblePhrase(theme, prev_phrase)) |
             ftxui::bold;
    } else if (t < 1.0f) {
      left = renderSpinnerPersistentPhrase(
                 spinner_visible,
                 renderPhraseWipe(theme, next_phrase,
                                  (t - kInvisibleEnd) / kFadeInSpan,
                                  /*fade_in=*/true)) |
             ftxui::bold;
    } else {
      left = renderSpinnerPersistentPhrase(
                 spinner_visible,
                 ftxui::text(next_phrase) |
                     ftxui::color(interpolatePhraseColor(theme, 1.0f))) |
             ftxui::bold;
    }
  } else {
    const std::string phrase =
        !model.phrase.empty()
            ? model.phrase
            : (phrases.empty() ? std::string("Standing by.") : phrases[index]);
    left = renderSpinnerPersistentPhrase(
               spinner_visible,
               ftxui::text(phrase) |
                   ftxui::color(interpolatePhraseColor(theme, 1.0f))) |
           ftxui::bold;
  }

  if (model.skin.live_row_glint && model.busy &&
      !model.phrase_transition_active) {
    GlintConfig cfg;
    cfg.target = GlintConfig::Target::Text;
    // Base → highlight → base reads as an actual sweep.
    cfg.gradientColors = {theme.base.highlight, theme.base.fg};
    cfg.glintSize = 10;
    cfg.intervalSeconds = 13.5f;
    cfg.easing = GlintEasing::EaseOut;
    cfg.durationSeconds = 4.8f;
    left = GlintEffect(left, cfg)->Render();
  }

  ftxui::Elements rows;
  ftxui::Elements first_row;
  first_row.push_back(left | ftxui::xflex);
  first_row.push_back(ftxui::text("  "));
  first_row.push_back(ftxui::text(right) | ftxui::color(theme.chat.timestamp));
  rows.push_back(ftxui::hbox(std::move(first_row)) | ftxui::xflex);

  if (model.has_plan_excerpt) {
    rows.push_back(ftxui::text("Plan: " + model.plan_title) | ftxui::bold |
                   ftxui::color(theme.base.fg));
    for (std::size_t i = 0; i < model.plan_rows.size(); ++i) {
      std::string line = "> " + model.plan_rows[i].title + " · " +
                         statusLabel(model.plan_rows[i].status);
      if (i + 1 == model.plan_rows.size() && model.hidden_plan_count > 0) {
        line += " ... +" + std::to_string(model.hidden_plan_count) + " more";
      }
      rows.push_back(
          ftxui::text(truncateText(
              line, std::max(12, ftxui::Terminal::Size().dimx - 2))) |
          ftxui::color(i == 0 ? theme.base.highlight : theme.base.dim));
    }
  }

  if (model.has_todo_excerpt) {
    rows.push_back(
        ftxui::text(
            "> " +
            truncateText(model.todo_excerpt,
                         std::max(12, ftxui::Terminal::Size().dimx - 2))) |
        ftxui::color(theme.base.highlight));
  }

  return ftxui::vbox(std::move(rows)) | ftxui::xflex | ftxui::xflex;
}

} // namespace firmius::tui
