#include "harness/Harness.hpp"
#include "agents/hooks/HookRegistry.hpp"
#include "agents/hooks/HookState.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "IHost.hpp"
#include "agents/Agent.hpp"
#include "agents/ContextBudget.hpp"
#include "agents/HintingLoader.hpp"
#include "agents/PurposeLoader.hpp"
#include "artifacts/ReferenceExpansion.hpp"
#include "hosts/DockerHost.hpp"
#include "hosts/LocalHost.hpp"
#include "mcp/McpManager.hpp"
#include "persistence/Journaler.hpp"
#include "persistence/HistoryEditor.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/BaseAPIKeyProvider.hpp"
#include "providers/BaseOAuthProvider.hpp"
#include "providers/ProviderRegistry.hpp"
#include "utils/PlatformPaths.hpp"
#include "utils/Logger.hpp"
#include "utils/PermissionProfiles.hpp"
#include "utils/FSUtil.hpp"
#include "utils/PlatformPaths.hpp"
#include "utils/StringUtil.hpp"
#include "utils/ToolSummaries.hpp"
#include "workflow/WorkflowLoader.hpp"
#include <Context.hpp>
#include <EnvLoader.hpp>
#include <Events.hpp>
#include <Panic.hpp>
#include <Serialization.hpp>

#include <memory>
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <thread>
#include <limits>

#include <iostream>

namespace {

static constexpr int kAgentReadyTimeoutMs = 2000;

const std::string PANIC_INFO_HARNESS_STATE = "harness_state";

uint64_t nowEpochMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

} // namespace

namespace firmius::core {

using namespace firmius::shared;

namespace {
const std::string FIRMIUS_DIR = ".firmius";
const std::string SESSION_FILE = "last_session.json";
const std::string PROJECT_SESSIONS_FILE = "last_sessions.json";
const std::string OWNER_PID_LABEL = "com.firmius.owner_pid";

std::string resolveRunnableLeadPersona(const std::string &requestedPersona) {
  if (!requestedPersona.empty() && PurposeLoader::isValid(requestedPersona)) {
    return requestedPersona;
  }

  const auto &cfg = shared::ConfigLoader::instance().getConfig();
  if (!cfg.defaultLeadPersona.empty() &&
      PurposeLoader::isValid(cfg.defaultLeadPersona)) {
    return cfg.defaultLeadPersona;
  }

  if (PurposeLoader::isValid("lead")) {
    return "lead";
  }

  const auto available = PurposeLoader::listPurposes();
  if (!available.empty()) {
    return available.front();
  }

  return requestedPersona.empty() ? "lead" : requestedPersona;
}

struct AgentHistoryScore {
  std::size_t turns = 0;
  uint64_t lastTimestamp = 0;
};

AgentHistoryScore scorePersistedAgent(ThreadManager &threadManager,
                                      const std::string &threadId,
                                      const std::string &agentId) {
  AgentHistoryScore score;
  try {
    const auto history = threadManager.loadAgentHistory(threadId, agentId);
    score.turns = history.turns.size();
    for (const auto &turn : history.turns) {
      for (const auto &message : turn.messages) {
        score.lastTimestamp = std::max(score.lastTimestamp, message.timestamp);
      }
    }
  } catch (...) {
    Logger::instance().logDebug("Harness: best-effort agent scoring failed for thread/agent lookup");
  }
  return score;
}

std::string chooseBestPersistedAgent(
    ThreadManager &threadManager, const std::string &threadId,
    const std::map<std::string, AgentManifestEntry> &manifest) {
  auto chooseFrom = [&](bool rootsOnly) {
    std::string bestId;
    AgentHistoryScore bestScore;
    for (const auto &[agentId, entry] : manifest) {
      if (rootsOnly && !entry.parentId.empty()) {
        continue;
      }
      const auto score = scorePersistedAgent(threadManager, threadId, agentId);
      if (bestId.empty() || score.turns > bestScore.turns ||
          (score.turns == bestScore.turns &&
           score.lastTimestamp > bestScore.lastTimestamp)) {
        bestId = agentId;
        bestScore = score;
      }
    }
    return bestId;
  };

  std::string selected = chooseFrom(true);
  if (selected.empty()) {
    selected = chooseFrom(false);
  }
  return selected;
}

bool ensureWritableDirectory(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec || !std::filesystem::exists(dir) ||
      !std::filesystem::is_directory(dir)) {
    return false;
  }

  const auto probe =
      dir / (".write_probe_" + shared::StringUtil::generateUuid());
  std::ofstream out(probe);
  if (!out.is_open()) {
    return false;
  }
  out << "ok";
  out.close();
  std::filesystem::remove(probe, ec);
  return true;
}

std::string resolveWritableFirmiusHome() {
  const std::filesystem::path userHome =
      firmius::shared::PlatformPaths::firmiusHomeDir();
  if (ensureWritableDirectory(userHome)) {
    return userHome.string();
  }

  const std::filesystem::path localHome =
      std::filesystem::current_path() / FIRMIUS_DIR;
  if (ensureWritableDirectory(localHome)) {
    return localHome.string();
  }

  const std::filesystem::path tempHome =
      shared::PlatformPaths::firmiusTempDir();
  if (ensureWritableDirectory(tempHome)) {
    return tempHome.string();
  }

  return tempHome.string();
}

std::string getFirmiusHome() { return resolveWritableFirmiusHome(); }

std::string normalizePathForComparison(const std::string &path) {
  if (path.empty()) {
    return path;
  }
  std::error_code ec;
  std::filesystem::path p(path);
  if (!p.is_absolute()) {
    p = std::filesystem::absolute(p, ec);
  }
  if (!ec) {
    const auto canon = std::filesystem::weakly_canonical(p, ec);
    if (!ec) {
      return canon.string();
    }
  }
  return p.string();
}

std::string currentWorkingDirectoryForComparison() {
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (ec) {
    return "";
  }
  return normalizePathForComparison(cwd.string());
}

std::string getSessionPath() { return getFirmiusHome() + "/" + SESSION_FILE; }

std::string getProjectSessionsPath(const std::string &cwd) {
  if (cwd.empty()) {
    return "";
  }

  std::filesystem::path projectDir = std::filesystem::path(cwd) / FIRMIUS_DIR;
  if (!ensureWritableDirectory(projectDir)) {
    return "";
  }
  return (projectDir / PROJECT_SESSIONS_FILE).string();
}

struct ProjectSessionEntry {
  std::string threadId;
  std::string focusedAgentId;
  std::string cwd;
  uint64_t lastActiveAt = 0;
};

std::vector<ProjectSessionEntry> loadProjectSessions(const std::string &cwd) {
  std::vector<ProjectSessionEntry> out;
  const std::string path = getProjectSessionsPath(cwd);
  if (path.empty()) {
    return out;
  }

  std::ifstream in(path);
  if (!in.is_open()) {
    return out;
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  in.close();

  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if (doc.HasParseError()) {
    return out;
  }

  auto parseEntry = [&](const rapidjson::Value &v)
      -> std::optional<ProjectSessionEntry> {
    if (!v.IsObject()) {
      return std::nullopt;
    }
    ProjectSessionEntry e;
    if (v.HasMember("threadId") && v["threadId"].IsString()) {
      e.threadId = v["threadId"].GetString();
    }
    if (v.HasMember("focusedAgentId") && v["focusedAgentId"].IsString()) {
      e.focusedAgentId = v["focusedAgentId"].GetString();
    }
    if (v.HasMember("cwd") && v["cwd"].IsString()) {
      e.cwd = v["cwd"].GetString();
    }
    if (v.HasMember("lastActiveAt") && v["lastActiveAt"].IsUint64()) {
      e.lastActiveAt = v["lastActiveAt"].GetUint64();
    }
    if (e.threadId.empty()) {
      return std::nullopt;
    }
    return e;
  };

  if (doc.IsArray()) {
    for (const auto &v : doc.GetArray()) {
      auto entry = parseEntry(v);
      if (entry.has_value()) {
        out.push_back(*entry);
      }
    }
  } else if (doc.IsObject() && doc.HasMember("sessions") &&
             doc["sessions"].IsArray()) {
    for (const auto &v : doc["sessions"].GetArray()) {
      auto entry = parseEntry(v);
      if (entry.has_value()) {
        out.push_back(*entry);
      }
    }
  }
  return out;
}

void persistProjectSessions(const std::string &cwd,
                            const std::vector<ProjectSessionEntry> &sessions) {
  const std::string path = getProjectSessionsPath(cwd);
  if (path.empty()) {
    return;
  }

  rapidjson::Document doc;
  doc.SetArray();
  auto &a = doc.GetAllocator();
  for (const auto &s : sessions) {
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("threadId", rapidjson::Value(s.threadId.c_str(), a), a);
    if (!s.focusedAgentId.empty()) {
      obj.AddMember("focusedAgentId",
                    rapidjson::Value(s.focusedAgentId.c_str(), a), a);
    }
    if (!s.cwd.empty()) {
      obj.AddMember("cwd", rapidjson::Value(s.cwd.c_str(), a), a);
    }
    if (s.lastActiveAt != 0) {
      obj.AddMember("lastActiveAt", s.lastActiveAt, a);
    }
    doc.PushBack(obj, a);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  std::ofstream out(path);
  if (out.is_open()) {
    out << buffer.GetString();
    out.close();
  }
}

std::string modelCacheKey(const firmius::shared::ModelInfo &model,
                          const std::string &providerId) {
  const std::string provider =
      model.provider.empty() ? providerId : model.provider;
  return provider + "\n" + model.id;
}

void persistSessionState(const std::string &threadId,
                         const std::string &focusedAgentId) {
  const uint64_t nowMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();

  if (!threadId.empty()) {
    doc.AddMember("threadId", rapidjson::Value(threadId.c_str(), a), a);
  }
  if (!focusedAgentId.empty()) {
    doc.AddMember("focusedAgentId", rapidjson::Value(focusedAgentId.c_str(), a),
                  a);
  }
  const std::string cwd = currentWorkingDirectoryForComparison();
  if (!threadId.empty() && !cwd.empty()) {
    doc.AddMember("cwd", rapidjson::Value(cwd.c_str(), a), a);
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  std::ofstream sessionFile(getSessionPath());
  if (sessionFile.is_open()) {
    sessionFile << buffer.GetString();
    sessionFile.close();
  }

  // Also keep a per-project MRU list.
  if (!threadId.empty() && !cwd.empty()) {
    auto sessions = loadProjectSessions(cwd);
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [&](const ProjectSessionEntry &e) {
                                    return e.threadId == threadId;
                                  }),
                   sessions.end());
    ProjectSessionEntry head;
    head.threadId = threadId;
    head.focusedAgentId = focusedAgentId;
    head.cwd = cwd;
    head.lastActiveAt = nowMs;
    sessions.insert(sessions.begin(), head);
    constexpr std::size_t kMaxSessions = 6;
    if (sessions.size() > kMaxSessions) {
      sessions.resize(kMaxSessions);
    }
    persistProjectSessions(cwd, sessions);
  }
}

bool isPidAlive(long long pidValue) {
  if (pidValue <= 0) {
    return false;
  }
#if defined(_WIN32)
  if (pidValue > static_cast<long long>(std::numeric_limits<DWORD>::max())) {
    return false;
  }
  HANDLE process =
      OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pidValue));
  if (process == nullptr) {
    return false;
  }
  const DWORD waitResult = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return waitResult == WAIT_TIMEOUT;
#else
  const pid_t pid = static_cast<pid_t>(pidValue);
  return kill(pid, 0) == 0 || errno == EPERM;
#endif
}

void appendUniqueAgentId(std::vector<std::string> &agentIds,
                         const std::string &agentId) {
  if (agentId.empty()) {
    return;
  }
  if (std::find(agentIds.begin(), agentIds.end(), agentId) == agentIds.end()) {
    agentIds.push_back(agentId);
  }
}

template <typename Fn>
bool waitFor(Fn &&fn, std::chrono::milliseconds timeout,
             std::chrono::milliseconds step = std::chrono::milliseconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) {
      return true;
    }
    std::this_thread::sleep_for(step);
  }
  return fn();
}

std::string trimFleetDiffPreview(const std::string &diffPreview,
                                 std::size_t maxLines = 24,
                                 std::size_t maxChars = 1400) {
  if (diffPreview.empty()) {
    return "";
  }
  std::istringstream stream(diffPreview);
  std::string line;
  std::string out;
  std::size_t lineCount = 0;
  while (std::getline(stream, line)) {
    if (lineCount >= maxLines || out.size() + line.size() + 1 > maxChars) {
      out += "\n[diff truncated]";
      break;
    }
    if (!out.empty()) {
      out += '\n';
    }
    out += line;
    ++lineCount;
  }
  return out;
}

size_t curlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

void killDockerContainer(const std::string &containerId) {
#if defined(_WIN32)
  (void)containerId;
#else
  CURL *curl = curl_easy_init();
  if (curl) {
    std::string response;
    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, "/var/run/docker.sock");
    curl_easy_setopt(
        curl, CURLOPT_URL,
        ("http://localhost/v1.44/containers/" + containerId + "?force=true")
            .c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
  }
#endif
}
} // namespace

Harness &Harness::instance() {
  static Harness instance;
  return instance;
}

Harness::Harness()
    : threadManager_(getFirmiusHome() + "/threads"), nextSubscriptionId_(0) {}

Harness::~Harness() {
  try {
    joinBackgroundThreads();
  } catch (...) {
    Logger::instance().logDebug("Harness: exception during destructor cleanup");
  }
}

void Harness::joinBackgroundThreads() {
  std::vector<std::thread> toJoin;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    toJoin = std::move(backgroundThreads_);
  }
  for (auto &thread : toJoin) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

void Harness::init() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  threadManager_ = ThreadManager(getFirmiusHome() + "/threads");
  currentThreadId_.clear();
  focusedAgentId_.clear();
  threadAgentMap_.clear();
  {
    std::lock_guard<std::mutex> modelLock(modelsMutex_);
    cachedModels_.clear();
    cachedModelKeys_.clear();
    loadingModelProviders_.clear();
    isRefreshingModels_ = false;
    modelsLoaded_ = false;
  }

  shared::Panic::addExtraInfo(
      PANIC_INFO_HARNESS_STATE, [this]() -> std::string {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::stringstream ss;
        ss << "Current Thread: "
           << (currentThreadId_.empty() ? "<none>" : currentThreadId_) << "\n";
        ss << "Focused Agent: "
           << (focusedAgentId_.empty() ? "<none>" : focusedAgentId_) << "\n";
        ss << "Locked Threads: " << lockManager_.size() << "\n";
        ss << "Subscribers: " << subscribers_.size() << "\n";
#if defined(_WIN32)
        ss << "PID: " << static_cast<long long>(GetCurrentProcessId()) << "\n";
#else
        ss << "PID: " << static_cast<long long>(getpid()) << "\n";
#endif
        return ss.str();
      });

  std::filesystem::create_directories(getFirmiusHome());

  PurposeLoader::bootstrapDefaults("prompts/");
  HintingLoader::bootstrapDefaults("hinting/");
  shared::ensureBuiltinPermissionProfiles();
  WorkflowLoader::bootstrapDefaults("workflows/");
  shared::ConfigLoader::instance().load();

  auto containers = DockerHost::listContainersWithLabel(OWNER_PID_LABEL);
  for (const auto &container : containers) {
    auto it = container.labels.find(OWNER_PID_LABEL);
    if (it != container.labels.end()) {
      try {
        const long long ownerPid = std::stoll(it->second);
        if (!isPidAlive(ownerPid)) {
          killDockerContainer(container.id);
        }
      } catch (...) {
        Logger::instance().logDebug("Harness: failed to check/kill orphaned Docker container");
      }
    }
  }

  if (!engineListenerRegistered_) {
    Engine::instance().addEventListener(
        [this](const firmius::shared::AppEvent &event) {
          this->routeEngineEvent(event);
        });
    engineListenerRegistered_ = true;
  }

  // Load last session from project-local MRU list first (per workspace dir),
  // then fall back to the global last_session.json.
  {
    const std::string cwd = currentWorkingDirectoryForComparison();
    const auto sessions = loadProjectSessions(cwd);
    if (!sessions.empty()) {
      currentThreadId_ = sessions.front().threadId;
      focusedAgentId_ = sessions.front().focusedAgentId;
    } else {
      std::ifstream sessionFile(getSessionPath());
      if (sessionFile.is_open()) {
        std::string content((std::istreambuf_iterator<char>(sessionFile)),
                            std::istreambuf_iterator<char>());
        sessionFile.close();

        rapidjson::Document doc;
        doc.Parse(content.c_str());
        if (!doc.HasParseError()) {
          if (doc.HasMember("threadId") && doc["threadId"].IsString()) {
            currentThreadId_ = doc["threadId"].GetString();
          }
          if (doc.HasMember("focusedAgentId") &&
              doc["focusedAgentId"].IsString()) {
            focusedAgentId_ = doc["focusedAgentId"].GetString();
          }
        }
      }
    }

    if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
      threadAgentMap_[currentThreadId_] = focusedAgentId_;
    }
  }

  // Model discovery is lazy. Eager startup fetch can fan out into provider-
  // specific background network activity before the user has opened any picker,
  // which is unnecessary churn and has caused startup-time crashes in
  // third-party provider discovery code.
}

