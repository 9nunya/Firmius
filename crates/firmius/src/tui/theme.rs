//! Theme system: one struct, five palettes, color math helpers.
//!
//! Every color in the TUI reads from a [`Theme`] rather than a hardcoded
//! [`ratatui::style::Color`] variant. All theme colors are `Color::Rgb` so
//! they interpolate cleanly for gradients and so a user can override a "dim
//! gray" with something warmer. The theme is stored on [`Model`] and threaded
//! through every `style` function and every presenter.

use ratatui::style::Color;

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Theme {
    pub name: &'static str,
    pub accent: Color,
    pub ok: Color,
    pub err: Color,
    pub warn: Color,
    pub dim: Color,
    /// Background for user-message blocks.
    pub dim_bg: Color,
    pub thinking: Color,
    /// Live-row gradient sweep, cool end.
    pub gradient_lo: Color,
    /// Live-row gradient sweep, warm end.
    pub gradient_hi: Color,
    /// Default text color (replaces `Style::default()`).
    pub fg: Color,
    /// Terminal background reference, for fades.
    pub bg: Color,
    pub border: Color,
    /// Modal/completion selected-row highlight background.
    pub selection_bg: Color,
}

/// Intensity of the short, one-way phrase-row glint. The phase is normalized
/// to a single pass; values outside the pass are static (zero intensity).
pub fn phrase_glint_intensity(phase: f32, len: usize, i: usize) -> f32 {
    if len == 0 || !(0.0..=1.0).contains(&phase) {
        return 0.0;
    }
    let center = phase * (len as f32 + 6.0) - 3.0;
    let distance = (i as f32 - center).abs();
    let band = (1.0 - distance / 3.0).clamp(0.0, 1.0);
    let fade = (1.0 - phase).powf(1.6);
    band * fade
}

/// Apply the shared glint intensity to an existing phrase gradient.
pub fn phrase_glint_at(theme: &Theme, base: Color, phase: f32, len: usize, i: usize) -> Color {
    lerp_color(base, theme.accent, phrase_glint_intensity(phase, len, i) * 0.85)
}

/// Deterministic gradient used by an event arrival cue.
///
/// Unlike the legacy live-row sweep this has no clock input.  A revision is
/// the only source of phase, so the same event always produces the same
/// gallery/test output and an unchanged event cannot animate by being
/// redrawn.
pub fn arrival_glint_at(theme: &Theme, revision: u64, len: usize, i: usize) -> Color {
    let phase = (revision % 4) as f32 / 4.0;
    gradient_at(theme, phase, len, i)
}

// ---------------------------------------------------------------------------
// Built-in themes
// ---------------------------------------------------------------------------

/// The default Firmius theme — today's cyan/green/red/yellow/magenta feel,
/// translated to RGB.
pub const FIRMUS: Theme = Theme {
    name: "firmius",
    accent: Color::Rgb(0, 200, 214),       // bright cyan
    ok: Color::Rgb(80, 220, 100),          // bright green
    err: Color::Rgb(255, 85, 85),          // bright red
    warn: Color::Rgb(255, 204, 0),         // amber-yellow
    dim: Color::Rgb(88, 88, 108),          // muted blue-gray
    dim_bg: Color::Rgb(26, 28, 38),        // dark slate
    thinking: Color::Rgb(200, 80, 220),    // magenta-purple
    gradient_lo: Color::Rgb(0, 180, 214),  // cyan
    gradient_hi: Color::Rgb(200, 80, 220), // magenta
    fg: Color::Rgb(220, 222, 230),         // light gray
    bg: Color::Rgb(12, 12, 18),            // near-black
    border: Color::Rgb(64, 66, 82),        // dim blue-gray
    selection_bg: Color::Rgb(40, 44, 58),  // dim slate
};

// Additional palettes.  Keeping these as plain `Theme` constants means they
// are available to the picker without any runtime allocation or parsing.
macro_rules! theme {
    ($name:literal, $accent:expr, $ok:expr, $err:expr, $warn:expr, $dim:expr,
     $dim_bg:expr, $thinking:expr, $lo:expr, $hi:expr, $fg:expr, $bg:expr,
     $border:expr, $selection:expr) => {
        Theme { name: $name, accent: $accent, ok: $ok, err: $err, warn: $warn,
            dim: $dim, dim_bg: $dim_bg, thinking: $thinking, gradient_lo: $lo,
            gradient_hi: $hi, fg: $fg, bg: $bg, border: $border,
            selection_bg: $selection }
    };
}

