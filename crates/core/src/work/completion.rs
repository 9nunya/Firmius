//! Typed completion returned by a managed worker's final assistant response.
//!
//! Workflow settlement is protocol, not a tool call. The driver owns the
//! state transition; the worker only returns this envelope. `status` controls
//! execution lifecycle, while `outcome` is deliberately open-ended so edges
//! can branch on workflow-specific values such as `approved` or
//! `needs_changes` without hard-coding workflow types into Firmius.

use super::{EvidenceLink, ExecutionStatus, Outcome, OutputContract, VerificationLevel};
use serde::Deserialize;
use serde_json::Value;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WorkerStatus {
    Succeeded,
    Failed,
    Blocked,
    Cancelled,
    Interrupted,
}

impl WorkerStatus {
    pub fn execution_status(self) -> ExecutionStatus {
        match self {
            Self::Succeeded => ExecutionStatus::Succeeded,
            Self::Failed => ExecutionStatus::Failed,
            Self::Blocked => ExecutionStatus::Blocked,
            Self::Cancelled => ExecutionStatus::Cancelled,
            Self::Interrupted => ExecutionStatus::Interrupted,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct WorkerCompletion {
    pub status: WorkerStatus,
    /// Dynamic branch label. `success`/`failure` receive their canonical
    /// Outcome variants; every other value remains a custom outcome.
    #[serde(default)]
    pub outcome: Option<String>,
    pub summary: String,
    #[serde(default)]
    pub output: Option<Value>,
    #[serde(default)]
    pub artifacts: Vec<String>,
    #[serde(default)]
    pub evidence: Vec<String>,
    #[serde(default)]
    pub evidence_links: Vec<EvidenceLink>,
    #[serde(default)]
    pub changed_files: Vec<String>,
    #[serde(default)]
    pub handoff: Option<String>,
    #[serde(default)]
    pub verification: VerificationLevel,
}

impl WorkerCompletion {
    /// Validate lifecycle semantics before a worker result is persisted. A
    /// worker may report its own evidence, but cannot self-grant reviewer
    /// authority; reviewer levels are established by an authorized
    /// annotation/transition outside this envelope.
    pub fn validate_semantics(&self) -> Result<(), String> {
        if self.status == WorkerStatus::Succeeded && self.summary.trim().is_empty() {
            return Err("succeeded worker completion must include a non-empty summary".into());
        }
        if self.verification > VerificationLevel::SelfVerified {
            return Err("worker cannot self-assert reviewed or independently_verified; an authorized reviewer transition is required".into());
        }
        match (self.status, self.outcome.as_deref()) {
            (WorkerStatus::Succeeded, Some("failure" | "failed" | "cancelled" | "interrupted")) => {
                Err("succeeded status contradicts failure/cancellation outcome".into())
            }
            (WorkerStatus::Failed, Some("success" | "succeeded")) => {
                Err("failed status contradicts success outcome".into())
            }
            (WorkerStatus::Cancelled, Some(value))
                if !matches!(value, "cancelled" | "interrupted") =>
            {
                Err("cancelled status requires a cancelled/interrupted outcome".into())
            }
            (WorkerStatus::Interrupted, Some(value))
                if !matches!(value, "interrupted" | "cancelled") =>
            {
                Err("interrupted status requires an interrupted/cancelled outcome".into())
            }
            (WorkerStatus::Blocked, Some("success" | "succeeded" | "failure" | "failed")) => {
                Err("blocked status contradicts success/failure outcome".into())
            }
            _ => Ok(()),
        }
    }

    pub fn outcome(&self) -> Outcome {
        match self.outcome.as_deref() {
            Some("success") | Some("succeeded") => Outcome::Success,
            Some("failure") | Some("failed") => Outcome::Failure,
            Some("cancelled") => Outcome::Cancelled,
            Some("interrupted") => Outcome::Interrupted,
            Some(value) => Outcome::Custom(value.to_string()),
            None => match self.status {
                WorkerStatus::Succeeded => Outcome::Success,
                WorkerStatus::Failed => Outcome::Failure,
                WorkerStatus::Blocked => Outcome::Blocked,
                WorkerStatus::Cancelled => Outcome::Cancelled,
                WorkerStatus::Interrupted => Outcome::Interrupted,
            },
        }
    }

    pub fn durable_summary(&self) -> String {
        match self.handoff.as_deref().filter(|value| !value.is_empty()) {
            Some(handoff) => format!("{}\n\nHandoff: {handoff}", self.summary),
            None => self.summary.clone(),
        }
    }

    pub fn validate_output(&self, contract: &OutputContract) -> Result<(), String> {
        if contract.required_fields.is_empty() {
            return Ok(());
        }
        let object = self
            .output
            .as_ref()
            .and_then(Value::as_object)
            .ok_or_else(|| "output must be an object".to_string())?;
        let missing = contract
            .required_fields
            .iter()
            .filter(|field| !object.contains_key(field.as_str()))
            .cloned()
            .collect::<Vec<_>>();
        if missing.is_empty() {
            Ok(())
        } else {
            Err(format!(
                "output is missing required fields: {}",
                missing.join(", ")
            ))
        }
    }
}

/// Parse a final response. Providers sometimes wrap otherwise valid JSON in a
/// markdown fence despite explicit instructions, so accept that harmless
/// wrapper while rejecting prose or ambiguous fragments.
pub fn parse_worker_completion(text: &str) -> Result<WorkerCompletion, String> {
    let trimmed = text.trim();
    let candidate = if trimmed.starts_with("```") && trimmed.ends_with("```") {
        let body = trimmed
            .strip_prefix("```json")
            .or_else(|| trimmed.strip_prefix("```JSON"))
            .or_else(|| trimmed.strip_prefix("```"))
            .unwrap_or(trimmed);
        body.strip_suffix("```").unwrap_or(body).trim()
    } else {
        trimmed
    };
    serde_json::from_str(candidate).map_err(|error| format!("invalid worker completion: {error}"))
}

/// Human-readable schema instruction embedded into the assignment prompt.
pub fn completion_instruction(contract: &OutputContract) -> String {
    let output_rule = if contract.required_fields.is_empty() {
        "`output` may be null or any JSON value.".to_string()
    } else {
        format!(
            "`output` must be an object containing: {}.",
            contract
                .required_fields
                .iter()
                .cloned()
                .map(|field| super::inputs::escape_untrusted(&field))
                .collect::<Vec<_>>()
                .join(", ")
        )
    };
    format!(
        "When the assignment is finished, your FINAL assistant response must be exactly one JSON object and no markdown or prose:\n\
{{\"status\":\"succeeded\",\"summary\":\"Implemented and verified the requested change.\",\"output\":null,\"evidence\":[\"cargo test -p firmius-core\"],\"changed_files\":[],\"verification\":\"self_verified\"}}\n\
For a review that completed successfully but found a defect, return this shape instead:\n\
{{\"status\":\"succeeded\",\"outcome\":\"rejected\",\"summary\":\"The implementation needs correction in the parser.\",\"output\":{{\"findings\":[\"The parser accepts an empty identifier.\"]}},\"evidence\":[],\"changed_files\":[],\"verification\":\"self_verified\"}}\n\
`status` must be one of `succeeded`, `failed`, `blocked`, `cancelled`, or `interrupted`. `verification` must be `none` or `self_verified`; reviewer authority is issued by the runtime. `status` controls execution. If `outcome` is omitted, it is derived from status (`success`, `failure`, `blocked`, `cancelled`, or `interrupted`). A supplied `outcome` is a dynamic branch label such as `approved`, `rejected`, or `needs_changes`; it does not replace `status`. {output_rule} Do not call a completion or yield tool."
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_empty_or_whitespace_success_summary() {
        for summary in ["", " \n\t "] {
            let completion = parse_worker_completion(&format!(
                r#"{{"status":"succeeded","summary":{summary:?}}}"#
            ))
            .unwrap();
            assert!(completion.validate_semantics().is_err());
        }
    }

    #[test]
    fn rejects_worker_claimed_reviewer_authority_and_contradictory_status() {
        let reviewed = parse_worker_completion(
            r#"{"status":"succeeded","summary":"done","verification":"reviewed"}"#,
        )
        .unwrap();
        assert!(reviewed.validate_semantics().is_err());
        let contradictory =
            parse_worker_completion(r#"{"status":"failed","outcome":"success","summary":"no"}"#)
                .unwrap();
        assert!(contradictory.validate_semantics().is_err());
        let custom = parse_worker_completion(
            r#"{"status":"succeeded","outcome":"approved","summary":"yes"}"#,
        )
        .unwrap();
        assert!(custom.validate_semantics().is_ok());
    }

    #[test]
    fn custom_outcome_does_not_change_success_status() {
        let completion = parse_worker_completion(
            r#"{"status":"succeeded","outcome":"approved","summary":"good"}"#,
        )
        .unwrap();
        assert_eq!(
            completion.status.execution_status(),
            ExecutionStatus::Succeeded
        );
        assert_eq!(completion.outcome(), Outcome::Custom("approved".into()));
    }

    #[test]
    fn succeeded_alias_maps_to_canonical_success() {
        let completion = parse_worker_completion(
            r#"{"status":"succeeded","outcome":"succeeded","summary":"done"}"#,
        )
        .unwrap();
        assert_eq!(completion.outcome(), Outcome::Success);
    }

    #[test]
    fn accepts_one_json_fence_but_not_surrounding_prose() {
        assert!(
            parse_worker_completion("```json\n{\"status\":\"failed\",\"summary\":\"no\"}\n```")
                .is_ok()
        );
        assert!(
            parse_worker_completion("done: {\"status\":\"succeeded\",\"summary\":\"yes\"}")
                .is_err()
        );
    }

    #[test]
    fn validates_dynamic_output_contract() {
        let completion = parse_worker_completion(
            r#"{"status":"succeeded","summary":"done","output":{"verdict":"approved"}}"#,
        )
        .unwrap();
        let contract = OutputContract {
            required_fields: ["verdict".into(), "notes".into()].into_iter().collect(),
        };
        assert_eq!(
            completion.validate_output(&contract).unwrap_err(),
            "output is missing required fields: notes"
        );
    }
}
