//! The retry engine: a pure decision machine that turns a stream of provider
//! failures into "retry (and how long to wait, and on which account)" or
//! "give up (and why)". It is deliberately free of any I/O, provider, or
//! `ProviderManager` dependency so it can be unit-tested exhaustively; the
//! agent owns the actual sleeping and account rebuilding.
//!
//! Flow:
//! ```text
//! let mut ctrl = RetryController::new(config, accounts);
//! loop {
//!     match provider.stream(req).await {
//!         Ok(stream) => break,           // success
//!         Err(err) => match ctrl.on_failure(&err) {
//!             RetryDecision::Retry { account_id, delay, .. } => {
//!                 // switch provider if account_id changed, sleep(delay), retry
//!             }
//!             RetryDecision::Exhausted { reason, last_error } => {
//!                 return Err(last_error);  // surface the real provider error
//!             }
//!         },
//!     }
//! }
//! ```

use crate::config::{FailureClasses, RetryConfig};
use crate::providers::ProviderError;
use std::time::Duration;

// ---------------------------------------------------------------------------
// Failure classification — the provider-agnostic seam
// ---------------------------------------------------------------------------

/// A normalized bucket for any provider failure. Providers report errors in
/// their own vocabulary ([`ProviderError`]); classification maps that onto the
/// small set of behaviors the retry policy reasons about.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FailureClass {
    /// Rate limited (HTTP 429/529). Backoff, honor `Retry-After`.
    RateLimited,
    /// Server-side error (HTTP 5xx). Transient; worth a retry.
    ServerError,
    /// Network/transport failure, or a request-timeout-ish status.
    Transport,
    /// The response could not be decoded. Occasionally transient.
    Decode,
    /// Authentication failed (HTTP 401/403 or `Auth`). A different account may
    /// succeed; the same one usually will not.
    Auth,
    /// A client request error that will never succeed on retry (other 4xx).
    NonRetryable,
}

impl FailureClass {
    /// Whether this class is eligible for the given [`FailureClasses`] toggles.
    fn allowed_by(self, classes: &FailureClasses) -> bool {
        match self {
            FailureClass::RateLimited => classes.rate_limited,
            FailureClass::ServerError => classes.server_error,
            FailureClass::Transport => classes.transport,
            FailureClass::Decode => classes.decode,
            FailureClass::Auth => classes.auth,
            // A genuine client error is never eligible, regardless of toggles.
            FailureClass::NonRetryable => false,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            FailureClass::RateLimited => "rate limited",
            FailureClass::ServerError => "server error",
            FailureClass::Transport => "transport error",
            FailureClass::Decode => "decode error",
            FailureClass::Auth => "auth error",
            FailureClass::NonRetryable => "non-retryable error",
        }
    }
}

/// Classify a provider error into a [`FailureClass`].
pub fn classify(error: &ProviderError) -> FailureClass {
    match error {
        ProviderError::Http(_) => FailureClass::Transport,
        ProviderError::Decode(_) => FailureClass::Decode,
        ProviderError::Auth(_) => FailureClass::Auth,
        ProviderError::Api { status, .. } => match status {
            429 | 529 => FailureClass::RateLimited,
            401 | 403 => FailureClass::Auth,
            408 | 425 => FailureClass::Transport,
            s if *s >= 500 => FailureClass::ServerError,
            _ => FailureClass::NonRetryable,
        },
    }
}