void Harness::shutdown() {
  Engine::instance().shutdown();
  mcp::McpManager::shared().shutdown();
  std::vector<std::shared_ptr<PendingPermissionRequest>> pendingRequests;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto &entry : pendingPermissionRequests_) {
      pendingRequests.push_back(entry.second);
    }
    pendingPermissionRequests_.clear();

    shared::Panic::removeExtraInfo(PANIC_INFO_HARNESS_STATE);

    lockManager_.releaseAll();

    persistSessionState(currentThreadId_, focusedAgentId_);
  }
  for (const auto &pending : pendingRequests) {
    {
      std::lock_guard<std::mutex> pendingLock(pending->mutex);
      pending->resolved = true;
      pending->response = PermissionResponse::Deny;
    }
    pending->cv.notify_all();
  }
  joinBackgroundThreads();
}

std::string Harness::newThread(HostCreationOptions hostOptions,
                               const std::string &cwd,
                               const std::string &leadPersona,
                               const std::string &initialMode) {
  std::string threadId;
  int ownerPid = -1;
  bool lockAcquired = false;
  std::string leadAgentId;
  ThreadMetadata metadata;

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    ThreadMetadata newMeta;
    newMeta.title = "New Thread";
    newMeta.hostOptions = hostOptions;
    newMeta.hostIdentifier =
        (hostOptions.type == HostType::Docker) ? "" : "localhost";

    std::string finalCwd = cwd;
    if (hostOptions.type == HostType::Docker &&
        (finalCwd.find("/home/") == 0 || finalCwd.find("/Users/") == 0)) {
      finalCwd = "/work";
    }
    newMeta.cwd = finalCwd;
    std::string effectiveLead = resolveRunnableLeadPersona(leadPersona);
    newMeta.leadPersona = effectiveLead;
    newMeta.initialMode = initialMode;

    threadId = threadManager_.createThread(newMeta);

    int fd = lockManager_.acquire(threadId);
    if (fd < 0) {
      ownerPid = (fd == -2) ? lockManager_.getOwnerPid(threadId) : -1;
    } else {
      lockAcquired = true;
      const std::string previousThreadId = currentThreadId_;
      if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
        threadAgentMap_[currentThreadId_] = focusedAgentId_;
      }

      // Clear agent registry when creating new thread to prevent stale agents
      auto allAgents = AgentRegistry::instance().listAll();
      for (const auto &agentId : allAgents) {
        Engine::instance().terminateAgent(agentId);
      }

      currentThreadId_ = threadId;
      focusedAgentId_.clear();
      clearQueue();
      metadata = threadManager_.getMetadata(threadId);
      if (!previousThreadId.empty() && previousThreadId != threadId) {
        lockManager_.release(previousThreadId);
      }
    }
  }

  if (!lockAcquired) {
    emitEvent(firmius::shared::ThreadLocked{threadId, ownerPid});
    return "";
  }

  if (materializeThreadLeadAgent(threadId, leadAgentId)) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_ == threadId) {
      focusedAgentId_ = leadAgentId;
    }
    threadAgentMap_[threadId] = leadAgentId;
    persistSessionState(currentThreadId_, focusedAgentId_);
  }

  emitEvent(firmius::shared::ThreadChanged{threadId, metadata});
  hooks::EventPayload payload;
  payload.threadId = threadId;
  payload.agentId = leadAgentId;
  payload.extra["cwd"] = metadata.cwd;
  payload.extra["lead_persona"] = metadata.leadPersona;
  auto fired = hooks::HookDispatcher::fire(WorkflowEventKind::ThreadStart, payload);
  if (fired.blocked) {
    emitEvent(firmius::shared::AgentError{
        "", fired.blockReason.empty() ? "thread_start blocked" : fired.blockReason});
  }
  return threadId;
}

std::optional<std::string>
Harness::materializeLeadAgentIdentity(const std::string &threadId) {
  if (threadId.empty()) {
    return std::nullopt;
  }

  ThreadMetadata metadata = threadManager_.getMetadata(threadId);
  const std::string requestedId = shared::StringUtil::generateUuid();
  const std::string leadPersona =
      resolveRunnableLeadPersona(metadata.leadPersona.empty() ? "lead"
                                                              : metadata.leadPersona);

  Engine::instance().summonAgent(threadId, leadPersona, "", true, "", "lead",
                                 "Lead", requestedId, "", "", "", {});

  if (!waitFor(
          [&]() {
            return AgentRegistry::instance().getAgent(requestedId) != nullptr;
          },
          std::chrono::milliseconds(kAgentReadyTimeoutMs))) {
    return std::nullopt;
  }

  return requestedId;
}

bool Harness::materializeThreadLeadAgent(const std::string &threadId,
                                         std::string &agentIdOut) {
  auto leadAgentId = materializeLeadAgentIdentity(threadId);
  if (!leadAgentId.has_value()) {
    return false;
  }
  agentIdOut = *leadAgentId;
  return true;
}

bool Harness::switchThread(const std::string &threadId) {
  ThreadMetadata threadMeta;
  std::map<std::string, AgentManifestEntry> manifest;
  bool alreadyLocked = false;
  int ownerPid = -1;
  bool manifestRecovered = false;
  std::string manifestError;
  std::string metadataError;

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (lockManager_.isLocked(threadId)) {
      alreadyLocked = true;
    }
  }

  if (!alreadyLocked) {
    int fd = lockManager_.acquire(threadId);
    if (fd < 0) {
      ownerPid = lockManager_.getOwnerPid(threadId);
      emitEvent(firmius::shared::ThreadLocked{threadId, ownerPid});
      return false;
    }
  }

  if (!threadManager_.tryGetMetadata(threadId, threadMeta, &metadataError)) {
    if (!alreadyLocked) {
      lockManager_.release(threadId);
    }
    emitEvent(firmius::shared::AgentError{
        "", "Thread '" + threadId +
                "' is incomplete or corrupt and could not be opened: " +
                metadataError});
    return false;
  }

  if (!threadManager_.tryReadAgentManifest(threadId, manifest,
                                           &manifestError)) {
    manifestRecovered = true;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string previousThreadId = currentThreadId_;
    if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
      threadAgentMap_[currentThreadId_] = focusedAgentId_;
    }

    // Clear agent registry when switching threads to prevent stale agents
    // and avoid hitting concurrent subagent limits
    auto allAgents = AgentRegistry::instance().listAll();
    for (const auto &agentId : allAgents) {
      Engine::instance().terminateAgent(agentId);
    }

    currentThreadId_ = threadId;

    auto it = threadAgentMap_.find(threadId);
    if (it != threadAgentMap_.end() &&
        manifest.find(it->second) != manifest.end()) {
      focusedAgentId_ = it->second;
    } else {
      focusedAgentId_.clear();
      focusedAgentId_ =
          chooseBestPersistedAgent(threadManager_, threadId, manifest);
    }

    clearQueue();
    if (!previousThreadId.empty() && previousThreadId != threadId) {
      lockManager_.release(previousThreadId);
    }
  }

  if (manifestRecovered) {
    emitEvent(firmius::shared::AgentError{
        "", "Thread '" + threadId +
                "' has a missing or corrupt agent manifest; continuing without "
                "restoring agents: " +
                manifestError});
  }
  emitEvent(firmius::shared::ThreadChanged{threadId, threadMeta});
  hooks::EventPayload payload;
  payload.threadId = threadId;
  payload.agentId = focusedAgentId_;
  payload.extra["cwd"] = threadMeta.cwd;
  payload.extra["lead_persona"] = threadMeta.leadPersona;
  auto fired = hooks::HookDispatcher::fire(WorkflowEventKind::ThreadResume, payload);
  if (fired.blocked) {
    emitEvent(firmius::shared::AgentError{
        "", fired.blockReason.empty() ? "thread_resume blocked" : fired.blockReason});
  }

  if (focusedAgentId_.empty() && !threadMeta.leadPersona.empty()) {
    std::string materializedAgentId;
    if (materializeThreadLeadAgent(threadId, materializedAgentId)) {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      focusedAgentId_ = materializedAgentId;
      threadAgentMap_[threadId] = materializedAgentId;
      persistSessionState(currentThreadId_, focusedAgentId_);
    }
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!focusedAgentId_.empty()) {
      threadAgentMap_[threadId] = focusedAgentId_;
    }
  }

  return true;
}

bool Harness::resumeLast() {
  std::string threadId;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_.empty()) {
      const std::string cwd = currentWorkingDirectoryForComparison();
      const auto sessions = loadProjectSessions(cwd);
      if (!sessions.empty()) {
        currentThreadId_ = sessions.front().threadId;
        focusedAgentId_ = sessions.front().focusedAgentId;
      } else {
        std::ifstream sessionFile(getSessionPath());
        if (sessionFile.is_open()) {
          std::string content((std::istreambuf_iterator<char>(sessionFile)),
                              std::istreambuf_iterator<char>());
          sessionFile.close();

          rapidjson::Document doc;
          doc.Parse(content.c_str());
          if (!doc.HasParseError() && doc.HasMember("threadId") &&
              doc["threadId"].IsString()) {
            currentThreadId_ = doc["threadId"].GetString();
          }
          if (!doc.HasParseError() && doc.HasMember("focusedAgentId") &&
              doc["focusedAgentId"].IsString()) {
            focusedAgentId_ = doc["focusedAgentId"].GetString();
          }
        }
      }
    }
    threadId = currentThreadId_;
  }

  const std::string currentCwd = currentWorkingDirectoryForComparison();
  auto chooseMostRecentThreadForCwd = [&](const std::string &cwd) {
    if (cwd.empty()) {
      return std::string{};
    }
    std::string bestThreadId;
    uint64_t bestLastActiveAt = 0;
    for (const auto &meta : threadManager_.listThreadsWithMetadata()) {
      if (meta.isBenchmarkRun) {
        continue;
      }
      if (normalizePathForComparison(meta.cwd) != cwd) {
        continue;
      }
      if (bestThreadId.empty() || meta.lastActiveAt > bestLastActiveAt) {
        bestThreadId = meta.threadId;
        bestLastActiveAt = meta.lastActiveAt;
      }
    }
    return bestThreadId;
  };

  if (threadId.empty()) {
    threadId = chooseMostRecentThreadForCwd(currentCwd);
    if (!threadId.empty()) {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      currentThreadId_ = threadId;
      focusedAgentId_.clear();
    }
  }

  if (threadId.empty()) {
    return false;
  }

  if (switchThread(threadId)) {
    return true;
  }

  const std::string fallbackThreadId = chooseMostRecentThreadForCwd(currentCwd);
  if (!fallbackThreadId.empty() && fallbackThreadId != threadId) {
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      currentThreadId_ = fallbackThreadId;
      focusedAgentId_.clear();
      threadAgentMap_.erase(threadId);
    }
    if (switchThread(fallbackThreadId)) {
      return true;
    }
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    currentThreadId_.clear();
    focusedAgentId_.clear();
    threadAgentMap_.erase(threadId);
    persistSessionState("", "");
  }

  Logger::instance().logWarning("Harness: skipped broken last session thread '" + threadId + "' during startup");
  return false;
}

void Harness::send(const std::string &text,
                   const std::vector<firmius::shared::ImageContent> &images) {
  std::string statusMessage;
  if (!dispatchRequestToAgent(currentThreadId_, focusedAgentId_, text, images,
                              statusMessage)) {
    emitEvent(firmius::shared::AgentError{"", statusMessage});
  }
}
bool Harness::sendToThreadAgent(
    const std::string &threadId, const std::string &agentId,
    const std::string &text,
    const std::vector<firmius::shared::ImageContent> &images) {
  std::string statusMessage;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (threadId.empty()) {
      emitEvent(firmius::shared::AgentError{"", "No target thread active"});
      return false;
    }
    currentThreadId_ = threadId;
    focusedAgentId_ = agentId;
    if (!agentId.empty()) {
      threadAgentMap_[threadId] = agentId;
    }
  }
  const bool ok = dispatchRequestToAgent(threadId, agentId, text, images, statusMessage);
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    currentThreadId_ = threadId;
    focusedAgentId_ = agentId;
    if (!agentId.empty()) {
      threadAgentMap_[threadId] = agentId;
    }
  }
  if (!ok) emitEvent(firmius::shared::AgentError{"", statusMessage});
  return ok;
}


