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
void noteTuiRequestAnimationFrameFromWelcomeScreen() __attribute__((weak));

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
  const std::filesystem::path artPath =
      firmius::shared::PlatformPaths::firmiusHomeDir() / "ART.md";

  std::ifstream in(artPath);
  if (in) {
    // Read all lines first.
    std::vector<std::string> raw_lines;
    std::string line;
    while (std::getline(in, line)) {
      raw_lines.push_back(line);
    }
    in.close();

    // Find minimum leading whitespace among non-blank lines.
    size_t min_indent = SIZE_MAX;
    for (const auto &l : raw_lines) {
      size_t first = l.find_first_not_of(" \t");
      if (first != std::string::npos) {
        min_indent = std::min(min_indent, first);
      }
    }
    if (min_indent == SIZE_MAX) min_indent = 0;

    // Build art, trimming the common left margin.
    AsciiArt art;
    for (const auto &raw : raw_lines) {
      std::string trimmed = raw;
      if (min_indent > 0 && trimmed.size() >= min_indent) {
        trimmed = trimmed.substr(min_indent);
      }
      art.lines.push_back(trimmed);
    }

    // Trim leading/trailing blank padding lines.
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

struct MovingStar {
  float x = 0.0f;
  float y = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float brightness = 1.0f;
};

struct ShootingStar {
  float x = 0.0f;
  float y = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float life = 0.0f;
  float max_life = 0.0f;
};

struct Firework {
  float x = 0.0f;
  float y = 0.0f;
  float vy = 0.0f;
  std::vector<std::pair<float, float>> particles;
  std::vector<float> particle_life;
  std::vector<ftxui::Color> particle_colors;
  float life = 0.0f;
  float max_life = 0.0f;
};

static ftxui::Color RainbowColor(float phase) {
  phase = std::fmod(phase, 7.0f);
  if (phase < 0.0f) phase += 7.0f;
  int idx = static_cast<int>(phase);
  switch (idx) {
    case 0: return ftxui::Color::Red;
    case 1: return ftxui::Color::Green;
    case 2: return ftxui::Color::Yellow;
    case 3: return ftxui::Color::Blue;
    case 4: return ftxui::Color::Magenta;
    case 5: return ftxui::Color::Cyan;
    case 6: return ftxui::Color::White;
    default: return ftxui::Color::White;
  }
}

} // namespace

