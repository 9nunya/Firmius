use std::fmt;
use std::io::{self, IsTerminal, Write};
use std::path::{Path, PathBuf};

use firmius_client::DaemonClient;
use firmius_protocol::{Request, Response};

const MARKER_NAME: &str = "firmius-install.json";
const OFFICIAL_REPO: &str = "9nunya/Firmius";
const INSTALL_SH_URL: &str =
    "https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh";
const INSTALL_PS1_URL: &str =
    "https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.ps1";
const OFFICIAL_GIT_URL: &str = "https://github.com/9nunya/Firmius.git";
const CONFIRM_PHRASE: &str = "update Firmius from 9nunya/Firmius";
const RELEASES_API: &str = "https://api.github.com/repos/9nunya/Firmius/releases/latest";

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum InstallChannel {
    ReleaseScript { repo: String, version: String },
    CargoGit { repo: String },
    CargoUnknown,
    Homebrew,
    Linuxbrew,
    Nix,
    Scoop,
    Chocolatey,
    Snap,
    SystemPackage,
    Source,
    Standalone,
    Unknown,
}

impl InstallInfo {
    pub(crate) fn summary(&self) -> String {
        self.channel.to_string()
    }
}

async fn execute_update(
    program: &str,
    args: &[String],
    _authorization: UpdateAuthorization,
) -> Result<(), Box<dyn std::error::Error>> {
    let status = tokio::process::Command::new(program)
        .args(args)
        .status()
        .await?;
    if !status.success() {
        return Err(format!("Firmius update command failed with status {status}").into());
    }
    Ok(())
}

fn confirm_update(
    channel: &str,
    source: &str,
    destination: &Path,
    phrase: &str,
) -> Result<UpdateAuthorization, Box<dyn std::error::Error>> {
    let stdin_terminal = io::stdin().is_terminal();
    let output_terminal = io::stderr().is_terminal();
    if !stdin_terminal || !output_terminal {
        return Err("Firmius update requires an interactive terminal; no network request was made and no process was launched.".into());
    }

    eprintln!("firmius: marker metadata is not authentication.");
    eprintln!("firmius: update channel: {channel}");
    eprintln!("firmius: fixed official update source: {source}");
    eprintln!("firmius: update destination: {}", destination.display());
    eprint!("firmius: type exactly `{phrase}` to continue: ");
    io::stderr().flush()?;
    let mut answer = String::new();
    let read = io::stdin().read_line(&mut answer)?;
    if !confirmation_authorized(
        stdin_terminal,
        output_terminal,
        (read != 0).then_some(answer.as_str()),
        phrase,
    ) {
        return Err("Firmius update confirmation did not match; no network request was made and no process was launched.".into());
    }
    Ok(UpdateAuthorization(()))
}

struct UpdateAuthorization(());

fn confirmation_authorized(
    stdin_terminal: bool,
    output_terminal: bool,
    answer: Option<&str>,
    phrase: &str,
) -> bool {
    stdin_terminal
        && output_terminal
        && answer.is_some_and(|answer| confirmation_matches(answer, phrase))
}

fn confirmation_matches(answer: &str, phrase: &str) -> bool {
    answer
        .strip_suffix("\r\n")
        .or_else(|| answer.strip_suffix('\n'))
        == Some(phrase)
}

