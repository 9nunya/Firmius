//! Split topology is independent of the native widget tree.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Axis {
    Horizontal,
    Vertical,
}
#[derive(Clone, Debug)]
pub(crate) enum Layout {
    Pane(String),
    Split {
        id: String,
        axis: Axis,
        ratio: f32,
        first: Box<Layout>,
        second: Box<Layout>,
    },
}
#[derive(Clone, Copy, Debug)]
pub(crate) struct Rect {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}
impl Default for Rect {
    fn default() -> Self {
        Self {
            x: 0.,
            y: 0.,
            width: 1.,
            height: 1.,
        }
    }
}
#[derive(Clone, Debug)]
pub(crate) struct Separator {
    pub id: String,
    pub axis: Axis,
    pub ratio: f32,
    pub bounds: Rect,
}
impl Layout {
    const MIN_RATIO: f32 = 0.25;
    const MAX_RATIO: f32 = 0.75;

    pub fn reverse_split(&mut self, target: &str) {
        if let Self::Split {
            id, first, second, ..
        } = self
        {
            if id == target {
                std::mem::swap(first, second);
            } else {
                first.reverse_split(target);
                second.reverse_split(target);
            }
        }
    }
    pub fn split(&mut self, pane: &str, new: String, id: String, axis: Axis) -> bool {
        match self {
            Self::Pane(current) if current == pane => {
                *self = Self::Split {
                    id,
                    axis,
                    ratio: 0.5,
                    first: Box::new(Self::Pane(current.clone())),
                    second: Box::new(Self::Pane(new)),
                };
                true
            }
            Self::Pane(_) => false,
            Self::Split { first, second, .. } => {
                first.split(pane, new.clone(), id.clone(), axis)
                    || second.split(pane, new, id, axis)
            }
        }
    }
    pub fn resize(&mut self, target: &str, value: f32) {
        if !value.is_finite() {
            return;
        }
        if let Self::Split {
            id,
            ratio,
            first,
            second,
            ..
        } = self
        {
            if id == target {
                *ratio = value.clamp(Self::MIN_RATIO, Self::MAX_RATIO);
            } else {
                first.resize(target, value);
                second.resize(target, value);
            }
        }
    }
    pub fn prune(self, panes: &[String]) -> Option<Self> {
        match self {
            Self::Pane(id) => panes.contains(&id).then_some(Self::Pane(id)),
            Self::Split {
                id,
                axis,
                ratio,
                first,
                second,
            } => match (first.prune(panes), second.prune(panes)) {
                (Some(first), Some(second)) => Some(Self::Split {
                    id,
                    axis,
                    ratio,
                    first: Box::new(first),
                    second: Box::new(second),
                }),
                (Some(remaining), None) | (None, Some(remaining)) => Some(remaining),
                _ => None,
            },
        }
    }
    pub fn regions(
        &self,
        rect: Rect,
        panes: &mut Vec<(String, Rect)>,
        handles: &mut Vec<Separator>,
    ) {
        match self {
            Self::Pane(id) => panes.push((id.clone(), rect)),
            Self::Split {
                id,
                axis,
                ratio,
                first,
                second,
            } => {
                handles.push(Separator {
                    id: id.clone(),
                    axis: *axis,
                    ratio: *ratio,
                    bounds: rect,
                });
                let (a, b) = match axis {
                    Axis::Horizontal => (
                        Rect {
                            width: rect.width * ratio,
                            ..rect
                        },
                        Rect {
                            x: rect.x + rect.width * ratio,
                            width: rect.width * (1. - ratio),
                            ..rect
                        },
                    ),
                    Axis::Vertical => (
                        Rect {
                            height: rect.height * ratio,
                            ..rect
                        },
                        Rect {
                            y: rect.y + rect.height * ratio,
                            height: rect.height * (1. - ratio),
                            ..rect
                        },
                    ),
                };
                first.regions(a, panes, handles);
                second.regions(b, panes, handles);
            }
        }
    }
}

/// Compact pane-switcher mode follows the window, not leftover workspace after
/// the sidebar. Sidebar rail/drawer motion therefore cannot hide splits.
pub(crate) fn window_is_compact(width: f32, height: f32, currently: bool) -> bool {
    let (width_limit, height_limit) = if currently {
        (504.0, 512.0)
    } else {
        (480.0, 480.0)
    };
    width < width_limit || height < height_limit
}

