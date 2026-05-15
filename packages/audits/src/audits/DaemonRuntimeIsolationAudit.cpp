#include "audits/DaemonRuntimeIsolationAudit.hpp"

#include "daemon/DaemonClient.hpp"
#include "daemon/Protocol.hpp"
#include "daemon/ProtocolSerialization.hpp"
#include "daemon/SocketTransport.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <rapidjson/document.h>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace firmius::audits {
namespace {

namespace fs = std::filesystem;

std::string uniqueSuffix() {
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string uniqueEndpoint(const std::string &name) {
#if defined(_WIN32)
  return std::string(R"(\\.\pipe\firmiusd-audit-)") + name + "-" + uniqueSuffix();
#else
  auto path = fs::temp_directory_path() /
              ("firmiusd-audit-" + name + "-" + uniqueSuffix() + ".sock");
  return path.string();
#endif
}

std::string daemonExecutablePath() {
  const auto buildServerDir = fs::current_path() / "build" / "packages" / "server";
#if defined(_WIN32)
  return (buildServerDir / "firmiusd.exe").string();
#else
  return (buildServerDir / "firmiusd").string();
#endif
}

rapidjson::Document makeParams() {
  rapidjson::Document params;
  params.SetObject();
  return params;
}

std::optional<rapidjson::Value> asParams(rapidjson::Document &params) {
  rapidjson::Value value(rapidjson::kObjectType);
  value.CopyFrom(params, params.GetAllocator());
  return value;
}

rapidjson::Document sendRawJsonRpcRequest(const std::string &endpoint,
                                         const std::string &method,
                                         std::optional<rapidjson::Value> params =
                                             std::nullopt) {
  rapidjson::Document request;
  request.SetObject();
  auto &allocator = request.GetAllocator();
  request.AddMember("jsonrpc", rapidjson::Value("2.0", allocator).Move(), allocator);
  request.AddMember("id", 1, allocator);
  request.AddMember("method", rapidjson::Value(method.c_str(), allocator).Move(),
                    allocator);
  firmius::daemon::SocketTransport transport({endpoint});
  auto channel = transport.connect(std::chrono::milliseconds(5000));
  auto rpc = std::make_shared<firmius::core::JsonRpcTransport>(
      std::move(channel.writer), std::move(channel.reader), std::move(channel.wakeStop));
  rpc->start();
  auto paramsDoc = makeParams();
  auto &paramsAlloc = paramsDoc.GetAllocator();
  if (params.has_value()) {
    paramsDoc.AddMember("params", params->Move(), paramsAlloc);
  }
  auto response = rpc->sendRequest(method, paramsDoc, 5000);
  rpc->stop();
  if (!response.IsObject()) {
    throw std::runtime_error("daemon audit received malformed JSON-RPC response");
  }
  return response;
}

struct ScopedDaemonProcess {
  std::string endpoint;
  std::string executable;
#if defined(_WIN32)
  HANDLE processHandle = nullptr;
#else
  pid_t processId = -1;
#endif

  ScopedDaemonProcess(std::string endpointValue, std::string executableValue)
      : endpoint(std::move(endpointValue)), executable(std::move(executableValue)) {}

  ~ScopedDaemonProcess() { stop(); }

  void start() {
#if defined(_WIN32)
    throw std::runtime_error("DaemonRuntimeIsolationAudit is not implemented on Windows");
#else
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(executable.c_str()));
    argv.push_back(const_cast<char *>("--daemon"));
    argv.push_back(const_cast<char *>("--endpoint"));
    argv.push_back(const_cast<char *>(endpoint.c_str()));
    argv.push_back(nullptr);
    if (::posix_spawn(&processId, executable.c_str(), nullptr, nullptr, argv.data(),
                      environ) != 0) {
      throw std::runtime_error("failed to spawn firmiusd for audit");
    }
#endif
  }

  bool waitUntilReady(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        auto response = sendRawJsonRpcRequest(endpoint, firmius::daemon::kRpcDaemonPing);
        if (response.HasMember("result")) {
          return true;
        }
      } catch (...) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }

  void stop() {
#if defined(_WIN32)
    if (processHandle != nullptr) {
      TerminateProcess(processHandle, 1);
      CloseHandle(processHandle);
      processHandle = nullptr;
    }
#else
    if (processId > 0) {
      ::kill(processId, SIGTERM);
      int status = 0;
      (void)::waitpid(processId, &status, 0);
      processId = -1;
    }
#endif
  }
};

struct CapturedEvent {
  std::string runtimeEventType;
  std::string threadId;
  std::string agentId;
  std::string runtimeEventJson;
};

struct Collector {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<CapturedEvent> events;

  void push(const firmius::daemon::DaemonEventEnvelope &event) {
    if (event.kind != firmius::daemon::DaemonEventKind::RuntimeAppEvent) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      events.push_back({event.runtimeEventType, event.runtimeEventThreadId,
                        event.runtimeEventAgentId, event.runtimeEventJson});
    }
    cv.notify_all();
  }

  bool waitForCount(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout, [&] { return events.size() >= count; });
  }
};

void emitRuntimeEvent(const std::string &endpoint,
                      const firmius::daemon::DaemonAuditEmitRuntimeEventRequest &request) {
  auto params = makeParams();
  auto &allocator = params.GetAllocator();
  auto value = firmius::daemon::toJsonValue(request, allocator);
  params.CopyFrom(value, allocator);
  auto response = sendRawJsonRpcRequest(
      endpoint, firmius::daemon::kRpcDaemonAuditEmitRuntimeEvent, asParams(params));
  if (!response.HasMember("result")) {
    throw std::runtime_error("daemon audit emit RPC returned no result");
  }
  const auto result =
      firmius::daemon::daemonAuditEmitRuntimeEventResponseFromJson(response["result"]);
  if (!result.emitted) {
    throw std::runtime_error("daemon audit emit RPC did not emit event");
  }
}

