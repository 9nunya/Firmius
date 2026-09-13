//! Workspace/file transport for local and SSH-backed sessions.
//!
//! Process control and filesystem access are separate capabilities. This
//! prevents an SSH process host from pretending that the agent's files are
//! local and gives every file tool one place to adopt remote semantics.

use async_trait::async_trait;
use fs2::FileExt;
use sha2::{Digest, Sha256};
use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::hash::{Hash, Hasher};
use std::io;
use std::path::{Path, PathBuf};

#[derive(Debug, thiserror::Error)]
pub enum WorkspaceError {
    #[error("invalid workspace path: {0}")]
    InvalidPath(String),
    #[error("workspace operation failed: {0}")]
    Operation(String),
    #[error("workspace root was replaced (expected {expected}, observed {observed})")]
    RootReplaced { expected: String, observed: String },
}

impl PlatformFileId {
    fn is_reliable(&self) -> bool {
        match self {
            #[cfg(unix)]
            Self::Unix { device, inode } => *device != 0 && *inode != 0,
            #[cfg(windows)]
            Self::Windows { volume, index } => *volume != 0 && *index != 0,
            #[cfg(not(any(unix, windows)))]
            Self::Unavailable => false,
        }
    }
}

impl PartialEq for WorkspaceIdentityKind {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (
                Self::Local {
                    canonical_root: left_root,
                    file_id: left_id,
                },
                Self::Local {
                    canonical_root: right_root,
                    file_id: right_id,
                },
            ) => {
                match (left_id.is_reliable(), right_id.is_reliable()) {
                    (true, true) => left_id == right_id,
                    (false, false) => left_root == right_root,
                    // A reliable filesystem ID and a path-only fallback do
                    // not contain enough common information to prove identity.
                    _ => false,
                }
            }
            (
                Self::Remote {
                    target: left_target,
                    root: left_root,
                },
                Self::Remote {
                    target: right_target,
                    root: right_root,
                },
            ) => left_target == right_target && left_root == right_root,
            _ => false,
        }
    }
}

impl Eq for WorkspaceIdentityKind {}

impl Hash for WorkspaceIdentityKind {
    fn hash<H: Hasher>(&self, state: &mut H) {
        match self {
            Self::Local {
                canonical_root,
                file_id,
            } => {
                "local".hash(state);
                if file_id.is_reliable() {
                    true.hash(state);
                    file_id.hash(state);
                } else {
                    false.hash(state);
                    canonical_root.hash(state);
                }
            }
            Self::Remote { target, root } => {
                "remote".hash(state);
                target.hash(state);
                root.hash(state);
            }
        }
    }
}

fn join_remote_root(root: &str, relative: &str) -> String {
    if root == "/" {
        format!("/{relative}")
    } else {
        format!("{}/{relative}", root.trim_end_matches('/'))
    }
}

/// Stable identity of the filesystem root edited by a session.
///
/// Local identities combine the canonical path with the platform file ID
/// (device/inode on Unix). Consequently path aliases identify the same root,
/// while [`WorkspaceIdentity::revalidate`] detects replacement at the path.
/// Remote identities are deliberately structural: they distinguish the SSH
/// target and a normalized absolute POSIX root, but cannot resolve SSH host
/// aliases, remote symlinks, mounts, or replacement without remote I/O.
#[derive(Debug, Clone)]
pub struct WorkspaceIdentity {
    kind: WorkspaceIdentityKind,
    // The spelling by which a local caller reached the root. This is excluded
    // from equality and hashing, but lets revalidation catch a retargeted
    // symlink as well as replacement at the canonical path.
    observed_root: Option<PathBuf>,
}

