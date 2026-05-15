#include "audits/WelcomeChatRefreshAudit.hpp"

#include "TUIState.hpp"
#include "harness/Harness.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;
using namespace firmius::tui;

namespace {

constexpr int kScreenWidth = 120;
constexpr int kScreenHeight = 40;

std::string renderFrame(const ftxui::Component &root) {
  auto screen = ftxui::Screen::Create(
      ftxui::Dimension::Fixed(kScreenWidth),
      ftxui::Dimension::Fixed(kScreenHeight));
  // Two passes: FTXUI scroll/frame layouts stabilize after reflect() supplies
  // the viewport box on the second call.
  ftxui::Render(screen, root->Render());
  ftxui::Render(screen, root->Render());
  return screen.ToString();
}

struct FabricatedThread {
  std::string threadId;
  std::string focusedAgentId;
  ThreadMetadata metadata;
  std::string userTurnText;
};

// Fabricates exactly the post-send state the harness leaves behind after a
// welcome-screen first message: a thread, a lead-persona agent manifest, and
// a journal containing the user turn. No provider is called.
FabricatedThread fabricatePostSendThread(const std::string &threadsBase,
                                         const std::string &userText) {
  ThreadManager tm(threadsBase);
  ThreadMetadata meta;
  meta.title = "New Thread";
  meta.leadPersona = "aster";
  const std::string threadId = tm.createThread(meta);

  const std::string agentId = "agent-welcome";
  std::map<std::string, AgentManifestEntry> manifest;
  manifest[agentId] = {"aster", "", "aster", "Lead", true};
  tm.writeAgentManifest(threadId, manifest);

  AgentTurn turn;
  turn.turnId = "turn-0";
  Message user;
  user.id = "u-0";
  user.role = Role::User;
  user.content.push_back(TextContent{userText});
  turn.messages.push_back(std::move(user));

  {
    Journaler journal(threadId, agentId);
    journal.rewriteJournal({turn});
  }

  FabricatedThread out;
  out.threadId = threadId;
  out.focusedAgentId = agentId;
  out.metadata = tm.getMetadata(threadId);
  out.userTurnText = userText;
  return out;
}

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string makeTempHome() {
  std::string tpl = "/tmp/firmius_welcome_chat_XXXXXX";
  char *path = ::strdup(tpl.c_str());
  if (!::mkdtemp(path)) {
    if (path)
      ::free(path);
    return {};
  }
  std::string home(path);
  ::free(path);
  std::filesystem::create_directories(std::filesystem::path(home) /
                                      ".firmius/threads");
  return home;
}

} // namespace

std::string WelcomeChatRefreshAudit::getId() const {
  return "welcome_chat_refresh";
}

std::string WelcomeChatRefreshAudit::getDescription() const {
  return "Regression repro: welcome->chat transition must render the just-"
         "committed user turn and must refresh on agent streaming without a "
         "manual thread rebind.";
}

