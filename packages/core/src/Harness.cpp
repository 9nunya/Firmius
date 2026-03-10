#include "harness/Harness.hpp"
#include "AgentRegistry.hpp"
#include "Engine.hpp"
#include "IHost.hpp"
#include "agents/PurposeLoader.hpp"
#include "hosts/DockerHost.hpp"
#include "hosts/LocalHost.hpp"
#include "persistence/ThreadManager.hpp"
#include "providers/ProviderRegistry.hpp"
#include "providers/oauth/BaseOAuthProvider.hpp"
#include "utils/StringUtil.hpp"
#include <Context.hpp>
#include <EnvLoader.hpp>
#include <Events.hpp>
#include <Panic.hpp>
#include <Serialization.hpp>

#include <memory>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <thread>

#include <iostream>

namespace {
const std::string PANIC_INFO_HARNESS_STATE = "harness_state";
}

namespace firmius::core {

using namespace firmius::shared;

namespace {
const std::string FIRMIUS_DIR = ".firmius";
const std::string SESSION_FILE = "last_session.json";
const std::string OWNER_PID_LABEL = "com.firmius.owner_pid";

std::string getFirmiusHome() {
  const char *home = std::getenv("HOME");
  if (!home) {
    home = "/tmp";
  }
  return std::string(home) + "/" + FIRMIUS_DIR;
}

std::string getSessionPath() { return getFirmiusHome() + "/" + SESSION_FILE; }

bool isPidAlive(pid_t pid) { return kill(pid, 0) == 0 || errno == EPERM; }

size_t curlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

void killDockerContainer(const std::string &containerId) {
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
}
} // namespace

Harness &Harness::instance() {
  static Harness instance;
  return instance;
}

Harness::Harness()
    : threadManager_(getFirmiusHome() + "/threads"), nextSubscriptionId_(0) {}

void Harness::init() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);

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
        ss << "PID: " << getpid() << "\n";
        return ss.str();
      });

  std::filesystem::create_directories(getFirmiusHome());

  PurposeLoader::bootstrapDefaults("prompts/");
  shared::ConfigLoader::instance().load();

  auto containers = DockerHost::listContainersWithLabel(OWNER_PID_LABEL);
  for (const auto &container : containers) {
    auto it = container.labels.find(OWNER_PID_LABEL);
    if (it != container.labels.end()) {
      try {
        pid_t ownerPid = std::stoi(it->second);
        if (!isPidAlive(ownerPid)) {
          killDockerContainer(container.id);
        }
      } catch (...) {
      }
    }
  }

  Engine::instance().addEventListener(
      [this](const firmius::shared::AppEvent &event) {
        this->routeEngineEvent(event);
      });

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
      if (doc.HasMember("focusedAgentId") && doc["focusedAgentId"].IsString()) {
        focusedAgentId_ = doc["focusedAgentId"].GetString();
      }
    }

    if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
      threadAgentMap_[currentThreadId_] = focusedAgentId_;
    }
  }

  // Trigger initial background model fetch
  listAllModels();
}

void Harness::shutdown() {
  Engine::instance().shutdown();
  std::vector<std::jthread> toJoin;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    toJoin = std::move(backgroundThreads_);

    shared::Panic::removeExtraInfo(PANIC_INFO_HARNESS_STATE);

    lockManager_.releaseAll();

    rapidjson::Document doc;
    doc.SetObject();
    auto &a = doc.GetAllocator();

    if (!currentThreadId_.empty()) {
      doc.AddMember("threadId", rapidjson::Value(currentThreadId_.c_str(), a),
                    a);
    }
    if (!focusedAgentId_.empty()) {
      doc.AddMember("focusedAgentId",
                    rapidjson::Value(focusedAgentId_.c_str(), a), a);
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::ofstream sessionFile(getSessionPath());
    if (sessionFile.is_open()) {
      sessionFile << buffer.GetString();
      sessionFile.close();
    }
  }
  toJoin.clear();
}

std::string Harness::newThread(HostCreationOptions hostOptions,
                               const std::string &cwd,
                               const std::string &leadPersona) {
  std::string threadId;
  int ownerPid = -1;
  bool lockAcquired = false;
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
    newMeta.leadPersona = leadPersona;

    threadId = threadManager_.createThread(newMeta);

    int fd = lockManager_.acquire(threadId);
    if (fd < 0) {
      ownerPid = (fd == -2) ? lockManager_.getOwnerPid(threadId) : -1;
    } else {
      lockAcquired = true;
      if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
        threadAgentMap_[currentThreadId_] = focusedAgentId_;
      }
      currentThreadId_ = threadId;
      focusedAgentId_.clear();
      clearQueue();
      metadata = threadManager_.getMetadata(threadId);
    }
  }

  if (!lockAcquired) {
    emitEvent(firmius::shared::ThreadLocked{threadId, ownerPid});
    return "";
  }

  emitEvent(firmius::shared::ThreadChanged{threadId, metadata});
  return threadId;
}

