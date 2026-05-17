#pragma once

#include "AppState.hpp"
#include "DaemonSession.hpp"

#include <string>
#include <vector>

namespace firmius::tui2 {

/// Translates user intents into daemon RPCs and state updates.
class ActionDispatcher {
public:
  ActionDispatcher(DaemonSession &session, AppState &state);

  /// Send a chat message to the current thread/agent.
  bool sendMessage(const std::string &text);

  /// Create a new thread and open it.
  bool createThread(const std::string &persona = "lead",
                    const std::string &mode = "");

  /// Open an existing thread by ID.
  bool openThread(const std::string &threadId);

  /// Interrupt the currently active agent.
  bool interruptAgent(bool flushQueuedMessages = false);

  /// Resolve a pending permission request.
  bool resolvePermission(const std::string &requestId,
                         firmius::shared::PermissionResponse response);

  /// Load transcript snapshot for the current thread into state.
  void loadTranscript();
  void loadTranscriptForAgent(const std::string &agentId, bool replace = false);

  /// Execute a workflow by ID. Creates a thread if none exists.
  bool executeWorkflow(const std::string &workflowId,
                       const std::vector<std::string> &args = {});

private:
  DaemonSession &session_;
  AppState &state_;
};

} // namespace firmius::tui2
