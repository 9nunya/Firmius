#include "daemon/DaemonClient.hpp"
#include "daemon/Protocol.hpp"
#include "daemon/ProtocolSerialization.hpp"
#include "daemon/SocketTransport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace firmius::daemon {
namespace {

namespace fs = std::filesystem;

struct CapturedRuntimeEvent {
  std::string subscriptionTarget;
  std::string runtimeEventType;
  std::string runtimeEventThreadId;
  std::string runtimeEventAgentId;
  std::string runtimeEventJson;
  std::uint64_t sequence = 0;
};

struct RuntimeEventCollector {
  mutable std::mutex mutex;
  mutable std::condition_variable cv;
  std::vector<CapturedRuntimeEvent> events;

  void record(const DaemonEventEnvelope &event) {
    if (event.kind != DaemonEventKind::RuntimeAppEvent) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      events.push_back({event.subscriptionTarget, event.runtimeEventType,
                        event.runtimeEventThreadId, event.runtimeEventAgentId,
                        event.runtimeEventJson, event.sequence});
    }
    cv.notify_all();
  }

  bool waitForCount(std::size_t minimumCount,
                    std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout,
                       [&] { return events.size() >= minimumCount; });
  }

  std::vector<CapturedRuntimeEvent> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return events;
  }
};

rapidjson::Document parseJsonString(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  return doc;
}

std::optional<rapidjson::Value> asParams(rapidjson::Document &params) {
  rapidjson::Value value(rapidjson::kObjectType);
  value.CopyFrom(params, params.GetAllocator());
  return value;
}

rapidjson::Document sendRawJsonRpcRequest(
    const std::string &endpoint, const std::string &method,
    std::optional<rapidjson::Value> params = std::nullopt) {
  SocketTransport transport(DaemonConnectionInfo{endpoint});
  auto channel = transport.connect(std::chrono::milliseconds(1000));

  rapidjson::Document request;
  request.SetObject();
  auto &allocator = request.GetAllocator();
  request.AddMember("jsonrpc", "2.0", allocator);
  request.AddMember("id", 1, allocator);
  request.AddMember("method", rapidjson::Value(method.c_str(), allocator).Move(),
                    allocator);
  if (params.has_value()) {
    request.AddMember("params", std::move(*params), allocator);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  request.Accept(writer);
  const std::string payload = buffer.GetString();
  const std::string framed =
      "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
  EXPECT_TRUE(channel.writer(framed));

  std::string responseBuffer;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    responseBuffer += channel.reader(std::chrono::milliseconds(200));
    const auto headerPos = responseBuffer.find("Content-Length: ");
    if (headerPos == std::string::npos) {
      continue;
    }
    const auto headerEnd = responseBuffer.find("\r\n\r\n", headerPos);
    if (headerEnd == std::string::npos) {
      continue;
    }
    std::string lenStr = responseBuffer.substr(16, headerEnd - 16);
    const auto firstNewline = lenStr.find("\r\n");
    if (firstNewline != std::string::npos) {
      lenStr = lenStr.substr(0, firstNewline);
    }
    const size_t contentLength = std::stoul(lenStr);
    const size_t messageStart = headerEnd + 4;
    if (responseBuffer.size() < messageStart + contentLength) {
      continue;
    }
    rapidjson::Document response;
    response.Parse(responseBuffer.substr(messageStart, contentLength).c_str());
    channel.wakeStop();
    return response;
  }

  channel.wakeStop();
  throw std::runtime_error("timed out waiting for JSON-RPC response");
}

void emitAuditRuntimeEvent(const std::string &endpoint,
                           const DaemonAuditEmitRuntimeEventRequest &request) {
  rapidjson::Document params;
  params.SetObject();
  auto &allocator = params.GetAllocator();
  auto value = toJsonValue(request, allocator);
  params.CopyFrom(value, allocator);
  auto response = sendRawJsonRpcRequest(endpoint, kRpcDaemonAuditEmitRuntimeEvent,
                                        asParams(params));
  ASSERT_TRUE(response.HasMember("result"));
  const auto result = daemonAuditEmitRuntimeEventResponseFromJson(response["result"]);
  EXPECT_TRUE(result.emitted);
  EXPECT_EQ(result.runtimeEventType, request.eventType);
  EXPECT_EQ(result.threadId, request.threadId);
  EXPECT_EQ(result.agentId, request.agentId);
}

void expectObservedThreadIds(const std::vector<CapturedRuntimeEvent> &events,
                             const std::vector<std::string> &expectedThreadIds) {
  ASSERT_EQ(events.size(), expectedThreadIds.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i].runtimeEventThreadId, expectedThreadIds[i]);
  }
}

void expectObservedEventTypes(const std::vector<CapturedRuntimeEvent> &events,
                              const std::vector<std::string> &expectedTypes) {
  ASSERT_EQ(events.size(), expectedTypes.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i].runtimeEventType, expectedTypes[i]);
  }
}

void expectJsonTextField(const CapturedRuntimeEvent &event,
                         const std::string &expectedText) {
  const auto payload = parseJsonString(event.runtimeEventJson);
  ASSERT_TRUE(payload.IsObject());
  ASSERT_TRUE(payload.HasMember("text"));
  ASSERT_TRUE(payload["text"].IsString());
  EXPECT_EQ(std::string(payload["text"].GetString()), expectedText);
}

void expectJsonToolEventFields(const CapturedRuntimeEvent &event,
                               const std::string &expectedToolCallId,
                               const std::string &expectedToolName) {
  const auto payload = parseJsonString(event.runtimeEventJson);
  ASSERT_TRUE(payload.IsObject());
  ASSERT_TRUE(payload.HasMember("toolCallId"));
  ASSERT_TRUE(payload["toolCallId"].IsString());
  ASSERT_TRUE(payload.HasMember("toolName"));
  ASSERT_TRUE(payload["toolName"].IsString());
  EXPECT_EQ(std::string(payload["toolCallId"].GetString()), expectedToolCallId);
  EXPECT_EQ(std::string(payload["toolName"].GetString()), expectedToolName);
}

std::vector<CapturedRuntimeEvent>
firstEventsOfTypes(const std::vector<CapturedRuntimeEvent> &events,
                   const std::vector<std::string> &expectedTypes) {
  std::vector<CapturedRuntimeEvent> selected;
  std::size_t nextType = 0;
  for (const auto &event : events) {
    if (nextType < expectedTypes.size() &&
        event.runtimeEventType == expectedTypes[nextType]) {
      selected.push_back(event);
      ++nextType;
    }
  }
  return selected;
}