ftxui::Component WelcomeScreen() {
  static const AsciiArt loaded_art = LoadArtOnce();

  struct Model {
    AsciiArt art;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_tick = start;
    std::chrono::steady_clock::time_point last_shooting = start;
    std::chrono::steady_clock::time_point last_firework = start;
    uint32_t seed = 0xF17A5u;
    std::vector<Star> stars;
    std::vector<MovingStar> moving_stars;
    std::vector<ShootingStar> shooting_stars;
    std::vector<Firework> fireworks;
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
    const float dt =
        std::clamp(std::chrono::duration<float>(now - model->last_tick).count(),
                   0.0f, 0.1f);
    const float frame_units = dt * 30.0f;
    model->last_tick = now;
    const float t =
        std::chrono::duration<float>(now - model->start).count();

    // Avoid FTXUI's global RAF scheduler (can cause unrelated full-frame
    // redraw pressure). Use TuiState's scoped animation tick instead. Request
    // it on every welcome render so the first paint arms the ticker; animation
    // advancement below uses real elapsed time, not render count, so mouse
    // movement can no longer speed up the starfield.
    if (firmius::tui::noteTuiRequestAnimationFrameFromWelcomeScreen) {
      firmius::tui::noteTuiRequestAnimationFrameFromWelcomeScreen();
    }

    // Initialize art and starfield.
    if (model->art.lines.empty()) {
      model->art = loaded_art;
    }
    if (w != model->last_w || h != model->last_h || model->stars.empty()) {
      model->last_w = w;
      model->last_h = h;
      const int star_count = std::max(12, (w * h) / 220);
      model->stars = MakeStars(star_count, w, h, model->seed);
      model->moving_stars.clear();
      for (const auto &s : model->stars) {
        MovingStar ms;
        ms.x = static_cast<float>(s.x);
        ms.y = static_cast<float>(s.y);
        ms.vx = 0.3f * (0.7f + 0.3f * std::sin(s.phase * 0.01f));
        ms.vy = 0.3f * (0.7f + 0.3f * std::cos(s.phase * 0.01f));
        ms.brightness = 0.6f + 0.4f * std::sin(t * 2.0f + s.phase * 0.01f);
        model->moving_stars.push_back(ms);
      }
    }

    // Update moving stars (wrap around).
    for (auto &ms : model->moving_stars) {
      ms.x += ms.vx * frame_units;
      ms.y += ms.vy * frame_units;
      if (ms.x < 0) ms.x += w;
      if (ms.x >= w) ms.x -= w;
      if (ms.y < 0) ms.y += h;
      if (ms.y >= h) ms.y -= h;
      ms.brightness = 0.5f + 0.5f * std::sin(t * 3.0f);
    }

    // Shooting stars: spawn periodically.
    if (model->shooting_stars.empty() &&
        std::chrono::duration<float>(now - model->last_shooting).count() > 2.0f &&
        w > 0) {
      ShootingStar ss;
      ss.x = static_cast<float>(rand() % w);
      ss.y = 0.0f;
      ss.vx = 4.0f + rand() % 4;
      ss.vy = 2.0f + rand() % 2;
      ss.life = 0.0f;
      ss.max_life = 2.0f;
      model->shooting_stars.push_back(ss);
      model->last_shooting = now;
    }
    // Update shooting stars.
    for (auto it = model->shooting_stars.begin(); it != model->shooting_stars.end(); ) {
      it->x += it->vx * frame_units;
      it->y += it->vy * frame_units;
      it->life += dt;
      if (it->life >= it->max_life || it->y > h) {
        it = model->shooting_stars.erase(it);
      } else {
        ++it;
      }
    }

    // Fireworks: spawn periodically.
    if (model->fireworks.empty() &&
        std::chrono::duration<float>(now - model->last_firework).count() > 5.0f &&
        w > 30 && h > 6) {
      Firework fw;
      fw.x = static_cast<float>(15 + rand() % (w - 30));
      fw.y = static_cast<float>(h - 5);
      fw.vy = -3.0f - rand() % 2;
      fw.life = 0.0f;
      fw.max_life = 2.5f;
      model->fireworks.push_back(fw);
      model->last_firework = now;
    }
    // Update fireworks.
    for (auto it = model->fireworks.begin(); it != model->fireworks.end(); ) {
      it->y += it->vy * frame_units;
      it->life += dt;
      // Explode at apex or after max_life.
      if ((it->vy < 0 && it->y < h / 3) || it->life > 0.5f) {
        int particle_count = 20 + rand() % 15;
        float angle_step = 2.0f * 3.14159f / particle_count;
        float speed = 1.5f + rand() % 2;
        ftxui::Color color = RainbowColor(rand() % 7);
        for (int i = 0; i < particle_count; ++i) {
          float angle = i * angle_step + (rand() % 100) * 0.01f;
          it->particles.emplace_back(
            it->x + speed * std::cos(angle),
            it->y + speed * std::sin(angle)
          );
          it->particle_life.push_back(0.0f);
          it->particle_colors.push_back(color);
        }
        it = model->fireworks.erase(it);
      } else {
        ++it;
      }
    }

    // Center the art.
    const int art_w = std::max(1, model->art.width);
    const int art_h = std::max(1, model->art.height);
    const int ox = std::max(0, (w - art_w) / 2);
    const int oy = std::max(0, (h - art_h) / 2 - 1);

    // Bright base color for logo.
    auto base_logo_color = theme.base.highlight;

    // Build the logo lines with per-character styling.
    ftxui::Elements logo_rows;
    logo_rows.reserve(static_cast<size_t>(model->art.lines.size()));

    for (int y = 0; y < static_cast<int>(model->art.lines.size()); ++y) {
      const std::string &line = model->art.lines[static_cast<size_t>(y)];
      ftxui::Elements cells;
      cells.reserve(line.size());

      for (int x = 0; x < static_cast<int>(line.size()); ++x) {
        const char ch = line[static_cast<size_t>(x)];
        auto el = ftxui::text(std::string(1, ch));

        if (ch != ' ') {
          // Spectral sparkle: rainbow colors cycle across the logo.
          float sparkle_phase = (x * 0.5f + y * 0.5f + t * 3.0f);
          ftxui::Color sparkle_color = RainbowColor(sparkle_phase);

          // Random glint: some characters get bold rainbow, others base color.
          float glint_prob = 0.3f + 0.2f * std::sin(t * 5.0f + x * 0.5f + y * 0.5f);
          if (glint_prob > 0.5f) {
            el = el | ftxui::color(sparkle_color) | ftxui::bold;
          } else {
            el = el | ftxui::color(base_logo_color);
          }
          el = el | ftxui::bold;
        } else {
          el = el | ftxui::color(theme.base.bg);
        }
        cells.push_back(el);
      }

      logo_rows.push_back(ftxui::hbox(std::move(cells)));
    }

    auto logo = ftxui::vbox(std::move(logo_rows));

    // Starfield background layer.
    auto star_layer = ftxui::Renderer([model, ox, oy, art_w, art_h, t, theme] {
      std::vector<std::string> rows(static_cast<size_t>(std::max(1, model->last_h)),
                                    std::string(static_cast<size_t>(std::max(1, model->last_w)), ' '));

      // Static twinkling stars.
      for (const auto &s : model->stars) {
        if (s.x < 0 || s.x >= model->last_w || s.y < 0 || s.y >= model->last_h) continue;
        if (s.x >= ox && s.x < ox + art_w && s.y >= oy && s.y < oy + art_h) continue;
        const float phase = (t * 2.2f) + static_cast<float>(s.phase) * 0.01f;
        const float tw = 0.5f + 0.5f * std::sin(phase);
        rows[static_cast<size_t>(s.y)][static_cast<size_t>(s.x)] =
            (tw > 0.82f) ? '*' : ((tw > 0.55f) ? '+' : '.');
      }

      // Moving stars (drifting).
      for (const auto &ms : model->moving_stars) {
        int mx = static_cast<int>(ms.x);
        int my = static_cast<int>(ms.y);
        if (mx >= 0 && mx < model->last_w && my >= 0 && my < model->last_h) {
          if (!(mx >= ox && mx < ox + art_w && my >= oy && my < oy + art_h)) {
            rows[my][mx] = (ms.brightness > 0.7f) ? 'o' : '.';
          }
        }
      }

      // Shooting stars with trails.
      for (const auto &ss : model->shooting_stars) {
        int len = 6;
        for (int i = 0; i < len; ++i) {
          float sx = ss.x - i * ss.vx * 0.25f;
          float sy = ss.y + i * ss.vy * 0.25f;
          if (sx >= 0 && sx < model->last_w && sy >= 0 && sy < model->last_h) {
            if (!(sx >= ox && sx < ox + art_w && sy >= oy && sy < oy + art_h)) {
              char c = (i == 0) ? '*' : ((i < 2) ? '+' : '.');
              rows[static_cast<int>(sy)][static_cast<int>(sx)] = c;
            }
          }
        }
      }

      // Fireworks: rockets + explosion particles.
      for (const auto &fw : model->fireworks) {
        int fy = static_cast<int>(fw.y);
        int fx = static_cast<int>(fw.x);
        if (fy >= 0 && fy < model->last_h && fx >= 0 && fx < model->last_w) {
          if (!(fx >= ox && fx < ox + art_w && fy >= oy && fy < oy + art_h)) {
            rows[fy][fx] = '^';
          }
        }
        for (size_t i = 0; i < fw.particles.size(); ++i) {
          float px = fw.particles[i].first;
          float py = fw.particles[i].second;
          if (px >= 0 && px < model->last_w && py >= 0 && py < model->last_h) {
            if (!(px >= ox && px < ox + art_w && py >= oy && py < oy + art_h)) {
              rows[static_cast<int>(py)][static_cast<int>(px)] = '*';
            }
          }
        }
      }

      ftxui::Elements out;
      out.reserve(rows.size());
      for (const auto &row : rows) {
        out.push_back(ftxui::text(row) | ftxui::color(theme.base.dim));
      }
      return ftxui::vbox(std::move(out)) | ftxui::flex;
    });

    // Header.
    auto header = ftxui::vbox(ftxui::Elements({
                      ftxui::text("Welcome to Firmius") | ftxui::bold |
                          ftxui::color(base_logo_color) | ftxui::center,
                      ftxui::text("Type a message to start") | ftxui::dim |
                          ftxui::center,
                  })) | ftxui::center;

    // Body: header + logo.
    auto body = ftxui::vbox(ftxui::Elements({
                    header,
                    ftxui::text(""),
                    logo | ftxui::center,
                })) | ftxui::center;

    // Stack: starfield (background) + body (foreground).
    return ftxui::dbox(ftxui::Elements({
               star_layer->Render(),
               body | ftxui::flex,
           })) | ftxui::flex | ftxui::center;
  });

  return renderer;
}

} // namespace firmius::tui