bool Harness::dispatchRequestToAgent(const std::string &threadId,
                                     const std::string &preferredAgentId,
                                     const std::string &text,
                                     const std::vector<ImageContent> &images,
                                     std::string &statusMessage) {
  std::string tid;
  ThreadMetadata metadata;
  std::string fid;
  std::string messageId;
  bool needsSummon = false;
  bool needsRestore = false;
  std::string requestedId;
  AgentManifestEntry restoreEntry;
  bool agentRunning = false;
  bool noThread = false;
  bool expansionFailed = false;
  std::string expansionError;
  std::string preparedText;

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_.empty() || threadId.empty() ||
        currentThreadId_ != threadId) {
      noThread = true;
    } else {
      tid = threadId;

      metadata = threadManager_.getMetadata(tid);
      const std::string resolvedLeadPersona =
          resolveRunnableLeadPersona(metadata.leadPersona);
      if (resolvedLeadPersona != metadata.leadPersona) {
        metadata.leadPersona = resolvedLeadPersona;
      }
      metadata.lastActiveAt = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      threadManager_.updateMetadata(tid, metadata);

      messageId = std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());

      try {
        preparedText = firmius::core::artifacts::expandInboundReferences(
            tid, metadata.cwd, text);
      } catch (const std::exception &e) {
        expansionFailed = true;
        expansionError = e.what();
      }

      if (!expansionFailed) {
        fid = preferredAgentId.empty() ? focusedAgentId_ : preferredAgentId;
        auto agent = fid.empty() ? nullptr : AgentRegistry::instance().getAgent(fid);
        if (!fid.empty() && !agent) {
          try {
            const auto manifest = threadManager_.readAgentManifest(tid);
            auto manifestIt = manifest.find(fid);
            if (manifestIt != manifest.end()) {
              needsRestore = true;
              restoreEntry = manifestIt->second;
            }
          } catch (...) {
            Logger::instance().logDebug("Harness: failed to read agent manifest during switch");
          }
        }

        if (fid.empty() || (!agent && !needsRestore)) {
          needsSummon = true;
          requestedId = shared::StringUtil::generateUuid();
          focusedAgentId_ = requestedId;
          threadAgentMap_[tid] = requestedId;
          fid = requestedId;
        } else if (needsRestore) {
          focusedAgentId_ = fid;
          threadAgentMap_[tid] = fid;
        } else {
          focusedAgentId_ = fid;
          threadAgentMap_[tid] = fid;
          if (agent && (agent->isRunning() || agent->isBooting())) {
            agentRunning = true;
          }
        }
      }
    }
  }

  if (noThread) {
    statusMessage = "No current thread active";
    return false;
  }

  if (expansionFailed) {
    statusMessage = "Reference expansion failed: " + expansionError;
    return false;
  }

  auto fireUserMessageHook = [&]() -> bool {
    hooks::EventPayload payload;
    payload.threadId = tid;
    payload.agentId = fid;
    payload.userMessage = preparedText;
    payload.extra["raw_user_message"] = text;
    payload.extra["message_id"] = messageId;
    auto fired = hooks::HookDispatcher::fire(WorkflowEventKind::UserMessage, payload);
    if (fired.blocked) {
      statusMessage =
          fired.blockReason.empty() ? "blocked by user_message hook"
                                    : fired.blockReason;
      return false;
    }
    return true;
  };

  if (needsSummon) {
    if (!fireUserMessageHook()) {
      return false;
    }
    emitEvent(
        firmius::shared::UserMessageSent{messageId, text, tid, fid, images});
    // Note: summonAgent will use the default model from ConfigLoader
    // which is what we want for a brand new lead agent in a thread.
    Engine::instance().summonAgent(tid, metadata.leadPersona, preparedText,
                                   true, "", "aster", "", requestedId, "", "",
                                   "", images);
    statusMessage = "Retry started on lead agent.";
    return true;
  }

  if (needsRestore) {
    try {
      Engine::instance().resumeAgent(tid, fid, restoreEntry.persona,
                                     restoreEntry.parentId,
                                     restoreEntry.friendlyName,
                                     restoreEntry.title,
                                     restoreEntry.persistHistory);
    } catch (const std::exception &e) {
      statusMessage =
          "Failed to restore focused agent: " + std::string(e.what());
      return false;
    }
    if (!waitFor(
            [&]() {
              auto restored = AgentRegistry::instance().getAgent(fid);
              return restored && !restored->isBooting();
            },
            std::chrono::milliseconds(kAgentReadyTimeoutMs))) {
      statusMessage = "Focused agent restore timed out.";
      return false;
    }

    auto restoredAgent = AgentRegistry::instance().getAgent(fid);
    if (!restoredAgent) {
      statusMessage = "Focused agent cannot be restored.";
      return false;
    }
    if (restoredAgent->isRunning()) {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      messageQueue_.push_back({messageId, preparedText, images, tid, fid});
      emitEvent(
          firmius::shared::MessageQueued{messageId, text, tid, fid, images});
      statusMessage = "Retry queued on running agent.";
      return true;
    }

    if (!fireUserMessageHook()) {
      return false;
    }
    emitEvent(
        firmius::shared::UserMessageSent{messageId, text, tid, fid, images});
    Engine::instance().executeTask(fid, preparedText, images);
    statusMessage = "Retry started.";
    return true;
  }

  if (agentRunning) {
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      messageQueue_.push_back({messageId, preparedText, images, tid, fid});
    }
    emitEvent(firmius::shared::MessageQueued{messageId, text, tid, fid, images});
    statusMessage = "Retry queued on running agent.";
    return true;
  }

  if (!fireUserMessageHook()) {
    return false;
  }
  emitEvent(
      firmius::shared::UserMessageSent{messageId, text, tid, fid, images});
  Engine::instance().executeTask(fid, preparedText, images);
  statusMessage = "Retry started.";
  return true;
}

bool Harness::executeWorkflow(const std::string &workflowId,
                              const std::vector<std::string> &args) {
  auto &loader = WorkflowLoader::instance();
  const Workflow *workflow = loader.getWorkflow(workflowId);

  if (!workflow) {
    emitEvent(
        firmius::shared::AgentError{"", "Workflow not found: " + workflowId});
    return false;
  }

  const bool advancedAction =
      workflow->action.kind != WorkflowActionKind::Prompt ||
      !workflow->action.scriptBody.empty() || !workflow->action.scriptFile.empty() ||
      !workflow->action.command.empty() || !workflow->action.stateWrites.empty() ||
      !workflow->action.composeSteps.empty() || workflow->emit.has_value();

  std::string builtPrompt;
  try {
    builtPrompt = workflow->build(args);
  } catch (const std::exception &e) {
    emitEvent(firmius::shared::AgentError{"", "Workflow argument error: " +
                                                  std::string(e.what())});
    return false;
  }

  if (advancedAction) {
    // Ensure the workflow has a concrete target agent id before running any
    // script action that may call agent.ask/agent.reset/agent.execute.
    std::string fid;
    {
      const std::string tid = currentThreadId();
      fid = focusedAgentId();

      std::map<std::string, AgentManifestEntry> manifest;
      try {
        manifest = threadManager_.readAgentManifest(tid);
      } catch (const std::exception &) {
        Logger::instance().logDebug("Harness: failed to read agent manifest for thread focus resolution");
      }

      if (fid.empty()) {
        fid = chooseBestPersistedAgent(threadManager_, tid, manifest);
      }
      if (fid.empty()) {
        std::string materializedAgentId;
        if (materializeThreadLeadAgent(tid, materializedAgentId)) {
          fid = materializedAgentId;
        }
      }
      if (!fid.empty()) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (currentThreadId_ == tid) {
          focusedAgentId_ = fid;
          threadAgentMap_[tid] = fid;
          persistSessionState(currentThreadId_, focusedAgentId_);
        }
      }

      if (!fid.empty() &&
          firmius::core::AgentRegistry::instance().getAgent(fid) == nullptr) {
        try {
          auto it = manifest.find(fid);
          if (it != manifest.end()) {
            const AgentManifestEntry &entry = it->second;
            Engine::instance().resumeAgent(tid, fid, entry.persona,
                                           entry.parentId,
                                           entry.friendlyName, entry.title,
                                           entry.persistHistory);
            waitFor(
                [&]() {
                  auto restored =
                      firmius::core::AgentRegistry::instance().getAgent(fid);
                  return restored && !restored->isBooting();
                },
                std::chrono::milliseconds(kAgentReadyTimeoutMs));
          }
        } catch (const std::exception &) {
          // If restore fails, the script action may still run, but any attempt
          // to target the focused agent will fail with Agent not found.
        }
      }
    }

    hooks::HookState::instance().bindThread(currentThreadId());
    hooks::EventPayload payload;
    payload.threadId = currentThreadId();
    payload.agentId = fid;
    payload.completedWorkflowId = workflow->id;
    payload.userMessage = builtPrompt;
    payload.extra["workflow_id"] = workflow->id;
    payload.extra["slash_command"] =
        workflow->slashCommand.value_or("/" + workflow->id);
    payload.extra["raw_args"] = args.empty() ? std::string{} : args.front();
    for (std::size_t i = 0; i < args.size(); ++i) {
      payload.extra["arg_" + std::to_string(i + 1)] = args[i];
    }

    auto outcome = hooks::HookDispatcher::runAction(*workflow, payload);
    hooks::HookDispatcher::settleOutcome(*workflow, outcome);
    if (workflow->id == "promise.command.promise") {
      Logger::instance().logDebug("Harness: executeWorkflow promise.command.promise"
                " thread_id=" + payload.threadId +
                " agent_id=" + payload.agentId +
                " decision=" + std::to_string(static_cast<int>(outcome.decision)) +
                " has_reminder=" +
                ((outcome.reminderForAgent.has_value() && !outcome.reminderForAgent->empty()) ? "true" : "false") +
                " reminder_size=" +
                std::to_string(outcome.reminderForAgent.has_value() ? outcome.reminderForAgent->size() : 0));
    }
    auto completed =
        hooks::HookDispatcher::fire(WorkflowEventKind::WorkflowComplete, payload);
    if (outcome.decision == hooks::HookOutcome::Decision::Block) {
      emitEvent(firmius::shared::AgentError{
          payload.agentId,
          outcome.blockReason.empty() ? "Workflow blocked" : outcome.blockReason});
      return false;
    }
    for (const auto &reminder : completed.injectedReminders) {
      if (reminder.empty()) {
        continue;
      }
      if (outcome.reminderForAgent.has_value() && !outcome.reminderForAgent->empty()) {
        *outcome.reminderForAgent += "\n" + reminder;
      } else {
        outcome.reminderForAgent = reminder;
      }
    }
    if (outcome.reminderForAgent.has_value() &&
        !firmius::shared::StringUtil::trim(*outcome.reminderForAgent).empty()) {
      std::string statusMessage;
      return dispatchRequestToAgent(currentThreadId(), focusedAgentId(),
                                    *outcome.reminderForAgent, {}, statusMessage);
    }
    return true;
  }

  send(builtPrompt);
  return true;
}

void Harness::abort() {
  std::string focusedAgentId;
  std::string threadId;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    focusedAgentId = focusedAgentId_;
    threadId = currentThreadId_;
  }
  if (focusedAgentId.empty()) {
    return;
  }

  abortAgent(threadId, focusedAgentId);
}

void Harness::abortAgent(const std::string &threadId,
                         const std::string &focusedAgentId) {
  if (focusedAgentId.empty()) {
    return;
  }

  auto agent = AgentRegistry::instance().getAgent(focusedAgentId);
  if (!agent)
    return;

  Engine::instance().cancelAgent(focusedAgentId);
  if (!threadId.empty()) {
    for (const auto &request : listPendingPermissionEscalations(threadId)) {
      if (request.agentId == focusedAgentId) {
        resolvePermissionEscalation(request.requestId,
                                    PermissionResponse::Deny);
      }
    }
  }

  // If focused agent is a subagent (has parentId), only interrupt it
  // If focused agent is a lead agent (no parentId), do NOT cancel async=true
  // subagents The subagent tool already handles cancellation of async=false
  // subagents when parent is interrupted
}

void Harness::abortAndFlushQueuedMessages() {
  std::string focusedAgentId;
  std::string threadId;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    focusedAgentId = focusedAgentId_;
    threadId = currentThreadId_;
  }

  abortAgentAndFlushQueuedMessages(threadId, focusedAgentId);
}

void Harness::abortAgentAndFlushQueuedMessages(const std::string &threadId,
                                               const std::string &focusedAgentId) {
  if (focusedAgentId.empty()) {
    return;
  }

  bool hasQueuedMessages = false;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!threadId.empty()) {
      hasQueuedMessages = std::any_of(
          messageQueue_.begin(), messageQueue_.end(),
          [&](const QueuedMessage &item) {
            return item.agentId == focusedAgentId && item.threadId == threadId;
          });
    }
  }

  if (!hasQueuedMessages) {
    abortAgent(threadId, focusedAgentId);
    return;
  }

  auto agent = AgentRegistry::instance().getAgent(focusedAgentId);
  if (!agent) {
    if (!threadId.empty()) {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      drainQueueForAgent(focusedAgentId, threadId);
    }
    return;
  }
  if (!(agent->isRunning() || agent->isBooting())) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    drainQueueForAgent(focusedAgentId, threadId);
    return;
  }

  // Always cancel the agent when user explicitly requests cancellation.
  // The running/booting check determines whether we wait for agent to settle
  // before draining.
  Engine::instance().cancelAgent(focusedAgentId);
  for (const auto &request : listPendingPermissionEscalations(threadId)) {
    if (request.agentId == focusedAgentId) {
      resolvePermissionEscalation(request.requestId, PermissionResponse::Deny);
    }
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    backgroundThreads_.emplace_back([this, focusedAgentId, threadId]() {
      while (true) {
        auto focusedAgent = AgentRegistry::instance().getAgent(focusedAgentId);
        if (!focusedAgent) {
          return;
        }
        if (!focusedAgent->isRunning() && !focusedAgent->isBooting()) {
          std::lock_guard<std::recursive_mutex> guard(mutex_);
          drainQueueForAgent(focusedAgentId, threadId);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }
}

int Harness::subscribe(
    std::function<void(const firmius::shared::AppEvent &)> callback) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  int id = nextSubscriptionId_++;
  subscribers_[id] = std::move(callback);
  return id;
}

void Harness::unsubscribe(const int &subscriptionId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  subscribers_.erase(subscriptionId);
}

void Harness::publishEvent(const firmius::shared::AppEvent &event) {
  emitEvent(event);
}

std::string Harness::currentThreadId() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return currentThreadId_;
}

std::string Harness::focusedAgentId() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return focusedAgentId_;
}

bool Harness::isDescendant(const std::string &agentId,
                           const std::string &ancestorId, int depth) {
  if (depth > 100)
    return false;
  if (agentId == ancestorId)
    return true;
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent)
    return false;
  const auto &parentId = agent->getContext().identity.parentId;
  if (parentId.empty())
    return false;
  return isDescendant(parentId, ancestorId, depth + 1);
}