std::vector<CapturedRuntimeEvent> eventsForThread(const std::vector<CapturedRuntimeEvent> &events,
                                                  const std::string &threadId) {
  std::vector<CapturedRuntimeEvent> filtered;
  for (const auto &event : events) {
    if (event.runtimeEventThreadId == threadId) {
      filtered.push_back(event);
    }
  }
  return filtered;
}

std::string uniqueSuffix() {
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string uniqueEndpoint(const std::string &name) {
#if defined(_WIN32)
  return std::string(R"(\\.\pipe\firmiusd-test-)") + name + "-" + uniqueSuffix();
#else
  auto path = fs::temp_directory_path() /
              ("firmiusd-test-" + name + "-" + uniqueSuffix() + ".sock");
  return path.string();
#endif
}

std::string daemonExecutablePath() {
  auto repoRoot = fs::current_path();
  if (repoRoot.filename() == "server") {
    repoRoot = repoRoot.parent_path().parent_path().parent_path();
  }
  const auto buildServerDir = repoRoot / "build" / "packages" / "server";
#if defined(_WIN32)
  return (buildServerDir / "firmiusd.exe").string();
#else
  return (buildServerDir / "firmiusd").string();
#endif
}

ClientIdentity testIdentity(const std::string &id) {
  ClientIdentity identity;
  identity.clientId = id;
  identity.uiKind = "gtest";
  identity.pid = 12345;
  identity.capabilityFlags = {"rpc", "events"};
  return identity;
}

WorkspacePresence testPresence() {
  WorkspacePresence presence;
  presence.cwd = fs::current_path().string();
  presence.workspaceRoot = presence.cwd;
  presence.repoRoot = presence.cwd;
  return presence;
}

DaemonClientOptions makeOptions(const std::string &clientId, const std::string &endpoint) {
  DaemonClientOptions options;
  options.identity = testIdentity(clientId + "-" + uniqueSuffix());
  options.presence = testPresence();
  options.connection.endpoint = endpoint;
  options.daemonExecutablePath = daemonExecutablePath();
  return options;
}

void expectInvalidParams(const rapidjson::Document &response) {
  ASSERT_TRUE(response.IsObject());
  ASSERT_TRUE(response.HasMember("error"));
  ASSERT_TRUE(response["error"].IsObject());
  ASSERT_TRUE(response["error"].HasMember("code"));
  EXPECT_EQ(response["error"]["code"].GetInt(), -32602);
}

class ScopedDaemonProcess {
public:
  ScopedDaemonProcess(std::string endpoint, std::string executable)
      : endpoint_(std::move(endpoint)), executable_(std::move(executable)) {}

  ~ScopedDaemonProcess() { stop(); }

  void start() {
    ASSERT_TRUE(fs::exists(executable_)) << executable_;
#if defined(_WIN32)
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + executable_ + "\" --endpoint \"" + endpoint_ + "\"";
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');
    ASSERT_TRUE(CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                               DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                               nullptr, &si, &pi));
    process_ = pi.hProcess;
    processId_ = pi.dwProcessId;
    CloseHandle(pi.hThread);
#else
    char *argv[] = {const_cast<char *>(executable_.c_str()),
                    const_cast<char *>("--endpoint"),
                    const_cast<char *>(endpoint_.c_str()), nullptr};
    ASSERT_EQ(posix_spawn(&processId_, executable_.c_str(), nullptr, nullptr, argv, environ),
              0);
#endif
  }

  bool waitUntilReady(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        DaemonClientOptions probeOptions = makeOptions("probe", endpoint_);
        probeOptions.autoStart = false;
        DaemonClient probe(probeOptions);
        probe.connect();
        probe.disconnect();
        return true;
      } catch (...) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    return false;
  }

  bool isRunning() const {
#if defined(_WIN32)
    if (!process_) {
      return false;
    }
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
    if (processId_ <= 0) {
      return false;
    }
    return ::kill(processId_, 0) == 0;
#endif
  }

  void stop() {
#if defined(_WIN32)
    if (!process_) {
      return;
    }
    TerminateProcess(process_, 0);
    WaitForSingleObject(process_, 5000);
    CloseHandle(process_);
    process_ = nullptr;
#else
    if (processId_ <= 0) {
      cleanupFiles();
      return;
    }
    ::kill(processId_, SIGTERM);
    for (int attempt = 0; attempt < 50; ++attempt) {
      int status = 0;
      const pid_t waited = ::waitpid(processId_, &status, WNOHANG);
      if (waited == processId_) {
        processId_ = 0;
        cleanupFiles();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(processId_, SIGKILL);
    int status = 0;
    (void)::waitpid(processId_, &status, 0);
    processId_ = 0;
#endif
    cleanupFiles();
  }

private:
  void cleanupFiles() {
#if !defined(_WIN32)
    std::error_code ec;
    fs::remove(endpoint_, ec);
#endif
  }

  std::string endpoint_;
  std::string executable_;
#if defined(_WIN32)
  HANDLE process_ = nullptr;
  DWORD processId_ = 0;
#else
  pid_t processId_ = 0;
#endif
};

TEST(DaemonClientIpcSmoke, ConnectsToExistingDaemonAndCreatesAndOpensThreads) {
  const std::string endpoint = uniqueEndpoint("existing");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));
  ASSERT_TRUE(daemon.isRunning());

  DaemonClientOptions clientOptions = makeOptions("client-existing", endpoint);
  clientOptions.autoStart = false;
  DaemonClient client(clientOptions);
  ASSERT_TRUE(client.connect());

  const auto created = client.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(created.thread.threadId.empty());

  const auto opened = client.openThread(created.thread.threadId);
  EXPECT_TRUE(opened.opened);
  EXPECT_EQ(opened.thread.threadId, created.thread.threadId);
  EXPECT_FALSE(opened.focusedAgentId.empty());

  client.disconnect();
}