#[derive(Debug, Clone)]
enum WorkspaceIdentityKind {
    Local {
        canonical_root: PathBuf,
        file_id: PlatformFileId,
    },
    Remote {
        target: String,
        root: String,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
enum PlatformFileId {
    #[cfg(unix)]
    Unix { device: u64, inode: u64 },
    #[cfg(windows)]
    Windows { volume: u32, index: u64 },
    #[cfg(not(any(unix, windows)))]
    Unavailable,
}

impl PartialEq for WorkspaceIdentity {
    fn eq(&self, other: &Self) -> bool {
        self.kind == other.kind
    }
}

/// Resolve a write destination under an existing local workspace root.
///
/// Existing ancestors are canonicalized so a symlink cannot escape the root;
/// nonexistent trailing components are retained. The destination itself may
/// be absent, but the workspace root must exist and be a directory.
pub fn normalize_write_path(workdir: &Path, relative: &str) -> Result<PathBuf, WorkspaceError> {
    let relative = safe_relative(relative)?;
    let canonical_root = fs::canonicalize(workdir).map_err(workspace_io)?;
    let metadata = fs::metadata(&canonical_root).map_err(workspace_io)?;
    if !metadata.is_dir() {
        return Err(WorkspaceError::InvalidPath(format!(
            "workspace root is not a directory: {}",
            workdir.display()
        )));
    }

    let mut resolved = canonical_root.clone();
    let components: Vec<&str> = relative.split('/').collect();
    let mut first_missing = components.len();
    for (index, component) in components.iter().enumerate() {
        let candidate = resolved.join(component);
        match fs::canonicalize(&candidate) {
            Ok(canonical) => {
                if !canonical.starts_with(&canonical_root) {
                    return Err(WorkspaceError::InvalidPath(format!(
                        "write path escapes workspace: {relative}"
                    )));
                }
                resolved = canonical;
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                if fs::symlink_metadata(&candidate)
                    .is_ok_and(|metadata| metadata.file_type().is_symlink())
                {
                    return Err(WorkspaceError::InvalidPath(format!(
                        "write path contains an unresolved symlink: {relative}"
                    )));
                }
                first_missing = index;
                break;
            }
            Err(error) => return Err(workspace_io(error)),
        }
    }
    for component in &components[first_missing..] {
        resolved.push(component);
    }
    if !resolved.starts_with(&canonical_root) {
        return Err(WorkspaceError::InvalidPath(format!(
            "write path escapes workspace: {relative}"
        )));
    }
    Ok(resolved)
}

fn inspect_local_root(root: &Path) -> Result<(PathBuf, PlatformFileId), WorkspaceError> {
    let canonical_root = fs::canonicalize(root).map_err(workspace_io)?;
    let metadata = fs::metadata(&canonical_root).map_err(workspace_io)?;
    if !metadata.is_dir() {
        return Err(WorkspaceError::InvalidPath(format!(
            "workspace root is not a directory: {}",
            root.display()
        )));
    }
    Ok((
        canonical_root.clone(),
        platform_file_id(&canonical_root, &metadata),
    ))
}

#[cfg(unix)]
fn platform_file_id(_root: &Path, metadata: &fs::Metadata) -> PlatformFileId {
    use std::os::unix::fs::MetadataExt;
    PlatformFileId::Unix {
        device: metadata.dev(),
        inode: metadata.ino(),
    }
}

#[cfg(windows)]
fn platform_file_id(root: &Path, _metadata: &fs::Metadata) -> PlatformFileId {
    windows_platform_file_id(root).unwrap_or(PlatformFileId::Windows {
        volume: 0,
        index: 0,
    })
}

#[cfg(windows)]
fn windows_platform_file_id(root: &Path) -> Option<PlatformFileId> {
    use std::os::windows::io::AsRawHandle;

    #[repr(C)]
    struct FileTime {
        dw_low_date_time: u32,
        dw_high_date_time: u32,
    }

    #[repr(C)]
    struct ByHandleFileInformation {
        dw_file_attributes: u32,
        ft_creation_time: FileTime,
        ft_last_access_time: FileTime,
        ft_last_write_time: FileTime,
        dw_volume_serial_number: u32,
        n_file_size_high: u32,
        n_file_size_low: u32,
        n_number_of_links: u32,
        n_file_index_high: u32,
        n_file_index_low: u32,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetFileInformationByHandle(
            handle: *mut core::ffi::c_void,
            info: *mut ByHandleFileInformation,
        ) -> i32;
    }

    let file = File::open(root).ok()?;
    let zero_time = FileTime {
        dw_low_date_time: 0,
        dw_high_date_time: 0,
    };
    let mut info = ByHandleFileInformation {
        dw_file_attributes: 0,
        ft_creation_time: zero_time,
        ft_last_access_time: zero_time,
        ft_last_write_time: zero_time,
        dw_volume_serial_number: 0,
        n_file_size_high: 0,
        n_file_size_low: 0,
        n_number_of_links: 0,
        n_file_index_high: 0,
        n_file_index_low: 0,
    };
    let ok = unsafe {
        GetFileInformationByHandle(
            file.as_raw_handle(),
            &mut info as *mut ByHandleFileInformation,
        )
    };
    if ok == 0 {
        return None;
    }
    Some(PlatformFileId::Windows {
        volume: info.dw_volume_serial_number,
        index: ((info.n_file_index_high as u64) << 32) | info.n_file_index_low as u64,
    })
}

#[cfg(not(any(unix, windows)))]
fn platform_file_id(_root: &Path, _metadata: &fs::Metadata) -> PlatformFileId {
    PlatformFileId::Unavailable
}

fn format_local_identity(root: &Path, file_id: &PlatformFileId) -> String {
    format!("local:{}#{file_id:?}", root.display())
}

#[cfg(unix)]
fn append_path_bytes(out: &mut Vec<u8>, path: &Path) {
    use std::os::unix::ffi::OsStrExt;
    out.extend_from_slice(path.as_os_str().as_bytes());
}

#[cfg(not(unix))]
fn append_path_bytes(out: &mut Vec<u8>, path: &Path) {
    out.extend_from_slice(path.to_string_lossy().as_bytes());
}

fn normalize_remote_root(root: &str) -> Result<String, WorkspaceError> {
    let root = root.trim();
    if !root.starts_with('/') || root.contains('\0') {
        return Err(WorkspaceError::InvalidPath(
            "remote workspace root must be an absolute POSIX path".into(),
        ));
    }
    let mut parts = Vec::new();
    for part in root.split('/') {
        match part {
            "" | "." => {}
            ".." => {
                return Err(WorkspaceError::InvalidPath(
                    "remote workspace root must not contain parent traversal".into(),
                ));
            }
            part => parts.push(part),
        }
    }
    if parts.is_empty() {
        Ok("/".into())
    } else {
        Ok(format!("/{}", parts.join("/")))
    }
}

fn workspace_io(error: io::Error) -> WorkspaceError {
    WorkspaceError::Operation(error.to_string())
}

impl Eq for WorkspaceIdentity {}

impl Hash for WorkspaceIdentity {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.kind.hash(state);
    }
}

impl WorkspaceIdentity {
    /// Identify an existing local directory. Relative paths are captured
    /// against the current directory so later revalidation is unambiguous.
    pub fn local(root: impl AsRef<Path>) -> Result<Self, WorkspaceError> {
        let supplied = root.as_ref();
        let observed_root = if supplied.is_absolute() {
            supplied.to_path_buf()
        } else {
            std::env::current_dir()
                .map_err(workspace_io)?
                .join(supplied)
        };
        let (canonical_root, file_id) = inspect_local_root(&observed_root)?;
        Ok(Self {
            kind: WorkspaceIdentityKind::Local {
                canonical_root,
                file_id,
            },
            observed_root: Some(observed_root),
        })
    }