void Harness::emitEvent(const firmius::shared::AppEvent &event) {
  // Debug logging for thinking, tool calls, and turn information
  if (debugLogging) {
    std::visit(
        [&](auto &&ev) {
          using T = std::decay_t<decltype(ev)>;

          if constexpr (std::is_same_v<T, AgentThinking>) {
            // Emit thinking chunks in italic
            std::cout << "\x1B[3m" << ev.delta << "\x1B[0m" << std::flush;
          } else if constexpr (std::is_same_v<T, AgentToolCallChunk>) {
            // Track tool call chunks (streaming args)
            auto &state = debugToolStates_[ev.toolCallId];
            state.agentId = ev.agentId;
            // Detect new tool call - Preparing phase
            if (state.name.empty() && state.args.empty()) {
              state.phase = DebugToolPhase::Preparing;
              // Show "Preparing" message when we first detect this tool
              if (!ev.nameDelta.empty()) {
                std::string preparingSummary = SummarizeToolCall(
                    ev.nameDelta, "", firmius::shared::ToolPhase::Preparing);
                std::cout << "\n-> " << preparingSummary << std::endl;
              }
            }
            state.name += ev.nameDelta;
            state.args += ev.argsDelta;
          } else if constexpr (std::is_same_v<T, AgentToolCall>) {
            // Track complete tool call
            auto &state = debugToolStates_[ev.toolCallId];
            state.agentId = ev.agentId;
            // Detect new tool call - Preparing phase
            bool isNewCall = state.name.empty() && state.args.empty();
            if (isNewCall) {
              state.phase = DebugToolPhase::Preparing;
              // Show "Preparing" message first
              std::string preparingSummary =
                  SummarizeToolCall(ev.toolName, ev.toolArgs,
                                    firmius::shared::ToolPhase::Preparing);
              std::cout << "\n-> " << preparingSummary << std::endl;
            }
            if (!ev.toolName.empty())
              state.name = ev.toolName;
            if (!ev.toolArgs.empty())
              state.args = ev.toolArgs;
            debugAgentToolMap_[ev.agentId] = ev.toolCallId;
            state.phase = DebugToolPhase::Called;
            std::string summary = SummarizeToolCall(
                state.name, state.args, firmius::shared::ToolPhase::Called);
            std::cout << "\n--> Called: " << summary << std::endl;
          } else if constexpr (std::is_same_v<T, AgentTurnCompleted>) {
            // Get context size from aggregate metrics
            uint32_t contextSize = ev.aggregateMetrics.tokens.contextSize;
            std::string agentId = ev.agentId;

            // Get current time in ms for deduplication
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();

            // Skip if we printed a turn header for this agent with same context
            // size recently (<100ms)
            std::string dedupKey = agentId + ":" + std::to_string(contextSize);
            bool shouldPrint = true;
            if (lastTurnCompletionTime_ > 0 &&
                now - lastTurnCompletionTime_ < 100) {
              // Too soon - likely duplicate event
              shouldPrint = false;
            }

            if (shouldPrint) {
              lastTurnCompletionTime_ = now;

              // Get agent info for display
              auto agent = AgentRegistry::instance().getAgent(agentId);
              std::string displayName = agentId;
              std::string modelInfo;
              if (agent) {
                const auto &ctx = agent->getContext();
                if (!ctx.identity.friendlyName.empty()) {
                  displayName = ctx.identity.friendlyName;
                }
                modelInfo = ctx.config.modelId;
              }

              // Format: -- jf8s LEAD T1 (CTX: 3824) --
              std::string shortId = agentId.substr(0, 4);
              // Count actual user/assistant messages, not tool results
              int userMsgCount = 0;
              for (const auto &msg : ev.turn.messages) {
                if (msg.role == firmius::shared::Role::User ||
                    msg.role == firmius::shared::Role::Assistant) {
                  userMsgCount++;
                }
              }
              std::string turnNum = std::to_string(userMsgCount);
              std::cout << "\n-- " << shortId << " " << displayName << " T"
                        << turnNum << " (CTX: " << contextSize << ") ["
                        << modelInfo << "] --" << std::endl;
              const std::string contextSummary =
                  summarizeContextWindowMetrics(ev.aggregateMetrics.context, 3);
              if (!contextSummary.empty() && contextSummary != "sent=0") {
                std::cout << "   context> " << contextSummary << std::endl;
              }
            }

            // Show tool results from this turn (Finished phase)
            for (const auto &msg : ev.turn.messages) {
              if (msg.role == firmius::shared::Role::ToolResult) {
                for (const auto &content : msg.content) {
                  if (auto *res =
                          std::get_if<firmius::shared::ToolResultContent>(
                              &content)) {
                    // Truncate result for display (max 60 chars)
                    std::string resultPreview = res->result;
                    // Remove newlines and extra whitespace for compact display
                    for (auto &c : resultPreview) {
                      if (c == '\n' || c == '\r' || c == '\t')
                        c = ' ';
                    }
                    // Trim leading/trailing whitespace
                    size_t start = resultPreview.find_first_not_of(" ");
                    size_t end = resultPreview.find_last_not_of(" ");
                    if (start != std::string::npos &&
                        end != std::string::npos) {
                      resultPreview =
                          resultPreview.substr(start, end - start + 1);
                    }
                    if (resultPreview.size() > 60) {
                      resultPreview = resultPreview.substr(0, 57) + "...";
                    }
                    std::cout << "--> Result [" << res->toolCallId << "] {"
                              << resultPreview << "}" << std::endl;
                  }
                }
              }
            }

            // Clear finished tool calls for this agent
            for (auto it = debugToolStates_.begin();
                 it != debugToolStates_.end();) {
              if (it->second.phase == DebugToolPhase::Finished ||
                  it->second.agentId == agentId) {
                it = debugToolStates_.erase(it);
              } else {
                ++it;
              }
            }
          } else if constexpr (std::is_same_v<T, AgentText>) {
            std::cout << ev.delta << std::flush;
          } else if constexpr (std::is_same_v<T, AgentProcessSpawned>) {
            std::cout << "[Process Spawned] " << ev.command << std::endl;
          } else if constexpr (std::is_same_v<T, AgentProcessOutput>) {
            std::cout << ev.output << std::flush;
          } else if constexpr (std::is_same_v<T, AgentError>) {
            std::cout << "[Error] " << ev.message << std::endl;
          } else if constexpr (std::is_same_v<T, AgentSpawned>) {
            std::string shortId = ev.agentId.substr(0, 4);
            std::cout << "\n-- " << shortId << " " << ev.friendlyName
                      << " (subagent of " << ev.parentId.substr(0, 4) << ") --"
                      << std::endl;
          }
        },
        event);
  }

  std::vector<std::function<void(const firmius::shared::AppEvent &)>> cbs;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto &entry : subscribers_) {
      cbs.push_back(entry.second);
    }
  }
  for (auto &cb : cbs) {
    cb(event);
  }
}

void Harness::routeEngineEvent(const firmius::shared::AppEvent &event) {
  std::vector<AppEvent> toEmit;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    std::string agentId;
    std::string parentId;

    std::visit(
        [&](auto &&ev) {
          using T = std::decay_t<decltype(ev)>;
          if constexpr (std::is_same_v<T, AgentSpawned>) {
            agentId = ev.agentId;
            parentId = ev.parentId;
          } else if constexpr (requires { ev.agentId; }) {
            agentId = ev.agentId;
          }

          if constexpr (requires { ev.parentId; }) {
            parentId = ev.parentId;
          }
        },
        event);

    auto resolveThreadForAgent = [&](const std::string &id) -> std::string {
      if (id.empty()) {
        return "";
      }
      auto resolvedAgent = AgentRegistry::instance().getAgent(id);
      if (resolvedAgent && resolvedAgent->getContext().history) {
        return resolvedAgent->getContext().history->threadId;
      }
      return "";
    };

    bool isAgentInCurrentThread = false;
    if (!agentId.empty()) {
      isAgentInCurrentThread =
          (resolveThreadForAgent(agentId) == currentThreadId_);
    }

    bool isAgentSpawnedEvent = std::holds_alternative<AgentSpawned>(event);
    bool isDescendantOfFocused =
        !focusedAgentId_.empty() && isDescendant(agentId, focusedAgentId_);
    bool shouldRouteEvent =
        isAgentInCurrentThread || isDescendantOfFocused || isAgentSpawnedEvent;
    if (!shouldRouteEvent)
      return;

    std::visit(
        [&](auto &&ev) {
          using T = std::decay_t<decltype(ev)>;
          if constexpr (std::is_same_v<T, AgentSpawned>) {
            auto agent = AgentRegistry::instance().getAgent(ev.agentId);
            std::string providerId;
            std::string modelId;
            uint32_t maxTokens = 0;
            if (agent) {
              const auto &config = agent->getContext().config;
              providerId = config.providerId;
              modelId = config.modelId;
              maxTokens = config.maxTokens.value_or(0);
            }

            AgentSpawned enriched = ev;
            enriched.providerId = providerId;
            enriched.modelId = modelId;
            enriched.maxTokens = maxTokens;
            toEmit.push_back(enriched);
            const std::string spawnedThreadId =
                resolveThreadForAgent(ev.agentId);
            if (!spawnedThreadId.empty()) {
              drainQueueForAgent(ev.agentId, spawnedThreadId);
            }

            if (!currentThreadId_.empty()) {
              try {
                auto manifest =
                    threadManager_.readAgentManifest(currentThreadId_);
                AgentManifestEntry entry;
                entry.persona = ev.personaName;
                entry.parentId = ev.parentId;
                entry.friendlyName = ev.friendlyName;
                entry.title = ev.title;
                entry.persistHistory = ev.persistHistory;
                manifest[ev.agentId] = entry;
                threadManager_.writeAgentManifest(currentThreadId_, manifest);
              } catch (...) {
                Logger::instance().logWarning("Harness: failed to write agent manifest on spawn event");
              }
            }
          } else if constexpr (std::is_same_v<T, AgentTurnCompleted>) {
            toEmit.push_back(ev);
            if (!currentThreadId_.empty() &&
                titleGeneratedThreads_.find(currentThreadId_) ==
                    titleGeneratedThreads_.end()) {
              std::string firstMessage;
              for (const auto &turnMsg : ev.turn.messages) {
                for (const auto &part : turnMsg.content) {
                  if (auto *txt = std::get_if<TextContent>(&part)) {
                    firstMessage += txt->text;
                  }
                }
              }
              if (!firstMessage.empty()) {
                titleGeneratedThreads_.insert(currentThreadId_);
                maybeGenerateTitle(currentThreadId_, firstMessage);
              }
            }
            const std::string agentThreadId = resolveThreadForAgent(ev.agentId);
            if (!agentThreadId.empty()) {
              // Check if this turn contains tool calls that are about to
              // execute
              bool hasPendingToolCalls = false;
              for (const auto &turnMsg : ev.turn.messages) {
                if (turnMsg.role == Role::Assistant) {
                  for (const auto &part : turnMsg.content) {
                    if (std::holds_alternative<
                            firmius::shared::ToolCallContent>(part)) {
                      hasPendingToolCalls = true;
                      break;
                    }
                  }
                }
              }

              // Only allow injection if the agent isn't about to run tools
              bool safeToInject = !hasPendingToolCalls;
              drainQueueForAgent(ev.agentId, agentThreadId, safeToInject);
            }
          } else if constexpr (std::is_same_v<T, AgentFileEdited>) {
            toEmit.push_back(ev);
            const std::string agentThreadId = resolveThreadForAgent(ev.agentId);
            if (!agentThreadId.empty()) {
              const auto peers = listFleetPeers(ev.agentId, agentThreadId);
              auto editorAgent = AgentRegistry::instance().getAgent(ev.agentId);
              std::string editorName = ev.agentId;
              if (editorAgent &&
                  !editorAgent->getContext().identity.friendlyName.empty()) {
                editorName = editorAgent->getContext().identity.friendlyName;
              }
              for (const auto &peerId : peers) {
                std::string msg = "Fleet edit notice: peer '" + editorName +
                                  "' edited " + ev.path + ".";
                if (ev.addedLines > 0 || ev.removedLines > 0) {
                  msg += "\nChange size: +" + std::to_string(ev.addedLines) +
                         " / -" + std::to_string(ev.removedLines) + ".";
                }
                const std::string diffPreview =
                    trimFleetDiffPreview(ev.diffPreview);
                if (!diffPreview.empty()) {
                  msg += "\n\nDiff preview:\n" + diffPreview;
                }
                msg +=
                    "\n\nRe-read this surface before further edits or verification.";
                queueInternalMessage(peerId, agentThreadId, msg);
              }
            }
          } else if constexpr (std::is_same_v<T, AgentError>) {
            toEmit.push_back(ev);
            const std::string agentThreadId = resolveThreadForAgent(ev.agentId);
            if (!agentThreadId.empty()) {
              drainQueueForAgent(ev.agentId, agentThreadId);
              drainInternalQueueForAgent(ev.agentId, agentThreadId);
              failOwnedLocks(ev.agentId, agentThreadId,
                             "Owner agent error: " + ev.message);
            }
          } else if constexpr (std::is_same_v<T, AgentInterrupted>) {
            toEmit.push_back(ev);
            const std::string agentThreadId = resolveThreadForAgent(ev.agentId);
            if (!agentThreadId.empty()) {
              drainQueueForAgent(ev.agentId, agentThreadId);
              drainInternalQueueForAgent(ev.agentId, agentThreadId);
              failOwnedLocks(ev.agentId, agentThreadId,
                             "Owner agent interrupted.");
            }
          } else if constexpr (std::is_same_v<T, AgentFinished>) {
            toEmit.push_back(ev);
            const std::string agentThreadId = resolveThreadForAgent(ev.agentId);
            if (!agentThreadId.empty()) {
              drainQueueForAgent(ev.agentId, agentThreadId);
              drainInternalQueueForAgent(ev.agentId, agentThreadId);
              failOwnedLocks(ev.agentId, agentThreadId,
                             "Owner agent exited: " + ev.outcome.text);
            }
          } else if constexpr (std::is_same_v<T, ThreadChanged> ||
                               std::is_same_v<T, ThreadLocked> ||
                               std::is_same_v<T, ThreadDeleted> ||
                               std::is_same_v<T, ConfigUpdated> ||
                               std::is_same_v<T, ThreadTitleUpdated> ||
                               std::is_same_v<T, MessageQueued> ||
                               std::is_same_v<T, MessageDequeued> ||
                               std::is_same_v<T, UserMessageSent>) {
            toEmit.push_back(ev);
          } else {
            toEmit.push_back(ev);
          }
        },
        event);
  }

  for (const auto &ev : toEmit) {
    emitEvent(ev);
  }
}

void Harness::deleteThread(const std::string &threadId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  if (threadId == currentThreadId_) {
    emitEvent(firmius::shared::AgentError{
        "", "Cannot delete the currently active thread"});
    return;
  }

  lockManager_.release(threadId);

  threadAgentMap_.erase(threadId);
  threadManager_.deleteThread(threadId);
  emitEvent(firmius::shared::ThreadDeleted{threadId});
}

void Harness::releaseThreadLock(const std::string &threadId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  lockManager_.release(threadId);
}

std::vector<ThreadMetadata> Harness::listThreads() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return threadManager_.listThreadsWithMetadata();
}

ThreadMetadata Harness::getThreadMetadata(const std::string &threadId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return threadManager_.getMetadata(threadId);
}

int Harness::getThreadLockOwnerPid(const std::string &threadId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (threadId.empty()) {
    return -1;
  }
  return lockManager_.getOwnerPid(threadId);
}

std::vector<std::string> Harness::listAgents(const std::string &threadId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::string tid = threadId.empty() ? currentThreadId_ : threadId;
  if (tid.empty())
    return {};

  std::vector<std::string> agents;
  try {
    auto manifest = threadManager_.readAgentManifest(tid);
    for (const auto &[agentId, entry] : manifest) {
      agents.push_back(agentId);
    }
  } catch (...) {
    Logger::instance().logDebug("Harness: failed to read agent manifest for listAgents");
  }
  return agents;
}

std::vector<shared::ThreadArtifactMetadata>
Harness::listArtifacts(const std::string &threadId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string tid = threadId.empty() ? currentThreadId_ : threadId;
  if (tid.empty()) {
    return {};
  }
  try {
    return threadManager_.listArtifacts(tid);
  } catch (...) {
    Logger::instance().logDebug("Harness: failed to list artifacts for thread");
  }
  return {};
}

bool Harness::setFocusedAgent(const std::string &agentId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty() ||
      !AgentRegistry::instance().getAgent(agentId)) {
    return false;
  }
  focusedAgentId_ = agentId;
  threadAgentMap_[currentThreadId_] = agentId;
  persistSessionState(currentThreadId_, focusedAgentId_);
  return true;
}

bool Harness::switchLeadPersona(const std::string &personaName) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty())
    return false;
  if (!PurposeLoader::isValid(personaName))
    return false;

  auto agent = focusedAgentId_.empty()
                   ? nullptr
                   : AgentRegistry::instance().getAgent(focusedAgentId_);
  if (agent) {
    if (!agent->getContext().identity.parentId.empty()) {
      return false;
    }
    if (agent->isRunning() || agent->isBooting()) {
      return false;
    }
  }

  ThreadMetadata metadata = threadManager_.getMetadata(currentThreadId_);
  metadata.leadPersona = personaName;
  threadManager_.updateMetadata(currentThreadId_, metadata);

  if (!agent) {
    return true;
  }

  Persona persona = PurposeLoader::load(personaName);
  auto &ctx = agent->getMutableContext();
  ctx.identity.name = persona.name;
  ctx.identity.role = persona.title;
  ctx.config.personaName = personaName;
  ctx.permissions.allowedScopes = persona.allowedScopes;

  AgentTurn nudgeTurn;
  nudgeTurn.turnId =
      "system-persona-switch-" +
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());
  Message nudgeMsg;
  nudgeMsg.role = Role::System;
  nudgeMsg.visibility = MessageVisibility::Internal;
  nudgeMsg.content.push_back(TextContent{
      "Lead persona switched to '" + persona.title +
      "'. Follow the new persona instructions for all future turns."});
  nudgeMsg.timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  nudgeTurn.messages.push_back(nudgeMsg);
  ctx.history->turns.push_back(nudgeTurn);

  if (ctx.config.persistHistory) {
    Journaler jnl(ctx.history->threadId, ctx.identity.id);
    jnl.appendTurn(nudgeTurn);
  }

  try {
    auto manifest = threadManager_.readAgentManifest(currentThreadId_);
    auto it = manifest.find(ctx.identity.id);
    if (it != manifest.end()) {
      it->second.persona = personaName;
      it->second.title = persona.title;
      threadManager_.writeAgentManifest(currentThreadId_, manifest);
    }
  } catch (...) {
    Logger::instance().logWarning("Harness: failed to update manifest during persona switch");
  }

  return true;
}

PermissionResponse
Harness::requestPermissionEscalation(PermissionEscalationRequest request) {
  return requestPermissionEscalationWithSuggestions(std::move(request), {});
}

