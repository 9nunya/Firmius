#include "lsp/LspServerManager.hpp"

#include "hosts/LocalHost.hpp"
#include <IHost.hpp>
#include <IHostProcess.hpp>
#include <lsp/LspClient.hpp>
#include <lsp/LspProtocol.hpp>
#include <lsp/LspServerSpec.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace firmius::core {

namespace {
namespace fs = std::filesystem;
using firmius::shared::IHost;
using firmius::shared::IHostProcess;
using firmius::shared::ProcessSnapshot;

constexpr size_t kMaxRetainedStderrLines = 200;
constexpr int kClientShutdownTimeoutMs = 2000;
constexpr auto kOutputPollInterval = std::chrono::milliseconds(10);

std::string sanitizeProjectName(std::string name) {
    if (name.empty() || name == "." || name == "/") {
        name = "workspace";
    }

    for (char& ch : name) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '.' && ch != '-' && ch != '_') {
            ch = '_';
        }
    }

    return name;
}

void appendStderrLines(std::vector<std::string>& lines, const std::string& data) {
    size_t start = 0;
    while (start <= data.size()) {
        const size_t newlinePos = data.find('\n', start);
        std::string line = newlinePos == std::string::npos
            ? data.substr(start)
            : data.substr(start, newlinePos - start);

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }

        if (newlinePos == std::string::npos) {
            break;
        }
        start = newlinePos + 1;
    }

    if (lines.size() > kMaxRetainedStderrLines) {
        const size_t trimCount = lines.size() - kMaxRetainedStderrLines;
        lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(trimCount));
    }
}

} // namespace

struct LspServerManager::ServerInstance {
    std::string specId;
    std::string projectRoot;
    std::unique_ptr<LspClient> client;
    std::unique_ptr<IHostProcess> process;
    std::vector<std::string> command;
    std::chrono::steady_clock::time_point lastUsed;
    bool healthy = false;
    ProcessSnapshot lastSnapshot{false, -1, {}, {}, 0.0};
    mutable std::mutex stderrMutex;
    std::vector<std::string> stderrLines;
};

LspServerManager& LspServerManager::instance() {
    static LspServerManager manager;
    return manager;
}

LspServerManager::~LspServerManager() {
    shutdownAll();
}

LspClient* LspServerManager::getOrCreateServer(const LspServerSpec& spec,
                                               const std::string& projectRoot,
                                               int initTimeoutMs) {
    const std::string canonicalProjectRoot = canonicalizeProjectRoot(projectRoot);
    const std::string poolKey = makePoolKey(spec.id, canonicalProjectRoot);

    std::unique_ptr<ServerInstance> staleInstance;
    LspClient* client = nullptr;

    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pool.find(poolKey);
        if (it != m_pool.end()) {
            if (refreshHealthLocked(*it->second)) {
                it->second->lastUsed = std::chrono::steady_clock::now();
                return it->second->client.get();
            }

            staleInstance = std::move(it->second);
            m_pool.erase(it);
        }

        auto [slotIt, inserted] = m_pool.emplace(poolKey, nullptr);
        (void)inserted;
        try {
            slotIt->second = spawnServer(spec, canonicalProjectRoot, initTimeoutMs);
            slotIt->second->lastUsed = std::chrono::steady_clock::now();
            client = slotIt->second->client.get();
        } catch (...) {
            m_pool.erase(slotIt);
            throw;
        }
    } catch (...) {
        if (staleInstance) {
            shutdownServerInstance(std::move(staleInstance));
        }
        throw;
    }

    if (staleInstance) {
        shutdownServerInstance(std::move(staleInstance));
    }

    return client;
}

void LspServerManager::releaseServer(const std::string& specId, const std::string& projectRoot) {
    const std::string canonicalProjectRoot = canonicalizeProjectRoot(projectRoot);
    const std::string poolKey = makePoolKey(specId, canonicalProjectRoot);

    std::unique_ptr<ServerInstance> staleInstance;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pool.find(poolKey);
        if (it == m_pool.end()) {
            return;
        }

        if (refreshHealthLocked(*it->second)) {
            it->second->lastUsed = std::chrono::steady_clock::now();
            return;
        }

        staleInstance = std::move(it->second);
        m_pool.erase(it);
    }

    shutdownServerInstance(std::move(staleInstance));
}