pub const LIGHT: Theme = theme!("light", Color::Rgb(0, 105, 140), Color::Rgb(25, 125, 55), Color::Rgb(190, 35, 45), Color::Rgb(170, 105, 0), Color::Rgb(95, 100, 110), Color::Rgb(232, 235, 240), Color::Rgb(125, 65, 150), Color::Rgb(0, 120, 170), Color::Rgb(175, 70, 150), Color::Rgb(35, 40, 50), Color::Rgb(250, 250, 248), Color::Rgb(170, 175, 185), Color::Rgb(215, 225, 238));
pub const OCEAN: Theme = theme!("ocean", Color::Rgb(70, 180, 220), Color::Rgb(90, 210, 150), Color::Rgb(240, 90, 110), Color::Rgb(245, 195, 80), Color::Rgb(90, 125, 150), Color::Rgb(20, 38, 58), Color::Rgb(180, 110, 220), Color::Rgb(35, 130, 210), Color::Rgb(120, 210, 220), Color::Rgb(210, 230, 240), Color::Rgb(10, 25, 45), Color::Rgb(45, 75, 105), Color::Rgb(30, 60, 85));
pub const FOREST: Theme = theme!("forest", Color::Rgb(100, 190, 130), Color::Rgb(150, 220, 100), Color::Rgb(230, 80, 70), Color::Rgb(235, 190, 70), Color::Rgb(100, 125, 100), Color::Rgb(20, 42, 28), Color::Rgb(180, 120, 190), Color::Rgb(70, 150, 100), Color::Rgb(180, 200, 90), Color::Rgb(215, 235, 210), Color::Rgb(15, 32, 20), Color::Rgb(50, 85, 55), Color::Rgb(35, 65, 40));
pub const SUNSET: Theme = theme!("sunset", Color::Rgb(255, 150, 70), Color::Rgb(120, 210, 110), Color::Rgb(255, 90, 100), Color::Rgb(255, 210, 80), Color::Rgb(150, 105, 100), Color::Rgb(48, 25, 28), Color::Rgb(205, 100, 180), Color::Rgb(220, 80, 100), Color::Rgb(255, 170, 60), Color::Rgb(245, 220, 210), Color::Rgb(35, 18, 25), Color::Rgb(100, 55, 60), Color::Rgb(75, 40, 45));
pub const DRACULA: Theme = theme!("dracula", Color::Rgb(189, 147, 249), Color::Rgb(80, 250, 123), Color::Rgb(255, 85, 85), Color::Rgb(255, 184, 108), Color::Rgb(98, 94, 125), Color::Rgb(40, 42, 54), Color::Rgb(255, 121, 198), Color::Rgb(139, 233, 253), Color::Rgb(189, 147, 249), Color::Rgb(248, 248, 242), Color::Rgb(40, 42, 54), Color::Rgb(68, 71, 90), Color::Rgb(68, 71, 90));
pub const TOKYO_NIGHT: Theme = theme!("tokyo-night", Color::Rgb(122, 162, 247), Color::Rgb(158, 206, 106), Color::Rgb(247, 118, 142), Color::Rgb(224, 175, 104), Color::Rgb(86, 95, 137), Color::Rgb(26, 27, 38), Color::Rgb(187, 154, 247), Color::Rgb(125, 207, 255), Color::Rgb(187, 154, 247), Color::Rgb(192, 202, 245), Color::Rgb(26, 27, 38), Color::Rgb(61, 66, 90), Color::Rgb(42, 46, 70));
pub const CATPPUCCIN: Theme = theme!("catppuccin", Color::Rgb(137, 180, 250), Color::Rgb(166, 227, 161), Color::Rgb(243, 139, 168), Color::Rgb(249, 226, 175), Color::Rgb(147, 153, 178), Color::Rgb(30, 30, 46), Color::Rgb(203, 166, 247), Color::Rgb(116, 199, 236), Color::Rgb(245, 194, 231), Color::Rgb(205, 214, 244), Color::Rgb(30, 30, 46), Color::Rgb(69, 71, 90), Color::Rgb(49, 50, 68));
pub const ROSE_PINE: Theme = theme!("rose-pine", Color::Rgb(196, 167, 231), Color::Rgb(156, 207, 163), Color::Rgb(235, 111, 146), Color::Rgb(246, 193, 119), Color::Rgb(144, 133, 166), Color::Rgb(25, 23, 36), Color::Rgb(235, 188, 186), Color::Rgb(156, 207, 216), Color::Rgb(235, 188, 186), Color::Rgb(224, 222, 244), Color::Rgb(25, 23, 36), Color::Rgb(64, 61, 82), Color::Rgb(45, 43, 58));
pub const SOLARIZED: Theme = theme!("solarized", Color::Rgb(38, 139, 210), Color::Rgb(133, 153, 0), Color::Rgb(220, 50, 47), Color::Rgb(181, 137, 0), Color::Rgb(101, 123, 131), Color::Rgb(0, 43, 54), Color::Rgb(211, 54, 130), Color::Rgb(42, 161, 152), Color::Rgb(203, 75, 22), Color::Rgb(238, 232, 213), Color::Rgb(0, 43, 54), Color::Rgb(7, 54, 66), Color::Rgb(7, 54, 66));
pub const EVERFOREST: Theme = theme!("everforest", Color::Rgb(127, 187, 179), Color::Rgb(167, 192, 128), Color::Rgb(230, 126, 128), Color::Rgb(219, 188, 127), Color::Rgb(133, 146, 137), Color::Rgb(39, 46, 40), Color::Rgb(211, 134, 155), Color::Rgb(131, 192, 146), Color::Rgb(230, 126, 128), Color::Rgb(211, 198, 170), Color::Rgb(39, 46, 40), Color::Rgb(75, 84, 76), Color::Rgb(54, 63, 56));
pub const MATRIX: Theme = theme!("matrix", Color::Rgb(0, 255, 90), Color::Rgb(80, 255, 80), Color::Rgb(255, 70, 70), Color::Rgb(220, 255, 60), Color::Rgb(40, 130, 60), Color::Rgb(0, 15, 4), Color::Rgb(0, 190, 150), Color::Rgb(0, 130, 40), Color::Rgb(0, 255, 90), Color::Rgb(170, 255, 180), Color::Rgb(0, 8, 2), Color::Rgb(0, 70, 20), Color::Rgb(0, 45, 12));
pub const CYBERPUNK: Theme = theme!("cyberpunk", Color::Rgb(255, 0, 180), Color::Rgb(0, 255, 180), Color::Rgb(255, 50, 90), Color::Rgb(255, 220, 0), Color::Rgb(130, 90, 150), Color::Rgb(25, 10, 35), Color::Rgb(150, 80, 255), Color::Rgb(0, 210, 255), Color::Rgb(255, 0, 180), Color::Rgb(245, 225, 255), Color::Rgb(18, 8, 28), Color::Rgb(90, 35, 110), Color::Rgb(55, 20, 70));
pub const PASTEL: Theme = theme!("pastel", Color::Rgb(120, 170, 230), Color::Rgb(120, 190, 140), Color::Rgb(220, 120, 140), Color::Rgb(220, 180, 100), Color::Rgb(140, 140, 160), Color::Rgb(42, 40, 55), Color::Rgb(190, 140, 210), Color::Rgb(120, 180, 220), Color::Rgb(220, 150, 190), Color::Rgb(230, 225, 240), Color::Rgb(38, 36, 50), Color::Rgb(78, 72, 95), Color::Rgb(58, 54, 72));
pub const COFFEE: Theme = theme!("coffee", Color::Rgb(210, 150, 95), Color::Rgb(150, 190, 110), Color::Rgb(220, 100, 90), Color::Rgb(235, 190, 90), Color::Rgb(145, 115, 100), Color::Rgb(45, 30, 23), Color::Rgb(190, 120, 170), Color::Rgb(170, 105, 75), Color::Rgb(220, 165, 90), Color::Rgb(235, 215, 190), Color::Rgb(35, 23, 18), Color::Rgb(90, 60, 45), Color::Rgb(65, 40, 30));
pub const VOLCANO: Theme = theme!("volcano", Color::Rgb(255, 110, 40), Color::Rgb(140, 210, 90), Color::Rgb(255, 60, 40), Color::Rgb(255, 200, 40), Color::Rgb(155, 90, 80), Color::Rgb(45, 18, 15), Color::Rgb(210, 80, 150), Color::Rgb(220, 55, 35), Color::Rgb(255, 155, 35), Color::Rgb(250, 220, 205), Color::Rgb(35, 12, 10), Color::Rgb(95, 40, 35), Color::Rgb(65, 25, 20));
pub const LAVENDER: Theme = theme!("lavender", Color::Rgb(175, 145, 240), Color::Rgb(120, 205, 150), Color::Rgb(240, 110, 150), Color::Rgb(240, 195, 110), Color::Rgb(130, 120, 155), Color::Rgb(35, 30, 48), Color::Rgb(220, 120, 200), Color::Rgb(125, 150, 235), Color::Rgb(220, 130, 210), Color::Rgb(230, 225, 245), Color::Rgb(28, 24, 40), Color::Rgb(75, 65, 100), Color::Rgb(52, 44, 70));