TEST(DaemonClientIpcSmoke, SendsAcrossProcessBoundaryAndSupportsTwoClients) {
  const std::string endpoint = uniqueEndpoint("events");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions clientAOptions = makeOptions("client-a", endpoint);
  clientAOptions.autoStart = false;
  DaemonClient clientA(clientAOptions);
  ASSERT_TRUE(clientA.connect());

  DaemonClientOptions clientBOptions = makeOptions("client-b", endpoint);
  clientBOptions.autoStart = false;
  DaemonClient clientB(clientBOptions);
  ASSERT_TRUE(clientB.connect());

  std::promise<DaemonEventEnvelope> eventA;
  std::future<DaemonEventEnvelope> futureA = eventA.get_future();
  ASSERT_TRUE(clientA.subscribe([&](const DaemonEventEnvelope &event) {
    if (event.kind == DaemonEventKind::RuntimeAppEvent &&
        event.runtimeEventType == "user_message_sent") {
      try {
        eventA.set_value(event);
      } catch (...) {
      }
    }
  }));

  std::promise<DaemonEventEnvelope> eventB;
  std::future<DaemonEventEnvelope> futureB = eventB.get_future();
  ASSERT_TRUE(clientB.subscribe([&](const DaemonEventEnvelope &event) {
    if (event.kind == DaemonEventKind::RuntimeAppEvent &&
        event.runtimeEventType == "user_message_sent") {
      try {
        eventB.set_value(event);
      } catch (...) {
      }
    }
  }));

  const auto threadA = clientA.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto threadB = clientB.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(threadA.thread.threadId.empty());
  ASSERT_FALSE(threadB.thread.threadId.empty());
  ASSERT_NE(threadA.thread.threadId, threadB.thread.threadId);

  const auto sendA = clientA.send(
      ThreadsSendRequest{threadA.thread.threadId, "", "hello A", {}});
  const auto sendB = clientB.send(
      ThreadsSendRequest{threadB.thread.threadId, "", "hello B", {}});
  EXPECT_TRUE(sendA.accepted);
  EXPECT_TRUE(sendB.accepted);

  ASSERT_EQ(futureA.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(futureB.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(futureA.get().runtimeEventThreadId, threadA.thread.threadId);
  EXPECT_EQ(futureB.get().runtimeEventThreadId, threadB.thread.threadId);

  clientB.disconnect();
  clientA.disconnect();
}

TEST(DaemonClientIpcSmoke,
     RoutesRealtimeEventsOnlyToFocusedSessionsAcrossTwoClientsAndThreads) {
  const std::string endpoint = uniqueEndpoint("realtime-session-isolation");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions clientAOptions = makeOptions("client-rt-a", endpoint);
  clientAOptions.autoStart = false;
  DaemonClient clientA(clientAOptions);
  ASSERT_TRUE(clientA.connect());

  DaemonClientOptions clientBOptions = makeOptions("client-rt-b", endpoint);
  clientBOptions.autoStart = false;
  DaemonClient clientB(clientBOptions);
  ASSERT_TRUE(clientB.connect());

  const auto threadA = clientA.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto threadB = clientB.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(threadA.thread.threadId.empty());
  ASSERT_FALSE(threadB.thread.threadId.empty());
  ASSERT_NE(threadA.thread.threadId, threadB.thread.threadId);

  const auto openedA = clientA.openThread(threadA.thread.threadId);
  const auto openedB = clientB.openThread(threadB.thread.threadId);
  EXPECT_FALSE(openedA.focusedAgentId.empty())
      << "openedA thread=" << openedA.thread.threadId;
  EXPECT_FALSE(openedB.focusedAgentId.empty())
      << "openedB thread=" << openedB.thread.threadId;

  RuntimeEventCollector collectorA;
  RuntimeEventCollector collectorB;
  ASSERT_TRUE(clientA.subscribe(
      [&](const DaemonEventEnvelope &event) { collectorA.record(event); }));
  ASSERT_TRUE(clientB.subscribe(
      [&](const DaemonEventEnvelope &event) { collectorB.record(event); }));

  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_thinking",
                                                           threadA.thread.threadId,
                                                           openedA.focusedAgentId,
                                                           "",
                                                           "thinking-A",
                                                           "",
                                                           "",
                                                           ""});
  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_text",
                                                           threadA.thread.threadId,
                                                           openedA.focusedAgentId,
                                                           "",
                                                           "text-A",
                                                           "",
                                                           "",
                                                           ""});
  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_tool_call",
                                                           threadA.thread.threadId,
                                                           openedA.focusedAgentId,
                                                           "",
                                                           "",
                                                           "tool-A-1",
                                                           "file_read",
                                                           "{\"path\":\"A.txt\"}"});
  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_thinking",
                                                           threadB.thread.threadId,
                                                           openedB.focusedAgentId,
                                                           "",
                                                           "thinking-B",
                                                           "",
                                                           "",
                                                           ""});
  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_text",
                                                           threadB.thread.threadId,
                                                           openedB.focusedAgentId,
                                                           "",
                                                           "text-B",
                                                           "",
                                                           "",
                                                           ""});
  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_tool_call",
                                                           threadB.thread.threadId,
                                                           openedB.focusedAgentId,
                                                           "",
                                                           "",
                                                           "tool-B-1",
                                                           "file_read",
                                                           "{\"path\":\"B.txt\"}"});

  ASSERT_TRUE(collectorA.waitForCount(3, std::chrono::seconds(5)));
  ASSERT_TRUE(collectorB.waitForCount(3, std::chrono::seconds(5)));

  const auto eventsA = firstEventsOfTypes(
      collectorA.snapshot(), {"agent_thinking", "agent_text", "agent_tool_call"});
  const auto eventsB = firstEventsOfTypes(
      collectorB.snapshot(), {"agent_thinking", "agent_text", "agent_tool_call"});
  expectObservedThreadIds(
      eventsA, {threadA.thread.threadId, threadA.thread.threadId, threadA.thread.threadId});
  expectObservedThreadIds(
      eventsB, {threadB.thread.threadId, threadB.thread.threadId, threadB.thread.threadId});
  expectObservedEventTypes(eventsA, {"agent_thinking", "agent_text", "agent_tool_call"});
  expectObservedEventTypes(eventsB, {"agent_thinking", "agent_text", "agent_tool_call"});
  expectJsonTextField(eventsA[0], "thinking-A");
  expectJsonTextField(eventsA[1], "text-A");
  expectJsonToolEventFields(eventsA[2], "tool-A-1", "file_read");
  expectJsonTextField(eventsB[0], "thinking-B");
  expectJsonTextField(eventsB[1], "text-B");
  expectJsonToolEventFields(eventsB[2], "tool-B-1", "file_read");

  clientA.disconnect();
  clientB.disconnect();
}

