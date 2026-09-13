//! Canonical cross-session memory.
//!
//! The store is deliberately daemon-usable but runtime-agnostic. It provides
//! versioned records, scoped retrieval, lifecycle operations, citations,
//! context packets, provenance, and explicit lifecycle controls.
//! Runtime policy and tool permissions belong at the caller boundary.
//!
//! Persistence is a daemon-owned SQLite database with schema migrations and a
//! legacy JSON import path. Writers are serialized with an inter-process lock
//! so independent daemon processes cannot lose a revision.

mod identity;
mod model;
mod store;

pub use identity::{
    ProjectIdentity, ProjectIdentitySource, canonicalize_git_remote, resolve_project_identity,
};
pub use model::*;
pub use store::{MemoryError, MemoryStore};
