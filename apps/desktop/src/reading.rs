//! Logical reading anchors survive row growth, disclosure and view switching.
use std::collections::HashMap;
#[derive(Clone, Debug)]
pub(crate) struct ReadingState {
    pub following: bool,
    pub y: f32,
    anchor: Option<(String, f32)>,
    bounds: HashMap<String, (f32, f32)>,
}
impl Default for ReadingState {
    fn default() -> Self {
        Self {
            following: true,
            y: 0.0,
            anchor: None,
            bounds: HashMap::new(),
        }
    }
}
impl ReadingState {
    pub fn scroll(&mut self, y: f32, following: bool) {
        self.y = y;
        self.following = following;
        self.anchor = if following {
            None
        } else {
            self.bounds
                .iter()
                .filter(|(_, (top, height))| *top <= -y && *top + *height > -y)
                .max_by(|a, b| a.1.0.total_cmp(&b.1.0))
                .map(|(key, (top, _))| (key.clone(), -y - top))
        };
    }
    pub fn measure(&mut self, key: String, top: f32, height: f32) -> Option<f32> {
        if !top.is_finite() || !height.is_finite() {
            return None;
        }
        self.bounds.insert(key.clone(), (top, height));
        if !self.following {
            if let Some((anchor, offset)) = &self.anchor {
                if anchor == &key {
                    let y = (-top - offset).min(0.0);
                    if (self.y - y).abs() > 0.5 {
                        self.y = y;
                        return Some(y);
                    }
                }
            }
        }
        None
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn growing_preceding_content_keeps_the_same_passage() {
        let mut state = ReadingState::default();
        state.measure("a".into(), 0.0, 100.0);
        state.measure("b".into(), 100.0, 200.0);
        state.scroll(-140.0, false);
        assert_eq!(state.measure("b".into(), 180.0, 200.0), Some(-220.0));
        assert_eq!(state.measure("b".into(), 180.0, 220.0), None);
        assert_eq!(state.y, -220.0);
        state.scroll(-300.0, true);
        assert_eq!(state.measure("b".into(), 220.0, 200.0), None);
    }
    #[test]
    fn view_reading_states_are_independent() {
        let mut first = ReadingState::default();
        first.measure("a".into(), 0.0, 200.0);
        first.scroll(-30.0, false);
        let mut second = first.clone();
        second.scroll(0.0, true);
        assert!(!first.following);
        assert_eq!(first.y, -30.0);
        assert!(second.following);
    }
}