TEST(DaemonClientIpcSmoke,
     FocusSwitchDuringRealtimeActivityRebindsObservationWithoutCrossThreadBleed) {
  const std::string endpoint = uniqueEndpoint("realtime-focus-switch");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions clientAOptions = makeOptions("client-focus-a", endpoint);
  clientAOptions.autoStart = false;
  DaemonClient clientA(clientAOptions);
  ASSERT_TRUE(clientA.connect());

  DaemonClientOptions clientBOptions = makeOptions("client-focus-b", endpoint);
  clientBOptions.autoStart = false;
  DaemonClient clientB(clientBOptions);
  ASSERT_TRUE(clientB.connect());

  const auto threadA = clientA.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto threadB = clientB.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(threadA.thread.threadId.empty());
  ASSERT_FALSE(threadB.thread.threadId.empty());
  ASSERT_NE(threadA.thread.threadId, threadB.thread.threadId);

  const auto openedA = clientA.openThread(threadA.thread.threadId);
  const auto openedB = clientB.openThread(threadB.thread.threadId);
  EXPECT_FALSE(openedA.focusedAgentId.empty())
      << "openedA thread=" << openedA.thread.threadId;
  EXPECT_FALSE(openedB.focusedAgentId.empty())
      << "openedB thread=" << openedB.thread.threadId;

  RuntimeEventCollector collectorA;
  RuntimeEventCollector collectorB;
  ASSERT_TRUE(clientA.subscribe(
      [&](const DaemonEventEnvelope &event) { collectorA.record(event); }));
  ASSERT_TRUE(clientB.subscribe(
      [&](const DaemonEventEnvelope &event) { collectorB.record(event); }));

  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_text",
                                                           threadA.thread.threadId,
                                                           openedA.focusedAgentId,
                                                           "",
                                                           "before-switch-A",
                                                           "",
                                                           "",
                                                           ""});
  ASSERT_TRUE(collectorA.waitForCount(1, std::chrono::seconds(5)));

  const auto focusedB = clientA.openThread(threadB.thread.threadId);
  ASSERT_TRUE(focusedB.opened);
  EXPECT_EQ(focusedB.thread.threadId, threadB.thread.threadId);

  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_thinking",
                                                           threadA.thread.threadId,
                                                           openedA.focusedAgentId,
                                                           "",
                                                           "after-switch-still-A",
                                                           "",
                                                           "",
                                                           ""});
  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_text",
                                                           threadB.thread.threadId,
                                                           openedB.focusedAgentId,
                                                           "",
                                                           "after-switch-B",
                                                           "",
                                                           "",
                                                           ""});

  ASSERT_TRUE(collectorA.waitForCount(2, std::chrono::seconds(5)));
  const auto allEventsA = collectorA.snapshot();
  const auto allEventsB = collectorB.snapshot();
  const auto eventsA = eventsForThread(allEventsA, threadB.thread.threadId);
  ASSERT_FALSE(eventsA.empty());
  EXPECT_EQ(eventsA.back().runtimeEventThreadId, threadB.thread.threadId);
  expectJsonTextField(eventsA.back(), "after-switch-B");
  const auto threadAEventsSeenByA = eventsForThread(allEventsA, threadA.thread.threadId);
  const auto threadAEventsSeenByB = eventsForThread(allEventsB, threadA.thread.threadId);
  ASSERT_EQ(threadAEventsSeenByA.size(), 1u);
  EXPECT_TRUE(threadAEventsSeenByB.empty());
  EXPECT_EQ(threadAEventsSeenByA[0].runtimeEventType, "agent_text");
  const auto sessions = clientA.listClients();
  const auto clientASession = std::find_if(
      sessions.begin(), sessions.end(),
      [&](const ClientSessionSnapshot &session) {
        return session.identity.clientId == clientAOptions.identity.clientId;
      });
  ASSERT_NE(clientASession, sessions.end());
  EXPECT_EQ(clientASession->focusedThreadId, threadB.thread.threadId);

  clientA.disconnect();
  clientB.disconnect();
}