    /// Identify a remote workspace without contacting it.
    ///
    /// Target spelling is preserved after trimming; in particular, two
    /// different SSH targets never collide merely because their roots match.
    pub fn remote(
        target: impl AsRef<str>,
        absolute_root: impl AsRef<str>,
    ) -> Result<Self, WorkspaceError> {
        let target = target.as_ref().trim();
        if target.is_empty() || target.chars().any(char::is_whitespace) {
            return Err(WorkspaceError::InvalidPath(
                "remote target must be non-empty and contain no whitespace".into(),
            ));
        }
        let root = normalize_remote_root(absolute_root.as_ref())?;
        Ok(Self {
            kind: WorkspaceIdentityKind::Remote {
                target: target.to_owned(),
                root,
            },
            observed_root: None,
        })
    }

    pub fn canonical_local_root(&self) -> Option<&Path> {
        match &self.kind {
            WorkspaceIdentityKind::Local { canonical_root, .. } => Some(canonical_root),
            WorkspaceIdentityKind::Remote { .. } => None,
        }
    }

    pub fn remote_target(&self) -> Option<&str> {
        match &self.kind {
            WorkspaceIdentityKind::Remote { target, .. } => Some(target),
            WorkspaceIdentityKind::Local { .. } => None,
        }
    }

    pub fn remote_root(&self) -> Option<&str> {
        match &self.kind {
            WorkspaceIdentityKind::Remote { root, .. } => Some(root),
            WorkspaceIdentityKind::Local { .. } => None,
        }
    }