void LspServerManager::shutdownAll() {
    std::vector<std::unique_ptr<ServerInstance>> instances;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        instances.reserve(m_pool.size());
        for (auto& [poolKey, instance] : m_pool) {
            (void)poolKey;
            instances.push_back(std::move(instance));
        }
        m_pool.clear();
    }

    for (auto& instance : instances) {
        shutdownServerInstance(std::move(instance));
    }
}

void LspServerManager::shutdownServer(const std::string& specId, const std::string& projectRoot) {
    const std::string canonicalProjectRoot = canonicalizeProjectRoot(projectRoot);
    const std::string poolKey = makePoolKey(specId, canonicalProjectRoot);
    shutdownServerInstance(extractServerLocked(poolKey));
}

bool LspServerManager::isServerHealthy(const std::string& specId, const std::string& projectRoot) {
    const std::string canonicalProjectRoot = canonicalizeProjectRoot(projectRoot);
    const std::string poolKey = makePoolKey(specId, canonicalProjectRoot);

    std::unique_ptr<ServerInstance> staleInstance;
    bool healthy = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pool.find(poolKey);
        if (it == m_pool.end()) {
            return false;
        }

        healthy = refreshHealthLocked(*it->second);
        if (healthy) {
            it->second->lastUsed = std::chrono::steady_clock::now();
        } else {
            staleInstance = std::move(it->second);
            m_pool.erase(it);
        }
    }

    if (staleInstance) {
        shutdownServerInstance(std::move(staleInstance));
    }

    return healthy;
}

size_t LspServerManager::activeServerCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pool.size();
}

