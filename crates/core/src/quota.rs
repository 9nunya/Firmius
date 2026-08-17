//! Optional account quota capabilities.
//!
//! Quota is deliberately separate from [`crate::Provider`]: most providers
//! have no quota endpoint, while subscription gateways may expose several
//! independent rolling meters.

use async_trait::async_trait;
use chrono::{DateTime, Utc};
use std::sync::Arc;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum QuotaAuth {
    ApiKey,
    WebSession,
    Custom(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuotaDescriptor {
    pub label: String,
    pub auth: QuotaAuth,
    pub meters: Vec<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct QuotaMeter {
    pub id: String,
    pub label: String,
    pub window: Option<String>,
    pub used: Option<u64>,
    pub limit: Option<u64>,
    pub remaining: Option<u64>,
    pub utilization_percent: Option<f64>,
    pub unit: Option<String>,
    pub reset_at: Option<DateTime<Utc>>,
    pub reset_in_seconds: Option<u64>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct QuotaSnapshot {
    pub account_id: String,
    pub observed_at: DateTime<Utc>,
    pub meters: Vec<QuotaMeter>,
    pub note: Option<String>,
}

#[derive(Debug, thiserror::Error)]
pub enum QuotaError {
    #[error("quota credentials are invalid or expired")]
    InvalidCredentials,
    #[error("quota endpoint is unavailable: {0}")]
    Unavailable(String),
    #[error("quota request failed: {0}")]
    Request(String),
    #[error("quota response could not be decoded: {0}")]
    Decode(String),
    #[error("quota API error: {0}")]
    Api(String),
}

#[async_trait]
pub trait QuotaSource: Send + Sync {
    fn descriptor(&self) -> QuotaDescriptor;
    async fn fetch(&self) -> Result<QuotaSnapshot, QuotaError>;
}

pub struct QuotaCapability {
    pub descriptor: QuotaDescriptor,
    pub source: Option<Arc<dyn QuotaSource>>,
}