/// Grayscale only — a stress test for state legibility.
pub const MONOCHROME: Theme = Theme {
    name: "monochrome",
    accent: Color::Rgb(160, 160, 160),
    ok: Color::Rgb(130, 130, 130),
    err: Color::Rgb(255, 255, 255),
    warn: Color::Rgb(180, 180, 180),
    dim: Color::Rgb(90, 90, 90),
    dim_bg: Color::Rgb(22, 22, 22),
    thinking: Color::Rgb(110, 110, 110),
    gradient_lo: Color::Rgb(70, 70, 70),
    gradient_hi: Color::Rgb(200, 200, 200),
    fg: Color::Rgb(200, 200, 200),
    bg: Color::Rgb(12, 12, 12),
    border: Color::Rgb(70, 70, 70),
    selection_bg: Color::Rgb(40, 40, 40),
};

/// Purplish — accent violet, ok teal-green, err magenta-red.
pub const JELLY: Theme = Theme {
    name: "jelly",
    accent: Color::Rgb(170, 120, 255),      // violet
    ok: Color::Rgb(80, 200, 140),           // teal-green
    err: Color::Rgb(255, 80, 120),          // magenta-red
    warn: Color::Rgb(255, 190, 70),         // amber
    dim: Color::Rgb(90, 80, 110),           // dim purple-gray
    dim_bg: Color::Rgb(28, 24, 40),         // dark purple
    thinking: Color::Rgb(130, 60, 180),     // deep purple
    gradient_lo: Color::Rgb(140, 100, 255), // violet
    gradient_hi: Color::Rgb(255, 100, 180), // pink
    fg: Color::Rgb(215, 210, 235),          // light lavender
    bg: Color::Rgb(14, 12, 22),             // dark
    border: Color::Rgb(62, 56, 82),
    selection_bg: Color::Rgb(42, 36, 58),
};

