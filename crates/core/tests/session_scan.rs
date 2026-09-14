use std::{fs, process::Command};

fn main() {
    if std::env::args().any(|arg| arg == "--scan-child") {
        assert!(firmius_core::persistence::list_sessions().unwrap().is_empty());
        return;
    }
    let root = std::env::temp_dir().join(format!("firmius-scan-{}", uuid::Uuid::new_v4()));
    let sessions = root.join("sessions");
    fs::create_dir_all(&sessions).unwrap();
    for index in 0..25 {
        fs::write(sessions.join(format!("bad-{index}.json")), "{private-invalid").unwrap();
    }
    let scan = || {
        let output = Command::new(std::env::current_exe().unwrap())
            .arg("--scan-child")
            .env("FIRMIUS_DATA_DIR", &root)
            .output().unwrap();
        assert!(output.status.success(), "{:?}", output);
        assert!(output.stdout.is_empty());
        assert!(output.stderr.is_empty());
    };
    scan();
    let log = fs::read_to_string(root.join("session-scan.log")).unwrap();
    assert!(log.contains("skipped 25"));
    assert!(log.contains("showing 20"));
    assert!(!log.contains("private-invalid"));
    scan();
    fs::remove_file(root.join("session-scan.log")).unwrap();
    fs::create_dir(root.join("session-scan.log")).unwrap();
    scan();
    assert!(!fs::read_dir(&root).unwrap().any(|entry| entry.unwrap().file_name().to_string_lossy().contains(".tmp.")));
    fs::remove_dir_all(root).unwrap();
}