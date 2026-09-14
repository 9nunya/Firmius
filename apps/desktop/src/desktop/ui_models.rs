//! UI-thread model patching. Never replace a model merely because a snapshot arrived.
use slint::{Model, ModelRc, SharedString, VecModel};

pub(super) fn patch<T: Clone + PartialEq + 'static>(
    previous: &ModelRc<T>,
    next: Vec<T>,
    key: impl Fn(&T) -> SharedString,
) -> ModelRc<T> {
    let Some(model) = previous.as_any().downcast_ref::<VecModel<T>>() else {
        return ModelRc::new(VecModel::from(next));
    };
    for (index, row) in next.iter().enumerate() {
        if model
            .row_data(index)
            .is_some_and(|old| key(&old) == key(row))
        {
            if model.row_data(index).as_ref() != Some(row) {
                model.set_row_data(index, row.clone());
            }
        } else {
            if let Some(existing) = (index..model.row_count())
                .find(|&i| model.row_data(i).is_some_and(|old| key(&old) == key(row)))
            {
                model.remove(existing);
            }
            model.insert(index, row.clone());
        }
    }
    while model.row_count() > next.len() {
        model.remove(next.len());
    }
    previous.clone()
}

pub(super) fn patch_rows(
    previous: &ModelRc<super::TranscriptRow>,
    mut next: Vec<super::TranscriptRow>,
) -> ModelRc<super::TranscriptRow> {
    // Runtime frames arrive at 30 Hz. Most frames only update the daemon
    // status projection, so avoid walking and patching every nested Slint
    // model when the transcript's visible scalar content is unchanged.
    if previous.row_count() == next.len()
        && previous.iter().zip(next.iter()).all(|(old, row)| {
            old.key == row.key
                && old.body == row.body
                && old.detail == row.detail
                && old.presenter == row.presenter
                && old.tone == row.tone
                && old.activity.summary == row.activity.summary
                && old.activity.state == row.activity.state
                && old.activity.invocation == row.activity.invocation
                && old.activity.command == row.activity.command
                && old.activity.output == row.activity.output
                && old.activity.patch == row.activity.patch
                && old.activity.omitted == row.activity.omitted
                && old.activity.files.row_count() == row.activity.files.row_count()
                && old.activity.lines.row_count() == row.activity.lines.row_count()
                && old.activity.matches.row_count() == row.activity.matches.row_count()
                && old.activity.resources.row_count() == row.activity.resources.row_count()
                && old.activity.children.row_count() == row.activity.children.row_count()
        })
    {
        return previous.clone();
    }
    let old: std::collections::HashMap<_, _> =
        previous.iter().map(|r| (r.key.clone(), r)).collect();
    for row in &mut next {
        if let Some(old) = old.get(&row.key) {
            if old
                .activity
                .children
                .iter()
                .eq(row.activity.children.iter())
            {
                row.activity.children = old.activity.children.clone();
            }
            let same_terminal = old.activity.terminal.row_count()
                == row.activity.terminal.row_count()
                && old
                    .activity
                    .terminal
                    .iter()
                    .zip(row.activity.terminal.iter())
                    .all(|(a, b)| a.spans.iter().eq(b.spans.iter()));
            if same_terminal {
                row.activity.terminal = old.activity.terminal.clone();
            }
            if old.activity.lines.iter().eq(row.activity.lines.iter()) {
                row.activity.lines = old.activity.lines.clone();
            }
            if old.activity.matches.iter().eq(row.activity.matches.iter()) {
                row.activity.matches = old.activity.matches.clone();
            }
            let mut files: Vec<_> = row.activity.files.iter().collect();
            for file in &mut files {
                if let Some(previous) = old.activity.files.iter().find(|f| f.path == file.path) {
                    if previous.lines.iter().eq(file.lines.iter()) {
                        file.lines = previous.lines;
                    }
                }
            }
            row.activity.files = patch(&old.activity.files, files, |f| f.path.clone());
            if old
                .activity
                .resources
                .iter()
                .eq(row.activity.resources.iter())
            {
                row.activity.resources = old.activity.resources.clone();
            }
            // Nested model identity matters too: unchanged markdown must not
            // invalidate its visual children or text selection.
            if old.blocks.iter().eq(row.blocks.iter()) {
                row.blocks = old.blocks.clone();
            }
        }
    }
    patch(previous, next, |row| row.key.clone())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn updates_insertions_and_removals_keep_model_identity() {
        let model = ModelRc::new(VecModel::from(vec![SharedString::from("a"), "c".into()]));
        let updated = patch(
            &model,
            vec!["a".into(), "b".into(), "c".into()],
            Clone::clone,
        );
        assert_eq!(model, updated);
        assert_eq!(model.iter().collect::<Vec<_>>(), vec!["a", "b", "c"]);
        let updated = patch(&model, vec!["c".into(), "b".into()], Clone::clone);
        assert_eq!(model, updated);
        assert_eq!(model.iter().collect::<Vec<_>>(), vec!["c", "b"]);
    }
    #[test]
    fn identical_projection_retains_nested_block_models() {
        let row = super::super::transcript_row("agent", "some **text**", "assistant", "message");
        let original_blocks = row.blocks.clone();
        let model = ModelRc::new(VecModel::from(vec![row]));
        let next = super::super::transcript_row("agent", "some **text**", "assistant", "message");
        let updated = patch_rows(&model, vec![next]);
        assert_eq!(model, updated);
        assert_eq!(model.row_data(0).unwrap().blocks, original_blocks);
    }

    #[test]
    fn omitted_notice_change_patches_activity_without_replacing_the_row_key() {
        let mut row = super::super::transcript_row("grep", "matches", "tool", "Completed");
        row.key = "lead:tool:1".into();
        row.activity.omitted = "".into();
        let model = ModelRc::new(VecModel::from(vec![row.clone()]));
        row.activity.omitted = "[...truncated at 200 matches...]".into();
        let updated = patch_rows(&model, vec![row]);
        assert_eq!(model, updated);
        assert_eq!(
            model.row_data(0).unwrap().activity.omitted,
            "[...truncated at 200 matches...]"
        );
    }
}