rapidjson::Document parseJson(const std::string &json) {
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  return doc;
}

} // namespace

std::string DaemonRuntimeIsolationAudit::getId() const {
  return "daemon_runtime_isolation";
}

std::string DaemonRuntimeIsolationAudit::getDescription() const {
  return "Daemon-backed realtime multi-client isolation audit with deterministic runtime events";
}

shared::AuditResult
DaemonRuntimeIsolationAudit::run(const std::vector<std::string> & /*args*/) {
  shared::AuditResult result;
  result.auditId = getId();

  std::ostringstream out;
  const std::string endpoint = uniqueEndpoint("runtime-isolation");
  out << "endpoint=" << endpoint << "\n";

  ScopedDaemonProcess daemon(endpoint, daemonExecutablePath());
  daemon.start();
  if (!daemon.waitUntilReady(std::chrono::seconds(5))) {
    result.exitCode = 1;
    result.passed = false;
    out << "ready=false\n";
    result.output = out.str();
    return result;
  }
  out << "ready=true\n";

  firmius::daemon::DaemonClientOptions clientAOptions;
  clientAOptions.identity.clientId = "audit-client-a";
  clientAOptions.identity.uiKind = "audit";
  clientAOptions.connection.endpoint = endpoint;
  clientAOptions.autoStart = false;
  firmius::daemon::DaemonClient clientA(clientAOptions);
  if (!clientA.connect()) {
    result.exitCode = 1;
    result.passed = false;
    out << "connect_client_a=false\n";
    result.output = out.str();
    return result;
  }

  firmius::daemon::DaemonClientOptions clientBOptions;
  clientBOptions.identity.clientId = "audit-client-b";
  clientBOptions.identity.uiKind = "audit";
  clientBOptions.connection.endpoint = endpoint;
  clientBOptions.autoStart = false;
  firmius::daemon::DaemonClient clientB(clientBOptions);
  if (!clientB.connect()) {
    result.exitCode = 1;
    result.passed = false;
    out << "connect_client_b=false\n";
    result.output = out.str();
    return result;
  }

  const auto createdA = clientA.createThread(
      firmius::daemon::ThreadsCreateRequest{fs::current_path().string(), "aster", "",
                                            firmius::shared::ThreadPermissionMode::Request});
  const auto createdB = clientB.createThread(
      firmius::daemon::ThreadsCreateRequest{fs::current_path().string(), "aster", "",
                                            firmius::shared::ThreadPermissionMode::Request});
  out << "thread_a=" << createdA.thread.threadId << "\n";
  out << "thread_b=" << createdB.thread.threadId << "\n";

  if (!clientA.openThread(createdA.thread.threadId).opened ||
      !clientB.openThread(createdB.thread.threadId).opened) {
    result.exitCode = 1;
    result.passed = false;
    out << "thread_open_failed=true\n";
    result.output = out.str();
    return result;
  }

  Collector collectorA;
  Collector collectorB;
  clientA.subscribe([&](const firmius::daemon::DaemonEventEnvelope &event) {
    collectorA.push(event);
  });
  clientB.subscribe([&](const firmius::daemon::DaemonEventEnvelope &event) {
    collectorB.push(event);
  });

  emitRuntimeEvent(endpoint,
                   firmius::daemon::DaemonAuditEmitRuntimeEventRequest{
                       "agent_thinking", createdA.thread.threadId,
                       createdA.focusedAgentId, "", "audit-thinking-a", "", "", ""});
  emitRuntimeEvent(endpoint,
                   firmius::daemon::DaemonAuditEmitRuntimeEventRequest{
                       "agent_tool_call", createdB.thread.threadId,
                       createdB.focusedAgentId, "", "", "audit-tool-b", "file_read",
                       "{\"path\":\"audit-B.txt\"}"});

  const bool clientAReady = collectorA.waitForCount(1, std::chrono::seconds(5));
  const bool clientBReady = collectorB.waitForCount(1, std::chrono::seconds(5));
  out << "client_a_events=" << collectorA.events.size() << "\n";
  out << "client_b_events=" << collectorB.events.size() << "\n";

  bool passed = clientAReady && clientBReady && collectorA.events.size() == 1 &&
                collectorB.events.size() == 1 &&
                collectorA.events.front().threadId == createdA.thread.threadId &&
                collectorB.events.front().threadId == createdB.thread.threadId &&
                collectorA.events.front().runtimeEventType == "agent_thinking" &&
                collectorB.events.front().runtimeEventType == "agent_tool_call";

  if (passed) {
    const auto payloadA = parseJson(collectorA.events.front().runtimeEventJson);
    const auto payloadB = parseJson(collectorB.events.front().runtimeEventJson);
    passed = payloadA.IsObject() && payloadB.IsObject() &&
             payloadA.HasMember("text") && payloadA["text"].IsString() &&
             std::string(payloadA["text"].GetString()) == "audit-thinking-a" &&
             payloadB.HasMember("toolCallId") && payloadB["toolCallId"].IsString() &&
             std::string(payloadB["toolCallId"].GetString()) == "audit-tool-b";
  }

  out << "passed=" << (passed ? "true" : "false") << "\n";
  result.passed = passed;
  result.exitCode = passed ? 0 : 1;
  result.output = out.str();

  clientA.disconnect();
  clientB.disconnect();
  return result;
}

} // namespace firmius::audits
