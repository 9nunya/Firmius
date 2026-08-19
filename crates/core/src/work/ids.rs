//! Stable identifiers used by the WorkGraph domain.
//!
//! IDs deliberately are not aliases for `String`: keeping them distinct makes
//! it difficult to accidentally connect a node to an attempt, or a result to
//! an unrelated graph. They remain human-serializable UUID strings so that
//! snapshots can be inspected and migrated without a custom format.

use serde::{Deserialize, Serialize};
use std::fmt;
use uuid::Uuid;

macro_rules! id_type {
    ($name:ident) => {
        #[derive(
            Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize,
        )]
        #[serde(transparent)]
        pub struct $name(Uuid);

        impl $name {
            pub fn new() -> Self {
                Self(Uuid::new_v4())
            }
            pub fn parse(value: &str) -> Result<Self, uuid::Error> {
                Ok(Self(Uuid::parse_str(value)?))
            }
            pub fn as_uuid(&self) -> Uuid {
                self.0
            }
        }

        impl Default for $name {
            fn default() -> Self {
                Self::new()
            }
        }
        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.0.fmt(f)
            }
        }
        impl From<Uuid> for $name {
            fn from(value: Uuid) -> Self {
                Self(value)
            }
        }
        impl From<$name> for Uuid {
            fn from(value: $name) -> Self {
                value.0
            }
        }
    };
}

id_type!(GraphId);
id_type!(NodeId);
id_type!(EdgeId);
id_type!(AttemptId);
id_type!(AssignmentId);
id_type!(ResultId);
id_type!(ManifestId);
id_type!(AnnotationId);
