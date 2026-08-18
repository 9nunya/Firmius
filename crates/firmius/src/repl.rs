//! The original line-based REPL, kept as the non-TTY fallback (pipes,
//! dumb terminals, `--repl`). Same behavior as before the TUI existed.

use std::io::{self, Write};
use std::sync::Arc;

use firmius_core::{Agent, AgentError, AgentEvent, Session};
use tokio::sync::Mutex;
use tokio_util::sync::CancellationToken;

pub async fn run(session: Arc<Mutex<Session>>, agent: Arc<Agent>) -> io::Result<()> {
    let (session_id, provider_id) = {
        let s = session.lock().await;
        (s.id.clone(), agent.config().provider_id)
    };
    println!(
        "firmius repl (session {}, provider {}). type /quit to exit.\n",
        session_id, provider_id
    );
    let stdin = io::stdin();
    loop {
        print!("> ");
        io::stdout().flush()?;
        let mut line = String::new();
        if stdin.read_line(&mut line)? == 0 {
            break;
        }
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if line == "/quit" || line == "/exit" {
            break;
        }

        let cancel = CancellationToken::new();
        let ctrl_c = cancel.clone();
        tokio::spawn(async move {
            let _ = tokio::signal::ctrl_c().await;
            ctrl_c.cancel();
        });

        let result = agent
            .prompt(line, cancel, |event| match event {
                AgentEvent::Thinking(delta) => {
                    eprint!("\x1b[90m{delta}\x1b[0m");
                    let _ = io::stderr().flush();
                }
                AgentEvent::UserMessage(_) => {}
                AgentEvent::Text(delta) => {
                    print!("{delta}");
                    let _ = io::stdout().flush();
                }
                AgentEvent::RetryScheduled {
                    account_id,
                    attempt,
                    delay_ms,
                    switched,
                    class,
                } => {
                    let action = if switched {
                        format!("switching to {account_id}")
                    } else {
                        format!("retrying on {account_id}")
                    };
                    let delay = if delay_ms >= 1000 {
                        format!("{:.2}s", delay_ms as f64 / 1000.0)
                    } else {
                        format!("{delay_ms}ms")
                    };
                    eprintln!(
                        "\x1b[90m[retry] {action} attempt {attempt} after {} in {delay}\x1b[0m",
                        class.label()
                    );
                }
                AgentEvent::ToolCallDelta { .. } => {}
                AgentEvent::ToolCallStarted { name, args, .. } => {
                    println!("\n\x1b[36m[tool] {name} {args}\x1b[0m");
                }
                AgentEvent::ToolResult {
                    name, ok, content, ..
                } => {
                    let status = if ok { "ok" } else { "err" };
                    let preview: String = content.chars().take(200).collect();
                    println!("\x1b[36m[tool:{status}] {name} -> {preview}\x1b[0m");
                }
                AgentEvent::Usage(u) => {
                    eprintln!(
                        "\x1b[90m[usage] in={} out={} cache_r={} cache_w={}\x1b[0m",
                        u.input_tokens, u.output_tokens, u.cache_read_tokens, u.cache_write_tokens
                    );
                }
                AgentEvent::TurnFinished
                | AgentEvent::CompactionScheduled { .. }
                | AgentEvent::CompactionStarted { .. }
                | AgentEvent::CompactionFinished { .. }
                | AgentEvent::CompactionDiscarded { .. }
                | AgentEvent::CompactionFailed { .. } => {}
            })
            .await;

        match result {
            Ok(_) => println!(),
            Err(AgentError::Cancelled(partial)) => {
                if !partial.is_empty() {
                    println!("\n\x1b[33m[cancelled, partial output above]\x1b[0m");
                } else {
                    eprintln!("\x1b[31mcancelled\x1b[0m");
                }
            }
            Err(e) => eprintln!("\n\x1b[31merror: {e}\x1b[0m"),
        }
    }

    if let Err(e) = session.lock().await.save() {
        eprintln!("warning: could not save session {session_id}: {e}");
    }
    Ok(())
}