PermissionResponse
Harness::requestPermissionEscalationWithSuggestions(
    PermissionEscalationRequest request,
    std::vector<PermissionSuggestion> suggestions) {
  auto pending = std::make_shared<PendingPermissionRequest>();
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (request.requestId.empty()) {
      request.requestId = "perm-" + std::to_string(++nextPermissionRequestId_);
    }

    // Pack suggestions into the request's wire payload so the TUI sees
    // the same `ruleId` list that the resolve path expects. Also keep
    // the structured suggestions on the pending record for application.
    request.suggestions.clear();
    request.suggestions.reserve(suggestions.size());
    for (const auto &s : suggestions) {
      shared::PermissionSuggestionWire wire;
      wire.label = s.label;
      wire.explanation = s.explanation;
      wire.ruleId = s.rule.id.empty()
                        ? "sugg_" + std::to_string(request.suggestions.size())
                        : s.rule.id;
      wire.category = s.rule.category;
      wire.decision = decisionToWire(s.rule.decision);
      wire.scope = scopeToWire(s.rule.scope);
      wire.comment = s.rule.comment;
      wire.match = s.rule.match;
      wire.defaultSelected = s.defaultSelected;
      request.suggestions.push_back(std::move(wire));
    }

    pending->request = request;
    pending->suggestions = std::move(suggestions);
    // Mirror wire ids back so resolve-with-rules can find them.
    for (size_t i = 0;
         i < pending->suggestions.size() && i < request.suggestions.size();
         ++i) {
      pending->suggestions[i].rule.id = request.suggestions[i].ruleId;
    }
    pendingPermissionRequests_[request.requestId] = pending;
  }

  emitEvent(request);

  std::unique_lock<std::mutex> pendingLock(pending->mutex);
  pending->cv.wait(pendingLock, [&pending] { return pending->resolved; });
  PermissionResponse response = pending->response;
  std::vector<std::string> selected = pending->selectedSuggestionIds;
  std::vector<PermissionSuggestion> suggestionsCopy = pending->suggestions;
  pendingLock.unlock();

  // Apply selected suggestions to the policy engine. Done outside the
  // pending mutex so we don't hold it across writeUser() I/O.
  if (!selected.empty()) {
    // Tag picks with the active mode's id by default — that way
    // switching modes wipes the user's choices, and "ask" mode
    // accumulates a personal allowlist over time without polluting
    // every other mode.
    const auto activeModeId = policyEngine().activeMode().id;
    for (const auto &id : selected) {
      for (const auto &s : suggestionsCopy) {
        if (s.rule.id != id) continue;
        // Generate a stable persistent id; the wire id was synthetic.
        PolicyRule persisted = s.rule;
        persisted.id.clear();
        // Session-scoped picks stay session-scoped (they don't survive
        // restart). Global-scoped picks get tagged with the active
        // mode so they only apply while that mode is selected.
        if (persisted.scope == RuleScope::Global &&
            persisted.modeId.empty()) {
          persisted.modeId = activeModeId;
        }
        policyEngine().upsertRule(std::move(persisted));
      }
    }
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pendingPermissionRequests_.erase(request.requestId);
  }

  return response;
}

PolicyEngine &Harness::policyEngine() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!policyEngine_) {
    std::filesystem::path projectPath;
    try {
      projectPath = std::filesystem::current_path();
    } catch (...) {
      Logger::instance().logDebug("Harness: could not determine current_path for policy engine");
    }
    policyEngine_ = std::make_unique<PolicyEngine>(
        std::filesystem::path{}, projectPath);
  }
  return *policyEngine_;
}

bool Harness::resolvePermissionEscalation(const std::string &requestId,
                                          PermissionResponse response) {
  std::shared_ptr<PendingPermissionRequest> pending;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = pendingPermissionRequests_.find(requestId);
    if (it == pendingPermissionRequests_.end()) {
      return false;
    }
    pending = it->second;
  }

  {
    std::lock_guard<std::mutex> pendingLock(pending->mutex);
    if (pending->resolved) {
      return false;
    }
    pending->resolved = true;
    pending->response = response;
  }

  pending->cv.notify_all();
  emitEvent(firmius::shared::PermissionEscalationResolved{
      pending->request.requestId, pending->request.threadId,
      pending->request.agentId, response});
  return true;
}

bool Harness::resolvePermissionEscalationWithRules(
    const std::string &requestId,
    const std::vector<std::string> &suggestionIds) {
  std::shared_ptr<PendingPermissionRequest> pending;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = pendingPermissionRequests_.find(requestId);
    if (it == pendingPermissionRequests_.end()) {
      return false;
    }
    pending = it->second;
  }

  {
    std::lock_guard<std::mutex> pendingLock(pending->mutex);
    if (pending->resolved) {
      return false;
    }
    pending->resolved = true;
    pending->response = PermissionResponse::AllowAlways;
    pending->selectedSuggestionIds = suggestionIds;
  }

  pending->cv.notify_all();
  emitEvent(firmius::shared::PermissionEscalationResolved{
      pending->request.requestId, pending->request.threadId,
      pending->request.agentId, PermissionResponse::AllowAlways});
  return true;
}

std::vector<PermissionEscalationRequest>
Harness::listPendingPermissionEscalations(const std::string &threadId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<PermissionEscalationRequest> requests;
  requests.reserve(pendingPermissionRequests_.size());
  for (auto &entry : pendingPermissionRequests_) {
    const auto &pending = entry.second;
    if (!pending) {
      continue;
    }
    const auto &request = pending->request;
    if (!threadId.empty() && !request.threadId.empty() &&
        request.threadId != threadId) {
      continue;
    }
    requests.push_back(request);
  }
  return requests;
}

bool Harness::markThreadAsBenchmark(const std::string &threadId,
                                    const std::string &benchmarkId,
                                    const std::string &benchmarkTaskId) {
  if (threadId.empty() || benchmarkId.empty()) {
    return false;
  }

  ThreadMetadata metadata;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      metadata = threadManager_.getMetadata(threadId);
    } catch (...) {
      Logger::instance().logWarning("Harness: failed to read thread metadata for benchmark marking");
      return false;
    }
    metadata.isBenchmarkRun = true;
    metadata.benchmarkId = benchmarkId;
    if (!benchmarkTaskId.empty()) {
      metadata.benchmarkTaskId = benchmarkTaskId;
    }
    if (metadata.title.empty() || metadata.title == "New Thread" ||
        metadata.title.rfind("Benchmark: ", 0) == 0) {
      metadata.title = "Benchmark: " + metadata.benchmarkId;
      if (!metadata.benchmarkTaskId.empty()) {
        metadata.title += " [" + metadata.benchmarkTaskId + "]";
      }
    }
    threadManager_.updateMetadata(threadId, metadata);
  }

  emitEvent(firmius::shared::ThreadMetadataUpdated{threadId, metadata});
  return true;
}

bool Harness::appendSystemMessage(const std::string &agentId,
                                  const std::string &text,
                                  MessageVisibility visibility) {
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent || text.empty()) {
    return false;
  }

  auto &ctx = agent->getMutableContext();
  if (!ctx.history) {
    return false;
  }

  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  AgentTurn turn;
  turn.turnId = "system-note-" + std::to_string(now);

  Message msg;
  msg.role = Role::System;
  msg.visibility = visibility;
  msg.content.push_back(TextContent{text});
  msg.timestamp = static_cast<uint64_t>(now);
  turn.messages.push_back(msg);

  agent->appendHistoryTurn(turn);

  emitEvent(firmius::shared::AgentTurnCompleted{
      agentId, turn, ctx.aggregateMetrics, ctx.identity.parentId});
  return true;
}

std::vector<ModelInfo> Harness::listAllModels() {
  std::vector<std::string> configuredProviderIds;
  std::vector<ModelInfo> snapshot;
  {
    std::lock_guard<std::mutex> lock(modelsMutex_);

    if (modelsLoaded_ || isRefreshingModels_) {
      return cachedModels_;
    }

    cachedModels_.clear();
    cachedModelKeys_.clear();
    loadingModelProviders_.clear();
    isRefreshingModels_ = true;

    for (const auto &providerId :
         provider::ProviderRegistry::instance().listProviderIds()) {
      auto provider = provider::ProviderRegistry::instance().getProvider(providerId);
      if (!provider || !provider->isConfigured()) {
        continue;
      }
      configuredProviderIds.push_back(providerId);
      loadingModelProviders_.insert(providerId);
    }
    snapshot = cachedModels_;
  }

  if (configuredProviderIds.empty()) {
    {
      std::lock_guard<std::mutex> lock(modelsMutex_);
      isRefreshingModels_ = false;
      modelsLoaded_ = true;
    }
    emitEvent(firmius::shared::AppEvent(firmius::shared::ModelsRefreshed{}));
    return snapshot;
  }

  auto remainingProviders =
      std::make_shared<std::atomic<std::size_t>>(configuredProviderIds.size());

  for (const auto &providerId : configuredProviderIds) {
    emitEvent(firmius::shared::AppEvent(
        firmius::shared::ProviderModelsFetchStarted{providerId}));

    {
      std::lock_guard<std::recursive_mutex> threadLock(mutex_);
      backgroundThreads_.emplace_back([this, providerId, remainingProviders]() {
      std::string error;
      auto provider = provider::ProviderRegistry::instance().getProvider(providerId);
      if (!provider) {
        error = "Provider unavailable";
      } else {
        try {
          provider->discoverModels([&](const ModelInfo &discoveredModel) {
            auto model = discoveredModel;
            if (model.provider.empty()) {
              model.provider = providerId;
            }

            bool inserted = false;
            {
              std::lock_guard<std::mutex> lock(modelsMutex_);
              inserted =
                  cachedModelKeys_.insert(modelCacheKey(model, providerId)).second;
              if (inserted) {
                cachedModels_.push_back(model);
              }
            }

            if (inserted) {
              emitEvent(
                  firmius::shared::AppEvent(firmius::shared::ModelDiscovered{
                      model}));
              emitEvent(
                  firmius::shared::AppEvent(firmius::shared::ModelsRefreshed{}));
            }
          });
        } catch (const std::exception &ex) {
          error = ex.what();
        } catch (...) {
          error = "Unknown error";
        }
      }

      bool allFinished = false;
      {
        std::lock_guard<std::mutex> lock(modelsMutex_);
        loadingModelProviders_.erase(providerId);
        if (remainingProviders->fetch_sub(1) == 1) {
          isRefreshingModels_ = false;
          modelsLoaded_ = true;
          allFinished = true;
        }
      }

      emitEvent(firmius::shared::AppEvent(
          firmius::shared::ProviderModelsFetchFinished{providerId, error}));
      emitEvent(firmius::shared::AppEvent(firmius::shared::ModelsRefreshed{}));

      if (allFinished) {
        emitEvent(firmius::shared::AppEvent(firmius::shared::ModelsRefreshed{}));
      }
      });
    }
  }

  return snapshot;
}

std::vector<ModelInfo> Harness::cachedModelsSnapshot() const {
  std::lock_guard<std::mutex> lock(modelsMutex_);
  return cachedModels_;
}

bool Harness::isModelsLoaded() const {
  std::lock_guard<std::mutex> lock(modelsMutex_);
  return modelsLoaded_;
}

std::vector<std::string> Harness::listProvidersFetchingModels() const {
  std::lock_guard<std::mutex> lock(modelsMutex_);
  return {loadingModelProviders_.begin(), loadingModelProviders_.end()};
}

void Harness::invalidateModelCache() {
  {
    std::lock_guard<std::mutex> lock(modelsMutex_);
    cachedModels_.clear();
    cachedModelKeys_.clear();
    loadingModelProviders_.clear();
    isRefreshingModels_ = false;
    modelsLoaded_ = false;
  }
  emitEvent(firmius::shared::AppEvent(firmius::shared::ModelsRefreshed{}));
}

const UserConfig &Harness::getConfig() const {
  return shared::ConfigLoader::instance().getConfig();
}

void Harness::updateConfig(const UserConfig &config) {
  shared::ConfigLoader::instance().updateConfig(config);
}

void Harness::saveConfig() { shared::ConfigLoader::instance().save(); }

void Harness::switchModel(const std::string &providerId,
                          const std::string &modelId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  auto agent = focusedAgentId_.empty()
                   ? nullptr
                   : AgentRegistry::instance().getAgent(focusedAgentId_);
  if (agent) {
    Engine::instance().switchAgentModel(focusedAgentId_, providerId, modelId);
  } else {
    auto config = shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = providerId;
    config.defaultModelId = modelId;
    shared::ConfigLoader::instance().updateConfig(config);
    shared::ConfigLoader::instance().save();
    emitEvent(firmius::shared::ConfigUpdated{});
  }
}

void Harness::switchModel(const std::string &providerId,
                          const std::string &modelId,
                          const std::string &variantName) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  auto agent = focusedAgentId_.empty()
                   ? nullptr
                   : AgentRegistry::instance().getAgent(focusedAgentId_);
  if (agent) {
    Engine::instance().switchAgentModel(focusedAgentId_, providerId, modelId,
                                        variantName);
  } else {
    auto config = shared::ConfigLoader::instance().getConfig();
    config.defaultProviderId = providerId;
    config.defaultModelId = modelId;
    config.defaultModelVariant = variantName;
    shared::ConfigLoader::instance().updateConfig(config);
    shared::ConfigLoader::instance().save();
    emitEvent(firmius::shared::ConfigUpdated{});
  }
}

void Harness::interruptAndSwitchModel(const std::string &providerId,
                                      const std::string &modelId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty()) {
    emitEvent(
        firmius::shared::AgentError{"", "No focused agent to switch model on"});
    return;
  }

  auto agent = AgentRegistry::instance().getAgent(focusedAgentId_);
  if (agent) {
    Engine::instance().cancelAgent(focusedAgentId_);
    if (!currentThreadId_.empty()) {
      for (const auto &request :
           listPendingPermissionEscalations(currentThreadId_)) {
        if (request.agentId == focusedAgentId_) {
          resolvePermissionEscalation(request.requestId,
                                      PermissionResponse::Deny);
        }
      }
    }
  }

  Engine::instance().switchAgentModel(focusedAgentId_, providerId, modelId);
}

UndoResult Harness::undoTurns(int count) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty()) {
    emitEvent(firmius::shared::AgentError{"", "No focused agent for undo"});
    return {};
  }
  auto result = Engine::instance().undoAgentTurns(focusedAgentId_, count);
  return result;
}

UndoResult Harness::undoMessages(int count) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty()) {
    emitEvent(firmius::shared::AgentError{"", "No focused agent for undo"});
    return {};
  }
  auto result = Engine::instance().undoAgentMessages(focusedAgentId_, count);
  return result;
}

UndoResult Harness::undoAfterTimestamp(uint64_t timestamp) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty()) {
    emitEvent(firmius::shared::AgentError{"", "No active thread for undo"});
    return {};
  }
  UndoResult aggregate;
  bool anyApplied = false;
  for (const auto &agentId : listAgents(currentThreadId_)) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent || agent->isRunning()) {
      continue;
    }
    const auto &history = agent->getContext().history;
    if (!history) {
      continue;
    }
    bool hasTurnAfterTimestamp = false;
    for (const auto &turn : history->turns) {
      for (const auto &msg : turn.messages) {
        if (msg.timestamp > timestamp) {
          hasTurnAfterTimestamp = true;
          break;
        }
      }
      if (hasTurnAfterTimestamp) {
        break;
      }
    }
    if (!hasTurnAfterTimestamp) {
      continue;
    }
    auto result = Engine::instance().undoAgentAfterTimestamp(agentId, timestamp);
    aggregate.turnsRemoved += result.turnsRemoved;
    aggregate.restoredTurns += result.restoredTurns;
    aggregate.compactionReversed =
        aggregate.compactionReversed || result.compactionReversed;
    aggregate.willExceedContext =
        aggregate.willExceedContext || result.willExceedContext;
    anyApplied = true;
  }
  if (!anyApplied) {
    emitEvent(firmius::shared::AgentError{"", "No undoable history after timestamp"});
  }
  return aggregate;
}

