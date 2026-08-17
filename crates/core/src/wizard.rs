//! Setup wizards: pure async state machines that collect the credentials for
//! a new account. A wizard emits [`Step`]s and consumes answers; the
//! frontend (TUI modal, REPL prompt, or a scripted test harness) decides how
//! to render and collect. Wizards never touch the network or the filesystem
//! — validation beyond shape happens at `build()` time.

use crate::ProviderSchema;
use async_trait::async_trait;
use serde_json::Value;

/// One question a wizard asks the user.
#[derive(Debug, Clone, PartialEq)]
pub enum Step {
    /// Free-text input. Render masked when `secret` is set.
    Prompt { label: String, secret: bool },
    /// Constrained choice: the answer must be one of the option values.
    Select {
        label: String,
        options: Vec<SelectOption>,
    },
    /// Open a browser URL and wait for an out-of-band OAuth callback. The
    /// frontend owns launching the URL; the wizard owns receiving credentials.
    OpenUrl { label: String, url: String },
}

/// One selectable option in a [`Step::Select`]. `value` is what the wizard
/// consumes; `label` is what a UI shows.
#[derive(Debug, Clone, PartialEq)]
pub struct SelectOption {
    pub value: String,
    pub label: String,
}

impl SelectOption {
    pub fn new(value: impl Into<String>, label: impl Into<String>) -> Self {
        Self {
            value: value.into(),
            label: label.into(),
        }
    }
}

/// What a wizard returns after consuming an answer.
#[derive(Debug, Clone, PartialEq)]
pub enum Outcome {
    /// Ask the next question.
    Next(Step),
    /// The account is fully specified: the completed schema (base URL,
    /// static models, api_type — whatever this kind fixes) plus the
    /// kind-specific credential blob to persist.
    Done {
        schema: ProviderSchema,
        credentials: Value,
    },
}

#[derive(Debug, thiserror::Error)]
pub enum WizardError {
    #[error("invalid answer: {0}")]
    InvalidAnswer(String),
}

/// A credential-collecting flow for one [`crate::kinds::AccountKind`].
/// Async from day one: OAuth kinds will poll device-code endpoints mid-flow.
#[async_trait]
pub trait SetupWizard: Send {
    /// The first step of the flow.
    async fn start(&mut self) -> Step;
    /// Consume one answer; yields the next step or the finished account.
    async fn answer(&mut self, input: String) -> Result<Outcome, WizardError>;
    /// Poll an answer-free step such as OAuth. Frontends call this from their
    /// normal tick loop while [`Step::OpenUrl`] is visible.
    async fn poll(&mut self) -> Result<Option<Outcome>, WizardError> {
        Ok(None)
    }
}

/// Drive a wizard to completion with scripted answers — the headless harness
/// tests use today and non-TTY frontends can reuse later. Errors when the
/// answers run out before the wizard finishes.
pub async fn run_wizard(
    wizard: &mut dyn SetupWizard,
    answers: impl IntoIterator<Item = impl Into<String>>,
) -> Result<(ProviderSchema, Value), String> {
    let mut step = wizard.start().await;
    for answer in answers {
        match wizard
            .answer(answer.into())
            .await
            .map_err(|e| e.to_string())?
        {
            Outcome::Next(next) => step = next,
            Outcome::Done {
                schema,
                credentials,
            } => {
                let _ = step;
                return Ok((schema, credentials));
            }
        }
    }
    let _ = step;
    Err("wizard did not finish: ran out of answers".to_string())
}

/// Validate a [`Step::Select`] answer against its options: case-insensitive
/// match on the option *value*, with the valid choices named in the error.
pub fn match_select(step: &Step, input: &str) -> Result<String, WizardError> {
    let Step::Select { options, .. } = step else {
        return Err(WizardError::InvalidAnswer(
            "current step is not a select".to_string(),
        ));
    };
    let input = input.trim();
    for option in options {
        if option.value.eq_ignore_ascii_case(input) {
            return Ok(option.value.clone());
        }
    }
    let valid: Vec<&str> = options.iter().map(|o| o.value.as_str()).collect();
    Err(WizardError::InvalidAnswer(format!(
        "expected one of: {}",
        valid.join(", ")
    )))
}