    /// Confirm that a local root still resolves to the captured path and file
    /// ID. Remote identity has no transport and can only be structurally
    /// validated, so remote revalidation currently succeeds without I/O.
    pub fn revalidate(&self) -> Result<(), WorkspaceError> {
        let WorkspaceIdentityKind::Local {
            canonical_root,
            file_id,
        } = &self.kind
        else {
            return Ok(());
        };
        let observed_root = self
            .observed_root
            .as_deref()
            .expect("local workspace identity has an observed root");
        let (observed_path, observed_id) = inspect_local_root(observed_root)?;
        if observed_path != *canonical_root || observed_id != *file_id {
            return Err(WorkspaceError::RootReplaced {
                expected: self.to_string(),
                observed: format_local_identity(&observed_path, &observed_id),
            });
        }
        Ok(())
    }

    /// Filesystem-safe, deterministic key for a lease protecting this root.
    pub fn lease_key(&self) -> String {
        let mut digest = Sha256::new();
        digest.update(self.stable_bytes());
        let digest = digest.finalize();
        let mut key = String::with_capacity(digest.len() * 2);
        for byte in digest {
            use fmt::Write as _;
            write!(&mut key, "{byte:02x}").expect("writing to a String cannot fail");
        }
        key
    }

    fn stable_bytes(&self) -> Vec<u8> {
        match &self.kind {
            WorkspaceIdentityKind::Local {
                canonical_root,
                file_id,
            } => {
                let mut bytes = b"local\0".to_vec();
                if file_id.is_reliable() {
                    bytes.extend_from_slice(format!("{file_id:?}").as_bytes());
                } else {
                    append_path_bytes(&mut bytes, canonical_root);
                }
                bytes
            }
            WorkspaceIdentityKind::Remote { target, root } => {
                let mut bytes = b"remote\0".to_vec();
                bytes.extend_from_slice(target.as_bytes());
                bytes.push(0);
                bytes.extend_from_slice(root.as_bytes());
                bytes
            }
        }
    }
}

impl fmt::Display for WorkspaceIdentity {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.kind {
            WorkspaceIdentityKind::Local {
                canonical_root,
                file_id,
            } => f.write_str(&format_local_identity(canonical_root, file_id)),
            WorkspaceIdentityKind::Remote { target, root } => {
                write!(f, "ssh://{target}{root}")
            }
        }
    }
}

/// An OS advisory exclusive lease for the writer of one workspace.
///
/// The caller is responsible for choosing a protected directory not writable
/// by untrusted users. Lock files intentionally remain after release: removing
/// them would allow two processes to lock different inodes under the same
/// name. Dropping this value releases the advisory lock.
#[derive(Debug)]
pub struct WorkspaceWriterLease {
    file: File,
    path: PathBuf,
}

#[derive(Debug, thiserror::Error)]
pub enum WorkspaceLeaseError {
    #[error("workspace writer lease is already held: {path}")]
    Busy { path: PathBuf },
    #[error("workspace writer lease operation failed for {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
}

