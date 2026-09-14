//! Deterministic native UI fixture, isolated from daemon and account mutations.
use super::*;
pub(super) fn run(
    ui: &MainWindow,
    path: &str,
    state: Arc<Mutex<DesktopState>>,
) -> Result<(), slint::PlatformError> {
    ui.set_onboarding_open(false);
    ui.set_active_title("Refactor the desktop workspace".into());
    ui.set_connection_status("Preview fixture · offline".into());
    ui.set_active_model("Example model".into());
    ui.set_focus_status(
        "Lead · Example model · persona coder · input 18,420 / output 2,310 tokens".into(),
    );
    ui.set_live_status(
        "Desktop refactor — Layout: complete · Command system: running · Review: waiting".into(),
    );
    ui.set_agents(ModelRc::new(VecModel::from(vec![
        AgentRow {
            id: "lead".into(),
            label: "Lead agent".into(),
            detail: "coder · working".into(),
            active: true,
        },
        AgentRow {
            id: "review".into(),
            label: "Reviewer".into(),
            detail: "reviewer · waiting".into(),
            active: false,
        },
    ])));
    sidebar::set_sessions(
        ui,
        vec![SessionRow {
            kind: "thread".into(),
            key: "demo".into(),
            collapsed: false,
            id: "demo".into(),
            host: "Local".into(),
            workspace: "/example/Firmius".into(),
            project: "Firmius".into(),
            title: "Refactor desktop".into(),
            detail: "2 agents · workspace".into(),
        }],
    );
    ui.set_transcript(ModelRc::new(VecModel::from(vec![
        transcript_row("YOU", "Make the desktop a usable workspace with a visible conversation, a proper composer, and inspectable agent work.", "user", "Today · 14:32"),
        transcript_row("FIRMIUS", "I found the layout constraint that compressed the conversation. I’m rebuilding the workspace shell and separating commands from rendering. The transcript now has a stable reading area and the composer stays anchored beneath it.", "assistant", "Lead · coder"),
        transcript_row("EDIT · workspace.slint", "@@ -12,2 +12,3 @@\n- fixed-width: 280px;\n+ horizontal-stretch: 1;\n+ min-width: 0px;", "tool", "Completed · +2 −1"),
        transcript_row("DELEGATE · Reviewer", "Inspect the conversation layout at compact and wide window sizes. Verify that model selection and command completion remain reachable.", "tool", "Running · reviewer"),
    ])));
    let mut fixture = DesktopState::default();
    let agent = |id: &str, label: &str, history: Vec<Message>| firmius_protocol::AgentSnapshot {
        record: firmius_core::AgentRecord {
            id: id.into(),
            provider_id: "fixture".into(),
            model: "Example model".into(),
            effort: None,
            system_prompt: None,
            persona: None,
            temperature: None,
            max_tokens: None,
            workdir: std::path::PathBuf::from("/example/project"),
            label: Some(label.into()),
            metadata: Default::default(),
            history,
            mailbox: vec![],
            active_goal_id: None,
            todo: Default::default(),
            compaction: None,
        },
        usage: Default::default(),
        total_usage: Default::default(),
        busy: false,
        processes: vec![],
        todo: None,
    };
    let snapshot = SessionSnapshot {
        session_id: "demo".into(),
        title: Some("Desktop foundations".into()),
        sequence: 0,
        primary_agent_id: "lead".into(),
        hierarchy: Default::default(),
        work: firmius_core::WorkSnapshot::new("demo", 0, Default::default()),
        active_turns: Default::default(),
        active_delegates: 0,
        live_events: vec![],
        agents: vec![
            agent(
                "lead",
                "Main",
                vec![
                    Message::text(
                        MessageRole::User,
                        "Make each conversation own its composer. Keep my place while tools stream.",
                    ),
                    Message::text(
                        MessageRole::Assistant,
                        "The editor and reading state belong to the conversation view. Incoming updates patch existing rows instead of replacing the transcript.",
                    ),
                ],
            ),
            agent(
                "review",
                "Review",
                vec![
                    Message::text(
                        MessageRole::User,
                        "Check focus changes and submission acknowledgements.",
                    ),
                    Message::text(
                        MessageRole::Assistant,
                        "A send acknowledgement must update its originating view. Switching to another pane must not clear a newer draft or send to a different agent.",
                    ),
                ],
            ),
        ],
    };
    fixture.accept_snapshot(&snapshot);
    fixture.shell.open(
        Route {
            session: Some("demo".into()),
            agent: Some("lead".into()),
            view: View::Conversation,
        },
        "Main".into(),
    );
    fixture
        .shell
        .active_mut()
        .unwrap()
        .composer
        .edit("Main draft stays here".into());
    fixture.shell.split();
    fixture.shell.active_mut().unwrap().route.agent = Some("review".into());
    fixture.shell.active_mut().unwrap().title = "Review".into();
    fixture
        .shell
        .active_mut()
        .unwrap()
        .composer
        .edit("Review has an independent draft".into());
    shell_ui::render(ui, &fixture);
    let scene = std::env::args()
        .find_map(|arg| arg.strip_prefix("--scene=").map(str::to_owned))
        .unwrap_or_default();
    if std::env::args().any(|arg| arg == "--compact") {
        ui.window().set_size(slint::LogicalSize::new(480.0, 700.0));
        ui.set_compact_layout(true);
        ui.set_sidebar_collapsed(true);
    }
    match scene.as_str() {
        "sidebar-open" => {
            ui.window().set_size(slint::LogicalSize::new(1280.0, 850.0));
            ui.set_sidebar_collapsed(false);
            ui.set_compact_layout(false);
        }
        "sidebar-closed" => {
            ui.window().set_size(slint::LogicalSize::new(1280.0, 850.0));
            ui.set_sidebar_collapsed(true);
            ui.set_compact_layout(false);
        }
        "compact" => {
            ui.window().set_size(slint::LogicalSize::new(479.0, 700.0));
            ui.set_compact_layout(true);
            ui.set_sidebar_collapsed(true);
        }
        "split" => {
            ui.window().set_size(slint::LogicalSize::new(1440.0, 900.0));
            ui.set_compact_layout(false);
            ui.set_sidebar_collapsed(false);
        }
        "text-scale" => {
            ui.window().set_size(slint::LogicalSize::new(1280.0, 850.0));
            ui.set_compact_layout(false);
        }
        "minimum" => {
            ui.window().set_size(slint::LogicalSize::new(360.0, 700.0));
            ui.set_compact_layout(true);
            ui.set_sidebar_collapsed(true);
        }
        "wide" => {
            ui.window().set_size(slint::LogicalSize::new(1440.0, 900.0));
            ui.set_compact_layout(false);
            ui.set_sidebar_collapsed(false);
        }
        "welcome" => {
            fixture.shell = crate::shell::Shell::default();
            fixture.shell.open(
                Route {
                    session: None,
                    agent: None,
                    view: View::Conversation,
                },
                "New thread".into(),
            );
            let tab = fixture.shell.active_mut().unwrap();
            tab.composer.provider = "Example account".into();
            tab.composer.model = "Reasoning model".into();
            tab.composer.workspace = "/example/Firmius".into();
            ui.set_model_options(ModelRc::new(VecModel::from(vec![
                ModelOption {
                    provider: "Example account".into(),
                    model: "Reasoning model".into(),
                    detail: "200k context".into(),
                },
                ModelOption {
                    provider: "Example account".into(),
                    model: "Fast model".into(),
                    detail: "128k context".into(),
                },
            ])));
        }
        "models" => {
            ui.set_model_options(ModelRc::new(VecModel::from(vec![
                ModelOption {
                    provider: "Example account".into(),
                    model: "Reasoning model".into(),
                    detail: "200k context · 3 effort modes".into(),
                },
                ModelOption {
                    provider: "Example account".into(),
                    model: "Fast model".into(),
                    detail: "128k context · 2 effort modes".into(),
                },
            ])));
            ui.set_model_draft("Reasoning model".into());
            ui.set_model_provider_draft("Example account".into());
            ui.set_effort_options(ModelRc::new(VecModel::from(vec![
                EffortOption {
                    name: "low".into(),
                    detail: "".into(),
                },
                EffortOption {
                    name: "medium".into(),
                    detail: "".into(),
                },
                EffortOption {
                    name: "high".into(),
                    detail: "".into(),
                },
            ])));
            ui.set_effort_draft("high".into());
            ui.set_model_picker_open(true);
        }
        "commands" => {
            ui.set_command_palette_open(true);
        }
        "gallery" => {
            fixture.shell.open(
                Route {
                    session: None,
                    agent: None,
                    view: View::Feature("gallery".into()),
                },
                "Components".into(),
            );
            shell_ui::render(ui, &fixture);
        }
        "work" => {
            let mut graph = firmius_core::WorkGraph::new(
                "Desktop foundations",
                Some("lead".into()),
                Default::default(),
            );
            for (key, title, status) in [
                (
                    "model",
                    "Track every tool delta",
                    firmius_core::ExecutionStatus::Succeeded,
                ),
                (
                    "views",
                    "Build specialized tool surfaces",
                    firmius_core::ExecutionStatus::Running,
                ),
                (
                    "review",
                    "Review keyboard and split interactions",
                    firmius_core::ExecutionStatus::Pending,
                ),
            ] {
                let mut node = firmius_core::WorkNode::new(key, title);
                node.status = status;
                node.description = Some("Desktop workspace".into());
                graph.view_order.push(node.id);
                graph.nodes.insert(node.id, node);
            }
            for pair in graph.view_order.windows(2) {
                let edge = firmius_core::WorkEdge {
                    id: firmius_core::EdgeId::new(),
                    from: pair[0],
                    to: pair[1],
                    kind: Default::default(),
                    condition: Default::default(),
                    on_outcome: None,
                    required: true,
                    binding: None,
                };
                graph.edges.insert(edge.id, edge);
            }
            fixture
                .snapshots
                .get_mut("demo")
                .unwrap()
                .work
                .state
                .graphs
                .insert(graph.id, graph);
            fixture.shell.open(
                Route {
                    session: Some("demo".into()),
                    agent: Some("lead".into()),
                    view: View::Work,
                },
                "Work".into(),
            );
            shell_ui::render(ui, &fixture);
        }
        "todo" => {
            let snapshot = fixture.snapshots.get_mut("demo").unwrap();
            snapshot.agents[0].busy = true;
            snapshot.agents[0].todo = Some(firmius_protocol::TodoProjectionDto {
                version: 1,
                agent_id: "lead".into(),
                revision: 3,
                pending: 1,
                in_progress: 1,
                blocked: 0,
                completed: 1,
                items: vec![
                    firmius_protocol::TodoItemDto {
                        id: "one".into(),
                        title: "Map the session tree".into(),
                        status: firmius_protocol::TodoStatusDto::Completed,
                        evidence_count: 1,
                        evidence_required: false,
                        waiting_reason: None,
                    },
                    firmius_protocol::TodoItemDto {
                        id: "two".into(),
                        title: "Build the desktop surfaces".into(),
                        status: firmius_protocol::TodoStatusDto::InProgress,
                        evidence_count: 0,
                        evidence_required: false,
                        waiting_reason: None,
                    },
                    firmius_protocol::TodoItemDto {
                        id: "three".into(),
                        title: "Review the interaction flow".into(),
                        status: firmius_protocol::TodoStatusDto::Pending,
                        evidence_count: 0,
                        evidence_required: false,
                        waiting_reason: None,
                    },
                ],
                completion: firmius_protocol::TodoCompletionDto::Waiting {
                    unfinished: 2,
                    blocked: 0,
                    evidence_deficits: 0,
                },
            });
        }
        "vertical" => {
            fixture.shell.split_axis(crate::layout::Axis::Vertical);
            shell_ui::render(ui, &fixture);
        }
        "settings" => {
            fixture.shell.open(
                Route {
                    session: None,
                    agent: None,
                    view: View::Settings,
                },
                "Settings".into(),
            );
            shell_ui::render(ui, &fixture);
        }
        "permission" => {
            ui.set_permission_open(true);
            ui.set_permission_tool("bash · run project checks".into());
            ui.set_permission_detail("cargo test --workspace\nWorkspace: example/project\nAgent: Lead · permission: execute command".into());
        }
        "onboarding" => {
            ui.set_onboarding_open(true);
        }
        _ => {}
    }
    if scene == "scroll" {
        let snapshot = fixture.snapshots.get_mut("demo").unwrap();
        for n in 0..120 {
            snapshot.agents[0].record.history.push(Message::text(MessageRole::Assistant,
                format!("Passage {n}. A stable reading anchor keeps this text visible during updates.\n\nThe row has variable height and can change while the conversation streams.")));
        }
    }
    *state.lock().unwrap() = fixture;
    shell_ui::render(ui, &state.lock().unwrap());
    if scene == "scroll" {
        run_scroll_checks(ui, state.clone(), path.to_owned());
    }
    let weak = ui.as_weak();
    let path = path.to_owned();
    slint::Timer::single_shot(Duration::from_secs(4), move || {
        if let Some(ui) = weak.upgrade() {
            let image = ui.window().take_snapshot().expect("native snapshot");
            let mut ppm = format!("P6\n{} {}\n255\n", image.width(), image.height()).into_bytes();
            for pixel in image.as_bytes().chunks_exact(4) {
                ppm.extend_from_slice(&pixel[..3]);
            }
            fs::write(path, ppm).expect("write preview");
            slint::quit_event_loop().unwrap();
        }
    });
    ui.run()
}