impl fmt::Display for InstallChannel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ReleaseScript { repo, .. } => write!(f, "release script ({repo})"),
            Self::CargoGit { repo } => write!(f, "Cargo (Git source: {repo})"),
            Self::CargoUnknown => f.write_str("Cargo (source unknown)"),
            Self::Homebrew => f.write_str("Homebrew"),
            Self::Linuxbrew => f.write_str("Linuxbrew"),
            Self::Nix => f.write_str("Nix"),
            Self::Scoop => f.write_str("Scoop"),
            Self::Chocolatey => f.write_str("Chocolatey"),
            Self::Snap => f.write_str("Snap"),
            Self::SystemPackage => f.write_str("system package manager"),
            Self::Source => f.write_str("source/development build"),
            Self::Standalone => f.write_str("standalone binary (unverified origin)"),
            Self::Unknown => f.write_str("unknown"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct InstallInfo {
    pub executable: PathBuf,
    pub marker: PathBuf,
    pub channel: InstallChannel,
    pub marker_warning: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum UpdatePlan {
    Confirm {
        program: String,
        args: Vec<String>,
        channel: &'static str,
        source: String,
        destination: PathBuf,
        phrase: &'static str,
    },
    Advice(String),
    Refuse(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Platform {
    Unix,
    Windows,
}

impl Platform {
    fn current() -> Self {
        if cfg!(windows) {
            Self::Windows
        } else {
            Self::Unix
        }
    }
}

pub(crate) async fn handle_early_command(
    args: &[String],
) -> Option<Result<(), Box<dyn std::error::Error>>> {
    let command = args.get(1).map(String::as_str)?;
    match command {
        "--help" | "-h" | "help" if args.len() == 2 => {
            print_cli_help();
            Some(Ok(()))
        }
        "--help" | "-h" | "help" => Some(Err(
            "firmius --help accepts no arguments; no command was run.".into(),
        )),
        "--version" | "-V" if args.len() == 2 => {
            println!("firmius {}", env!("CARGO_PKG_VERSION"));
            Some(Ok(()))
        }
        "--version" | "-V" => Some(Err(
            "firmius --version accepts no arguments; no command was run.".into(),
        )),
        "doctor" | "install-info" if args.len() == 2 => Some(print_install_info()),
        "doctor" | "install-info" => Some(Err(
            "firmius doctor accepts no arguments; no command was run.".into(),
        )),
        "update" | "--update" if args.len() == 2 => Some(run_update().await),
        "update" | "--update" => Some(Err(
            "firmius update accepts no options (there is no --yes bypass); no network request was made and no process was launched."
                .into(),
        )),
        "update-check" if args.len() == 2 => Some(run_update_check().await),
        "update-check" => Some(Err("firmius update-check accepts no arguments; no update was performed.".into())),
        "daemon-stop" if args.len() == 2 => Some(stop_running_daemon_for_update().await),
        "daemon-stop" => Some(Err("firmius daemon-stop accepts no arguments; no command was run.".into())),
        _ => None,
    }
}

fn print_cli_help() {
    println!(
        "Firmius — autonomous terminal-native work\n\n\
Usage:\n  firmius [options]\n  firmius <command>\n\n\
Startup options:\n  --resume <id>              Resume a saved session\n  --list-sessions            List saved sessions and exit\n  --reset-onboarding         Show the first-run launchpad again\n  --ssh <host> <directory>   Start a session in a remote SSH workspace\n\n\
Commands:\n  doctor                     Diagnose install, daemon, and provider state\n  install-info               Alias for doctor\n  ssh-hosts                  List SSH config and known_hosts targets\n  prompt [persona]           Print the effective persona prompt\n  goal <operation>           Create or inspect a durable autonomous goal\n  update-check               Check the latest official release\n  update                     Update through the detected install channel\n  daemon                    Run the local daemon in the foreground\n  daemon-stop               Stop the local daemon\n  --version                 Print the installed version\n  --help                    Print this help\n\n\
Examples:\n  firmius goal create \"Ship the next release\"\n  FIRMIUS_PROVIDER=openai firmius goal create \"Run the release checks\"\n  firmius --ssh build /srv/project\n

Inside the TUI, type /help for session commands and shortcuts."
    );
    println!(
        "SSH workspace shortcuts: `firmius ssh-hosts add <alias> <absolute-dir>`, `firmius ssh-hosts open <alias>`, and `firmius ssh-hosts remove <alias>`.\n"
    );
    println!("Use `firmius ssh-hosts list` as an explicit alias for the default listing.\n");
}

pub(crate) async fn run_update_check() -> Result<(), Box<dyn std::error::Error>> {
    let info = detect();
    let client = reqwest::Client::builder()
        .user_agent(format!("firmius/{}", env!("CARGO_PKG_VERSION")))
        .build()?;
    let payload: serde_json::Value = client
        .get(RELEASES_API)
        .send()
        .await?
        .error_for_status()?
        .json()
        .await?;
    let latest = payload
        .get("tag_name")
        .and_then(|value| value.as_str())
        .unwrap_or("")
        .trim_start_matches('v');
    if latest.is_empty() {
        return Err("official release response did not contain tag_name".into());
    }
    let current = env!("CARGO_PKG_VERSION");
    println!("Current: {current}");
    println!("Latest:  {latest}");
    if version_tuple(latest) > version_tuple(current) {
        println!(
            "Update available: run `firmius update` after reviewing the channel and confirmation prompt."
        );
    } else {
        println!("Firmius is up to date.");
    }
    println!("Install channel: {}", info.channel);
    Ok(())
}

fn version_tuple(value: &str) -> (u64, u64, u64) {
    let mut parts = value
        .trim_start_matches('v')
        .split('.')
        .map(|part| part.split('-').next().unwrap_or("0").parse().unwrap_or(0));
    (
        parts.next().unwrap_or(0),
        parts.next().unwrap_or(0),
        parts.next().unwrap_or(0),
    )
}

fn print_install_info() -> Result<(), Box<dyn std::error::Error>> {
    let info = detect();
    println!("Firmius {}", env!("CARGO_PKG_VERSION"));
    println!("Executable: {}", info.executable.display());
    println!("Install channel: {}", info.channel);
    println!("Install marker: {}", info.marker.display());
    if let Some(warning) = &info.marker_warning {
        println!("Marker status: ignored ({warning})");
    } else if info.marker.exists() {
        println!("Marker status: valid metadata (not authentication)");
    } else {
        println!("Marker status: not present; channel inferred conservatively");
    }
    print_runtime_diagnostics();
    match plan_update(&info, Platform::current()) {
        UpdatePlan::Confirm {
            channel,
            source,
            destination,
            phrase,
            ..
        } => println!(
            "Update method: interactive confirmation required\nUpdate channel: {channel}\nFixed official update source: {source}\nUpdate destination: {}\nMarker warning: marker metadata is not authentication\nConfirmation phrase: {phrase}",
            destination.display()
        ),
        UpdatePlan::Advice(advice) => println!("Update method: {advice}"),
        UpdatePlan::Refuse(reason) => println!("Update method: unavailable ({reason})"),
    }
    Ok(())
}

fn print_runtime_diagnostics() {
    let data = firmius_core::data_dir();
    let storage = if data.exists() {
        if std::fs::metadata(&data).is_ok() {
            "readable"
        } else {
            "unreadable"
        }
    } else if data.parent().is_some_and(|parent| parent.exists()) {
        "not initialized (parent exists)"
    } else {
        "not initialized (parent missing)"
    };
    println!("Runtime data: {} ({storage})", data.display());

    let daemon = data.join("daemon.json");
    let daemon_status = match std::fs::read_to_string(&daemon) {
        Ok(contents)
            if serde_json::from_str::<firmius_protocol::DaemonEndpoint>(&contents).is_ok() =>
        {
            if firmius_service::is_locked(&data) {
                "running (profile lease held)"
            } else {
                "stale endpoint metadata (lease free)"
            }
        }
        Ok(_) => "invalid endpoint metadata",
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            "not running or never started"
        }
        Err(_) => "endpoint metadata unreadable",
    };
    println!("Daemon: {daemon_status}");

    let account_dir = data.join("accounts");
    let account_count = std::fs::read_dir(&account_dir)
        .ok()
        .into_iter()
        .flatten()
        .filter_map(Result::ok)
        .filter(|entry| entry.path().extension().is_some_and(|ext| ext == "json"))
        .count();
    if account_count == 0 {
        println!("Providers: none configured (open the onboarding provider setup)");
    } else {
        println!(
            "Providers: {account_count} configured account{}",
            if account_count == 1 { "" } else { "s" }
        );
    }
}

async fn run_update() -> Result<(), Box<dyn std::error::Error>> {
    let info = detect();
    eprintln!("firmius: detected {} install", info.channel);
    if let Some(warning) = &info.marker_warning {
        eprintln!("firmius: warning: ignored install marker: {warning}");
    }

    match plan_update(&info, Platform::current()) {
        UpdatePlan::Confirm {
            program,
            args,
            channel,
            source,
            destination,
            phrase,
        } => {
            let authorization = confirm_update(channel, &source, &destination, phrase)?;
            stop_running_daemon_for_update().await?;
            execute_update(&program, &args, authorization).await?;
            eprintln!("firmius: update completed; restart Firmius to use the new version.");
            Ok(())
        }
        UpdatePlan::Advice(advice) => Err(format!(
            "Firmius will not replace a package-manager or source install. {advice}"
        )
        .into()),
        UpdatePlan::Refuse(reason) => Err(reason.into()),
    }
}

/// A CLI update must account for the daemon using the same executable. Stop
/// the daemon through its authenticated local protocol before replacing the
/// binary, then wait for its profile lease to be released. If the endpoint is
/// stale while the lease is held, fail closed rather than updating underneath
/// an unknown live runtime.
async fn stop_running_daemon_for_update() -> Result<(), Box<dyn std::error::Error>> {
    let root = firmius_core::data_dir();
    if !firmius_service::is_locked(&root) {
        return Ok(());
    }
    let endpoint = firmius_service::endpoint_path(&root);
    let client = DaemonClient::connect(&endpoint).await.map_err(|error| {
        format!(
            "Firmius daemon is running but could not be contacted at {}: {error}; update aborted",
            endpoint.display()
        )
    })?;
    match client.request(Request::Shutdown).await {
        Ok(Response::Ack) => {}
        Ok(response) => {
            client.close().await;
            return Err(format!("daemon refused the update shutdown request: {response:?}").into());
        }
        Err(error) => {
            client.close().await;
            return Err(format!("daemon shutdown request failed: {error}").into());
        }
    }
    client.close().await;
    let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(10);
    while firmius_service::is_locked(&root) {
        if tokio::time::Instant::now() >= deadline {
            return Err(
                "daemon did not release its profile lease after shutdown; update aborted".into(),
            );
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    eprintln!("firmius: stopped daemon before update");
    Ok(())
}

pub(crate) fn detect() -> InstallInfo {
    let executable = std::env::current_exe()
        .map(|path| std::fs::canonicalize(&path).unwrap_or(path))
        .unwrap_or_else(|_| PathBuf::from("firmius"));
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from);
    detect_install(&executable, home.as_deref())
}

pub(crate) fn detect_install(executable: &Path, home: Option<&Path>) -> InstallInfo {
    let marker = executable
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join(MARKER_NAME);
    let mut marker_warning = None;

    if marker.exists() {
        match read_marker(&marker, executable, home) {
            Ok(channel) => {
                return InstallInfo {
                    executable: executable.to_path_buf(),
                    marker,
                    channel,
                    marker_warning: None,
                };
            }
            Err(error) => marker_warning = Some(error),
        }
    }

    InstallInfo {
        executable: executable.to_path_buf(),
        marker,
        channel: infer_channel(executable, home),
        marker_warning,
    }
}

fn read_marker(
    path: &Path,
    executable: &Path,
    home: Option<&Path>,
) -> Result<InstallChannel, String> {
    let bytes = std::fs::read(path).map_err(|error| error.to_string())?;
    if bytes.len() > 16 * 1024 {
        return Err("marker is unexpectedly large".into());
    }
    let value: serde_json::Value =
        serde_json::from_slice(&bytes).map_err(|error| format!("invalid JSON: {error}"))?;
    let object = value
        .as_object()
        .ok_or_else(|| "marker must be a JSON object".to_string())?;
    let channel = object
        .get("channel")
        .and_then(|value| value.as_str())
        .ok_or_else(|| "marker has no channel".to_string())?;
    let repo = object
        .get("repo")
        .and_then(|value| value.as_str())
        .ok_or_else(|| "marker has no repo".to_string())?;
    let version = object
        .get("version")
        .and_then(|value| value.as_str())
        .ok_or_else(|| "marker has no version".to_string())?;

    if !valid_repo(repo) || version.is_empty() || version.len() > 128 {
        return Err("marker repo or version failed validation".into());
    }
    match channel {
        "release-script" if repo == OFFICIAL_REPO => Ok(InstallChannel::ReleaseScript {
            repo: repo.into(),
            version: version.into(),
        }),
        "release-script" => Err(format!(
            "release-script marker names non-official repository {repo}"
        )),
        "cargo-registry" => Err(
            "cargo-registry metadata cannot select an updater: Firmius is not published on crates.io"
                .into(),
        ),
        "cargo-git"
            if repo == OFFICIAL_REPO
                && infer_channel(executable, home) == InstallChannel::CargoUnknown =>
        {
            Ok(InstallChannel::CargoGit { repo: repo.into() })
        }
        "cargo-git" => Err(
            "cargo-git marker is not an official Firmius Git install in a Cargo bin directory"
                .into(),
        ),
        "cargo" => {
            Err("cargo marker is not adjacent to a Cargo bin executable".into())
        }
        _ => Err(format!("unsupported marker channel {channel:?}")),
    }
}

fn valid_repo(repo: &str) -> bool {
    let mut parts = repo.split('/');
    let valid_part = |part: &str| {
        !part.is_empty()
            && part.len() <= 100
            && part
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    };
    matches!((parts.next(), parts.next(), parts.next()), (Some(a), Some(b), None) if valid_part(a) && valid_part(b))
}

fn infer_channel(executable: &Path, home: Option<&Path>) -> InstallChannel {
    let normalized = executable
        .to_string_lossy()
        .replace('\\', "/")
        .to_lowercase();
    let home_normalized = home.map(|path| {
        path.to_string_lossy()
            .replace('\\', "/")
            .trim_end_matches('/')
            .to_lowercase()
    });

    if home_normalized
        .as_ref()
        .is_some_and(|home| normalized.starts_with(&format!("{home}/.cargo/bin/")))
    {
        // A Cargo bin path alone does not reveal whether the binary came from
        // crates.io, --git, or --path. Never guess a network update source.
        InstallChannel::CargoUnknown
    } else if normalized.contains("/homebrew/cellar/") || normalized.contains("/usr/local/cellar/")
    {
        InstallChannel::Homebrew
    } else if normalized.contains("/home/linuxbrew/.linuxbrew/")
        || normalized.contains("/.linuxbrew/cellar/")
    {
        InstallChannel::Linuxbrew
    } else if normalized.starts_with("/nix/store/") || normalized.contains("/.nix-profile/bin/") {
        InstallChannel::Nix
    } else if normalized.contains("/scoop/apps/") || normalized.contains("/scoop/shims/") {
        InstallChannel::Scoop
    } else if normalized.contains("/chocolatey/bin/") || normalized.contains("/chocolatey/lib/") {
        InstallChannel::Chocolatey
    } else if normalized.starts_with("/snap/") || normalized.contains("/snap/bin/") {
        InstallChannel::Snap
    } else if normalized.contains("/target/debug/") || normalized.contains("/target/release/") {
        InstallChannel::Source
    } else if normalized.starts_with("/usr/bin/") || normalized.starts_with("/bin/") {
        InstallChannel::SystemPackage
    } else if home_normalized
        .as_ref()
        .is_some_and(|home| normalized.starts_with(&format!("{home}/.local/bin/")))
        || normalized.starts_with("/usr/local/bin/")
    {
        InstallChannel::Standalone
    } else {
        InstallChannel::Unknown
    }
}

pub(crate) fn plan_update(info: &InstallInfo, platform: Platform) -> UpdatePlan {
    match &info.channel {
        InstallChannel::ReleaseScript { .. } => {
            let destination = info.executable.clone();
            let install_dir = destination
                .parent()
                .unwrap_or_else(|| Path::new("."))
                .to_string_lossy()
                .into_owned();
            match platform {
                Platform::Unix => UpdatePlan::Confirm {
                    program: "sh".into(),
                    args: vec![
                        "-c".into(),
                        format!("curl -fsSL '{INSTALL_SH_URL}' | FIRMIUS_REPO='{OFFICIAL_REPO}' FIRMIUS_VERSION=latest sh -s -- --dir \"$1\""),
                        "firmius-update".into(),
                        install_dir,
                    ],
                    channel: "official release installer",
                    source: INSTALL_SH_URL.into(),
                    destination,
                    phrase: CONFIRM_PHRASE,
                },
                Platform::Windows => UpdatePlan::Confirm {
                    program: "powershell.exe".into(),
                    args: vec![
                        "-NoProfile".into(),
                        "-NonInteractive".into(),
                        "-ExecutionPolicy".into(),
                        "Bypass".into(),
                        "-Command".into(),
                        format!("$env:FIRMIUS_INSTALL_DIR=$args[0]; $env:FIRMIUS_REPO='{OFFICIAL_REPO}'; $env:FIRMIUS_VERSION='latest'; & ([scriptblock]::Create((Invoke-RestMethod '{INSTALL_PS1_URL}')))"),
                        install_dir,
                    ],
                    channel: "official release installer",
                    source: INSTALL_PS1_URL.into(),
                    destination,
                    phrase: CONFIRM_PHRASE,
                },
            }
        }
        InstallChannel::CargoGit { .. } => UpdatePlan::Confirm {
            program: "cargo".into(),
            args: vec![
                "install".into(),
                "--locked".into(),
                "--force".into(),
                "--git".into(),
                OFFICIAL_GIT_URL.into(),
                "--bin".into(),
                "firmius".into(),
                "firmius".into(),
            ],
            channel: "official Cargo Git install",
            source: OFFICIAL_GIT_URL.into(),
            destination: info.executable.clone(),
            phrase: CONFIRM_PHRASE,
        },
        InstallChannel::CargoUnknown => UpdatePlan::Refuse(
            "This executable is in Cargo's bin directory, but its source (crates.io, Git, or local path) is unknown. Reinstall with the official release installer or rerun the original Cargo command; no network request was made.".into(),
        ),
        InstallChannel::Homebrew | InstallChannel::Linuxbrew => {
            UpdatePlan::Advice("Run `brew upgrade firmius`.".into())
        }
        InstallChannel::Nix => UpdatePlan::Advice(
            "Update the Nix flake/profile that owns this executable (for a named profile package, run `nix profile upgrade firmius`).".into(),
        ),
        InstallChannel::Scoop => UpdatePlan::Advice("Run `scoop update firmius`.".into()),
        InstallChannel::Chocolatey => {
            UpdatePlan::Advice("Run `choco upgrade firmius`.".into())
        }
        InstallChannel::Snap => {
            UpdatePlan::Advice("Run `sudo snap refresh firmius`.".into())
        }
        InstallChannel::SystemPackage => UpdatePlan::Advice(format!(
            "Use the system package manager that owns `{}` (for example `apt upgrade`, `dnf upgrade`, or `pacman -Syu`); Firmius cannot safely choose it for you.",
            info.executable.display()
        )),
        InstallChannel::Source => UpdatePlan::Advice(
            "Run `git pull`, rebuild Firmius, and replace/install the binary using the same build workflow.".into(),
        ),
        InstallChannel::Standalone => UpdatePlan::Refuse(
            "This standalone binary has no valid install marker, so its origin cannot be verified. Reinstall from https://github.com/9nunya/Firmius/releases or with the official installer; no files were changed.".into(),
        ),
        InstallChannel::Unknown => UpdatePlan::Refuse(
            "The install channel is unknown. Firmius will not destructively replace this executable; use `firmius doctor` and reinstall through a known channel.".into(),
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn info(path: &str, channel: InstallChannel) -> InstallInfo {
        InstallInfo {
            executable: PathBuf::from(path),
            marker: PathBuf::from(path).parent().unwrap().join(MARKER_NAME),
            channel,
            marker_warning: None,
        }
    }

    #[test]
    fn update_check_version_comparison_is_numeric() {
        assert!(version_tuple("1.10.0") > version_tuple("1.9.9"));
        assert_eq!(version_tuple("v2.4"), (2, 4, 0));
    }

    #[test]
    fn classifies_common_install_paths_conservatively() {
        let home = Path::new("/home/alice");
        assert_eq!(
            infer_channel(Path::new("/home/alice/.cargo/bin/firmius"), Some(home)),
            InstallChannel::CargoUnknown
        );
        assert_eq!(
            infer_channel(
                Path::new("/opt/homebrew/Cellar/firmius/1/bin/firmius"),
                Some(home)
            ),
            InstallChannel::Homebrew
        );
        assert_eq!(
            infer_channel(
                Path::new("/home/linuxbrew/.linuxbrew/Cellar/firmius/1/bin/firmius"),
                Some(home)
            ),
            InstallChannel::Linuxbrew
        );
        assert_eq!(
            infer_channel(Path::new("/work/Firmius/target/debug/firmius"), Some(home)),
            InstallChannel::Source
        );
        assert_eq!(
            infer_channel(Path::new("/home/alice/.local/bin/firmius"), Some(home)),
            InstallChannel::Standalone
        );
        assert_eq!(
            infer_channel(Path::new("/odd/place/firmius"), Some(home)),
            InstallChannel::Unknown
        );
    }

    #[test]
    fn package_managers_and_unknown_never_self_replace() {
        assert!(
            matches!(plan_update(&info("/opt/homebrew/bin/firmius", InstallChannel::Homebrew), Platform::Unix), UpdatePlan::Advice(text) if text.contains("brew upgrade"))
        );
        assert!(
            matches!(plan_update(&info("/odd/firmius", InstallChannel::Unknown), Platform::Unix), UpdatePlan::Refuse(text) if text.contains("will not destructively replace"))
        );
    }

    #[test]
    fn release_marker_requires_confirmation_before_an_update() {
        let plan = plan_update(
            &info(
                "/custom/bin/firmius",
                InstallChannel::ReleaseScript {
                    repo: "attacker/forged-marker".into(),
                    version: "hostile-version".into(),
                },
            ),
            Platform::Unix,
        );
        assert!(matches!(
            plan,
            UpdatePlan::Confirm { source, destination, phrase, .. }
                if source.contains(INSTALL_SH_URL)
                    && destination == Path::new("/custom/bin/firmius")
                    && phrase == CONFIRM_PHRASE
        ));
    }

    #[test]
    fn marker_must_name_the_official_repository() {
        let dir = std::env::temp_dir().join(format!("firmius-install-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let executable = dir.join("firmius");
        std::fs::write(&executable, b"").unwrap();
        std::fs::write(
            dir.join(MARKER_NAME),
            r#"{"channel":"release-script","repo":"someone/fork","version":"v1"}"#,
        )
        .unwrap();
        let detected = detect_install(&executable, None);
        assert_eq!(detected.channel, InstallChannel::Unknown);
        assert!(detected.marker_warning.unwrap().contains("non-official"));
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn official_release_marker_is_metadata_for_custom_directory() {
        let dir =
            std::env::temp_dir().join(format!("firmius-install-valid-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let executable = dir.join("firmius");
        std::fs::write(&executable, b"").unwrap();
        std::fs::write(
            dir.join(MARKER_NAME),
            r#"{"channel":"release-script","repo":"9nunya/Firmius","version":"v1"}"#,
        )
        .unwrap();
        let detected = detect_install(&executable, None);
        assert!(matches!(
            &detected.channel,
            InstallChannel::ReleaseScript { version, .. } if version == "v1"
        ));
        assert_eq!(detected.marker_warning, None);
        assert!(matches!(
            plan_update(&detected, Platform::Unix),
            UpdatePlan::Confirm { phrase, .. } if phrase == CONFIRM_PHRASE
        ));
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn forged_official_marker_cannot_authorize_noninteractive_execution() {
        let detected = info(
            "/custom/bin/firmius",
            InstallChannel::ReleaseScript {
                repo: OFFICIAL_REPO.into(),
                version: "v999".into(),
            },
        );
        assert!(matches!(
            plan_update(&detected, Platform::Unix),
            UpdatePlan::Confirm { .. }
        ));
        assert!(!confirmation_authorized(
            false,
            true,
            Some("update Firmius from 9nunya/Firmius\n"),
            CONFIRM_PHRASE
        ));
    }

    #[test]
    fn forged_registry_marker_in_cargo_bin_cannot_select_the_network() {
        let home = std::env::temp_dir().join(format!(
            "firmius-install-registry-test-{}",
            std::process::id()
        ));
        let bin = home.join(".cargo/bin");
        let _ = std::fs::remove_dir_all(&home);
        std::fs::create_dir_all(&bin).unwrap();
        let executable = bin.join("firmius");
        std::fs::write(&executable, b"").unwrap();
        std::fs::write(
            bin.join(MARKER_NAME),
            r#"{"channel":"cargo-registry","repo":"9nunya/Firmius","version":"1.0.0"}"#,
        )
        .unwrap();

        let detected = detect_install(&executable, Some(&home));
        assert_eq!(detected.channel, InstallChannel::CargoUnknown);
        assert!(
            detected
                .marker_warning
                .as_deref()
                .is_some_and(|warning| warning.contains("not published on crates.io"))
        );
        assert!(matches!(
            plan_update(&detected, Platform::Unix),
            UpdatePlan::Refuse(reason) if reason.contains("no network request")
        ));
        std::fs::remove_dir_all(home).unwrap();
    }

    #[test]
    fn forged_cargo_marker_outside_cargo_bin_is_ignored() {
        let dir =
            std::env::temp_dir().join(format!("firmius-install-cargo-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let executable = dir.join("firmius");
        std::fs::write(&executable, b"").unwrap();
        std::fs::write(
            dir.join(MARKER_NAME),
            r#"{"channel":"cargo-git","repo":"9nunya/Firmius","version":"source"}"#,
        )
        .unwrap();
        let detected = detect_install(&executable, Some(Path::new("/home/alice")));
        assert_eq!(detected.channel, InstallChannel::Unknown);
        assert!(detected.marker_warning.unwrap().contains("Cargo bin"));
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn unmarked_cargo_path_refuses_to_guess_a_source() {
        let detected = detect_install(
            Path::new("/home/alice/.cargo/bin/firmius"),
            Some(Path::new("/home/alice")),
        );
        assert_eq!(detected.channel, InstallChannel::CargoUnknown);
        assert!(matches!(
            plan_update(&detected, Platform::Unix),
            UpdatePlan::Refuse(text) if text.contains("source") && text.contains("no network request")
        ));
    }

    #[test]
    fn cargo_git_marker_requires_confirmation_and_uses_fixed_official_source() {
        let plan = plan_update(
            &info(
                "/home/a/.cargo/bin/firmius",
                InstallChannel::CargoGit {
                    repo: "attacker/forged-marker".into(),
                },
            ),
            Platform::Unix,
        );
        assert!(matches!(
            plan,
            UpdatePlan::Confirm { program, args, source, phrase, .. }
                if program == "cargo"
                    && args.iter().any(|arg| arg == "https://github.com/9nunya/Firmius.git")
                    && source.contains("https://github.com/9nunya/Firmius.git")
                    && phrase == CONFIRM_PHRASE
        ));
    }

    #[test]
    fn confirmation_is_exact_and_rejects_eof_or_mismatch() {
        assert!(confirmation_authorized(
            true,
            true,
            Some("update Firmius from 9nunya/Firmius\n"),
            CONFIRM_PHRASE
        ));
        assert!(confirmation_authorized(
            true,
            true,
            Some("update Firmius from 9nunya/Firmius\r\n"),
            CONFIRM_PHRASE
        ));
        assert!(!confirmation_authorized(true, true, None, CONFIRM_PHRASE));
        assert!(!confirmation_authorized(
            false,
            true,
            Some("update Firmius from 9nunya/Firmius\n"),
            CONFIRM_PHRASE
        ));
        assert!(!confirmation_authorized(
            true,
            false,
            Some("update Firmius from 9nunya/Firmius\n"),
            CONFIRM_PHRASE
        ));
        assert!(!confirmation_authorized(
            true,
            true,
            Some("update firmius from 9nunya/Firmius\n"),
            CONFIRM_PHRASE
        ));
        assert!(!confirmation_authorized(
            true,
            true,
            Some(" update Firmius from 9nunya/Firmius\n"),
            CONFIRM_PHRASE
        ));
        assert!(!confirmation_matches(
            "update firmius from 9nunya/Firmius\n",
            CONFIRM_PHRASE
        ));
        assert!(!confirmation_matches(CONFIRM_PHRASE, CONFIRM_PHRASE));
        assert!(!confirmation_matches(
            "update Firmius from 9nunya/Firmius\n\n",
            CONFIRM_PHRASE
        ));
    }

    #[tokio::test]
    async fn early_commands_reject_extra_arguments_without_running() {
        for args in [
            vec!["firmius", "update", "junk"],
            vec!["firmius", "update", "--yes"],
            vec!["firmius", "--version", "junk"],
            vec!["firmius", "doctor", "junk"],
        ] {
            let args = args.into_iter().map(String::from).collect::<Vec<_>>();
            let result = handle_early_command(&args)
                .await
                .expect("recognized early command");
            assert!(
                result.is_err(),
                "extra arguments must be rejected: {args:?}"
            );
        }
    }
}