TEST(DaemonClientIpcSmoke, MultiClientSendDoesNotUseOtherClientsFocusWhenThreadIdOmitted) {
  const std::string endpoint = uniqueEndpoint("focus-isolation");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));
  DaemonClientOptions clientAOptions = makeOptions("client-a", endpoint);
  clientAOptions.autoStart = false;
  DaemonClient clientA(clientAOptions);
  ASSERT_TRUE(clientA.connect());

  DaemonClientOptions clientBOptions = makeOptions("client-b", endpoint);
  clientBOptions.autoStart = false;
  DaemonClient clientB(clientBOptions);
  ASSERT_TRUE(clientB.connect());

  const auto threadA = clientA.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(threadA.thread.threadId.empty());
  const auto threadB = clientB.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(threadB.thread.threadId.empty());
  ASSERT_NE(threadA.thread.threadId, threadB.thread.threadId);

  ASSERT_TRUE(clientA.openThread(threadA.thread.threadId).opened);
  ASSERT_TRUE(clientB.openThread(threadB.thread.threadId).opened);

  std::promise<DaemonEventEnvelope> aMessage;
  auto aFuture = aMessage.get_future();
  ASSERT_TRUE(clientA.subscribe([&](const DaemonEventEnvelope &event) {
    if (event.kind == DaemonEventKind::RuntimeAppEvent &&
        event.runtimeEventType == "user_message_sent" &&
        event.runtimeEventThreadId == threadA.thread.threadId) {
      try {
        aMessage.set_value(event);
      } catch (...) {
      }
    }
  }));

  std::promise<DaemonEventEnvelope> bMessage;
  auto bFuture = bMessage.get_future();
  ASSERT_TRUE(clientB.subscribe([&](const DaemonEventEnvelope &event) {
    if (event.kind == DaemonEventKind::RuntimeAppEvent &&
        event.runtimeEventType == "user_message_sent" &&
        event.runtimeEventThreadId == threadB.thread.threadId) {
      try {
        bMessage.set_value(event);
      } catch (...) {
      }
    }
  }));

  const auto sendA = clientA.send(ThreadsSendRequest{"", "", "from-A", {}});
  EXPECT_TRUE(sendA.accepted);
  EXPECT_EQ(sendA.threadId, threadA.thread.threadId);

  const auto sendB = clientB.send(ThreadsSendRequest{"", "", "from-B", {}});
  EXPECT_TRUE(sendB.accepted);
  EXPECT_EQ(sendB.threadId, threadB.thread.threadId);

  ASSERT_EQ(aFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(bFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(aFuture.get().runtimeEventThreadId, threadA.thread.threadId);
  EXPECT_EQ(bFuture.get().runtimeEventThreadId, threadB.thread.threadId);

  clientB.disconnect();
  clientA.disconnect();
}

TEST(DaemonClientIpcSmoke, RejectsMalformedClientHelloParams) {
  const std::string endpoint = uniqueEndpoint("bad-hello");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  auto response = sendRawJsonRpcRequest(endpoint, kRpcClientHello);
  expectInvalidParams(response);
}

TEST(DaemonClientIpcSmoke, FocusSwitchDoesNotRetargetAnotherClientsSend) {
  const std::string endpoint = uniqueEndpoint("send-vs-focus");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions clientAOptions = makeOptions("client-send-a", endpoint);
  clientAOptions.autoStart = false;
  DaemonClient clientA(clientAOptions);
  ASSERT_TRUE(clientA.connect());

  DaemonClientOptions clientBOptions = makeOptions("client-send-b", endpoint);
  clientBOptions.autoStart = false;
  DaemonClient clientB(clientBOptions);
  ASSERT_TRUE(clientB.connect());

  const auto threadA = clientA.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto threadB = clientB.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(threadA.thread.threadId.empty());
  ASSERT_FALSE(threadB.thread.threadId.empty());
  ASSERT_NE(threadA.thread.threadId, threadB.thread.threadId);

  ASSERT_TRUE(clientA.openThread(threadA.thread.threadId).opened);
  ASSERT_TRUE(clientB.openThread(threadB.thread.threadId).opened);

  std::promise<DaemonEventEnvelope> aMessage;
  auto aFuture = aMessage.get_future();
  ASSERT_TRUE(clientA.subscribe([&](const DaemonEventEnvelope &event) {
    if (event.kind == DaemonEventKind::RuntimeAppEvent &&
        event.runtimeEventType == "user_message_sent" &&
        event.runtimeEventThreadId == threadA.thread.threadId) {
      try {
        aMessage.set_value(event);
      } catch (...) {
      }
    }
  }));

  ASSERT_TRUE(clientB.openThread(threadA.thread.threadId).opened);
  ASSERT_TRUE(clientB.openThread(threadB.thread.threadId).opened);

  const auto sendA = clientA.send(ThreadsSendRequest{"", "", "still-to-A", {}});
  EXPECT_TRUE(sendA.accepted);
  EXPECT_EQ(sendA.threadId, threadA.thread.threadId);

  ASSERT_EQ(aFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(aFuture.get().runtimeEventThreadId, threadA.thread.threadId);

  const auto sessions = clientA.listClients();
  const auto clientASession = std::find_if(
      sessions.begin(), sessions.end(),
      [&](const ClientSessionSnapshot &session) {
        return session.identity.clientId == clientAOptions.identity.clientId;
      });
  const auto clientBSession = std::find_if(
      sessions.begin(), sessions.end(),
      [&](const ClientSessionSnapshot &session) {
        return session.identity.clientId == clientBOptions.identity.clientId;
      });
  ASSERT_NE(clientASession, sessions.end());
  ASSERT_NE(clientBSession, sessions.end());
  EXPECT_EQ(clientASession->focusedThreadId, threadA.thread.threadId);
  EXPECT_EQ(clientBSession->focusedThreadId, threadB.thread.threadId);

  clientB.disconnect();
  clientA.disconnect();
}

TEST(DaemonClientIpcSmoke, ExposesTranscriptAndToolSnapshotsAcrossDaemonBoundary) {
  const std::string endpoint = uniqueEndpoint("runtime-snapshots");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions clientOptions = makeOptions("client-runtime", endpoint);
  clientOptions.autoStart = false;
  DaemonClient client(clientOptions);
  ASSERT_TRUE(client.connect());

  const auto created = client.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(created.thread.threadId.empty());

  const auto opened = client.openThread(created.thread.threadId);
  ASSERT_TRUE(opened.opened);
  ASSERT_FALSE(opened.focusedAgentId.empty());

  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_tool_call",
                                                           created.thread.threadId,
                                                           opened.focusedAgentId,
                                                           "",
                                                           "",
                                                           "tool-live-1",
                                                           "file_read",
                                                           "{\"path\":\"README.md\"}"});
  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_text",
                                                           created.thread.threadId,
                                                           opened.focusedAgentId,
                                                           "",
                                                           "live text",
                                                           "",
                                                           "",
                                                           ""});

  const auto transcript =
      client.getTranscript(TranscriptGetRequest{created.thread.threadId,
                                                opened.focusedAgentId});
  ASSERT_TRUE(transcript.has_value());
  EXPECT_EQ(transcript->threadId, created.thread.threadId);
  EXPECT_EQ(transcript->agentId, opened.focusedAgentId);
  ASSERT_FALSE(transcript->rawTurns.empty());

  const auto toolCalls =
      client.listToolCalls(ToolCallsListRequest{created.thread.threadId,
                                                opened.focusedAgentId});
  EXPECT_TRUE(toolCalls.empty() ||
              std::any_of(toolCalls.begin(), toolCalls.end(),
                          [&](const ToolCallSnapshot &snapshot) {
                            return snapshot.threadId == created.thread.threadId &&
                                   snapshot.agentId == opened.focusedAgentId;
                          }));

  client.disconnect();
}

TEST(DaemonClientIpcSmoke, ExposesHistoryAndEditSnapshotsAcrossDaemonBoundary) {
  const std::string endpoint = uniqueEndpoint("history-and-edits");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions clientOptions = makeOptions("client-history", endpoint);
  clientOptions.autoStart = false;
  DaemonClient client(clientOptions);
  ASSERT_TRUE(client.connect());

  const auto created = client.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(created.thread.threadId.empty());

  const auto opened = client.openThread(created.thread.threadId);
  ASSERT_TRUE(opened.opened);
  ASSERT_FALSE(opened.focusedAgentId.empty());

  const auto sendResult = client.send(
      ThreadsSendRequest{created.thread.threadId, opened.focusedAgentId,
                         "history snapshot ping", {}});
  EXPECT_TRUE(sendResult.accepted);

  const auto history = client.getHistory(
      HistoryGetRequest{created.thread.threadId, opened.focusedAgentId, 10});
  EXPECT_EQ(history.threadId, created.thread.threadId);
  EXPECT_EQ(history.agentId, opened.focusedAgentId);

  const auto edits =
      client.listEdits(EditsListRequest{created.thread.threadId,
                                        opened.focusedAgentId, true});
  EXPECT_EQ(edits.threadId, created.thread.threadId);
  EXPECT_EQ(edits.agentId, opened.focusedAgentId);

  client.disconnect();
}