fn run_scroll_checks(ui: &MainWindow, state: Arc<Mutex<DesktopState>>, path: String) {
    let weak = ui.as_weak();
    slint::Timer::single_shot(Duration::from_millis(500), move || {
        let ui = weak.upgrade().unwrap();
        ui.window()
            .dispatch_event(slint::platform::WindowEvent::PointerScrolled {
                position: slint::LogicalPosition::new(450.0, 300.0),
                delta_x: 0.0,
                delta_y: 240.0,
            });
        let weak = ui.as_weak();
        slint::Timer::single_shot(Duration::from_millis(300), move || {
            let ui = weak.upgrade().unwrap();
            let view = ui.get_viewports().row_data(0).unwrap();
            let id = view.tab_id.to_string();
            let before = state
                .lock()
                .unwrap()
                .shell
                .tab(&id)
                .unwrap()
                .reading
                .clone();
            assert!(
                !before.following,
                "native upward wheel must pause following"
            );
            assert!(before.y < 0.0, "long fixture must be scrollable");
            let models = ui.get_viewports();
            {
                let mut s = state.lock().unwrap();
                for _ in 0..10 {
                    shell_ui::render(&ui, &s);
                }
                assert_eq!(models, ui.get_viewports());
                assert_eq!(view.rows, ui.get_viewports().row_data(0).unwrap().rows);
                s.snapshots.get_mut("demo").unwrap().agents[0]
                    .record
                    .history
                    .push(Message::text(
                        MessageRole::Assistant,
                        "New streamed passage",
                    ));
                shell_ui::render(&ui, &s);
            }
            let weak = ui.as_weak();
            slint::Timer::single_shot(Duration::from_millis(400), move || {
                let ui = weak.upgrade().unwrap();
                let after = state
                    .lock()
                    .unwrap()
                    .shell
                    .tab(&id)
                    .unwrap()
                    .reading
                    .clone();
                assert!(!after.following);
                assert!(
                    (before.y - after.y).abs() <= 2.0,
                    "append moved the reading position: {} -> {}",
                    before.y,
                    after.y
                );
                ui.invoke_edit_view_draft(id.clone().into(), "Changed Main only".into());
                let s = state.lock().unwrap();
                assert_eq!(s.shell.tab(&id).unwrap().composer.text, "Changed Main only");
                assert_eq!(
                    s.shell.viewports[1].tabs[0].composer.text,
                    "Review has an independent draft"
                );
                fs::write(format!("{path}.checks"), "PASS native wheel pauses follow\nPASS no-op snapshots retain model identity\nPASS append preserves reading position\nPASS independent pane drafts\n").unwrap();
            });
        });
    });
}