bool Harness::switchThread(const std::string &threadId) {
  ThreadMetadata threadMeta;
  bool alreadyLocked = false;
  int ownerPid = -1;

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

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!currentThreadId_.empty() && !focusedAgentId_.empty()) {
      threadAgentMap_[currentThreadId_] = focusedAgentId_;
    }

    currentThreadId_ = threadId;

    auto manifest = threadManager_.readAgentManifest(threadId);
    for (const auto &[agentId, entry] : manifest) {
      Engine::instance().resumeAgent(threadId, agentId, entry.persona,
                                     entry.parentId, entry.friendlyName,
                                     entry.title, entry.persistHistory);
    }

    auto it = threadAgentMap_.find(threadId);
    if (it != threadAgentMap_.end()) {
      focusedAgentId_ = it->second;
    } else if (!manifest.empty()) {
      focusedAgentId_ = manifest.begin()->first;
    } else {
      focusedAgentId_.clear();
    }

    clearQueue();
    threadMeta = threadManager_.getMetadata(threadId);
  }

  emitEvent(firmius::shared::ThreadChanged{threadId, threadMeta});
  return true;
}

bool Harness::resumeLast() {
  std::string threadId;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_.empty()) {
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
      }
    }
    threadId = currentThreadId_;
  }

  if (threadId.empty())
    return false;
  return switchThread(threadId);
}

void Harness::send(const std::string &text) {
  std::string tid;
  ThreadMetadata metadata;
  std::string fid;
  std::string messageId;
  bool needsSummon = false;
  std::string requestedId;
  bool agentRunning = false;
  bool noThread = false;

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (currentThreadId_.empty()) {
      noThread = true;
    } else {
      tid = currentThreadId_;

      metadata = threadManager_.getMetadata(tid);
      metadata.lastActiveAt = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      threadManager_.updateMetadata(tid, metadata);

      messageId = std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());

      fid = focusedAgentId_;
      if (fid.empty() || !AgentRegistry::instance().getAgent(fid)) {
        needsSummon = true;
        requestedId = shared::StringUtil::generateUuid();
        focusedAgentId_ = requestedId;
        threadAgentMap_[tid] = requestedId;
      } else {
        auto agent = AgentRegistry::instance().getAgent(fid);
        if (agent && (agent->isRunning() || agent->isBooting())) {
          agentRunning = true;
          messageQueue_.push({messageId, text});
        }
      }
    }
  }

  if (noThread) {
    emitEvent(firmius::shared::AgentError{"", "No current thread active"});
    return;
  }

  emitEvent(firmius::shared::UserMessageSent{messageId, text, tid});

  if (needsSummon) {
    Engine::instance().summonAgent(tid, metadata.leadPersona, text, true, "",
                                   "lead", "", requestedId);
    return;
  }

  if (agentRunning) {
    emitEvent(firmius::shared::MessageQueued{messageId, text});
    return;
  }

  Engine::instance().executeTask(fid, text);
}

void Harness::abort() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (focusedAgentId_.empty())
    return;

  auto agent = AgentRegistry::instance().getAgent(focusedAgentId_);
  if (!agent)
    return;

  auto procIds = agent->getBlockingProcessIds();
  for (const auto &procId : procIds) {
    try {
      if (std::all_of(procId.begin(), procId.end(), ::isdigit)) {
        kill(std::stoi(procId), SIGKILL);
      }
      agent->getHost()->killBackgroundProcess(procId);
    } catch (...) {
    }
  }

  agent->interrupt();
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
  std::vector<std::function<void(const firmius::shared::AppEvent &)>> cbs;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto &[id, cb] : subscribers_) {
      cbs.push_back(cb);
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

    bool isAgentInCurrentThread = false;
    if (!agentId.empty()) {
      auto agent = AgentRegistry::instance().getAgent(agentId);
      if (agent) {
        std::string agentThreadId = agent->getContext().history->threadId;
        isAgentInCurrentThread = (agentThreadId == currentThreadId_);
      }
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
            if (ev.agentId == focusedAgentId_) {
              drainQueue();
            }
          } else if constexpr (std::is_same_v<T, AgentCompleted>) {
            toEmit.push_back(ev);
            if (ev.agentId == focusedAgentId_) {
              drainQueue();
            }
          } else if constexpr (std::is_same_v<T, AgentFinished>) {
            toEmit.push_back(ev);
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

std::vector<ThreadMetadata> Harness::listThreads() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return threadManager_.listThreadsWithMetadata();
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
  }
  return agents;
}