std::optional<shared::TranscriptUndoAction> Harness::undoTurnsWithRedo(int count) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty()) return std::nullopt;
  return Engine::instance().undoAgentTurnsWithRedo(focusedAgentId_, count);
}

std::optional<shared::TranscriptUndoAction> Harness::undoMessagesWithRedo(int count) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty()) return std::nullopt;
  return Engine::instance().undoAgentMessagesWithRedo(focusedAgentId_, count);
}

std::optional<shared::TranscriptUndoAction>
Harness::undoAfterTimestampWithRedo(uint64_t timestamp) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty()) return std::nullopt;
  return Engine::instance().undoAgentAfterTimestampWithRedo(focusedAgentId_, timestamp);
}

shared::TranscriptRedoEligibility
Harness::evaluateTranscriptRedo(const std::string &undoActionId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty() || undoActionId.empty()) {
    return {};
  }
  return Engine::instance().evaluateTranscriptRedo(currentThreadId_, undoActionId);
}

std::optional<shared::TranscriptRedoAction>
Harness::redoTranscriptUndoAction(const std::string &undoActionId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty() || undoActionId.empty()) {
    return std::nullopt;
  }
  auto result =
      Engine::instance().redoTranscriptUndoAction(focusedAgentId_, undoActionId);
  return result;
}

std::vector<shared::EditBatchSummary>
Harness::listEditBatches(const std::string &threadId,
                         const shared::EditHistoryFilters &filters) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string effectiveThreadId = threadId.empty() ? currentThreadId_ : threadId;
  if (effectiveThreadId.empty()) {
    return {};
  }
  return Engine::instance().listAgentEditBatches(effectiveThreadId, filters);
}

shared::EditUndoEligibility
Harness::evaluateEditBatchUndo(const std::string &editBatchId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty() || editBatchId.empty()) {
    return {};
  }
  return Engine::instance().evaluateEditBatchUndo(currentThreadId_, editBatchId);
}

std::optional<shared::EditUndoAction>
Harness::undoEditBatch(const std::string &editBatchId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty() || editBatchId.empty()) {
    return std::nullopt;
  }
  return Engine::instance().undoEditBatch(focusedAgentId_, editBatchId);
}

shared::EditRedoEligibility
Harness::evaluateEditBatchRedo(const std::string &undoActionId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty() || undoActionId.empty()) {
    return {};
  }
  return Engine::instance().evaluateEditBatchRedo(currentThreadId_, undoActionId);
}

std::optional<shared::EditRedoAction>
Harness::redoEditUndoAction(const std::string &undoActionId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty() || undoActionId.empty()) {
    return std::nullopt;
  }
  return Engine::instance().redoEditUndoAction(focusedAgentId_, undoActionId);
}

// ── Compound rewind ─────────────────────────────────────────────────────
//
// Atomically (well, best-effort atomically — see comment below) rolls back
// transcript turns and any edit batches authored after `targetTurnId`.
//
// "Atomic" here is conditional on the pre-flight passing for every batch
// we plan to undo. Once we start applying edit undos, each one commits to
// disk individually; if a later step fails (eligibility flipped to blocked
// because of a concurrent edit somewhere) we currently cannot re-apply
// the earlier batches. We document this caveat in the result message and
// arrange for the pre-flight to catch the common cases.
//
// Why not also pre-flight transcript undo? Because the transcript-undo
// path doesn't expose an "evaluate" — it just succeeds or returns nullopt.
// In practice it only fails when the agent is running or there's nothing
// to undo, both of which we check separately.
Harness::CompoundRewindResult
Harness::compoundRewind(const std::string &threadId,
                         const std::string &agentId,
                         const std::string &targetTurnId,
                         CompoundRewindMode mode) {
  CompoundRewindResult result;

  if (threadId.empty() || agentId.empty() || targetTurnId.empty()) {
    result.errorMessage = "thread_id, agent_id, and target_turn_id are required";
    return result;
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);

  // ── Resolve the target turn and how many turns sit after it ──
  //
  // We deliberately don't call getAgentHistoryPtr() — that helper insists
  // currentThreadId_ matches even when the caller already passed a
  // threadId. compoundRewind is meant to be addressable for any thread
  // (the daemon may invoke it for a thread that isn't the focused one),
  // so we go straight through the in-memory agent first, then fall back
  // to the thread manager keyed on the caller-supplied threadId.
  std::shared_ptr<shared::AgentHistory> history;
  if (auto agent = AgentRegistry::instance().getAgent(agentId)) {
    auto inMemory = agent->getContext().history;
    if (inMemory && !inMemory->turns.empty()) {
      history = std::make_shared<shared::AgentHistory>(*inMemory);
    }
  }
  if (!history) {
    auto loaded = threadManager_.loadAgentHistory(threadId, agentId);
    if (!loaded.turns.empty()) {
      history = std::make_shared<shared::AgentHistory>(std::move(loaded));
    }
  }
  if (!history || history->turns.empty()) {
    result.errorMessage = "agent has no history";
    return result;
  }
  int targetIdx = -1;
  for (int i = 0; i < static_cast<int>(history->turns.size()); ++i) {
    if (history->turns[i].turnId == targetTurnId) {
      targetIdx = i;
      break;
    }
  }
  if (targetIdx < 0) {
    result.errorMessage = "target turn not found in agent history";
    return result;
  }
  // We rewind to BEFORE the target turn — that is, we discard the target
  // turn itself plus everything after. (Claude Code-style: clicking "rewind
  // to this message" rewinds the user message AND everything that came
  // after, leaving the transcript ready for a fresh prompt.)
  const int turnsAfter = static_cast<int>(history->turns.size()) - targetIdx;
  if (turnsAfter <= 0) {
    result.errorMessage = "nothing to rewind past target turn";
    return result;
  }

  // ── Find the edit batches authored after the target turn ──
  // The "after target turn" set is: any batch whose turnId is the target
  // turn or any later turn. We use the turn index to gate, since batch
  // turn IDs are stable strings keyed by the turn that authored them.
  std::unordered_set<std::string> discardedTurnIds;
  for (int i = targetIdx; i < static_cast<int>(history->turns.size()); ++i) {
    discardedTurnIds.insert(history->turns[i].turnId);
  }

  shared::EditHistoryFilters filters;
  filters.agentId = agentId;
  filters.includeUndone = false;  // already-undone batches are not relevant
  auto allBatches = Engine::instance().listAgentEditBatches(threadId, filters);

  std::vector<shared::EditBatchSummary> targets;
  for (const auto &batch : allBatches) {
    if (discardedTurnIds.count(batch.turnId)) {
      targets.push_back(batch);
    }
  }
  // Newest first — we undo in reverse order so dependencies unwind cleanly.
  std::sort(targets.begin(), targets.end(),
            [](const auto &a, const auto &b) { return a.createdAt > b.createdAt; });

  // ── Mode dispatch ──
  const bool wantsCode = (mode == CompoundRewindMode::RestoreCode ||
                          mode == CompoundRewindMode::RestoreCodeAndConversation);
  const bool wantsTranscript = (mode == CompoundRewindMode::RestoreConversation ||
                                mode == CompoundRewindMode::RestoreCodeAndConversation);

  // ── Pre-flight: check every edit batch we plan to undo ──
  // Build the set of batch ids we're collectively undoing so the
  // eligibility blocker check doesn't reject an earlier batch on the
  // grounds that a later same-turn batch is still applied — that
  // later batch is also being undone in the same compound, so it's
  // not a real blocker.
  std::unordered_set<std::string> coUndoBatchIds;
  for (const auto &batch : targets) {
    coUndoBatchIds.insert(batch.editBatchId);
  }
  if (wantsCode) {
    for (const auto &batch : targets) {
      auto elig = Engine::instance().evaluateEditBatchUndo(
          threadId, batch.editBatchId, coUndoBatchIds);
      if (!elig.undoable) {
        result.errorMessage = "edit batch " + batch.editBatchId + " is not undoable";
        if (!elig.reason.empty()) {
          result.errorMessage += ": " + elig.reason;
        }
        return result;
      }
    }
  }

  // ── Apply edit undos (newest first) ──
  if (wantsCode) {
    for (const auto &batch : targets) {
      try {
        auto undoAction = Engine::instance().undoEditBatch(agentId, batch.editBatchId);
        if (undoAction.resultStatus !=
            shared::EditUndoResultStatus::Succeeded) {
          // The undo refused (most likely RejectedDiverged because the
          // user tampered with a file post-creation). The action was
          // still persisted with the rejection status, so a future
          // redo / inspection has the audit trail. We treat this as a
          // partial rewind: stop here, surface the reason.
          result.applied = false;
          result.errorMessage =
              std::string("partial rewind: edit batch ") +
              batch.editBatchId + " was not undoable (" +
              undoAction.resultJson + ")";
          return result;
        }
        result.editUndoActionIds.push_back(undoAction.undoActionId);
      } catch (const std::exception &e) {
        // Half-applied state: we already undid earlier batches. Surface the
        // partial state rather than try to redo, which would compound the
        // failure if any redo step also fails.
        result.applied = false;
        result.errorMessage =
            std::string("partial rewind: failed undoing edit batch ") +
            batch.editBatchId + ": " + e.what();
        return result;
      }
    }
  }

  // ── Apply transcript undo ──
  // We track whether transcript truncation actually happened. When it
  // does, removedTurns / removedCount drive the redo payload. When the
  // user picked code-only mode we still want to persist a
  // TranscriptUndoAction so /redo can find the linked editUndoActionIds
  // — without that record, the user's code-only undo is invisible to
  // the redo picker. The action just has redoAvailable gated on
  // whether we have anything to redo (turns OR edits).
  std::vector<shared::AgentTurn> removedTurns;
  int removedCount = 0;
  if (wantsTranscript) {
    // We deliberately DON'T route through Engine::undoAgentTurnsWithRedo
    // here for two reasons:
    //   1. It requires the agent to be registered in AgentRegistry. Lazy
    //      thread loading means a freshly-resumed thread has the persisted
    //      agent on disk but no live registration until the first send.
    //   2. It calls applyReverseFileEdits() on the discarded turns, which
    //      means "Restore conversation only" would also reverse files —
    //      the opposite of what the user asked for. (Code restore is
    //      explicit via the wantsCode branch above.)
    //
    // Instead we do an offline transcript-only undo:
    //   * load the persisted history,
    //   * snip the last `turnsAfter` turns,
    //   * persist the resulting history via Journaler::rewriteJournal,
    //   * write a TranscriptUndoAction so a future redo can replay it.
    //
    // The redo payload captures the discarded turns verbatim; we don't
    // try to capture the engine-side aggregateMetrics deltas, which is
    // the same trade-off Engine::undoAgentTurnsWithRedo has historically
    // accepted.
    std::vector<shared::AgentTurn> turns;
    if (auto agent = AgentRegistry::instance().getAgent(agentId)) {
      // Mirror the engine's writer-of-record. If the agent IS live we go
      // through it so its in-memory history stays in sync.
      auto loaded = agent->getMutableContext().history;
      if (loaded) {
        turns = loaded->turns;
      }
    }
    if (turns.empty()) {
      ThreadManager tm(ThreadManager::defaultBasePath());
      auto loaded = tm.loadAgentHistory(threadId, agentId);
      turns = std::move(loaded.turns);
    }
    if (turns.empty()) {
      result.errorMessage = "transcript undo: no history to undo";
      return result;
    }

    // We don't use HistoryEditor::undoTurns here — that helper has a
    // safety floor that refuses to drop below 2 turns (it's designed
    // for "undo last N" with a system+persona prelude assumption). The
    // rewind UX explicitly chose a specific user-message turn to rewind
    // TO, so we trust the targetIdx we computed earlier and just chop
    // everything from targetIdx onward. The user's choice wins over
    // the heuristic floor.
    if (targetIdx < 0 ||
        targetIdx >= static_cast<int>(turns.size())) {
      result.errorMessage = "transcript undo: target turn index out of range";
      return result;
    }
    removedTurns.assign(turns.begin() + targetIdx, turns.end());
    turns.erase(turns.begin() + targetIdx, turns.end());
    removedCount = static_cast<int>(removedTurns.size());

    // Persist the new (truncated) history. Journaler::rewriteJournal
    // performs a full rewrite of the agent's journal table.
    try {
      Journaler jnl(threadId, agentId);
      jnl.rewriteJournal(turns);
      jnl.flush();
    } catch (const std::exception &e) {
      result.errorMessage =
          std::string("transcript undo: failed to persist new history: ") +
          e.what();
      return result;
    }

    // Sync the live agent's in-memory history if it exists, so the next
    // /send doesn't replay the truncated tail.
    if (auto agent = AgentRegistry::instance().getAgent(agentId)) {
      if (agent->getMutableContext().history) {
        agent->getMutableContext().history->turns = turns;
      }
    }
  }

  // ── Persist the TranscriptUndoAction ──
  // We always write this when ANY work happened (transcript OR code).
  // For code-only undos, the action has empty TranscriptRedoPayloads but
  // a populated editUndoActionIds list — that's enough for /redo to find
  // it and replay the edit redos. Without this record, code-only undos
  // were invisible to the redo picker.
  const bool didAnyWork = removedCount > 0 || !result.editUndoActionIds.empty();
  if (didAnyWork) {
    ThreadManager tm(ThreadManager::defaultBasePath());
    shared::TranscriptUndoAction action;
    action.undoActionId = shared::StringUtil::generateUuid();
    action.threadId = threadId;
    action.agentId = agentId;
    action.scopeType = wantsTranscript ? "turns" : "edits_only";
    action.scopeArgJson =
        std::string("{\"count\":") + std::to_string(removedCount) + "}";
    action.createdAt = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    // Redo is available if there's anything to redo: turns we captured
    // OR edit batches we undid. A code-only undo has redoAvailable=true
    // because /redo's edit-replay path can put the files back even
    // though there are no turns to re-append.
    action.redoAvailable = didAnyWork;
    action.reason = "undone";
    action.editUndoActionIds = result.editUndoActionIds;

    std::vector<shared::TranscriptRedoPayload> payloads;
    if (!removedTurns.empty()) {
      shared::TranscriptRedoPayload payload;
      payload.undoActionId = action.undoActionId;
      payload.threadId = threadId;
      payload.agentId = agentId;
      payload.ordinal = 0;
      payload.turns = std::move(removedTurns);
      payloads.push_back(std::move(payload));
    }
    try {
      tm.writeTranscriptUndoAction(threadId, action, payloads);
    } catch (const std::exception &e) {
      // Best-effort — the history is already truncated. Surface the error
      // but don't pretend the rewind failed; the visible effect already
      // happened.
      result.errorMessage =
          std::string("transcript undo: persisted history but failed to write "
                      "redo action: ") +
          e.what();
    }

    result.undoActionId = action.undoActionId;
    result.turnsUndone = removedCount;
  }

  result.applied = true;
  return result;
}