/// The Nord palette (nordtheme.com).
pub const NORD: Theme = Theme {
    name: "nord",
    accent: Color::Rgb(136, 192, 208),      // nord8
    ok: Color::Rgb(163, 190, 140),          // nord14
    err: Color::Rgb(191, 97, 106),          // nord11
    warn: Color::Rgb(235, 203, 139),        // nord13
    dim: Color::Rgb(76, 86, 106),           // nord3
    dim_bg: Color::Rgb(46, 52, 64),         // nord0
    thinking: Color::Rgb(180, 142, 173),    // nord15 (purple)
    gradient_lo: Color::Rgb(94, 129, 172),  // nord10
    gradient_hi: Color::Rgb(136, 192, 208), // nord8
    fg: Color::Rgb(216, 222, 233),          // nord4
    bg: Color::Rgb(46, 52, 64),             // nord0
    border: Color::Rgb(59, 66, 82),         // nord1
    selection_bg: Color::Rgb(59, 66, 82),   // nord1
};

/// Gruvbox dark palette.
pub const GRUVBOX: Theme = Theme {
    name: "gruvbox",
    accent: Color::Rgb(131, 165, 152),     // blue
    ok: Color::Rgb(184, 187, 38),          // green
    err: Color::Rgb(251, 73, 52),          // red
    warn: Color::Rgb(250, 189, 47),        // yellow
    dim: Color::Rgb(146, 131, 116),        // gray
    dim_bg: Color::Rgb(40, 40, 40),        // bg0
    thinking: Color::Rgb(211, 134, 155),   // purple
    gradient_lo: Color::Rgb(69, 133, 136), // cyan-dark
    gradient_hi: Color::Rgb(250, 189, 47), // yellow
    fg: Color::Rgb(235, 219, 178),         // fg0
    bg: Color::Rgb(40, 40, 40),            // bg0
    border: Color::Rgb(60, 56, 54),        // bg1
    selection_bg: Color::Rgb(60, 56, 54),  // bg1
};