std::vector<std::string> LspServerManager::activeServerIds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> ids;
    ids.reserve(m_pool.size());
    for (const auto& [poolKey, instance] : m_pool) {
        if (instance) {
            ids.push_back(poolKey);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<std::string> LspServerManager::getServerStderr(const std::string& specId,
                                                           const std::string& projectRoot,
                                                           size_t maxLines) const {
    if (maxLines == 0) {
        return {};
    }

    const std::string canonicalProjectRoot = canonicalizeProjectRoot(projectRoot);
    const std::string poolKey = makePoolKey(specId, canonicalProjectRoot);

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pool.find(poolKey);
    if (it == m_pool.end() || !it->second) {
        return {};
    }

    std::lock_guard<std::mutex> stderrLock(it->second->stderrMutex);
    const auto& lines = it->second->stderrLines;
    const size_t count = std::min(maxLines, lines.size());
    return std::vector<std::string>(lines.end() - static_cast<std::ptrdiff_t>(count), lines.end());
}

void LspServerManager::setHostForTesting(std::shared_ptr<firmius::shared::IHost> host) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_host = std::move(host);
}

std::string LspServerManager::canonicalizeProjectRoot(const std::string& projectRoot) {
    const fs::path original = projectRoot.empty() ? fs::path(".") : fs::path(projectRoot);
    std::error_code ec;

    fs::path canonical = fs::weakly_canonical(original, ec);
    if (!ec) {
        return canonical.string();
    }

    canonical = fs::canonical(original, ec);
    if (!ec) {
        return canonical.string();
    }

    canonical = fs::absolute(original, ec);
    if (!ec) {
        return canonical.lexically_normal().string();
    }

    return original.lexically_normal().string();
}

std::string LspServerManager::makePoolKey(const std::string& specId, const std::string& canonicalProjectRoot) {
    return specId + ":" + canonicalProjectRoot;
}

std::string LspServerManager::shellCommandForArgs(const std::vector<std::string>& command) {
    auto quoteArg = [](const std::string& arg) {
        if (arg.find_first_of(" \t\"'") == std::string::npos) {
            return arg;
        }
        std::string quoted = "\"";
        for (const char ch : arg) {
            if (ch == '\\' || ch == '"') {
                quoted.push_back('\\');
            }
            quoted.push_back(ch);
        }
        quoted.push_back('"');
        return quoted;
    };

    std::ostringstream out;
    for (size_t i = 0; i < command.size(); ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << quoteArg(command[i]);
    }
    return out.str();
}

std::string LspServerManager::jdtlsDataRoot() {
    return (fs::temp_directory_path() / "firmius-jdtls").string();
}

std::string LspServerManager::projectNameForPath(const std::string& path) {
    return sanitizeProjectName(fs::path(path).filename().string());
}

std::unique_ptr<LspServerManager::ServerInstance> LspServerManager::spawnServer(const LspServerSpec& spec,
                                                                                const std::string& canonicalProjectRoot,
                                                                                int initTimeoutMs) const {
    std::vector<std::string> command = spec.resolveCommand();
    if (command.empty()) {
        throw std::runtime_error("LspServerManager: no executable found for server spec '" + spec.id + "'");
    }

    if (command.front() == "jdtls") {
        std::error_code ec;
        const fs::path dataRoot(jdtlsDataRoot());
        fs::create_directories(dataRoot, ec);
        if (ec) {
            throw std::runtime_error("LspServerManager: failed to create jdtls data root: " + ec.message());
        }

        const fs::path dataPath = dataRoot / projectNameForPath(canonicalProjectRoot);
        ec.clear();
        fs::create_directories(dataPath, ec);
        if (ec) {
            throw std::runtime_error("LspServerManager: failed to create jdtls workspace: " + ec.message());
        }

        command.push_back("-data");
        command.push_back(dataPath.string());
    }

    auto host = m_host;
    if (!host) {
        host = std::make_shared<LocalHost>();
    }

    auto instance = std::make_unique<ServerInstance>();
    instance->specId = spec.id;
    instance->projectRoot = canonicalProjectRoot;
    instance->command = command;
    instance->lastUsed = std::chrono::steady_clock::now();

    try {
        instance->process = host->spawn(shellCommandForArgs(command), canonicalProjectRoot);

        ServerInstance* rawInstance = instance.get();
        instance->process->onOutput([rawInstance](const std::string& data, bool isError) {
            if (!isError || data.empty()) {
                return;
            }
            std::lock_guard<std::mutex> lock(rawInstance->stderrMutex);
            appendStderrLines(rawInstance->stderrLines, data);
        });

        IHostProcess* process = instance->process.get();
        instance->client = std::make_unique<LspClient>(
            [process](const std::string& data) {
                try {
                    process->write(data);
                    return true;
                } catch (...) {
                    return false;
                }
            },
            [process](std::chrono::milliseconds timeout) {
                const auto before = process->inspect();
                const auto baseline = before.stdoutData.size();
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                while (std::chrono::steady_clock::now() < deadline) {
                    const auto current = process->inspect();
                    if (current.stdoutData.size() > baseline) {
                        return current.stdoutData.substr(baseline);
                    }
                    if (!current.running) {
                        return std::string{};
                    }
                    std::this_thread::sleep_for(kOutputPollInterval);
                }
                return std::string{};
            },
            fileUri(canonicalProjectRoot),
            canonicalProjectRoot);

        if (!instance->client->initialize(initTimeoutMs)) {
            throw std::runtime_error("LspServerManager: initialize failed for server spec '" + spec.id + "'");
        }

        instance->healthy = true;
        return instance;
    } catch (...) {
        shutdownServerInstance(std::move(instance));
        throw;
    }
}

bool LspServerManager::refreshHealthLocked(ServerInstance& instance) {
    bool alive = false;
    if (instance.process) {
        try {
            instance.lastSnapshot = instance.process->inspect();
            alive = instance.lastSnapshot.running;
        } catch (...) {
            alive = false;
        }
    }

    instance.healthy = alive && instance.client && instance.client->isInitialized();
    return instance.healthy;
}

std::unique_ptr<LspServerManager::ServerInstance> LspServerManager::extractServerLocked(const std::string& poolKey) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pool.find(poolKey);
    if (it == m_pool.end()) {
        return nullptr;
    }

    auto instance = std::move(it->second);
    m_pool.erase(it);
    return instance;
}

void LspServerManager::shutdownServerInstance(std::unique_ptr<ServerInstance> instance) const {
    if (!instance) {
        return;
    }

    instance->healthy = false;

    if (instance->client) {
        try {
            instance->client->shutdown(kClientShutdownTimeoutMs);
        } catch (...) {
        }
        instance->client.reset();
    }

    if (instance->process) {
        try {
            instance->process->kill();
        } catch (...) {
        }
        instance->process.reset();
    }
}

} // namespace firmius::core