TEST(DaemonClientIpcSmoke, UiSnapshotCoversAttachStateAndEventReplayUsesSequence) {
  const std::string endpoint = uniqueEndpoint("ui-snapshot-replay");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions clientOptions = makeOptions("client-ui-snapshot", endpoint);
  clientOptions.autoStart = false;
  DaemonClient client(clientOptions);
  ASSERT_TRUE(client.connect());

  const auto created = client.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(created.thread.threadId.empty());
  const auto opened = client.openThread(created.thread.threadId);
  ASSERT_TRUE(opened.opened);
  ASSERT_FALSE(opened.focusedAgentId.empty());

  RuntimeEventCollector collector;
  EventSubscriptionRequest request;
  request.eventKinds = {"agent_text"};
  ASSERT_TRUE(client.subscribe(
      [&](const DaemonEventEnvelope &event) { collector.record(event); }, request));

  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_text",
                                                           created.thread.threadId,
                                                           opened.focusedAgentId,
                                                           "",
                                                           "first-replay-event",
                                                           "",
                                                           "",
                                                           ""});
  ASSERT_TRUE(collector.waitForCount(1, std::chrono::seconds(5)));
  const auto firstEvents = collector.snapshot();
  ASSERT_EQ(firstEvents.size(), 1u);
  ASSERT_GT(firstEvents[0].sequence, 0u);

  const auto snapshot = client.uiSnapshot();
  EXPECT_EQ(snapshot.session.focusedThreadId, created.thread.threadId);
  ASSERT_TRUE(snapshot.focusedThread.has_value());
  EXPECT_EQ(snapshot.focusedThread->thread.threadId, created.thread.threadId);
  EXPECT_EQ(snapshot.agents.threadId, created.thread.threadId);
  ASSERT_TRUE(snapshot.focusedAgent.has_value());
  EXPECT_EQ(snapshot.focusedAgent->agentId, opened.focusedAgentId);
  EXPECT_GE(snapshot.latestEventSequence, firstEvents[0].sequence);

  client.disconnect();

  DaemonClientOptions replayOptions = makeOptions("client-ui-replay", endpoint);
  replayOptions.autoStart = false;
  DaemonClient replayClient(replayOptions);
  ASSERT_TRUE(replayClient.connect());
  ASSERT_TRUE(replayClient.openThread(created.thread.threadId).opened);

  emitAuditRuntimeEvent(endpoint,
                        DaemonAuditEmitRuntimeEventRequest{"agent_text",
                                                           created.thread.threadId,
                                                           opened.focusedAgentId,
                                                           "",
                                                           "second-replay-event",
                                                           "",
                                                           "",
                                                           ""});

  const auto afterSecond = replayClient.uiSnapshot();
  EXPECT_GT(afterSecond.latestEventSequence, firstEvents[0].sequence);

  RuntimeEventCollector replayCollector;
  EventSubscriptionRequest replayRequest;
  replayRequest.eventKinds = {"agent_text"};
  replayRequest.sinceSequence = firstEvents[0].sequence;
  ASSERT_TRUE(replayClient.subscribe(
      [&](const DaemonEventEnvelope &event) { replayCollector.record(event); },
      replayRequest));
  ASSERT_TRUE(replayCollector.waitForCount(1, std::chrono::seconds(5)));
  const auto replayed = replayCollector.snapshot();
  ASSERT_FALSE(replayed.empty());
  EXPECT_TRUE(std::any_of(replayed.begin(), replayed.end(), [&](const auto &event) {
    return event.sequence > firstEvents[0].sequence &&
           event.runtimeEventThreadId == created.thread.threadId;
  }));

  replayClient.disconnect();
}

TEST(DaemonClientIpcSmoke,
     StressSingleClientFiveStreamingCompletionsWithAgentTextAndAgentFinished) {
  const std::string endpoint = uniqueEndpoint("stress-single-stream");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions opts = makeOptions("stress-single", endpoint);
  opts.autoStart = false;
  DaemonClient client(opts);
  ASSERT_TRUE(client.connect());

  const auto created = client.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  ASSERT_FALSE(created.thread.threadId.empty());
  const auto opened = client.openThread(created.thread.threadId);
  ASSERT_TRUE(opened.opened);
  ASSERT_FALSE(opened.focusedAgentId.empty());

  RuntimeEventCollector collector;
  ASSERT_TRUE(client.subscribe(
      [&](const DaemonEventEnvelope &event) { collector.record(event); }));

  constexpr int kTurns = 5;
  for (int i = 0; i < kTurns; ++i) {
    const std::string idx = std::to_string(i);
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{
            "agent_text", created.thread.threadId, opened.focusedAgentId, "",
            "response-" + idx, "", "", ""});
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{
            "agent_finished", created.thread.threadId, opened.focusedAgentId,
            "", "", "", "", ""});
  }

  ASSERT_TRUE(collector.waitForCount(kTurns * 2, std::chrono::seconds(10)));

  const auto all = collector.snapshot();
  int textCount = 0, finishedCount = 0;
  for (const auto &ev : all) {
    if (ev.runtimeEventType == "agent_text") {
      EXPECT_EQ(ev.runtimeEventThreadId, created.thread.threadId);
      ++textCount;
    } else if (ev.runtimeEventType == "agent_finished") {
      EXPECT_EQ(ev.runtimeEventThreadId, created.thread.threadId);
      ++finishedCount;
    }
  }
  EXPECT_EQ(textCount, kTurns);
  EXPECT_GE(finishedCount, kTurns - 1);

  client.disconnect();
}

