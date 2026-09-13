use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};

use firmius_protocol::{DaemonEndpoint, PROTOCOL_VERSION};
use fs2::FileExt;
use uuid::Uuid;

pub fn endpoint_path(root: &Path) -> PathBuf {
    root.join("daemon.json")
}

/// Return whether another daemon currently owns the profile lease.
/// This intentionally does not inspect endpoint reachability: a held lock is
/// authoritative and prevents callers from starting an embedded second
/// runtime while the daemon is booting or briefly unavailable.
pub fn is_locked(root: &Path) -> bool {
    let path = root.join("daemon.lock");
    let Ok(file) = OpenOptions::new().read(true).write(true).open(path) else {
        return false;
    };
    if file.try_lock_exclusive().is_ok() {
        let _ = file.unlock();
        false
    } else {
        true
    }
}

/// Exclusive ownership of the canonical profile and daemon epoch.
pub struct DaemonLease {
    lock: File,
    endpoint_path: PathBuf,
    epoch: Uuid,
}

impl DaemonLease {
    pub fn acquire(root: &Path) -> Result<Self, String> {
        std::fs::create_dir_all(root)
            .map_err(|error| format!("create {}: {error}", root.display()))?;
        let lock_path = root.join("daemon.lock");
        let mut options = OpenOptions::new();
        options.create(true).read(true).write(true).truncate(false);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut lock = options
            .open(&lock_path)
            .map_err(|error| format!("open {}: {error}", lock_path.display()))?;
        lock.try_lock_exclusive().map_err(|error| {
            format!(
                "another Firmius daemon owns {} ({error})",
                lock_path.display()
            )
        })?;
        lock.set_len(0)
            .and_then(|()| writeln!(lock, "{}", std::process::id()))
            .and_then(|()| lock.sync_all())
            .map_err(|error| format!("write {}: {error}", lock_path.display()))?;
        Ok(Self {
            lock,
            endpoint_path: endpoint_path(root),
            epoch: Uuid::new_v4(),
        })
    }

    pub fn epoch(&self) -> Uuid {
        self.epoch
    }

    pub fn publish(
        &self,
        address: String,
        daemon_id: Uuid,
        auth_token: String,
    ) -> Result<DaemonEndpoint, String> {
        let endpoint = DaemonEndpoint {
            version: PROTOCOL_VERSION,
            address,
            auth_token,
            daemon_id,
            epoch: self.epoch,
            pid: std::process::id(),
        };
        let bytes = serde_json::to_vec_pretty(&endpoint)
            .map_err(|error| format!("encode daemon endpoint: {error}"))?;
        let temp = self
            .endpoint_path
            .with_extension(format!("json.tmp-{}", self.epoch));
        let mut options = OpenOptions::new();
        options.create_new(true).write(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file = options
            .open(&temp)
            .map_err(|error| format!("create {}: {error}", temp.display()))?;
        file.write_all(&bytes)
            .and_then(|()| file.sync_all())
            .map_err(|error| format!("write {}: {error}", temp.display()))?;
        std::fs::rename(&temp, &self.endpoint_path)
            .map_err(|error| format!("publish {}: {error}", self.endpoint_path.display()))?;
        Ok(endpoint)
    }
}

impl Drop for DaemonLease {
    fn drop(&mut self) {
        let owns_endpoint = std::fs::read(&self.endpoint_path)
            .ok()
            .and_then(|bytes| serde_json::from_slice::<DaemonEndpoint>(&bytes).ok())
            .is_some_and(|endpoint| endpoint.epoch == self.epoch);
        if owns_endpoint {
            let _ = std::fs::remove_file(&self.endpoint_path);
        }
        let _ = self.lock.unlock();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_root(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "firmius-daemon-{name}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }

    #[test]
    fn lease_is_exclusive_and_cleans_its_own_endpoint() {
        let root = temp_root("lease");
        let lease = DaemonLease::acquire(&root).unwrap();
        assert!(DaemonLease::acquire(&root).is_err());
        let endpoint = lease
            .publish("127.0.0.1:1".into(), Uuid::new_v4(), "secret".into())
            .unwrap();
        assert_eq!(endpoint.epoch, lease.epoch());
        drop(lease);
        assert!(!endpoint_path(&root).exists());
        std::fs::remove_dir_all(root).ok();
    }
}