/// Secondary metadata and floats collapse inside a pane, independently of the
/// window-level switcher. 24 px / 32 px hysteresis matches the design contract.
pub(crate) fn pane_is_compact(width: f32, height: f32, currently: bool) -> bool {
    let (width_limit, height_limit) = if currently {
        (324.0, 252.0)
    } else {
        (300.0, 220.0)
    };
    width < width_limit || height < height_limit
}

pub(crate) fn drop_edge(local_x: f32, local_y: f32) -> &'static str {
    if local_x < 0.22 {
        "left"
    } else if local_x > 0.78 {
        "right"
    } else if local_y < 0.22 {
        "top"
    } else if local_y > 0.78 {
        "bottom"
    } else {
        "center"
    }
}

pub(crate) fn drop_preview(bounds: Rect, edge: &str) -> Rect {
    match edge {
        "left" => Rect {
            width: bounds.width * 0.5,
            ..bounds
        },
        "right" => Rect {
            x: bounds.x + bounds.width * 0.5,
            width: bounds.width * 0.5,
            ..bounds
        },
        "top" => Rect {
            height: bounds.height * 0.5,
            ..bounds
        },
        "bottom" => Rect {
            y: bounds.y + bounds.height * 0.5,
            height: bounds.height * 0.5,
            ..bounds
        },
        _ => bounds,
    }
}

pub(crate) fn hit_pane<'a>(
    panes: &'a [(String, Rect)],
    x: f32,
    y: f32,
) -> Option<&'a (String, Rect)> {
    panes.iter().find(|(_, rect)| {
        x >= rect.x && y >= rect.y && x <= rect.x + rect.width && y <= rect.y + rect.height
    })
}