/// Best-effort extraction of a server-provided retry delay from an error body
/// or status. Returns the seconds a `Retry-After` style hint suggests. Today
/// the providers do not surface headers, so this reads the numeric hint from a
/// JSON error body when present (e.g. `{"error":{"retry_after":12}}`). When the
/// providers start attaching headers this is where they get honored.
pub fn retry_after_hint(error: &ProviderError) -> Option<Duration> {
    let ProviderError::Api { body, .. } = error else {
        return None;
    };
    let value: serde_json::Value = serde_json::from_str(body).ok()?;
    // Accept a few common shapes without being fussy.
    for path in [
        value.get("retry_after"),
        value.get("error").and_then(|e| e.get("retry_after")),
        value.get("retry_after_seconds"),
        value
            .get("error")
            .and_then(|e| e.get("retry_after_seconds")),
    ]
    .into_iter()
    .flatten()
    {
        if let Some(secs) = path.as_f64().filter(|s| *s >= 0.0) {
            return Some(Duration::from_millis((secs * 1000.0) as u64));
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------

/// Why the engine stopped retrying. Each variant maps to a distinct user-facing
/// message; all of them carry the underlying provider error so the UI can show
/// what actually went wrong.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExhaustionReason {
    /// Retries are disabled entirely (config `enabled = false`).
    Disabled,
    /// The failure class is never retryable (e.g. a 400 bad request), or the
    /// config toggles excluded it from both retry and switch.
    NonRetryable,
    /// This account's per-account attempts ran out and either switching is off
    /// or there is only one account.
    AccountRetriesExhausted,
    /// Every candidate account was tried and all failed.
    AllAccountsFailed,
    /// The overall wall-clock budget (`max_elapsed_ms`) was exceeded.
    TimeBudgetExceeded,
    /// The `max_accounts` cap was reached before success.
    AccountCapReached,
}

impl ExhaustionReason {
    /// A short human sentence for the transcript/status line.
    pub fn describe(self, accounts_tried: usize) -> String {
        match self {
            ExhaustionReason::Disabled => "retries are disabled".to_string(),
            ExhaustionReason::NonRetryable => "the error is not retryable".to_string(),
            ExhaustionReason::AccountRetriesExhausted => {
                "retries on this account are exhausted".to_string()
            }
            ExhaustionReason::AllAccountsFailed => {
                format!("all {accounts_tried} account(s) for this provider failed")
            }
            ExhaustionReason::TimeBudgetExceeded => {
                "the retry time budget was exceeded".to_string()
            }
            ExhaustionReason::AccountCapReached => {
                "the maximum number of accounts to try was reached".to_string()
            }
        }
    }
}

/// What the controller decided to do about one failure.
#[derive(Debug, Clone)]
pub enum RetryDecision {
    /// Try again. The caller should switch to `account_id` if it differs from
    /// the account it just used, wait `delay`, then re-issue the request.
    Retry {
        /// The account to use for the next attempt.
        account_id: String,
        /// How long to wait before the next attempt.
        delay: Duration,
        /// 1-based attempt number *on `account_id`* that is about to run.
        attempt: u32,
        /// True when this decision moves to a different account than the last.
        switched: bool,
    },
    /// Stop. Surface `last_error` with an explanation built from `reason`.
    Exhausted {
        reason: ExhaustionReason,
        last_error: ProviderError,
    },
}

// ---------------------------------------------------------------------------
// Jitter — deterministic-in-tests, cheap, no dependency
// ---------------------------------------------------------------------------

/// A tiny, self-contained jitter source. We do not pull in `rand` for this:
/// a SplitMix64 step seeded from the clock (or a fixed seed in tests) is more
/// than enough entropy for spreading retry delays.
#[derive(Debug, Clone)]
pub struct Jitter {
    state: u64,
}

impl Jitter {
    pub fn from_entropy() -> Self {
        let seed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0x9E37_79B9_7F4A_7C15)
            ^ (std::process::id() as u64).rotate_left(17);
        Self { state: seed | 1 }
    }

    pub fn seeded(seed: u64) -> Self {
        Self { state: seed | 1 }
    }

    /// Next float in `[0, 1)`.
    fn next_unit(&mut self) -> f64 {
        // SplitMix64.
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^= z >> 31;
        // Top 53 bits to a double in [0,1).
        (z >> 11) as f64 / (1u64 << 53) as f64
    }

    /// Apply a symmetric jitter of `fraction` (0..=1) to `base_ms`.
    /// `fraction = 0` returns `base_ms` unchanged (used by tests).
    pub fn apply(&mut self, base_ms: u64, fraction: f64) -> u64 {
        let fraction = fraction.clamp(0.0, 1.0);
        if fraction == 0.0 || base_ms == 0 {
            return base_ms;
        }
        // Map [0,1) to [-fraction, +fraction).
        let signed = (self.next_unit() * 2.0 - 1.0) * fraction;
        let scaled = base_ms as f64 * (1.0 + signed);
        scaled.max(0.0) as u64
    }
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

/// Drives the retry decision for one turn. Construct it with the resolved
/// [`RetryConfig`] and the ordered list of candidate account ids (the account
/// currently in use first, then its siblings). Feed it each failure via
/// [`on_failure`](Self::on_failure).
pub struct RetryController {
    config: RetryConfig,
    /// Candidate accounts in the order they will be tried.
    accounts: Vec<String>,
    /// Index into `accounts` of the account currently being attempted.
    current: usize,
    /// Attempts already spent on the current account (initial try included).
    attempts_on_current: u32,
    /// Distinct accounts attempted so far (for the `max_accounts` cap and
    /// for reporting).
    accounts_tried: usize,
    /// Wall-clock start, for the `max_elapsed_ms` budget.
    started: std::time::Instant,
    jitter: Jitter,
}

impl RetryController {
    /// `accounts` must be non-empty and start with the account in use. When
    /// only one account is known, switching is inherently impossible and the
    /// controller falls back to same-account retries.
    pub fn new(config: RetryConfig, accounts: Vec<String>) -> Self {
        Self::with_jitter(config, accounts, Jitter::from_entropy())
    }

    /// Test seam: inject a seeded jitter for reproducible delays.
    pub fn with_jitter(config: RetryConfig, accounts: Vec<String>, jitter: Jitter) -> Self {
        debug_assert!(!accounts.is_empty(), "at least one account is required");
        Self {
            config,
            accounts,
            current: 0,
            attempts_on_current: 1, // the initial attempt has already happened
            accounts_tried: 1,
            started: std::time::Instant::now(),
            jitter,
        }
    }

    /// The account id currently in use.
    pub fn current_account(&self) -> &str {
        &self.accounts[self.current]
    }

    /// How many distinct accounts have been attempted so far.
    pub fn accounts_tried(&self) -> usize {
        self.accounts_tried
    }

    /// Whether a further account exists to switch to.
    fn has_next_account(&self) -> bool {
        self.current + 1 < self.accounts.len()
    }

    /// Whether the account cap (if any) still permits another account.
    fn under_account_cap(&self) -> bool {
        self.config.max_accounts == 0 || self.accounts_tried < self.config.max_accounts as usize
    }

    fn time_budget_exceeded(&self) -> bool {
        self.config.max_elapsed_ms != 0
            && self.started.elapsed() >= Duration::from_millis(self.config.max_elapsed_ms)
    }

    /// Compute the delay for the upcoming `attempt` on the current account,
    /// honoring `Retry-After` when configured and larger than the backoff.
    fn delay_for(&mut self, attempt: u32, error: &ProviderError) -> Duration {
        let base = self
            .config
            .backoff
            .base_delay_ms(attempt.saturating_sub(1).max(1));
        let jittered = self.jitter.apply(base, self.config.backoff.jitter);
        let mut delay = Duration::from_millis(jittered);
        if self.config.respect_retry_after
            && let Some(hint) = retry_after_hint(error)
            && hint > delay
        {
            delay = hint;
        }
        delay
    }

    /// Decide what to do about `error`, advancing internal counters. Call once
    /// per failed attempt.
    pub fn on_failure(&mut self, error: &ProviderError) -> RetryDecision {
        if !self.config.enabled {
            return self.stop(ExhaustionReason::Disabled, error);
        }

        let class = classify(error);
        if class == FailureClass::NonRetryable {
            return self.stop(ExhaustionReason::NonRetryable, error);
        }

        if self.time_budget_exceeded() {
            return self.stop(ExhaustionReason::TimeBudgetExceeded, error);
        }

        let may_retry_same = class.allowed_by(&self.config.retry_on);
        let may_switch = self.config.account_switching && class.allowed_by(&self.config.switch_on);

        // 1. Same-account retry, while attempts remain and the class allows it.
        if may_retry_same && self.attempts_on_current < self.config.max_attempts_per_account {
            self.attempts_on_current += 1;
            let attempt = self.attempts_on_current;
            let delay = self.delay_for(attempt, error);
            return RetryDecision::Retry {
                account_id: self.current_account().to_string(),
                delay,
                attempt,
                switched: false,
            };
        }

        // 2. Switch to the next account, if switching is permitted and one is
        //    available under the cap.
        if may_switch {
            if !self.has_next_account() {
                return self.stop(ExhaustionReason::AllAccountsFailed, error);
            }
            if !self.under_account_cap() {
                return self.stop(ExhaustionReason::AccountCapReached, error);
            }
            self.current += 1;
            self.accounts_tried += 1;
            self.attempts_on_current = 1; // this is the first try on the new account
            let delay = self.delay_for(1, error);
            return RetryDecision::Retry {
                account_id: self.current_account().to_string(),
                delay,
                attempt: 1,
                switched: true,
            };
        }

        // 3. Nothing left to do. Pick the most accurate reason.
        if self.accounts.len() > 1 && self.config.account_switching {
            self.stop(ExhaustionReason::AllAccountsFailed, error)
        } else {
            self.stop(ExhaustionReason::AccountRetriesExhausted, error)
        }
    }

    fn stop(&self, reason: ExhaustionReason, error: &ProviderError) -> RetryDecision {
        RetryDecision::Exhausted {
            reason,
            last_error: error.clone(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{BackoffConfig, BackoffStrategy};

    fn no_jitter_config(max_attempts: u32, switching: bool) -> RetryConfig {
        RetryConfig {
            max_attempts_per_account: max_attempts,
            account_switching: switching,
            backoff: BackoffConfig {
                strategy: BackoffStrategy::Fixed,
                initial_ms: 10,
                multiplier: 2.0,
                max_delay_ms: 1000,
                jitter: 0.0,
            },
            ..Default::default()
        }
    }

    fn controller(config: RetryConfig, accounts: &[&str]) -> RetryController {
        RetryController::with_jitter(
            config,
            accounts.iter().map(|s| s.to_string()).collect(),
            Jitter::seeded(42),
        )
    }

    fn rate_limited() -> ProviderError {
        ProviderError::Api {
            status: 429,
            body: String::new(),
        }
    }

    fn server_error() -> ProviderError {
        ProviderError::Api {
            status: 503,
            body: String::new(),
        }
    }

    #[test]
    fn classifies_common_statuses() {
        assert_eq!(classify(&rate_limited()), FailureClass::RateLimited);
        assert_eq!(classify(&server_error()), FailureClass::ServerError);
        assert_eq!(
            classify(&ProviderError::Http("reset".into())),
            FailureClass::Transport
        );
        assert_eq!(
            classify(&ProviderError::Auth("bad key".into())),
            FailureClass::Auth
        );
        assert_eq!(
            classify(&ProviderError::Api {
                status: 400,
                body: String::new()
            }),
            FailureClass::NonRetryable
        );
    }

    #[test]
    fn single_account_retries_then_exhausts() {
        // 3 attempts total on one account = 2 retries after the initial try.
        let mut ctrl = controller(no_jitter_config(3, false), &["only"]);

        let d1 = ctrl.on_failure(&server_error());
        assert!(matches!(
            d1,
            RetryDecision::Retry {
                attempt: 2,
                switched: false,
                ..
            }
        ));
        let d2 = ctrl.on_failure(&server_error());
        assert!(matches!(
            d2,
            RetryDecision::Retry {
                attempt: 3,
                switched: false,
                ..
            }
        ));
        let d3 = ctrl.on_failure(&server_error());
        match d3 {
            RetryDecision::Exhausted { reason, last_error } => {
                assert_eq!(reason, ExhaustionReason::AccountRetriesExhausted);
                assert!(matches!(last_error, ProviderError::Api { status: 503, .. }));
            }
            _ => panic!("expected exhaustion"),
        }
    }

    #[test]
    fn switches_accounts_then_reports_all_failed() {
        // 1 attempt per account, switching on, two accounts.
        let mut ctrl = controller(no_jitter_config(1, true), &["a", "b"]);

        // First failure: no same-account retries left (max=1), switch to "b".
        let d1 = ctrl.on_failure(&rate_limited());
        match d1 {
            RetryDecision::Retry {
                account_id,
                switched,
                attempt,
                ..
            } => {
                assert_eq!(account_id, "b");
                assert!(switched);
                assert_eq!(attempt, 1);
            }
            _ => panic!("expected switch"),
        }

        // Second failure on "b": no more accounts, all failed.
        let d2 = ctrl.on_failure(&rate_limited());
        match d2 {
            RetryDecision::Exhausted { reason, .. } => {
                assert_eq!(reason, ExhaustionReason::AllAccountsFailed);
            }
            _ => panic!("expected all-accounts-failed"),
        }
        assert_eq!(ctrl.accounts_tried(), 2);
    }

    #[test]
    fn retries_same_account_before_switching() {
        // 2 attempts per account, switching on, two accounts.
        let mut ctrl = controller(no_jitter_config(2, true), &["a", "b"]);

        // Retry on "a" first.
        let d1 = ctrl.on_failure(&server_error());
        assert!(matches!(
            d1,
            RetryDecision::Retry { ref account_id, switched: false, .. } if account_id == "a"
        ));
        // Now "a" is exhausted; switch to "b".
        let d2 = ctrl.on_failure(&server_error());
        assert!(matches!(
            d2,
            RetryDecision::Retry { ref account_id, switched: true, .. } if account_id == "b"
        ));
    }

    #[test]
    fn non_retryable_stops_immediately_with_real_error() {
        let mut ctrl = controller(no_jitter_config(5, true), &["a", "b"]);
        let bad = ProviderError::Api {
            status: 400,
            body: "bad request".into(),
        };
        match ctrl.on_failure(&bad) {
            RetryDecision::Exhausted { reason, last_error } => {
                assert_eq!(reason, ExhaustionReason::NonRetryable);
                assert!(matches!(last_error, ProviderError::Api { status: 400, .. }));
            }
            _ => panic!("expected immediate stop"),
        }
    }

    #[test]
    fn disabled_config_never_retries() {
        let mut config = no_jitter_config(5, true);
        config.enabled = false;
        let mut ctrl = controller(config, &["a", "b"]);
        assert!(matches!(
            ctrl.on_failure(&server_error()),
            RetryDecision::Exhausted {
                reason: ExhaustionReason::Disabled,
                ..
            }
        ));
    }

    #[test]
    fn auth_error_switches_but_does_not_retry_same_account() {
        // Defaults: retry_on.auth = false, switch_on.auth = true.
        let mut ctrl = controller(no_jitter_config(5, true), &["a", "b"]);
        let auth = ProviderError::Auth("expired".into());
        // Should skip same-account retries and jump straight to "b".
        match ctrl.on_failure(&auth) {
            RetryDecision::Retry {
                account_id,
                switched,
                ..
            } => {
                assert_eq!(account_id, "b");
                assert!(switched);
            }
            _ => panic!("expected switch on auth"),
        }
    }

    #[test]
    fn account_cap_limits_switching() {
        let mut config = no_jitter_config(1, true);
        config.max_accounts = 2; // may try at most 2 accounts total
        let mut ctrl = controller(config, &["a", "b", "c"]);

        // a -> b (2nd account, allowed).
        assert!(matches!(
            ctrl.on_failure(&rate_limited()),
            RetryDecision::Retry { switched: true, .. }
        ));
        // b -> would be 3rd account, cap reached.
        match ctrl.on_failure(&rate_limited()) {
            RetryDecision::Exhausted { reason, .. } => {
                assert_eq!(reason, ExhaustionReason::AccountCapReached);
            }
            _ => panic!("expected cap reached"),
        }
    }

    #[test]
    fn respects_retry_after_hint_when_longer() {
        let mut config = no_jitter_config(3, false);
        config.respect_retry_after = true;
        let mut ctrl = controller(config, &["a"]);
        let err = ProviderError::Api {
            status: 429,
            body: r#"{"error":{"retry_after":5}}"#.into(),
        };
        match ctrl.on_failure(&err) {
            RetryDecision::Retry { delay, .. } => {
                assert_eq!(delay, Duration::from_secs(5));
            }
            _ => panic!("expected retry honoring hint"),
        }
    }
}