/// All built-in themes, in the order they appear in the `/theme` picker.
pub fn all() -> &'static [Theme] {
    &[FIRMUS, MONOCHROME, JELLY, NORD, GRUVBOX, LIGHT, OCEAN, FOREST,
      SUNSET, DRACULA, TOKYO_NIGHT, CATPPUCCIN, ROSE_PINE, SOLARIZED,
      EVERFOREST, MATRIX, CYBERPUNK, PASTEL, COFFEE, VOLCANO, LAVENDER]
}

/// Look up a theme by name (case-insensitive).
pub fn by_name(name: &str) -> Option<Theme> {
    let lower = name.to_lowercase();
    all().iter().copied().find(|t| t.name == lower)
}

/// The default theme.
pub const fn default_theme() -> Theme {
    FIRMUS
}

// ---------------------------------------------------------------------------
// Color math helpers
// ---------------------------------------------------------------------------

/// Linear interpolation between two colors. `t` is clamped to `[0, 1]`.
/// Only `Color::Rgb` is supported; other variants are returned unchanged.
pub fn lerp_color(a: Color, b: Color, t: f32) -> Color {
    let t = t.clamp(0.0, 1.0);
    match (a, b) {
        (Color::Rgb(r1, g1, b1), Color::Rgb(r2, g2, b2)) => {
            Color::Rgb(lerp_u8(r1, r2, t), lerp_u8(g1, g2, t), lerp_u8(b1, b2, t))
        }
        _ => a,
    }
}

/// Blend `c` toward white by `amount` (`0.0` = unchanged, `1.0` = white).
pub fn lighten(c: Color, amount: f32) -> Color {
    lerp_color(c, Color::Rgb(255, 255, 255), amount)
}

/// Blend `c` toward black by `amount` (`0.0` = unchanged, `1.0` = black).
pub fn darken(c: Color, amount: f32) -> Color {
    lerp_color(c, Color::Rgb(0, 0, 0), amount)
}

fn lerp_u8(a: u8, b: u8, t: f32) -> u8 {
    let a = a as f32;
    let b = b as f32;
    (a + (b - a) * t).round() as u8
}