impl WorkspaceWriterLease {
    pub fn acquire(
        identity: &WorkspaceIdentity,
        protected_lease_dir: impl AsRef<Path>,
    ) -> Result<Self, WorkspaceLeaseError> {
        let lease_dir = protected_lease_dir.as_ref();
        fs::create_dir_all(lease_dir).map_err(|source| WorkspaceLeaseError::Io {
            path: lease_dir.to_path_buf(),
            source,
        })?;
        let path = lease_dir.join(format!("{}.lock", identity.lease_key()));
        let mut options = OpenOptions::new();
        options.read(true).write(true).create(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let file = options
            .open(&path)
            .map_err(|source| WorkspaceLeaseError::Io {
                path: path.clone(),
                source,
            })?;
        match FileExt::try_lock_exclusive(&file) {
            Ok(()) => Ok(Self { file, path }),
            Err(source) if source.kind() == io::ErrorKind::WouldBlock => {
                Err(WorkspaceLeaseError::Busy { path })
            }
            Err(source) => Err(WorkspaceLeaseError::Io { path, source }),
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for WorkspaceWriterLease {
    fn drop(&mut self) {
        let _ = FileExt::unlock(&self.file);
    }
}

#[async_trait]
pub trait Workspace: Send + Sync {
    fn is_local(&self) -> bool {
        false
    }
    /// Stable edit-coordination identity for this workspace and workdir.
    fn identity(&self, workdir: &Path) -> Result<WorkspaceIdentity, WorkspaceError>;
    /// Normalize a tool-facing relative path into this workspace's stable
    /// resource namespace. No remote I/O is performed.
    fn normalize_write_resource(
        &self,
        workdir: &Path,
        relative: &str,
    ) -> Result<PathBuf, WorkspaceError>;
    async fn read(&self, workdir: &Path, path: &str) -> Result<Vec<u8>, WorkspaceError>;
    async fn write(&self, workdir: &Path, path: &str, bytes: &[u8]) -> Result<(), WorkspaceError>;
    async fn remove(&self, workdir: &Path, path: &str) -> Result<(), WorkspaceError>;
    async fn list(&self, workdir: &Path, path: Option<&str>)
    -> Result<Vec<String>, WorkspaceError>;
    async fn find_files(
        &self,
        workdir: &Path,
        path: Option<&str>,
        include_ignored: bool,
    ) -> Result<Vec<String>, WorkspaceError>;
}

#[derive(Debug, Default, Clone, Copy)]
pub struct LocalWorkspace;

#[async_trait]
impl Workspace for LocalWorkspace {
    fn is_local(&self) -> bool {
        true
    }

    fn identity(&self, workdir: &Path) -> Result<WorkspaceIdentity, WorkspaceError> {
        WorkspaceIdentity::local(workdir)
    }

    fn normalize_write_resource(
        &self,
        workdir: &Path,
        relative: &str,
    ) -> Result<PathBuf, WorkspaceError> {
        normalize_write_path(workdir, relative)
    }

    async fn read(&self, workdir: &Path, path: &str) -> Result<Vec<u8>, WorkspaceError> {
        let path = local_path(workdir, path)?;
        tokio::fs::read(path)
            .await
            .map_err(|e| WorkspaceError::Operation(e.to_string()))
    }

    async fn write(&self, workdir: &Path, path: &str, bytes: &[u8]) -> Result<(), WorkspaceError> {
        let path = local_path(workdir, path)?;
        if let Some(parent) = path.parent() {
            tokio::fs::create_dir_all(parent)
                .await
                .map_err(|e| WorkspaceError::Operation(e.to_string()))?;
        }
        tokio::fs::write(path, bytes)
            .await
            .map_err(|e| WorkspaceError::Operation(e.to_string()))
    }

    async fn remove(&self, workdir: &Path, path: &str) -> Result<(), WorkspaceError> {
        let path = local_path(workdir, path)?;
        tokio::fs::remove_file(path)
            .await
            .map_err(|e| WorkspaceError::Operation(e.to_string()))
    }

    async fn list(
        &self,
        workdir: &Path,
        path: Option<&str>,
    ) -> Result<Vec<String>, WorkspaceError> {
        let requested = path.unwrap_or(".");
        let path = if requested.is_empty() || requested == "." {
            workdir.to_path_buf()
        } else {
            local_path(workdir, requested)?
        };
        let mut dir = tokio::fs::read_dir(path)
            .await
            .map_err(|e| WorkspaceError::Operation(e.to_string()))?;
        let mut entries = Vec::new();
        while let Some(entry) = dir
            .next_entry()
            .await
            .map_err(|e| WorkspaceError::Operation(e.to_string()))?
        {
            entries.push(entry.file_name().to_string_lossy().into_owned());
        }
        entries.sort();
        Ok(entries)
    }

    async fn find_files(
        &self,
        workdir: &Path,
        path: Option<&str>,
        _include_ignored: bool,
    ) -> Result<Vec<String>, WorkspaceError> {
        let root = if path.unwrap_or(".") == "." {
            workdir.to_path_buf()
        } else {
            local_path(workdir, path.unwrap())?
        };
        let mut out = Vec::new();
        let mut stack = vec![root];
        while let Some(dir) = stack.pop() {
            let mut entries = tokio::fs::read_dir(&dir)
                .await
                .map_err(|e| WorkspaceError::Operation(e.to_string()))?;
            while let Some(entry) = entries
                .next_entry()
                .await
                .map_err(|e| WorkspaceError::Operation(e.to_string()))?
            {
                let path = entry.path();
                if entry
                    .file_type()
                    .await
                    .map_err(|e| WorkspaceError::Operation(e.to_string()))?
                    .is_dir()
                {
                    stack.push(path);
                } else if path.is_file() {
                    out.push(
                        path.strip_prefix(workdir)
                            .unwrap_or(&path)
                            .to_string_lossy()
                            .into_owned(),
                    );
                }
            }
        }
        out.sort();
        Ok(out)
    }
}

/// SSH-backed workspace using `ssh host sh -lc ...`. Payloads are sent via
/// stdin for writes, avoiding shell interpolation and command-line limits.
pub struct RemoteWorkspace {
    target: String,
    root: Option<String>,
}

impl RemoteWorkspace {
    pub fn new(target: impl Into<String>, root: Option<String>) -> Self {
        Self {
            target: target.into(),
            root,
        }
    }
    pub fn target(&self) -> &str {
        &self.target
    }

    fn remote_base(&self, workdir: &Path) -> Result<String, WorkspaceError> {
        if workdir.as_os_str().is_empty() {
            Ok(self.root.as_deref().unwrap_or(".").to_owned())
        } else {
            workdir
                .to_str()
                .map(str::to_owned)
                .ok_or_else(|| WorkspaceError::InvalidPath("workdir is not UTF-8".into()))
        }
    }

    fn remote_path(&self, workdir: &Path, path: &str) -> Result<String, WorkspaceError> {
        let base = self.remote_base(workdir)?;
        if path.is_empty() || path == "." {
            Ok(base)
        } else {
            let rel = safe_relative(path)?;
            Ok(join_remote_root(&base, &rel))
        }
    }

    async fn command(
        &self,
        script: String,
        input: Option<&[u8]>,
    ) -> Result<Vec<u8>, WorkspaceError> {
        let mut cmd = tokio::process::Command::new("ssh");
        cmd.args(["-T", &self.target, "sh", "-lc", &script]);
        if input.is_some() {
            cmd.stdin(std::process::Stdio::piped());
        }
        cmd.stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped());
        let mut child = cmd
            .spawn()
            .map_err(|e| WorkspaceError::Operation(e.to_string()))?;
        if let Some(input) = input {
            use tokio::io::AsyncWriteExt;
            child
                .stdin
                .take()
                .unwrap()
                .write_all(input)
                .await
                .map_err(|e| WorkspaceError::Operation(e.to_string()))?;
        }
        let output = child
            .wait_with_output()
            .await
            .map_err(|e| WorkspaceError::Operation(e.to_string()))?;
        if !output.status.success() {
            let detail = String::from_utf8_lossy(&output.stderr).trim().to_string();
            return Err(WorkspaceError::Operation(if detail.is_empty() {
                format!("remote command exited with {}", output.status)
            } else {
                detail
            }));
        }
        Ok(output.stdout)
    }
}

#[async_trait]
impl Workspace for RemoteWorkspace {
    fn identity(&self, workdir: &Path) -> Result<WorkspaceIdentity, WorkspaceError> {
        WorkspaceIdentity::remote(&self.target, self.remote_base(workdir)?)
    }

    fn normalize_write_resource(
        &self,
        workdir: &Path,
        relative: &str,
    ) -> Result<PathBuf, WorkspaceError> {
        let root = normalize_remote_root(&self.remote_base(workdir)?)?;
        let relative = safe_relative(relative)?;
        Ok(PathBuf::from(join_remote_root(&root, &relative)))
    }

    async fn read(&self, workdir: &Path, path: &str) -> Result<Vec<u8>, WorkspaceError> {
        let path = shell_quote(&self.remote_path(workdir, path)?);
        self.command(format!("cat -- {path}"), None).await
    }
    async fn write(&self, workdir: &Path, path: &str, bytes: &[u8]) -> Result<(), WorkspaceError> {
        let path = shell_quote(&self.remote_path(workdir, path)?);
        self.command(
            // Keep the dirname result quoted. Remote workspaces commonly live
            // below paths containing spaces, and an unquoted command
            // substitution silently splits those paths into multiple args.
            format!("parent=$(dirname -- {path}) && mkdir -p -- \"$parent\" && cat > {path}"),
            Some(bytes),
        )
        .await
        .map(|_| ())
    }
    async fn remove(&self, workdir: &Path, path: &str) -> Result<(), WorkspaceError> {
        let path = shell_quote(&self.remote_path(workdir, path)?);
        self.command(format!("rm -- {path}"), None)
            .await
            .map(|_| ())
    }
    async fn list(
        &self,
        workdir: &Path,
        path: Option<&str>,
    ) -> Result<Vec<String>, WorkspaceError> {
        let path = shell_quote(&self.remote_path(workdir, path.unwrap_or("."))?);
        let bytes = self
            .command(
                // `-printf` is GNU-only; `-exec basename` works with both
                // GNU and BSD find implementations used by SSH targets.
                format!("find {path} -mindepth 1 -maxdepth 1 -exec basename {{}} \\;"),
                None,
            )
            .await?;
        let mut entries: Vec<String> = String::from_utf8_lossy(&bytes)
            .lines()
            .map(str::to_owned)
            .collect();
        entries.sort();
        Ok(entries)
    }

    async fn find_files(
        &self,
        workdir: &Path,
        path: Option<&str>,
        include_ignored: bool,
    ) -> Result<Vec<String>, WorkspaceError> {
        let root = self.remote_path(workdir, path.unwrap_or("."))?;
        let root_quoted = shell_quote(&root);
        // Match local glob semantics when the remote directory is a Git
        // checkout: ignored files stay out of searches unless requested.
        // Fall back to `find` for ordinary directories and non-Git hosts.
        let script = if include_ignored {
            format!("find {root_quoted} -type f -print")
        } else {
            format!(
                "if cd {root_quoted} 2>/dev/null && git rev-parse --show-toplevel >/dev/null 2>&1; then git ls-files --cached --others --exclude-standard -- . | while IFS= read -r file; do [ -f \"$file\" ] && printf '%s\\n' \"$file\"; done; else find {root_quoted} -type f -print; fi"
            )
        };
        let bytes = self.command(script, None).await?;
        let base = root.trim_end_matches('/');
        let mut files: Vec<String> = String::from_utf8_lossy(&bytes)
            .lines()
            .filter_map(|line| {
                if !include_ignored && !line.starts_with('/') {
                    return Some(line.to_string());
                }
                line.strip_prefix(base)
                    .map(|p| p.trim_start_matches('/').to_string())
            })
            .collect();
        files.sort();
        Ok(files)
    }
}

fn safe_relative(path: &str) -> Result<String, WorkspaceError> {
    if path.is_empty() || Path::new(path).is_absolute() {
        return Err(WorkspaceError::InvalidPath(path.into()));
    }
    let mut parts = Vec::new();
    let normalized = path.replace('\\', "/");
    for part in normalized.split('/') {
        if part.is_empty() || part == "." {
            continue;
        }
        if part == ".." {
            return Err(WorkspaceError::InvalidPath(path.into()));
        }
        parts.push(part);
    }
    if parts.is_empty() {
        return Err(WorkspaceError::InvalidPath(path.into()));
    }
    Ok(parts.join("/"))
}

fn local_path(workdir: &Path, path: &str) -> Result<PathBuf, WorkspaceError> {
    Ok(workdir.join(safe_relative(path)?))
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    struct TestDir(PathBuf);

    impl TestDir {
        fn new(label: &str) -> Self {
            let nonce = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let path = std::env::temp_dir().join(format!(
                "firmius-workspace-{label}-{}-{nonce}",
                std::process::id()
            ));
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }

    impl Drop for TestDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn rejects_traversal_and_absolute_paths() {
        assert!(safe_relative("../secret").is_err());
        assert!(safe_relative("/etc/passwd").is_err());
        assert_eq!(safe_relative("src/./main.rs").unwrap(), "src/main.rs");
    }
    #[test]
    fn quotes_remote_paths() {
        assert_eq!(shell_quote("/srv/a b"), "'/srv/a b'");
        assert_eq!(shell_quote("a'b"), "'a'\\''b'");
    }

    #[test]
    fn local_identity_canonicalizes_aliases() {
        let temp = TestDir::new("alias");
        fs::create_dir(temp.0.join("root")).unwrap();
        let direct = WorkspaceIdentity::local(temp.0.join("root")).unwrap();
        let dotted = WorkspaceIdentity::local(temp.0.join(".").join("root")).unwrap();
        assert_eq!(direct, dotted);
        assert_eq!(direct.lease_key(), dotted.lease_key());
    }

    #[cfg(unix)]
    #[test]
    fn local_identity_canonicalizes_symlink_aliases_and_detects_retarget() {
        use std::os::unix::fs::symlink;

        let temp = TestDir::new("symlink-alias");
        let first = temp.0.join("first");
        let second = temp.0.join("second");
        fs::create_dir(&first).unwrap();
        fs::create_dir(&second).unwrap();
        let alias = temp.0.join("alias");
        symlink(&first, &alias).unwrap();

        let direct = WorkspaceIdentity::local(&first).unwrap();
        let through_alias = WorkspaceIdentity::local(&alias).unwrap();
        assert_eq!(direct, through_alias);

        fs::remove_file(&alias).unwrap();
        symlink(&second, &alias).unwrap();
        assert!(matches!(
            through_alias.revalidate(),
            Err(WorkspaceError::RootReplaced { .. })
        ));
    }

    #[test]
    fn local_identity_detects_root_replacement() {
        let temp = TestDir::new("replacement");
        let root = temp.0.join("root");
        fs::create_dir(&root).unwrap();
        let identity = WorkspaceIdentity::local(&root).unwrap();
        fs::rename(&root, temp.0.join("old-root")).unwrap();
        fs::create_dir(&root).unwrap();
        assert!(matches!(
            identity.revalidate(),
            Err(WorkspaceError::RootReplaced { .. })
        ));
    }

    #[test]
    fn remote_identity_normalizes_root_but_keeps_target_distinct() {
        let first = WorkspaceIdentity::remote("deploy-a", "/srv//app/./").unwrap();
        let alias = WorkspaceIdentity::remote(" deploy-a ", "/srv/app").unwrap();
        let other_target = WorkspaceIdentity::remote("deploy-b", "/srv/app").unwrap();
        assert_eq!(first, alias);
        assert_ne!(first, other_target);
        assert_ne!(first.lease_key(), other_target.lease_key());
        assert_eq!(first.remote_root(), Some("/srv/app"));
        assert!(WorkspaceIdentity::remote("deploy-a", "srv/app").is_err());
        assert!(WorkspaceIdentity::remote("deploy-a", "/srv/../app").is_err());
    }

    #[test]
    fn remote_workspace_exposes_identity_and_normalized_write_resource() {
        let workspace = RemoteWorkspace::new("build-a", Some("/srv//app/./".into()));
        let identity = workspace.identity(Path::new("")).unwrap();
        assert_eq!(identity.remote_target(), Some("build-a"));
        assert_eq!(identity.remote_root(), Some("/srv/app"));
        assert_eq!(
            workspace
                .normalize_write_resource(Path::new(""), "src/./main.rs")
                .unwrap(),
            PathBuf::from("/srv/app/src/main.rs")
        );
        assert!(
            workspace
                .normalize_write_resource(Path::new(""), "../escape")
                .is_err()
        );

        let other_target = RemoteWorkspace::new("build-b", Some("/srv/app".into()));
        assert_ne!(identity, other_target.identity(Path::new("")).unwrap());
    }

    #[test]
    fn writer_lease_excludes_second_holder_and_drop_releases() {
        let temp = TestDir::new("lease");
        let workspace = temp.0.join("workspace");
        fs::create_dir(&workspace).unwrap();
        let identity = WorkspaceIdentity::local(workspace).unwrap();
        let leases = temp.0.join("protected-leases");

        let first = WorkspaceWriterLease::acquire(&identity, &leases).unwrap();
        assert!(matches!(
            WorkspaceWriterLease::acquire(&identity, &leases),
            Err(WorkspaceLeaseError::Busy { .. })
        ));
        let lease_path = first.path().to_path_buf();
        drop(first);
        let second = WorkspaceWriterLease::acquire(&identity, &leases).unwrap();
        assert_eq!(second.path(), lease_path);
    }

    #[test]
    fn write_path_supports_missing_leaf_and_rejects_symlink_escape() {
        let temp = TestDir::new("write-path");
        let root = temp.0.join("root");
        let existing = root.join("existing");
        fs::create_dir_all(&existing).unwrap();
        assert_eq!(
            normalize_write_path(&root, "existing/new/leaf.txt").unwrap(),
            fs::canonicalize(existing).unwrap().join("new/leaf.txt")
        );

        #[cfg(unix)]
        {
            use std::os::unix::fs::symlink;
            let outside = temp.0.join("outside");
            fs::create_dir(&outside).unwrap();
            symlink(&outside, root.join("escape")).unwrap();
            assert!(normalize_write_path(&root, "escape/file.txt").is_err());
        }
    }
}
