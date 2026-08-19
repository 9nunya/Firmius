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

// ---------------------------------------------------------------------------
// Five themes
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
    &[FIRMUS, MONOCHROME, JELLY, NORD, GRUVBOX]
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
}
