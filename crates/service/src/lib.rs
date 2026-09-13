//! Daemon-owned Firmius runtime and authenticated local IPC service.

mod conflict_messaging;
mod coordinator_store;
mod lifecycle;
mod runtime;
mod server;

pub use conflict_messaging::{ConflictMessenger, ConflictMetadata, ConflictRegistration};
pub use coordinator_store::CoordinatorStore;
pub use lifecycle::{DaemonLease, endpoint_path, is_locked};
pub use runtime::{DaemonRuntime, RuntimeParts, load_runtime_parts};
pub use server::{DaemonOptions, RunningDaemon, start_daemon};

pub async fn run_default() -> Result<(), String> {
    let root = firmius_core::data_dir();
    let running = start_daemon(DaemonOptions::new(root)).await?;
    running.wait().await
}