pub(crate) fn adjacent_pane(layout: &Layout, focused: &str, direction: &str) -> Option<String> {
    let (mut panes, mut handles) = (Vec::new(), Vec::new());
    layout.regions(Rect::default(), &mut panes, &mut handles);
    let current = panes.iter().find(|(id, _)| id == focused)?.1;
    let mut best: Option<(f32, String)> = None;
    for (id, rect) in &panes {
        if id == focused {
            continue;
        }
        let overlap_y =
            current.y.max(rect.y) < (current.y + current.height).min(rect.y + rect.height);
        let overlap_x =
            current.x.max(rect.x) < (current.x + current.width).min(rect.x + rect.width);
        let (ok, dist) = match direction {
            "left" if overlap_y && rect.x + rect.width <= current.x + 0.02 => {
                (true, current.x - (rect.x + rect.width))
            }
            "right" if overlap_y && rect.x + 0.02 >= current.x + current.width => {
                (true, rect.x - (current.x + current.width))
            }
            "up" if overlap_x && rect.y + rect.height <= current.y + 0.02 => {
                (true, current.y - (rect.y + rect.height))
            }
            "down" if overlap_x && rect.y + 0.02 >= current.y + current.height => {
                (true, rect.y - (current.y + current.height))
            }
            _ => (false, 0.0),
        };
        if ok && best.as_ref().is_none_or(|(best_dist, _)| dist < *best_dist) {
            best = Some((dist, id.clone()));
        }
    }
    best.map(|(_, id)| id)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn resize_rejects_invalid_values_and_preserves_minimum_panes() {
        let mut layout = Layout::Pane("a".into());
        assert!(layout.split("a", "b".into(), "split".into(), Axis::Horizontal));
        layout.resize("split", 0.02);
        let (mut panes, mut handles) = (vec![], vec![]);
        layout.regions(Rect::default(), &mut panes, &mut handles);
        assert_eq!(handles[0].ratio, 0.25);
        assert_eq!(panes[0].1.width, 0.25);
        assert_eq!(panes[1].1.width, 0.75);

        layout.resize("split", f32::NAN);
        panes.clear();
        handles.clear();
        layout.regions(Rect::default(), &mut panes, &mut handles);
        assert_eq!(handles[0].ratio, 0.25);
    }

    #[test]
    fn nested_splits_resize_and_collapse_without_losing_siblings() {
        let mut layout = Layout::Pane("a".into());
        assert!(layout.split("a", "b".into(), "ab".into(), Axis::Horizontal));
        assert!(layout.split("b", "c".into(), "bc".into(), Axis::Vertical));
        layout.resize("ab", 0.6);
        let (mut panes, mut handles) = (vec![], vec![]);
        layout.regions(Rect::default(), &mut panes, &mut handles);
        assert_eq!(panes.len(), 3);
        assert_eq!(handles.len(), 2);
        assert_eq!(panes[0].1.width, 0.6);
        assert_eq!(panes[2].1.y, 0.5);
        let layout = layout.prune(&["a".into(), "c".into()]).unwrap();
        panes.clear();
        handles.clear();
        layout.regions(Rect::default(), &mut panes, &mut handles);
        assert_eq!(panes.len(), 2);
        assert_eq!(handles.len(), 1);
        assert_eq!(panes[1].1.height, 1.0);
    }

    #[test]
    fn window_compact_ignores_sidebar_width_and_uses_hysteresis() {
        assert!(!window_is_compact(1280.0, 850.0, false));
        assert!(!window_is_compact(960.0, 700.0, false));
        assert!(!window_is_compact(719.0, 700.0, false));
        assert!(!window_is_compact(480.0, 700.0, false));
        assert!(window_is_compact(479.0, 700.0, false));
        assert!(window_is_compact(360.0, 700.0, false));
        assert!(window_is_compact(490.0, 700.0, true));
        assert!(!window_is_compact(504.0, 700.0, true));
        let workspace_after_sidebar = 960.0 - 244.0;
        assert!(workspace_after_sidebar / 2.0 < 480.0);
        assert!(
            !window_is_compact(960.0, 700.0, false),
            "sidebar leftover must not flip compact mode"
        );
    }

    #[test]
    fn pane_compact_uses_local_geometry_not_window() {
        assert!(!pane_is_compact(400.0, 400.0, false));
        assert!(pane_is_compact(280.0, 400.0, false));
        assert!(pane_is_compact(400.0, 200.0, false));
        assert!(pane_is_compact(310.0, 400.0, true));
        assert!(!pane_is_compact(324.0, 252.0, true));
    }

    #[test]
    fn drop_targets_use_visible_bounds_including_compact_fill() {
        let visible = Rect {
            x: 0.0,
            y: 0.0,
            width: 1.0,
            height: 1.0,
        };
        assert_eq!(drop_edge(0.1, 0.5), "left");
        assert_eq!(drop_edge(0.9, 0.5), "right");
        assert_eq!(drop_edge(0.5, 0.1), "top");
        assert_eq!(drop_edge(0.5, 0.9), "bottom");
        assert_eq!(drop_edge(0.5, 0.5), "center");
        let preview = drop_preview(visible, "right");
        assert!((preview.x - 0.5).abs() < f32::EPSILON);
        assert!((preview.width - 0.5).abs() < f32::EPSILON);
        let hidden_split = vec![
            (
                "a".into(),
                Rect {
                    x: 0.0,
                    y: 0.0,
                    width: 0.5,
                    height: 1.0,
                },
            ),
            (
                "b".into(),
                Rect {
                    x: 0.5,
                    y: 0.0,
                    width: 0.5,
                    height: 1.0,
                },
            ),
        ];
        assert_eq!(hit_pane(&hidden_split, 0.75, 0.5).unwrap().0, "b");
        assert_eq!(
            hit_pane(&[("focused".into(), visible)], 0.75, 0.5)
                .unwrap()
                .0,
            "focused"
        );
    }

    #[test]
    fn adjacent_pane_follows_shared_edges() {
        let mut layout = Layout::Pane("a".into());
        assert!(layout.split("a", "b".into(), "ab".into(), Axis::Horizontal));
        assert!(layout.split("b", "c".into(), "bc".into(), Axis::Vertical));
        assert_eq!(adjacent_pane(&layout, "a", "right").as_deref(), Some("b"));
        assert_eq!(adjacent_pane(&layout, "b", "left").as_deref(), Some("a"));
        assert_eq!(adjacent_pane(&layout, "b", "down").as_deref(), Some("c"));
        assert_eq!(adjacent_pane(&layout, "c", "up").as_deref(), Some("b"));
        assert_eq!(adjacent_pane(&layout, "a", "left"), None);
    }
}