Harness::CompoundRedoResult
Harness::compoundRedo(const std::string &threadId,
                      const std::string &agentId,
                      const std::string &undoActionId,
                      CompoundRedoMode mode) {
  CompoundRedoResult result;

  if (threadId.empty() || agentId.empty() || undoActionId.empty()) {
    result.errorMessage =
        "thread_id, agent_id, and undo_action_id are required";
    return result;
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ThreadManager tm(ThreadManager::defaultBasePath());

  // ── Resolve the undo action and its captured payload ──
  auto undoAction = tm.findTranscriptUndoAction(threadId, undoActionId);
  if (!undoAction.has_value()) {
    result.errorMessage = "transcript undo action not found";
    return result;
  }
  if (!undoAction->redoAvailable) {
    result.errorMessage = "redo is no longer available for this undo action";
    return result;
  }
  auto payloads = tm.loadTranscriptRedoPayloads(threadId, undoActionId);

  const bool wantsCode = (mode == CompoundRedoMode::RestoreCode ||
                          mode == CompoundRedoMode::RestoreCodeAndConversation);
  const bool wantsTranscript =
      (mode == CompoundRedoMode::RestoreConversation ||
       mode == CompoundRedoMode::RestoreCodeAndConversation);

  // ── Apply edit redos in original (forward) order ──
  // The undo actions were recorded newest-first during compoundRewind, so
  // to redo them we walk in reverse — the oldest edit batch redoes first,
  // matching how the agent originally applied them. This matters when a
  // chain edits the same file: the create must come back before the
  // overwrite tries to land.
  if (wantsCode) {
    for (auto it = undoAction->editUndoActionIds.rbegin();
         it != undoAction->editUndoActionIds.rend(); ++it) {
      try {
        auto redoAction = Engine::instance().redoEditUndoAction(agentId, *it);
        if (!redoAction.has_value()) {
          result.applied = false;
          result.errorMessage =
              std::string("partial redo: edit undo action ") + *it +
              " was not redoable";
          return result;
        }
        result.editRedoActionIds.push_back(redoAction->redoActionId);
      } catch (const std::exception &e) {
        result.applied = false;
        result.errorMessage =
            std::string("partial redo: failed redoing edit undo ") + *it +
            ": " + e.what();
        return result;
      }
    }
  }

  // ── Re-append captured turns ──
  // Mirror compoundRewind's offline pattern: load history, append
  // payload turns, persist via Journaler::rewriteJournal, sync the live
  // agent's in-memory history if it exists. We deliberately avoid
  // Engine paths that require AgentRegistry registration, for the same
  // lazy-thread-loading reason as compoundRewind.
  if (wantsTranscript && !payloads.empty()) {
    std::vector<shared::AgentTurn> turns;
    if (auto agent = AgentRegistry::instance().getAgent(agentId)) {
      auto loaded = agent->getMutableContext().history;
      if (loaded) {
        turns = loaded->turns;
      }
    }
    if (turns.empty()) {
      auto loaded = tm.loadAgentHistory(threadId, agentId);
      turns = std::move(loaded.turns);
    }
    int redone = 0;
    // Sort payloads by ordinal so multi-payload actions reapply in the
    // same order they were captured.
    std::sort(payloads.begin(), payloads.end(),
              [](const auto &a, const auto &b) {
                return a.ordinal < b.ordinal;
              });
    for (const auto &payload : payloads) {
      for (const auto &turn : payload.turns) {
        turns.push_back(turn);
        ++redone;
      }
    }

    try {
      Journaler jnl(threadId, agentId);
      jnl.rewriteJournal(turns);
      jnl.flush();
    } catch (const std::exception &e) {
      result.errorMessage =
          std::string("transcript redo: failed to persist new history: ") +
          e.what();
      return result;
    }

    if (auto agent = AgentRegistry::instance().getAgent(agentId)) {
      if (agent->getMutableContext().history) {
        agent->getMutableContext().history->turns = turns;
      }
    }
    result.turnsRedone = redone;
  }

  // ── Mark the undo action as no longer redoable ──
  // Once the user redoes, the captured payload has been re-applied; we
  // don't support double-redo so we flip the flag. The TranscriptRedoPayload
  // rows are kept for audit but findTranscriptUndoAction will report
  // redoAvailable=false next time.
  try {
    tm.markTranscriptUndoRedoAvailability(threadId, undoActionId, false);
  } catch (const std::exception &e) {
    // Non-fatal — the redo already happened on disk. Log via errorMessage
    // so callers who care can surface it, but result.applied = true.
    result.errorMessage =
        std::string("redo applied but failed to mark availability: ") +
        e.what();
  }

  result.applied = true;
  return result;
}

void Harness::compactFocusedAgent() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty()) {
    emitEvent(firmius::shared::AgentError{"", "No focused agent to compact"});
    return;
  }
  auto agent = AgentRegistry::instance().getAgent(focusedAgentId_);
  if (!agent) {
    emitEvent(firmius::shared::AgentError{"", "Focused agent not found"});
    return;
  }
  if (agent->isRunning()) {
    emitEvent(firmius::shared::AgentError{
        "", "Cannot compact while agent is running"});
    return;
  }
  Engine::instance().compactAgent(focusedAgentId_);
}

void Harness::maybeGenerateTitle(const std::string &threadId,
                                 const std::string &firstMessage) {
  backgroundThreads_.emplace_back([this, threadId, firstMessage]() {
    try {
      std::string agentId;
      {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        agentId = threadAgentMap_[threadId];
      }

      auto metadata = threadManager_.getMetadata(threadId);
      auto agent = AgentRegistry::instance().getAgent(agentId);
      if (!agent)
        return;

      const auto &agentConfig = agent->getContext().config;

      // ---- Pick the titler model ----
      //
      // Order of preference, FROM CHEAPEST TO MOST WASTEFUL:
      //   1. UserConfig.purposeRoutes["titler"] resolves to a category in
      //      modelRouterCategories — use the first model in that category.
      //      This is the explicit "I picked a fast cheap titler" knob.
      //   2. UserConfig.defaultRouteCategory — fall back to the user's
      //      "default cheap fleet" if they set one and no titler-specific
      //      override exists.
      //   3. As a last resort, use the agent's CURRENT model. This was the
      //      old behaviour; it's wrong for premium-quota providers because
      //      it spends $$ on every new thread just to pick a 60-char title.
      //      We log a one-line breadcrumb so the user notices and adds a
      //      titler route in settings.
      const auto &userCfg = shared::ConfigLoader::instance().getConfig();
      std::string titlerProviderId;
      std::string titlerModelId;
      std::string titlerVariant;
      auto pickFromCategory =
          [&](const shared::ModelRouteCategory &cat) -> bool {
        if (cat.models.empty()) return false;
        const auto &m = cat.models.front();
        titlerProviderId = m.providerId;
        titlerModelId = m.modelId;
        titlerVariant = m.variantName;
        return true;
      };
      if (auto it = userCfg.purposeRoutes.find("titler");
          it != userCfg.purposeRoutes.end()) {
        auto cat = userCfg.modelRouterCategories.find(it->second);
        if (cat != userCfg.modelRouterCategories.end()) {
          (void)pickFromCategory(cat->second);
        }
      }
      if (titlerProviderId.empty() && !userCfg.defaultRouteCategory.empty()) {
        auto cat =
            userCfg.modelRouterCategories.find(userCfg.defaultRouteCategory);
        if (cat != userCfg.modelRouterCategories.end()) {
          (void)pickFromCategory(cat->second);
        }
      }
      if (titlerProviderId.empty()) {
        titlerProviderId = agentConfig.providerId;
        titlerModelId = agentConfig.modelId;
        titlerVariant = agentConfig.modelVariant;
        // One-time log so the user can wire a real titler preset and stop
        // burning premium quota on auto-generated titles.
        static std::once_flag warnOnce;
        std::call_once(warnOnce, [&]() {
          Logger::instance().logWarning(
              "Harness: title generation falling back to chat model ("
              + titlerProviderId + "/" + titlerModelId + "). Set "
              "userConfig.purposeRoutes[\"titler\"] to a cheap fast route to "
              "avoid burning premium quota on every new thread.");
        });
      }

      auto provider =
          firmius::provider::ProviderRegistry::instance().getProvider(
              titlerProviderId);
      if (!provider) return;

      std::string titlerPrompt = PurposeLoader::load("titler").identityPrompt;
      std::string fullPrompt =
          titlerPrompt + "\n\nUser's first message: " + firstMessage;

      std::string generatedTitle;
      shared::AgentHistory history;
      history.threadId = threadId;
      shared::AgentTurn turn;
      turn.turnId = "titler-turn";
      shared::Message msg;
      msg.role = shared::Role::User;
      msg.content.push_back(shared::TextContent{fullPrompt});
      msg.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      turn.messages.push_back(msg);
      history.turns.push_back(turn);

      firmius::provider::ProviderOptions opts;
      opts.modelId = titlerModelId;
      try {
        auto modelInfo = provider->getModelInfo(titlerModelId);
        for (const auto &v : modelInfo.variants) {
          if (v.variantName == titlerVariant) {
            opts.modelVariantJson = v.extraMetadataJson;
            break;
          }
        }
      } catch (...) {
        Logger::instance().logDebug("Harness: failed to resolve model variant for title generation");
      }
      provider->stream(history, opts, [&](const shared::StreamEvent &ev) {
        if (auto *txt = std::get_if<shared::TextChunk>(&ev)) {
          generatedTitle += txt->delta;
        }
      });

      generatedTitle = StringUtil::trim(generatedTitle);
      if (!generatedTitle.empty() && generatedTitle.length() <= 100) {
        metadata.title = generatedTitle;
        metadata.lastActiveAt = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        threadManager_.updateMetadata(threadId, metadata);
        emitEvent(
            firmius::shared::ThreadTitleUpdated{threadId, generatedTitle});
      }
    } catch (...) {
      Logger::instance().logWarning("Harness: title generation failed for thread");
    }
  });
}

void Harness::drainQueueForAgent(const std::string &agentId,
                                 const std::string &threadId,
                                 bool allowRunningInjection) {
  if (messageQueue_.empty() || agentId.empty() || threadId.empty()) {
    return;
  }

  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    return;
  }
  if (agent->getContext().history &&
      agent->getContext().history->threadId != threadId) {
    return;
  }
  if (agent->isBooting()) {
    return;
  }
  const bool agentRunning = agent->isRunning();
  if (agentRunning && !allowRunningInjection) {
    return;
  }

  std::vector<QueuedMessage> batch;
  std::deque<QueuedMessage> remaining;
  while (!messageQueue_.empty()) {
    QueuedMessage item = std::move(messageQueue_.front());
    messageQueue_.pop_front();
    if (item.agentId == agentId && item.threadId == threadId) {
      batch.push_back(std::move(item));
    } else {
      remaining.push_back(std::move(item));
    }
  }
  messageQueue_ = std::move(remaining);

  if (batch.empty()) {
  
  for (const auto &item : batch) {
    emitEvent(firmius::shared::InternalMessageDequeued{item.id, item.threadId, item.agentId});
  }
    return;
  }

  for (const auto &item : batch) {
    emitEvent(
        firmius::shared::MessageDequeued{item.id, item.threadId, item.agentId});
    emitEvent(firmius::shared::UserMessageSent{item.id, item.text, item.threadId,
                                               item.agentId, item.images});
  }

  auto appendQueuedUserTurn = [&](const QueuedMessage &item) {
    auto &ctx = agent->getMutableContext();
    if (ctx.history) {
      AgentTurn taskTurn;
      taskTurn.turnId =
          "user-task-" + std::to_string(ctx.history->turns.size());
      Message taskMsg;
      taskMsg.role = Role::User;
      taskMsg.content.push_back(TextContent{item.text});

      for (const auto &img : item.images) {
        taskMsg.content.push_back(img);
      }

      auto now = std::chrono::system_clock::now();
      taskMsg.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now.time_since_epoch())
              .count());
      taskTurn.messages.push_back(taskMsg);
      agent->appendHistoryTurn(taskTurn);
    }
  };

  if (agentRunning) {
    for (const auto &item : batch) {
      appendQueuedUserTurn(item);
    }
    return;
  }

  if (batch.size() > 1) {
    for (size_t i = 0; i + 1 < batch.size(); ++i) {
      appendQueuedUserTurn(batch[i]);
    }
  }

  Engine::instance().executeTask(agentId, batch.back().text,
                                 batch.back().images);
}

void Harness::clearQueue() {
  while (!messageQueue_.empty()) {
    messageQueue_.pop_front();
  }
}

void Harness::clearQueueForAgentThread(const std::string &agentId,
                                       const std::string &threadId) {
  if (agentId.empty() || threadId.empty() || messageQueue_.empty()) {
    return;
  }
  std::deque<QueuedMessage> remaining;
  while (!messageQueue_.empty()) {
    QueuedMessage item = std::move(messageQueue_.front());
    messageQueue_.pop_front();
    if (item.agentId == agentId && item.threadId == threadId) {
      continue;
    }
    remaining.push_back(std::move(item));
  }
  messageQueue_ = std::move(remaining);
}

void Harness::drainInternalQueueForAgent(const std::string &agentId,
                                         const std::string &threadId) {
  drainInternalQueueForAgent(agentId, threadId, false);
}

void Harness::drainInternalQueueForAgent(const std::string &agentId,
                                         const std::string &threadId,
                                         bool allowRunningInjection) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (internalQueue_.empty() || agentId.empty() || threadId.empty()) {
    return;
  }
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    return;
  }
  if (agent->getContext().history &&
      agent->getContext().history->threadId != threadId) {
    return;
  }
  if (agent->isBooting()) {
    return;
  }
  const bool agentRunning = agent->isRunning();
  if (agentRunning && !allowRunningInjection) {
    return;
  }

  std::vector<QueuedInternalMessage> batch;
  std::deque<QueuedInternalMessage> remaining;
  while (!internalQueue_.empty()) {
    QueuedInternalMessage item = std::move(internalQueue_.front());
    internalQueue_.pop_front();
    if (item.agentId == agentId && item.threadId == threadId) {
      batch.push_back(std::move(item));
    } else {
      remaining.push_back(std::move(item));
    }
  }
  internalQueue_ = std::move(remaining);
  if (batch.empty()) {
    return;
  }

  // Emit dequeued events for TUI
  for (const auto &item : batch) {
    emitEvent(firmius::shared::InternalMessageDequeued{item.id, item.threadId, item.agentId});
  }

  if (agentRunning) {
    // When agent is running, just append to history - the agent loop will
    // pick it up on the next turn iteration
    for (const auto &item : batch) {
      appendInternalMessage(agent, item.text);
    }
    return;
  }

  // When agent is not running, append and let caller handle resumption
  for (const auto &item : batch) {
    appendInternalMessage(agent, item.text);
  }
}

void Harness::queueInternalMessage(const std::string &agentId,
                                   const std::string &threadId,
                                   const std::string &text) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (agentId.empty() || threadId.empty() || text.empty()) {
    return;
  }
  auto agent = AgentRegistry::instance().getAgent(agentId);
  if (!agent) {
    return;
  }
  if (agent->isBooting() || agent->isRunning()) {
    QueuedInternalMessage item;
    item.id = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    item.text = text;
    item.threadId = threadId;
    item.agentId = agentId;
    internalQueue_.push_back(item);
    emitEvent(firmius::shared::InternalMessageQueued{item.id, text, threadId, agentId});
    return;
  }
  appendInternalMessage(agent, text);
}

void Harness::appendInternalMessage(std::shared_ptr<shared::IAgent> agent,
                                    const std::string &text) {
  if (!agent || text.empty()) {
    return;
  }
  auto &ctx = agent->getMutableContext();
  if (!ctx.history) {
    return;
  }

  AgentTurn nudgeTurn;
  nudgeTurn.turnId =
      "fleet-nudge-" +
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());
  Message nudgeMsg;
  nudgeMsg.role = Role::System;
  nudgeMsg.visibility = MessageVisibility::Internal;
  nudgeMsg.content.push_back(TextContent{text});
  nudgeMsg.timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  nudgeTurn.messages.push_back(nudgeMsg);
  agent->appendHistoryTurn(nudgeTurn);
}

std::string Harness::resolveFleetRoot(const std::string &agentId) {
  if (agentId.empty()) {
    return "";
  }
  auto current = AgentRegistry::instance().getAgent(agentId);
  int depth = 0;
  while (current && depth < 100) {
    const std::string parentId = current->getContext().identity.parentId;
    if (parentId.empty()) {
      return current->getContext().identity.id;
    }
    current = AgentRegistry::instance().getAgent(parentId);
    depth++;
  }
  return agentId;
}

std::vector<std::string> Harness::listFleetPeers(const std::string &agentId,
                                                 const std::string &threadId) {
  std::vector<std::string> peers;
  if (agentId.empty() || threadId.empty()) {
    return peers;
  }
  const std::string root = resolveFleetRoot(agentId);
  if (root.empty()) {
    return peers;
  }
  for (const auto &candidateId : AgentRegistry::instance().listAll()) {
    if (candidateId == agentId) {
      continue;
    }
    auto candidate = AgentRegistry::instance().getAgent(candidateId);
    if (!candidate || !candidate->getContext().history) {
      continue;
    }
    if (candidate->getContext().history->threadId != threadId) {
      continue;
    }
    if (resolveFleetRoot(candidateId) == root) {
      peers.push_back(candidateId);
    }
  }
  return peers;
}

