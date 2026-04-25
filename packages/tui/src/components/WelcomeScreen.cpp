#include "components/WelcomeScreen.hpp"

#include "ThemeManager.hpp"
#include "utils/PlatformPaths.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ftxui/component/animation.hpp>
#include <ftxui/dom/elements.hpp>
#include <random>
#include <string>
#include <vector>

namespace firmius::tui {

namespace {

struct AsciiArt {
  std::vector<std::string> lines;
  int width = 0;
  int height = 0;
};
static bool LineIsBlank(const std::string &line) {
  for (char ch : line) {
    if (ch != ' ' && ch != '\t' && ch != '\r') {
      return false;
    }
  }
  return true;
}

static AsciiArt LoadArtOnce() {
  // The code does not install assets. CMake installs ART.md to the firmius home dir.
  const std::filesystem::path artPath =
      firmius::shared::PlatformPaths::firmiusHomeDir() / "ART.md";

  std::ifstream in(artPath);
  if (in) {
    AsciiArt art;
    std::string line;
    while (std::getline(in, line)) {
      // Preserve spacing exactly; getline drops the newline but keeps trailing spaces.
      art.lines.push_back(line);
    }

    // Trim leading/trailing blank padding lines so the welcome layout is stable.
    while (!art.lines.empty() && LineIsBlank(art.lines.front())) {
      art.lines.erase(art.lines.begin());
    }
    while (!art.lines.empty() && LineIsBlank(art.lines.back())) {
      art.lines.pop_back();
    }

    for (const auto &trimmed : art.lines) {
      art.width = std::max<int>(art.width, static_cast<int>(trimmed.size()));
    }
    art.height = static_cast<int>(art.lines.size());
    if (art.height > 0) {
      return art;
    }
  }

  AsciiArt fallback;
  fallback.lines = {
      "@@@@@@@  FIRMIUS",
      "(missing ~/.firmius/ART.md)",
  };
  fallback.width = 0;
  for (const auto &line : fallback.lines) {
    fallback.width = std::max<int>(fallback.width, static_cast<int>(line.size()));
  }
  fallback.height = static_cast<int>(fallback.lines.size());
  return fallback;
}

struct Star {
  int x = 0;
  int y = 0;
  int phase = 0;
};

static std::vector<Star> MakeStars(int count, int max_x, int max_y,
                                  uint32_t seed) {
  std::vector<Star> stars;
  stars.reserve(static_cast<size_t>(count));
  if (max_x <= 0 || max_y <= 0) {
    return stars;
  }
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> dx(0, max_x - 1);
  std::uniform_int_distribution<int> dy(0, max_y - 1);
  std::uniform_int_distribution<int> dp(0, 1000);
  for (int i = 0; i < count; ++i) {
    Star s;
    s.x = dx(rng);
    s.y = dy(rng);
    s.phase = dp(rng);
    stars.push_back(s);
  }
  return stars;
}

static ftxui::Color LerpColor(const ftxui::Color &a, const ftxui::Color & /*b*/,
                             float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return a;
  /*
  return ftxui::Color::RGB(
      static_cast<uint8_t>(a.red() + (b.red() - a.red()) * t),
      static_cast<uint8_t>(a.green() + (b.green() - a.green()) * t),
      static_cast<uint8_t>(a.blue() + (b.blue() - a.blue()) * t));
  */
}

static float SmoothStep(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

} // namespace

ftxui::Component WelcomeScreen() {
  static const AsciiArt art = LoadArtOnce();

  // Persistent animation state (owned by the renderer closure).
  struct Model {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_tick = start;
    uint32_t seed = 0xF17A5u;
    std::vector<Star> stars;
    int last_w = -1;
    int last_h = -1;
  };
  auto model = std::make_shared<Model>();

  auto renderer = ftxui::Renderer([model] {
    const auto &theme = ThemeManager::instance().getCurrentTheme();

    const auto terminal = ftxui::Terminal::Size();
    const int w = terminal.dimx;
    const int h = terminal.dimy;

    const auto now = std::chrono::steady_clock::now();
    const float t =
        std::chrono::duration<float>(now - model->start).count();

    // Ensure we tick even if there is no input: this drives the animation.
    if (now - model->last_tick >= std::chrono::milliseconds(33)) {
      model->last_tick = now;
      ftxui::animation::RequestAnimationFrame();
    }

    // Keep a stable star field per terminal size.
    if (w != model->last_w || h != model->last_h || model->stars.empty()) {
      model->last_w = w;
      model->last_h = h;
      const int star_count = std::max(12, (w * h) / 220);
      model->stars = MakeStars(star_count, w, h, model->seed);
    }

    // Center the art.
    const int art_w = std::max(1, art.width);
    const int art_h = std::max(1, art.height);
    const int ox = std::max(0, (w - art_w) / 2);
    const int oy = std::max(0, (h - art_h) / 2 - 1);

    // Animation feature #1: starfield with glints (background twinkles).
    // Animation feature #2: sweeping diagonal shimmer across the logo.
    // Animation feature #3: subtle per-frame breathing gradient (hue shift).

    auto color_a = theme.base.highlight;
    auto color_b = theme.chat.agent_prefix;
    const float breathe = 0.5f + 0.5f * std::sin(t * 0.65f);
    const auto base_logo_color = LerpColor(color_a, color_b, SmoothStep(breathe));

    // Shimmer sweep diagonal coordinate (in logo-local space).
    const float sweep_speed = 18.0f;
    const float sweep = std::fmod(t * sweep_speed, static_cast<float>(art_w + art_h + 20));

    // Build the logo lines with per-character styling.
    ftxui::Elements logo_rows;
    logo_rows.reserve(static_cast<size_t>(art.lines.size()));

    for (int y = 0; y < static_cast<int>(art.lines.size()); ++y) {
      const std::string &line = art.lines[static_cast<size_t>(y)];
      ftxui::Elements cells;
      cells.reserve(line.size());

      for (int x = 0; x < static_cast<int>(line.size()); ++x) {
        const char ch = line[static_cast<size_t>(x)];
        // Keep spaces, so centering remains faithful.
        auto el = ftxui::text(std::string(1, ch));

        if (ch != ' ') {
          // Diagonal shimmer band: make it bright + slightly different color.
          const float diag = static_cast<float>(x + y);
          const float dist = std::fabs(diag - sweep);
          const float band = std::clamp(1.0f - dist / 6.0f, 0.0f, 1.0f);
          const auto shimmer_color =
              LerpColor(base_logo_color, theme.status_bar.idle.normal.fg,
                        SmoothStep(band));

          // Slight flicker to avoid looking too "flat".
          const float flicker = 0.85f + 0.15f * std::sin(t * 8.0f + diag * 0.25f);
          const auto final_color =
              LerpColor(theme.base.dim, shimmer_color, flicker);

          el = el | ftxui::color(final_color);

          if (band > 0.75f) {
            el = el | ftxui::bold;
          }
        } else {
          el = el | ftxui::color(theme.base.bg);
        }
        cells.push_back(el);
      }

      logo_rows.push_back(ftxui::hbox(std::move(cells)));
    }

    auto logo = ftxui::vbox(std::move(logo_rows));

    // Overlay stars behind the logo using dbox + filler alignment.
    // We render stars as an element and then stack the logo on top.
    auto star_layer = ftxui::Renderer([model, ox, oy, art_w, art_h, t, theme] {
      // Render a sparse set of stars as rows of text.
      // Because FTXUI elements are row-oriented, we build a map of stars by row.
      std::vector<std::string> rows(static_cast<size_t>(std::max(1, model->last_h)),
                                    std::string(static_cast<size_t>(std::max(1, model->last_w)), ' '));

      for (const auto &s : model->stars) {
        if (s.x < 0 || s.x >= model->last_w || s.y < 0 || s.y >= model->last_h) {
          continue;
        }

        // Avoid drawing stars inside the logo bounding box so the logo stays clean.
        if (s.x >= ox && s.x < ox + art_w && s.y >= oy && s.y < oy + art_h) {
          continue;
        }

        const float phase = (t * 2.2f) + static_cast<float>(s.phase) * 0.01f;
        const float tw = 0.5f + 0.5f * std::sin(phase);
        rows[static_cast<size_t>(s.y)][static_cast<size_t>(s.x)] =
            (tw > 0.82f) ? '*' : ((tw > 0.55f) ? '+' : '.');
      }

      ftxui::Elements out;
      out.reserve(rows.size());
      for (const auto &row : rows) {
        out.push_back(ftxui::text(row) | ftxui::color(theme.base.dim));
      }
      return ftxui::vbox(std::move(out)) | ftxui::flex;
    });

    // Title + hint lines.
    auto header = ftxui::vbox(ftxui::Elements({
                      ftxui::text("Welcome to Firmius") | ftxui::bold |
                          ftxui::color(base_logo_color) | ftxui::center,
                      ftxui::text("Type a message to start") | ftxui::dim |
                          ftxui::center,
                  })) |
                  ftxui::center;

    // Center logo with padding so it doesn't overlap the header.
    auto body = ftxui::vbox(ftxui::Elements({
                    header,
                    ftxui::text(""),
                    logo | ftxui::center,
                })) |
                ftxui::center;

    // Stack: stars (dim) + body (logo + header).
    return ftxui::dbox(ftxui::Elements({
               star_layer->Render(),
               body | ftxui::flex,
           })) |
           ftxui::flex | ftxui::center;
  });

  return renderer;
}

} // namespace firmius::tui