bool Harness::setFocusedAgent(const std::string &agentId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty() ||
      !AgentRegistry::instance().getAgent(agentId)) {
    return false;
  }
  focusedAgentId_ = agentId;
  threadAgentMap_[currentThreadId_] = agentId;
  return true;
}

std::vector<ModelInfo> Harness::listAllModels() {
  std::lock_guard<std::mutex> lock(modelsMutex_);

  if (modelsLoaded_) {
    return cachedModels_;
  }

  if (isRefreshingModels_) {
    return {}; // Still loading...
  }

  isRefreshingModels_ = true;
  std::thread([this]() {
    std::vector<ModelInfo> all;
    auto providerIds = provider::ProviderRegistry::instance().listProviderIds();
    for (const auto &pid : providerIds) {
      auto prov = provider::ProviderRegistry::instance().getProvider(pid);
      if (prov) {
        try {
          auto models = prov->listModels();
          all.insert(all.end(), models.begin(), models.end());
        } catch (...) {
        }
      }
    }

    {
      std::lock_guard<std::mutex> innerLock(modelsMutex_);
      cachedModels_ = std::move(all);
      isRefreshingModels_ = false;
      modelsLoaded_ = true;
    }

    // Emit event so TUI knows models are ready
    emitEvent(firmius::shared::AppEvent(firmius::shared::ModelsRefreshed{}));
  }).detach();

  return {};
}

const UserConfig &Harness::getConfig() {
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
    auto procIds = agent->getBlockingProcessIds();
    for (const auto &procId : procIds) {
      try {
        if (std::all_of(procId.begin(), procId.end(), ::isdigit)) {
          kill(std::stoi(procId), SIGKILL);
        }
        agent->getHost()->killBackgroundProcess(procId);
      } catch (...) {
      }
    }
    agent->interrupt();
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
  if (focusedAgentId_.empty()) {
    emitEvent(firmius::shared::AgentError{"", "No focused agent for undo"});
    return {};
  }
  auto result =
      Engine::instance().undoAgentAfterTimestamp(focusedAgentId_, timestamp);
  return result;
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

      const auto &config = agent->getContext().config;
      auto provider =
          firmius::provider::ProviderRegistry::instance().getProvider(
              config.providerId);
      if (!provider)
        return;

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
      opts.modelId = config.modelId;
      try {
        auto modelInfo = provider->getModelInfo(config.modelId);
        for (const auto &v : modelInfo.variants) {
          if (v.variantName == config.modelVariant) {
            opts.modelVariantJson = v.extraMetadataJson;
            break;
          }
        }
      } catch (...) {
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
    }
  });
}

void Harness::drainQueue() {
  if (messageQueue_.empty()) {
    return;
  }

  auto [id, text] = messageQueue_.front();
  std::string agentId = focusedAgentId_;
  messageQueue_.pop();

  emitEvent(firmius::shared::MessageDequeued{id});
  Engine::instance().executeTask(agentId, text);
}

void Harness::clearQueue() {
  while (!messageQueue_.empty()) {
    messageQueue_.pop();
  }
}

void Harness::writeInterruptionRecord() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (currentThreadId_.empty())
    return;

  ThreadMetadata metadata;
  try {
    metadata = threadManager_.getMetadata(currentThreadId_);
  } catch (...) {
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
  return threadManager_.loadAgentHistory(currentThreadId_, agentId);
}

std::shared_ptr<shared::AgentHistory>
Harness::getAgentHistoryPtr(const std::string &agentId) const {
  std::lock_guard<std::recursive_mutex> lock(
      const_cast<std::recursive_mutex &>(mutex_));
  if (!agentId.empty()) {
    auto agent = AgentRegistry::instance().getAgent(agentId);
    if (agent) {
      return agent->getContext().history;
    }
  }
  if (currentThreadId_.empty())
    return std::make_shared<shared::AgentHistory>();
  return std::make_shared<shared::AgentHistory>(
      threadManager_.loadAgentHistory(currentThreadId_, agentId));
}

std::vector<shared::OAuthAccount>
Harness::getAccounts(const std::string &providerId) {
  auto prov = provider::ProviderRegistry::instance().getProvider(providerId);
  auto oauthProv = dynamic_cast<provider::BaseOAuthProvider *>(prov.get());
  if (oauthProv) {
    return oauthProv->getAccounts();
  }
  return {};
}

void Harness::deleteAccount(const std::string &providerId,
                            const std::string &identifier) {
  auto prov = provider::ProviderRegistry::instance().getProvider(providerId);
  auto oauthProv = dynamic_cast<provider::BaseOAuthProvider *>(prov.get());
  if (oauthProv) {
    oauthProv->deleteAccount(identifier);
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
  return {};
}

} // namespace firmius::core
