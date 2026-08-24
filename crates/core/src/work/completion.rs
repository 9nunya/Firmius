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
    pub fn outcome(&self) -> Outcome {
        match self.outcome.as_deref() {
            Some("success") | Some("succeeded") => Outcome::Success,
            Some("failure") | Some("failed") => Outcome::Failure,
            Some("cancelled") => Outcome::Cancelled,
            Some("interrupted") => Outcome::Interrupted,
            Some(value) => Outcome::Custom(value.to_string()),
            None if self.status == WorkerStatus::Succeeded => Outcome::Success,
            None => Outcome::Failure,
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
                .collect::<Vec<_>>()
                .join(", ")
        )
    };
    format!(
        "When the assignment is finished, your FINAL assistant response must be exactly one JSON object and no markdown or prose:\n\
{{\"status\":\"succeeded|failed|blocked|cancelled|interrupted\",\"outcome\":\"dynamic workflow outcome\",\"summary\":\"concise result\",\"output\":null,\"artifacts\":[],\"evidence\":[],\"evidence_links\":[],\"changed_files\":[],\"handoff\":null,\"verification\":\"none|self_verified|reviewed|independently_verified\"}}\n\
`status` controls execution; `outcome` is a dynamic branch label such as `success`, `approved`, `rejected`, or `needs_changes`. {output_rule} Do not call a completion or yield tool."
    )
}

#[cfg(test)]
mod tests {
    use super::*;

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