std::size_t Harness::failOwnedLocks(const std::string &agentId,
                                    const std::string &threadId,
                                    const std::string &reason) {
  if (agentId.empty() || threadId.empty()) {
    return 0;
  }
  ThreadManager tm(ThreadManager::defaultBasePath());
  std::size_t failed = 0;
  std::vector<std::string> lockIds;
  tm.mutateFleetState(threadId, [&](FleetState &state) {
    const uint64_t now = nowEpochMs();
    for (auto &lock : state.locks) {
      if (lock.ownerAgentId != agentId) {
        continue;
      }
      if (lock.status == "open" || lock.status.empty()) {
        lock.status = "failed";
        lock.reason = reason;
        lock.updatedAt = now;
        failed++;
        lockIds.push_back(lock.lockId);
      }
    }
  });
  if (failed > 0) {
    std::string joined;
    for (size_t i = 0; i < lockIds.size(); ++i) {
      if (i > 0) {
        joined += ", ";
      }
      joined += lockIds[i];
    }
    std::string msg =
        "You exited with active fleet locks still open. They were marked "
        "failed: " +
        joined + ". Release or fail locks explicitly before finishing.";
    queueInternalMessage(agentId, threadId, msg);
  }
  return failed;
}

bool Harness::retryLastRequest(std::string &statusMessage) {
  std::string threadId;
  std::string preferredAgentId;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_.empty()) {
      statusMessage = "No current thread active.";
      return false;
    }
    threadId = currentThreadId_;
    preferredAgentId = focusedAgentId_;
    clearQueueForAgentThread(preferredAgentId, threadId);
  }

  auto targetAgentId = resolveRetryTargetAgentId(threadId, preferredAgentId);
  if (!targetAgentId.has_value()) {
    statusMessage =
        "No focused agent with restorable history is available in this thread.";
    return false;
  }
  const std::string agentId = *targetAgentId;
  const bool hasResumableTurn =
      snapshotResumableTurnForAgent(threadId, agentId).has_value();

  auto resumedAgent = AgentRegistry::instance().getAgent(agentId);
  if (!resumedAgent) {
    AgentManifestEntry entry;
    try {
      auto manifest = threadManager_.readAgentManifest(threadId);
      auto it = manifest.find(agentId);
      if (it == manifest.end()) {
        statusMessage =
            "Focused agent cannot be restored from persisted history.";
        return false;
      }
      entry = it->second;
      Engine::instance().resumeAgent(threadId, agentId, entry.persona,
                                     entry.parentId, entry.friendlyName,
                                     entry.title, entry.persistHistory);
    } catch (const std::exception &e) {
      statusMessage =
          "Failed to restore focused agent: " + std::string(e.what());
      return false;
    }

    if (!waitFor(
            [&]() {
              auto restored = AgentRegistry::instance().getAgent(agentId);
              return restored && !restored->isBooting();
            },
            std::chrono::milliseconds(kAgentReadyTimeoutMs))) {
      statusMessage = "Focused agent restore timed out.";
      return false;
    }
    resumedAgent = AgentRegistry::instance().getAgent(agentId);
  }

  if (!resumedAgent) {
    statusMessage = "Focused agent cannot be resumed.";
    return false;
  }
  if (resumedAgent->isRunning() || resumedAgent->isBooting()) {
    statusMessage = "Focused agent is busy.";
    return false;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    focusedAgentId_ = agentId;
    threadAgentMap_[threadId] = agentId;
  }

  resumedAgent->clearInterrupt();
  resumedAgent->getMutableContext().state.currentStatus = AgentStatus::Idle;
  try {
    Engine::instance().resumeTask(agentId);
  } catch (const std::exception &e) {
    statusMessage = "Failed to awaken focused agent: " + std::string(e.what());
    return false;
  }

  statusMessage =
      hasResumableTurn
          ? "Resuming focused agent from the last failed or cancelled turn."
          : "Awakening focused agent from existing thread history.";
  return true;
}

std::optional<shared::ThreadMetadata::RetryableRequest>
Harness::snapshotResumableTurnForAgent(const std::string &threadId,
                                       const std::string &agentId) {
  if (threadId.empty() || agentId.empty()) {
    return std::nullopt;
  }

  std::shared_ptr<AgentHistory> history;
  if (auto liveAgent = AgentRegistry::instance().getAgent(agentId)) {
    history = liveAgent->getContext().history;
  }
  AgentHistory persistedHistory;
  if (!history) {
    try {
      persistedHistory = threadManager_.loadAgentHistory(threadId, agentId);
      history = std::make_shared<AgentHistory>(persistedHistory);
    } catch (...) {
      Logger::instance().logDebug("Harness: failed to load agent history for retry resolution");
      return std::nullopt;
    }
  }
  if (!history || history->turns.empty()) {
    return std::nullopt;
  }

  auto isCancellationNotice = [](const Message &message) {
    if (message.role != Role::System) {
      return false;
    }
    for (const auto &part : message.content) {
      if (const auto *notice = std::get_if<NoticeContent>(&part)) {
        if (notice->title == "Agent Cancelled") {
          return true;
        }
      }
    }
    return false;
  };

  for (auto it = history->turns.rbegin(); it != history->turns.rend(); ++it) {
    if (it->turnId.rfind("user-task-", 0) != 0 || it->messages.empty()) {
      continue;
    }

    const auto &message = it->messages.front();
    if (message.role != Role::User) {
      continue;
    }

    std::optional<Role> finalRoleAfterTurn;
    bool sawCancellationNotice = false;
    bool sawCancelledStopReason = false;
    for (auto later = it.base(); later != history->turns.end(); ++later) {
      if (later->messages.empty()) {
        sawCancelledStopReason = sawCancelledStopReason ||
                                 later->stopReason == StopReason::Cancelled;
        continue;
      }
      finalRoleAfterTurn = later->messages.back().role;
      sawCancellationNotice =
          sawCancellationNotice || isCancellationNotice(later->messages.back());
      sawCancelledStopReason =
          sawCancelledStopReason || later->stopReason == StopReason::Cancelled;
    }

    const bool isCancelledTurn = it->turnId.rfind("cancelled-", 0) == 0;
    if (!finalRoleAfterTurn.has_value() ||
        (!isCancelledTurn && !sawCancellationNotice &&
         !sawCancelledStopReason && *finalRoleAfterTurn != Role::Error)) {
      return std::nullopt;
    }

    ThreadMetadata::RetryableRequest request;
    request.targetAgentId = agentId;
    request.turnId = it->turnId;
    request.recordedAt = message.timestamp;
    request.eligible = true;
    for (const auto &part : message.content) {
      if (auto *textPart = std::get_if<TextContent>(&part)) {
        request.text = textPart->text;
      } else if (auto *imagePart = std::get_if<ImageContent>(&part)) {
        request.images.push_back(*imagePart);
      }
    }
    return request;
  }

  return std::nullopt;
}

std::optional<shared::ThreadMetadata::RetryableRequest>
Harness::recoverLastResumableTurnForThread(
    const std::string &threadId, const std::string &preferredAgentId) {
  if (threadId.empty()) {
    return std::nullopt;
  }

  std::vector<std::string> candidateAgentIds;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    appendUniqueAgentId(candidateAgentIds, preferredAgentId);
    if (threadId == currentThreadId_) {
      appendUniqueAgentId(candidateAgentIds, focusedAgentId_);
    }
    auto mapped = threadAgentMap_.find(threadId);
    if (mapped != threadAgentMap_.end()) {
      appendUniqueAgentId(candidateAgentIds, mapped->second);
    }
    try {
      const auto manifest = threadManager_.readAgentManifest(threadId);
      for (const auto &[agentId, entry] : manifest) {
        if (entry.parentId.empty()) {
          appendUniqueAgentId(candidateAgentIds, agentId);
        }
      }
      for (const auto &[agentId, entry] : manifest) {
        if (!entry.parentId.empty()) {
          appendUniqueAgentId(candidateAgentIds, agentId);
        }
      }
    } catch (...) {
      Logger::instance().logDebug("Harness: failed to read manifest for retry target resolution");
    }
  }

  for (const auto &agentId : threadManager_.listAgents(threadId)) {
    appendUniqueAgentId(candidateAgentIds, agentId);
  }

  std::optional<shared::ThreadMetadata::RetryableRequest> latest;
  for (const auto &agentId : candidateAgentIds) {
    auto candidate = snapshotResumableTurnForAgent(threadId, agentId);
    if (!candidate.has_value()) {
      continue;
    }
    if (!latest.has_value() || candidate->recordedAt > latest->recordedAt) {
      latest = candidate;
    }
  }

  return latest;
}

std::optional<std::string>
Harness::resolveRetryTargetAgentId(const std::string &threadId,
                                   const std::string &preferredAgentId) {
  if (threadId.empty()) {
    return std::nullopt;
  }

  if (!preferredAgentId.empty()) {
    return preferredAgentId;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (threadId == currentThreadId_ && !focusedAgentId_.empty()) {
      return focusedAgentId_;
    }
    auto mapped = threadAgentMap_.find(threadId);
    if (mapped != threadAgentMap_.end() && !mapped->second.empty()) {
      return mapped->second;
    }
    try {
      const auto manifest = threadManager_.readAgentManifest(threadId);
      for (const auto &[agentId, entry] : manifest) {
        if (entry.parentId.empty()) {
          return agentId;
        }
      }
      if (!manifest.empty()) {
        return manifest.begin()->first;
      }
    } catch (...) {
      Logger::instance().logDebug("Harness: failed to read manifest for lead agent identity");
    }
  }

  const auto agents = threadManager_.listAgents(threadId);
  if (!agents.empty()) {
    return agents.front();
  }
  return std::nullopt;
}

void Harness::writeInterruptionRecord() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty())
    return;

  ThreadMetadata metadata;
  try {
    metadata = threadManager_.getMetadata(currentThreadId_);
  } catch (...) {
    Logger::instance().logDebug("Harness: failed to read metadata for interruption record");
    return;
  }

  std::string journalDir =
      getFirmiusHome() + "/threads/" + currentThreadId_ + "/journal";
  std::filesystem::create_directories(journalDir);

  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();

  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();

  doc.AddMember("type", "interrupted", a);
  doc.AddMember("timestamp", static_cast<uint64_t>(now), a);

  rapidjson::Value toolsArray(rapidjson::kArrayType);
  auto activeAgents = AgentRegistry::instance().listAll();
  for (const auto &agentId : activeAgents) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent)
      continue;
    const auto &state = agent->getContext().state;
    for (const auto &tcId : state.pendingToolCalls) {
      toolsArray.PushBack(rapidjson::Value(tcId.c_str(), a), a);
    }
  }
  doc.AddMember("inFlightTools", toolsArray, a);

  rapidjson::Value subagentsArray(rapidjson::kArrayType);
  for (const auto &agentId : activeAgents) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (!agent)
      continue;
    if (agent->isRunning()) {
      subagentsArray.PushBack(rapidjson::Value(agentId.c_str(), a), a);
    }
  }
  doc.AddMember("activeSubagents", subagentsArray, a);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  std::string path =
      journalDir + "/interruption_" + std::to_string(now) + ".json";
  std::ofstream file(path);
  if (file.is_open()) {
    file << buffer.GetString();
  }
}

shared::AgentHistory
Harness::getAgentHistory(const std::string &agentId) const {
  std::lock_guard<std::recursive_mutex> lock(
      const_cast<std::recursive_mutex &>(mutex_));
  if (currentThreadId_.empty())
    return {};
  // For tests that use legacy agent IDs (non-UUID), avoid blocking on any
  // background resume/bootstrap by serving persisted history directly.
  if (agentId.find('-') != std::string::npos && AgentRegistry::instance().getAgent(agentId)) {
    auto history = AgentRegistry::instance().getAgent(agentId)->getContext().history;
    if (history) {
      return *history;
    }
  }
  return threadManager_.loadAgentHistory(currentThreadId_, agentId);
}

std::shared_ptr<shared::AgentHistory>
Harness::getAgentHistoryPtr(const std::string &agentId) const {
  std::lock_guard<std::recursive_mutex> lock(
      const_cast<std::recursive_mutex &>(mutex_));
  if (currentThreadId_.empty())
    return nullptr;

  if (auto agent = AgentRegistry::instance().getAgent(agentId)) {
    auto history = agent->getContext().history;
    if (history && !history->turns.empty()) {
      return std::make_shared<shared::AgentHistory>(*history);
    }
  }

  auto history = threadManager_.loadAgentHistory(currentThreadId_, agentId);
  if (history.turns.empty()) {
    return nullptr;
  }
  return std::make_shared<shared::AgentHistory>(std::move(history));
}

std::vector<shared::OAuthAccount>
Harness::getAccounts(const std::string &providerId) {
  auto prov = provider::ProviderRegistry::instance().getProvider(providerId);
  auto oauthProv = dynamic_cast<provider::BaseOAuthProvider *>(prov.get());
  if (oauthProv) {
    return oauthProv->getAccounts();
  }

  auto apiKeyProv = dynamic_cast<provider::BaseAPIKeyProvider *>(prov.get());
  if (apiKeyProv) {
    std::vector<shared::OAuthAccount> result;
    auto accounts = apiKeyProv->getAccounts();
    for (const auto &acc : accounts) {
      shared::OAuthAccount oauthAcc;
      oauthAcc.identifier = acc.identifier;
      oauthAcc.accessToken = acc.apiKey;
      oauthAcc.rateLimited = acc.rateLimited;
      oauthAcc.backoffUntil = acc.backoffUntil;
      oauthAcc.metadata = acc.metadata;
      oauthAcc.metadata["keyPrefix"] = acc.keyPrefix;
      result.push_back(oauthAcc);
    }
    return result;
  }

  return {};
}

void Harness::deleteAccount(const std::string &providerId,
                            const std::string &identifier) {
  auto prov = provider::ProviderRegistry::instance().getProvider(providerId);
  auto oauthProv = dynamic_cast<provider::BaseOAuthProvider *>(prov.get());
  if (oauthProv) {
    oauthProv->deleteAccount(identifier);
    return;
  }

  auto apiKeyProv = dynamic_cast<provider::BaseAPIKeyProvider *>(prov.get());
  if (apiKeyProv) {
    apiKeyProv->deleteAccount(identifier);
    return;
  }
}

std::map<std::string, std::vector<shared::QuotaBucket>>
Harness::getAllQuotas(const std::string &providerId) {
  auto prov = provider::ProviderRegistry::instance().getProvider(providerId);
  auto oauthProv = dynamic_cast<provider::BaseOAuthProvider *>(prov.get());
  if (oauthProv) {
    oauthProv->refreshQuotas();
    return oauthProv->getAllQuotas();
  }
  auto apiKeyProv = dynamic_cast<provider::BaseAPIKeyProvider *>(prov.get());
  if (apiKeyProv && apiKeyProv->supportsQuotaTracking()) {
    apiKeyProv->refreshQuotas();
    return apiKeyProv->getAllQuotas();
  }
  return {};
}

std::map<std::string, std::vector<shared::QuotaBucket>>
Harness::getCachedAllQuotas(const std::string &providerId) {
  auto prov = provider::ProviderRegistry::instance().getProvider(providerId);
  auto oauthProv = dynamic_cast<provider::BaseOAuthProvider *>(prov.get());
  if (oauthProv) {
    return oauthProv->getAllQuotas();
  }
  auto apiKeyProv = dynamic_cast<provider::BaseAPIKeyProvider *>(prov.get());
  if (apiKeyProv && apiKeyProv->supportsQuotaTracking()) {
    return apiKeyProv->getAllQuotas();
  }
  return {};
}

} // namespace firmius::core