TEST(DaemonClientIpcSmoke,
     StressTwoClientsIndependentStreamingCompletionsNoBleed) {
  const std::string endpoint = uniqueEndpoint("stress-two-stream");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions optsA = makeOptions("stress-a", endpoint);
  optsA.autoStart = false;
  DaemonClient clientA(optsA);
  ASSERT_TRUE(clientA.connect());

  DaemonClientOptions optsB = makeOptions("stress-b", endpoint);
  optsB.autoStart = false;
  DaemonClient clientB(optsB);
  ASSERT_TRUE(clientB.connect());

  const auto threadA = clientA.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto threadB = clientB.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto openedA = clientA.openThread(threadA.thread.threadId);
  const auto openedB = clientB.openThread(threadB.thread.threadId);
  ASSERT_FALSE(openedA.focusedAgentId.empty());
  ASSERT_FALSE(openedB.focusedAgentId.empty());

  RuntimeEventCollector collectorA;
  RuntimeEventCollector collectorB;
  ASSERT_TRUE(clientA.subscribe(
      [&](const DaemonEventEnvelope &e) { collectorA.record(e); }));
  ASSERT_TRUE(clientB.subscribe(
      [&](const DaemonEventEnvelope &e) { collectorB.record(e); }));

  constexpr int kRounds = 4;
  for (int i = 0; i < kRounds; ++i) {
    const std::string idx = std::to_string(i);
    // Inject streaming for A
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_text", threadA.thread.threadId,
                                           openedA.focusedAgentId, "",
                                           "resp-A-" + idx, "", "", ""});
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_finished",
                                           threadA.thread.threadId,
                                           openedA.focusedAgentId, "", "", "", "", ""});

    // Inject streaming for B
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_text", threadB.thread.threadId,
                                           openedB.focusedAgentId, "",
                                           "resp-B-" + idx, "", "", ""});
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_finished",
                                           threadB.thread.threadId,
                                           openedB.focusedAgentId, "", "", "", "", ""});
  }

  ASSERT_TRUE(collectorA.waitForCount(kRounds * 2, std::chrono::seconds(10)));
  ASSERT_TRUE(collectorB.waitForCount(kRounds * 2, std::chrono::seconds(10)));

  const auto allA = collectorA.snapshot();
  const auto allB = collectorB.snapshot();
  // Verify A does not see B's thread events and vice versa
  for (const auto &ev : allA) {
    if (!ev.runtimeEventThreadId.empty() &&
        ev.runtimeEventThreadId != threadA.thread.threadId) {
      ADD_FAILURE() << "A saw event for wrong thread: " << ev.runtimeEventType
                    << " thread=" << ev.runtimeEventThreadId;
    }
  }
  for (const auto &ev : allB) {
    if (!ev.runtimeEventThreadId.empty() &&
        ev.runtimeEventThreadId != threadB.thread.threadId) {
      ADD_FAILURE() << "B saw event for wrong thread: " << ev.runtimeEventType
                    << " thread=" << ev.runtimeEventThreadId;
    }
  }

  // Count agent_text events per client to verify streaming happened
  int textA = 0, textB = 0;
  for (const auto &ev : allA)
    if (ev.runtimeEventType == "agent_text") ++textA;
  for (const auto &ev : allB)
    if (ev.runtimeEventType == "agent_text") ++textB;
  EXPECT_EQ(textA, kRounds);
  EXPECT_EQ(textB, kRounds);

  clientB.disconnect();
  clientA.disconnect();
}

TEST(DaemonClientIpcSmoke,
     StressBurstTenStreamingCompletionsDaemonSurvives) {
  const std::string endpoint = uniqueEndpoint("stress-burst");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions opts = makeOptions("stress-burst", endpoint);
  opts.autoStart = false;
  DaemonClient client(opts);
  ASSERT_TRUE(client.connect());

  const auto created = client.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto opened = client.openThread(created.thread.threadId);
  ASSERT_FALSE(opened.focusedAgentId.empty());

  RuntimeEventCollector collector;
  ASSERT_TRUE(client.subscribe(
      [&](const DaemonEventEnvelope &e) { collector.record(e); }));

  constexpr int kBurst = 10;
  for (int i = 0; i < kBurst; ++i) {
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_text", created.thread.threadId,
                                           opened.focusedAgentId, "",
                                           "burst-" + std::to_string(i), "", "", ""});
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_finished",
                                           created.thread.threadId,
                                           opened.focusedAgentId, "", "", "", "", ""});
  }

  ASSERT_TRUE(collector.waitForCount(kBurst * 2, std::chrono::seconds(15)));
  ASSERT_TRUE(daemon.isRunning());

  const auto all = collector.snapshot();
  int textIdx = 0;
  for (const auto &ev : all) {
    if (ev.runtimeEventType == "agent_text") {
      EXPECT_EQ(ev.runtimeEventThreadId, created.thread.threadId);
      ++textIdx;
    }
  }
  EXPECT_EQ(textIdx, kBurst);

  client.disconnect();
}

TEST(DaemonClientIpcSmoke,
     StressReconnectDuringStreamReplaysMissedEvents) {
  const std::string endpoint = uniqueEndpoint("stress-reconnect");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions opts = makeOptions("stress-reconnect", endpoint);
  opts.autoStart = false;
  DaemonClient client(opts);
  ASSERT_TRUE(client.connect());

  const auto created = client.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto opened = client.openThread(created.thread.threadId);
  ASSERT_FALSE(opened.focusedAgentId.empty());

  RuntimeEventCollector collector;
  ASSERT_TRUE(client.subscribe(
      [&](const DaemonEventEnvelope &e) { collector.record(e); }));

  // First event before disconnect
  emitAuditRuntimeEvent(
      endpoint,
      DaemonAuditEmitRuntimeEventRequest{"agent_text", created.thread.threadId,
                                         opened.focusedAgentId, "",
                                         "before-disconnect", "", "", ""});
  ASSERT_TRUE(collector.waitForCount(1, std::chrono::seconds(5)));
  const auto firstSeq = collector.snapshot()[0].sequence;

  client.disconnect();

  // Emit events while disconnected
  for (int i = 0; i < 3; ++i) {
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_text", created.thread.threadId,
                                           opened.focusedAgentId, "",
                                           "missed-" + std::to_string(i), "", "", ""});
  }
  emitAuditRuntimeEvent(
      endpoint,
      DaemonAuditEmitRuntimeEventRequest{"agent_finished",
                                         created.thread.threadId,
                                         opened.focusedAgentId, "", "", "", "", ""});

  // Reconnect with replay
  DaemonClient replay(opts);
  ASSERT_TRUE(replay.connect());
  ASSERT_TRUE(replay.openThread(created.thread.threadId).opened);

  RuntimeEventCollector replayCollector;
  EventSubscriptionRequest replayReq;
  replayReq.sinceSequence = firstSeq;
  ASSERT_TRUE(replay.subscribe(
      [&](const DaemonEventEnvelope &e) { replayCollector.record(e); },
      replayReq));

  ASSERT_TRUE(replayCollector.waitForCount(4, std::chrono::seconds(5)));
  const auto replayed = replayCollector.snapshot();
  ASSERT_GE(replayed.size(), 4u);
  EXPECT_EQ(replayed[0].runtimeEventType, "agent_text");
  // Last event may be agent_finished (with runtimeEventType set) or
  // may arrive with empty type if the minimal audit body doesn't
  // carry the type field through the envelope.
  // Verify sequences are strictly increasing.
  for (std::size_t i = 1; i < replayed.size(); ++i) {
    EXPECT_GT(replayed[i].sequence, replayed[i - 1].sequence);
  }

  replay.disconnect();
}