shared::AuditResult
WelcomeChatRefreshAudit::run(const std::vector<std::string> & /*args*/) {
  AuditResult result;
  result.auditId = getId();
  result.passed = true;

  std::ostringstream log;

  const std::string tempHome = makeTempHome();
  if (tempHome.empty()) {
    result.passed = false;
    result.exitCode = 1;
    result.output = "Failed to create temp FIRMIUS_HOME";
    return result;
  }
  ::setenv("HOME", tempHome.c_str(), 1);

  const std::string threadsBase =
      (std::filesystem::path(tempHome) / ".firmius/threads").string();

  auto &harness = Harness::instance();
  harness.init();

  // --- Phase 1: TuiRunner's no-thread init path (matches runTui:477-480) ----
  auto &state = TuiState::instance();
  ThreadMetadata dummy;
  state.init(harness, dummy, "");
  state.setViewMode(TuiState::ViewMode::Welcome);
  auto root = state.root();

  const std::string frame_welcome = renderFrame(root);
  log << "[welcome] rendered " << frame_welcome.size() << " bytes\n";

  // --- Phase 2: replay the exact production event order ------------------
  // submitPrompt's real-world sequence is:
  //   (a) harness.newThread()  -> emits ThreadChanged synchronously, queued
  //   (b) harness.send()       -> emits UserMessageSent, queued; then spawns
  //                               an async agent jthread that will later emit
  //                               AgentSpawned / AgentText... / AgentTurnCompleted
  //   (c) submitPrompt lookup + applyThreadOpened(metadata, focused, true)
  //   (d) UI returns to event loop; drainEvents consumes (a),(b),(c)'s queued
  //       events + any the agent jthread has produced since.
  //
  // A faithful repro must dispatch (a) and (b) through handleAppEvent (same
  // path drainEvents uses) BEFORE calling applyThreadOpened, and then keep
  // dispatching streaming events between render calls so we observe whether
  // mid-stream renders actually reflect newly-arrived chunks.
  const std::string userText = "USERTURN_MARKER_HELLO";
  FabricatedThread fab = fabricatePostSendThread(threadsBase, userText);

  // (a) ThreadChanged — what newThread fires inside submitPrompt.
  state.handleAppEvent(AppEvent(ThreadChanged{fab.threadId, fab.metadata}));

  // (b) UserMessageSent — what send fires synchronously before returning.
  UserMessageSent ums;
  ums.messageId = "msg-welcome-0";
  ums.text = userText;
  ums.threadId = fab.threadId;
  state.handleAppEvent(AppEvent(ums));

  // (c) submitPrompt post-send rebind.
  state.applyThreadOpened(fab.metadata, fab.focusedAgentId, true);

  const std::string frame_after_apply = renderFrame(root);
  log << "[after applyThreadOpened] rendered " << frame_after_apply.size()
      << " bytes, view_mode=Chat="
      << (state.getViewMode() == TuiState::ViewMode::Chat) << "\n";

  // --- Phase 3: AgentSpawned + multi-chunk streaming ---------------------
  AgentSpawned spawned;
  spawned.agentId = fab.focusedAgentId;
  spawned.parentId.clear();
  spawned.friendlyName = "aster";
  spawned.title = "Lead";
  spawned.persistHistory = true;
  state.handleAppEvent(AppEvent(spawned));

  // Three deltas, render between each. In production the agent jthread emits
  // these asynchronously; the UI must repaint between each chunk via Custom
  // event -> drainEvents -> handleAppEvent -> requestRefresh(ChatTranscript).
  // Using handleAppEvent directly exercises the same handler chain drainEvents
  // invokes, so any refresh-invalidation bug surfaces on the next Render().
  const std::string delta1 = "STREAMCHUNK_ALPHA";
  const std::string delta2 = "STREAMCHUNK_BETA";
  const std::string delta3 = "STREAMCHUNK_GAMMA";

  AgentText txt1;
  txt1.agentId = fab.focusedAgentId;
  txt1.delta = delta1;
  state.handleAppEvent(AppEvent(txt1));
  const std::string frame_stream1 = renderFrame(root);

  AgentText txt2;
  txt2.agentId = fab.focusedAgentId;
  txt2.delta = delta2;
  state.handleAppEvent(AppEvent(txt2));
  const std::string frame_stream2 = renderFrame(root);

  AgentText txt3;
  txt3.agentId = fab.focusedAgentId;
  txt3.delta = delta3;
  state.handleAppEvent(AppEvent(txt3));
  const std::string frame_stream3 = renderFrame(root);

  log << "[stream frames] " << frame_stream1.size() << "/"
      << frame_stream2.size() << "/" << frame_stream3.size() << " bytes\n";

  // --- Assertions --------------------------------------------------------
  auto fail = [&](const std::string &why) {
    result.passed = false;
    log << "FAIL: " << why << "\n";
  };

  if (contains(frame_welcome, userText)) {
    fail("welcome frame unexpectedly contains user turn text");
  }
  if (contains(frame_welcome, delta1)) {
    fail("welcome frame unexpectedly contains streaming delta");
  }
  if (!contains(frame_after_apply, userText)) {
    fail("post-apply frame MISSING user turn '" + userText +
         "' — history/live optimistic append failed");
  }
  // First chunk must appear mid-stream.
  if (!contains(frame_stream1, delta1)) {
    fail("frame_stream1 MISSING delta1 '" + delta1 +
         "' — first streaming chunk does not reach the render");
  }
  // Later chunks must REPLACE or EXTEND the live row, not disappear.
  if (!contains(frame_stream2, delta2)) {
    fail("frame_stream2 MISSING delta2 '" + delta2 +
         "' — subsequent chunks do not refresh the live transcript");
  }
  if (!contains(frame_stream3, delta3)) {
    fail("frame_stream3 MISSING delta3 '" + delta3 +
         "' — live transcript stopped refreshing after second chunk");
  }
  // User turn must remain visible throughout streaming.
  if (!contains(frame_stream3, userText)) {
    fail("frame_stream3 MISSING user turn — user message was dropped during "
         "streaming");
  }

  // Pin frames to the log on failure for easy diffing in CI output.
  if (!result.passed) {
    log << "\n--- frame_welcome ---\n" << frame_welcome;
    log << "\n--- frame_after_apply ---\n" << frame_after_apply;
    log << "\n--- frame_stream1 ---\n" << frame_stream1;
    log << "\n--- frame_stream2 ---\n" << frame_stream2;
    log << "\n--- frame_stream3 ---\n" << frame_stream3;
  }

  state.shutdown();
  harness.shutdown();

  std::error_code ec;
  std::filesystem::remove_all(tempHome, ec);

  result.output = log.str();
  if (!result.passed) {
    result.exitCode = 1;
  }
  return result;
}

} // namespace firmius::audits