/// Gradient color at a position along a sweep.
///
/// `t` is a normalized time value (typically derived from elapsed wall time,
/// modulo some period), `len` is the total number of characters, and `i` is
/// the character index. The result is a triangular wave between
/// `theme.gradient_lo` and `theme.gradient_hi` so the sweep loops without a
/// visible seam.
pub fn gradient_at(theme: &Theme, t: f32, len: usize, i: usize) -> Color {
    // Position of this character along the sweep: combines the character
    // index with a time-based phase so the wave moves across the string.
    let char_pos = (i as f32) / (1.0 + (len as f32).max(1.0));
    let phase = (t + char_pos) % 1.0;
    // Triangle wave: 0→lo, 0.5→hi, 1.0→lo
    let triangle = if phase < 0.5 {
        phase * 2.0
    } else {
        (1.0 - phase) * 2.0
    };
    lerp_color(theme.gradient_lo, theme.gradient_hi, triangle)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lerp_color_endpoints() {
        let a = Color::Rgb(0, 0, 0);
        let b = Color::Rgb(255, 255, 255);
        assert_eq!(lerp_color(a, b, 0.0), a);
        assert_eq!(lerp_color(a, b, 1.0), b);
    }

    #[test]
    fn lerp_color_midpoint() {
        let a = Color::Rgb(0, 0, 0);
        let b = Color::Rgb(200, 100, 50);
        let mid = lerp_color(a, b, 0.5);
        assert_eq!(mid, Color::Rgb(100, 50, 25));
    }

    #[test]
    fn lerp_color_clamps() {
        let a = Color::Rgb(0, 0, 0);
        let b = Color::Rgb(255, 255, 255);
        assert_eq!(lerp_color(a, b, -0.5), a);
        assert_eq!(lerp_color(a, b, 1.5), b);
    }

    #[test]
    fn lighten_toward_white() {
        let c = Color::Rgb(0, 0, 0);
        let light = lighten(c, 0.5);
        assert_eq!(light, Color::Rgb(128, 128, 128));
        assert_eq!(lighten(c, 1.0), Color::Rgb(255, 255, 255));
        assert_eq!(lighten(c, 0.0), c);
    }

    #[test]
    fn darken_toward_black() {
        let c = Color::Rgb(255, 255, 255);
        let dark = darken(c, 0.5);
        assert_eq!(dark, Color::Rgb(128, 128, 128));
        assert_eq!(darken(c, 1.0), Color::Rgb(0, 0, 0));
        assert_eq!(darken(c, 0.0), c);
    }

    #[test]
    fn all_themes_are_rgb_only() {
        for theme in all() {
            let fields = [
                theme.accent,
                theme.ok,
                theme.err,
                theme.warn,
                theme.dim,
                theme.dim_bg,
                theme.thinking,
                theme.gradient_lo,
                theme.gradient_hi,
                theme.fg,
                theme.bg,
                theme.border,
                theme.selection_bg,
            ];
            for color in fields {
                assert!(
                    matches!(color, Color::Rgb(_, _, _)),
                    "theme '{}' has a non-RGB color: {:?}",
                    theme.name,
                    color
                );
            }
        }
    }

    #[test]
    fn all_themes_have_distinct_gradient_ends() {
        for theme in all() {
            assert_ne!(
                theme.gradient_lo, theme.gradient_hi,
                "theme '{}' has identical gradient endpoints",
                theme.name
            );
        }
    }

    #[test]
    fn by_name_finds_themes() {
        assert_eq!(by_name("firmius"), Some(FIRMUS));
        assert_eq!(by_name("nord"), Some(NORD));
        assert_eq!(by_name("NORD"), Some(NORD));
        assert_eq!(by_name("gruvbox"), Some(GRUVBOX));
        assert_eq!(by_name("nonexistent"), None);
    }

    #[test]
    fn registry_contains_distinct_selectable_palettes() {
        assert_eq!(all().len(), 21);
        let mut names = all().iter().map(|theme| theme.name).collect::<Vec<_>>();
        names.sort_unstable();
        names.dedup();
        assert_eq!(names.len(), all().len());
        assert_eq!(by_name("LIGHT"), Some(LIGHT));
        assert!(matches!(LIGHT.fg, Color::Rgb(_, _, _)));
        assert!(matches!(LIGHT.bg, Color::Rgb(_, _, _)));
    }

    #[test]
    fn default_theme_is_firmius() {
        assert_eq!(default_theme(), FIRMUS);
    }

    #[test]
    fn gradient_at_loops_continuously() {
        let theme = FIRMUS;
        // Value at t and t+1.0 should match (period = 1.0).
        for i in 0..10 {
            let c1 = gradient_at(&theme, 0.3, 10, i);
            let c2 = gradient_at(&theme, 1.3, 10, i);
            assert_eq!(c1, c2, "gradient should loop with period 1.0 at char {i}");
        }
    }

    #[test]
    fn gradient_at_produces_rgb() {
        let theme = FIRMUS;
        for i in 0..5 {
            let c = gradient_at(&theme, 0.0, 5, i);
            assert!(matches!(c, Color::Rgb(_, _, _)));
        }
    }

    #[test]
    fn arrival_glint_is_deterministic_and_revision_driven() {
        let theme = FIRMUS;
        assert_eq!(
            arrival_glint_at(&theme, 7, 12, 3),
            arrival_glint_at(&theme, 7, 12, 3)
        );
        assert_ne!(
            arrival_glint_at(&theme, 0, 12, 3),
            arrival_glint_at(&theme, 1, 12, 3)
        );
    }

    #[test]
    fn phrase_glint_is_narrow_eased_and_static_outside_pass() {
        assert_eq!(phrase_glint_intensity(-0.1, 20, 3), 0.0);
        assert_eq!(phrase_glint_intensity(1.1, 20, 3), 0.0);
        let active = (0..20)
            .map(|i| phrase_glint_intensity(0.5, 20, i))
            .filter(|v| *v > 0.0)
            .count();
        assert!(active <= 6);
        assert!(phrase_glint_intensity(0.5, 20, 10) > phrase_glint_intensity(0.9, 20, 10));
    }

    #[test]
    fn phrase_glint_interpolates_toward_theme_accent() {
        let theme = Theme {
            accent: Color::Rgb(200, 0, 0),
            ..FIRMUS
        };
        let base = Color::Rgb(0, 0, 200);
        // At the sweep center intensity is 0.85, so the result should move
        // toward the theme accent rather than toward white.
        let glint = phrase_glint_at(&theme, base, 0.5, 1, 0);
        assert!(matches!(glint, Color::Rgb(r, g, b) if r > 0 && b < 200 && g == 0));
        assert_ne!(glint, lighten(base, 0.85));
    }
}