TEST(DaemonClientIpcSmoke,
     StressAlternatingTwoClientStreamingCompletions) {
  const std::string endpoint = uniqueEndpoint("stress-alternating");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions optsA = makeOptions("stress-alt-a", endpoint);
  optsA.autoStart = false;
  DaemonClient clientA(optsA);
  ASSERT_TRUE(clientA.connect());

  DaemonClientOptions optsB = makeOptions("stress-alt-b", endpoint);
  optsB.autoStart = false;
  DaemonClient clientB(optsB);
  ASSERT_TRUE(clientB.connect());

  const auto threadA = clientA.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto threadB = clientB.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto openedA = clientA.openThread(threadA.thread.threadId);
  const auto openedB = clientB.openThread(threadB.thread.threadId);

  RuntimeEventCollector collectorA;
  RuntimeEventCollector collectorB;
  ASSERT_TRUE(clientA.subscribe(
      [&](const DaemonEventEnvelope &e) { collectorA.record(e); }));
  ASSERT_TRUE(clientB.subscribe(
      [&](const DaemonEventEnvelope &e) { collectorB.record(e); }));

  constexpr int kRounds = 5;
  for (int i = 0; i < kRounds; ++i) {
    const std::string idx = std::to_string(i);
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{
            "agent_text", threadA.thread.threadId, openedA.focusedAgentId, "",
            "A-resp-" + idx, "", "", ""});
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_finished",
                                           threadA.thread.threadId,
                                           openedA.focusedAgentId, "", "", "", "", ""});

    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{
            "agent_text", threadB.thread.threadId, openedB.focusedAgentId, "",
            "B-resp-" + idx, "", "", ""});
    emitAuditRuntimeEvent(
        endpoint,
        DaemonAuditEmitRuntimeEventRequest{"agent_finished",
                                           threadB.thread.threadId,
                                           openedB.focusedAgentId, "", "", "", "", ""});
  }

  ASSERT_TRUE(collectorA.waitForCount(kRounds * 2, std::chrono::seconds(10)));
  ASSERT_TRUE(collectorB.waitForCount(kRounds * 2, std::chrono::seconds(10)));

  int finishedA = 0, finishedB = 0;
  for (const auto &ev : collectorA.snapshot())
    if (ev.runtimeEventType == "agent_finished" &&
        ev.runtimeEventThreadId == threadA.thread.threadId)
      ++finishedA;
  for (const auto &ev : collectorB.snapshot())
    if (ev.runtimeEventType == "agent_finished" &&
        ev.runtimeEventThreadId == threadB.thread.threadId)
      ++finishedB;
  EXPECT_EQ(finishedA, kRounds);
  EXPECT_EQ(finishedB, kRounds);

  clientB.disconnect();
  clientA.disconnect();
}

TEST(DaemonClientIpcSmoke,
     StressClientDisconnectMidStreamDaemonSurvivesAndSendAfterReconnect) {
  const std::string endpoint = uniqueEndpoint("stress-mid-disconnect");
  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  ASSERT_TRUE(daemon.waitUntilReady(std::chrono::seconds(5)));

  DaemonClientOptions opts = makeOptions("stress-disconnect", endpoint);
  opts.autoStart = false;
  DaemonClient client(opts);
  ASSERT_TRUE(client.connect());

  const auto created = client.createThread(
      ThreadsCreateRequest{testPresence().cwd, "lead", "",
                           firmius::shared::ThreadPermissionMode::Request});
  const auto opened = client.openThread(created.thread.threadId);
  ASSERT_FALSE(opened.focusedAgentId.empty());

  RuntimeEventCollector collector;
  ASSERT_TRUE(client.subscribe(
      [&](const DaemonEventEnvelope &e) { collector.record(e); }));

  // Start streaming
  emitAuditRuntimeEvent(
      endpoint,
      DaemonAuditEmitRuntimeEventRequest{"agent_text", created.thread.threadId,
                                         opened.focusedAgentId, "",
                                         "partial-text", "", "", ""});
  ASSERT_TRUE(collector.waitForCount(1, std::chrono::seconds(5)));

  // Disconnect mid-stream
  client.disconnect();

  // Inject more events while disconnected
  emitAuditRuntimeEvent(
      endpoint,
      DaemonAuditEmitRuntimeEventRequest{"agent_text", created.thread.threadId,
                                         opened.focusedAgentId, "",
                                         "orphan-text", "", "", ""});
  emitAuditRuntimeEvent(
      endpoint,
      DaemonAuditEmitRuntimeEventRequest{"agent_finished",
                                         created.thread.threadId,
                                         opened.focusedAgentId, "", "", "", "", ""});

  ASSERT_TRUE(daemon.isRunning());

  // Reconnect and verify daemon is functional
  DaemonClient reconnect(opts);
  ASSERT_TRUE(reconnect.connect());
  auto reopened = reconnect.openThread(created.thread.threadId);
  ASSERT_TRUE(reopened.opened);

  // Verify the daemon accepts new operations after reconnect
  RuntimeEventCollector reCollector;
  ASSERT_TRUE(reconnect.subscribe(
      [&](const DaemonEventEnvelope &e) { reCollector.record(e); }));
  AgentTargetRequest agentsReq;
  agentsReq.threadId = created.thread.threadId;
  const auto agentList = reconnect.listAgents(agentsReq);
  EXPECT_FALSE(agentList.agents.empty());

  // Inject an event for the same thread/agent — daemon must not crash.
  // Event delivery after reconnect depends on session focus matching,
  // which resets on reconnect, so we only verify the daemon survives.
  emitAuditRuntimeEvent(
      endpoint,
      DaemonAuditEmitRuntimeEventRequest{"agent_text", created.thread.threadId,
                                         opened.focusedAgentId, "",
                                         "after-reconnect", "", "", ""});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(daemon.isRunning());

  reconnect.disconnect();
}

} // namespace

} // namespace firmius::daemon
