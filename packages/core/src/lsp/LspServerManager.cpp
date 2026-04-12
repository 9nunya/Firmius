#include "lsp/LspServerManager.hpp"

#include <lsp/LspClient.hpp>
#include <lsp/LspProtocol.hpp>
#include <lsp/LspServerSpec.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace firmius::core {

namespace {
namespace fs = std::filesystem;

constexpr size_t kMaxRetainedStderrLines = 200;
constexpr int kClientShutdownTimeoutMs = 2000;
constexpr int kTerminateGraceMs = 1000;
constexpr int kTerminationPollMs = 50;

std::runtime_error makeErrnoError(const std::string& message) {
    const int err = errno;
    return std::runtime_error(message + ": " + std::string(std::strerror(err)));
}

void closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void closePipe(int pipeFds[2]) {
    closeFd(pipeFds[0]);
    closeFd(pipeFds[1]);
}

void createPipe(int pipeFds[2], const std::string& label) {
    pipeFds[0] = -1;
    pipeFds[1] = -1;
    if (::pipe(pipeFds) != 0) {
        throw makeErrnoError("LspServerManager: failed to create " + label + " pipe");
    }
}

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


bool reapIfExited(pid_t& pid) {
    if (pid <= 0) {
        return true;
    }

    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
        pid = -1;
        return true;
    }

    if (waited == -1 && errno == ECHILD) {
        pid = -1;
        return true;
    }

    return false;
}

std::vector<char*> buildArgv(std::vector<std::string>& command) {
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (auto& arg : command) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    return argv;
}

} // namespace

struct LspServerManager::ServerInstance {
    std::string specId;
    std::string projectRoot;
    std::unique_ptr<LspClient> client;
    pid_t pid = -1;
    std::vector<std::string> command;
    std::chrono::steady_clock::time_point lastUsed;
    bool healthy = false;
    int stdinFd = -1;
    int stdoutFd = -1;
    int stderrFd = -1;
    int stderrWakePipe[2]{-1, -1};
    mutable std::mutex stderrMutex;
    std::vector<std::string> stderrLines;
    std::jthread stderrReader;
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
        const fs::path dataRoot("/tmp/firmius-jdtls");
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

    int stdinPipe[2]{-1, -1};
    int stdoutPipe[2]{-1, -1};
    int stderrPipe[2]{-1, -1};

    try {
        createPipe(stdinPipe, "stdin");
        createPipe(stdoutPipe, "stdout");
        createPipe(stderrPipe, "stderr");
    } catch (...) {
        closePipe(stdinPipe);
        closePipe(stdoutPipe);
        closePipe(stderrPipe);
        throw;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const auto error = makeErrnoError("LspServerManager: fork failed for server spec '" + spec.id + "'");
        closePipe(stdinPipe);
        closePipe(stdoutPipe);
        closePipe(stderrPipe);
        throw error;
    }

    if (pid == 0) {
        ::dup2(stdinPipe[0], STDIN_FILENO);
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::dup2(stderrPipe[1], STDERR_FILENO);

        closePipe(stdinPipe);
        closePipe(stdoutPipe);
        closePipe(stderrPipe);

        std::vector<char*> argv = buildArgv(command);
        ::execvp(argv[0], argv.data());

        const int err = errno;
        const std::string message = "LspServerManager: execvp failed for '" + command.front() + "': " + std::string(std::strerror(err)) + "\n";
        (void)::write(STDERR_FILENO, message.c_str(), message.size());
        _exit(127);
    }

    closeFd(stdinPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[1]);

    auto instance = std::make_unique<ServerInstance>();
    instance->specId = spec.id;
    instance->projectRoot = canonicalProjectRoot;
    instance->pid = pid;
    instance->command = command;
    instance->stdinFd = stdinPipe[1];
    instance->stdoutFd = stdoutPipe[0];
    instance->stderrFd = stderrPipe[0];
    instance->lastUsed = std::chrono::steady_clock::now();

    try {
        createPipe(instance->stderrWakePipe, "stderr wake");

        ServerInstance* rawInstance = instance.get();
        instance->stderrReader = std::jthread([rawInstance](std::stop_token stopToken) {
            auto appendLine = [rawInstance](std::string line) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                std::lock_guard<std::mutex> lock(rawInstance->stderrMutex);
                rawInstance->stderrLines.push_back(std::move(line));
                if (rawInstance->stderrLines.size() > kMaxRetainedStderrLines) {
                    const size_t trimCount = rawInstance->stderrLines.size() - kMaxRetainedStderrLines;
                    rawInstance->stderrLines.erase(
                        rawInstance->stderrLines.begin(),
                        rawInstance->stderrLines.begin() + static_cast<std::ptrdiff_t>(trimCount));
                }
            };

            std::string buffer;
            char chunk[1024];
            struct pollfd fds[2]{};
            fds[0].fd = rawInstance->stderrFd;
            fds[0].events = POLLIN;
            fds[1].fd = rawInstance->stderrWakePipe[0];
            fds[1].events = POLLIN;

            while (!stopToken.stop_requested()) {
                fds[0].revents = 0;
                fds[1].revents = 0;

                const int result = ::poll(fds, 2, 500);
                if (result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }

                if (result == 0) {
                    continue;
                }

                if (fds[1].revents & POLLIN) {
                    break;
                }

                if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                    continue;
                }

                const ssize_t bytesRead = ::read(rawInstance->stderrFd, chunk, sizeof(chunk));
                if (bytesRead < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }

                if (bytesRead == 0) {
                    break;
                }

                buffer.append(chunk, static_cast<size_t>(bytesRead));
                size_t newlinePos = std::string::npos;
                while ((newlinePos = buffer.find('\n')) != std::string::npos) {
                    appendLine(buffer.substr(0, newlinePos));
                    buffer.erase(0, newlinePos + 1);
                }
            }

            if (!buffer.empty()) {
                appendLine(std::move(buffer));
            }
        });

        instance->client = std::make_unique<LspClient>(instance->stdinFd,
                                                       instance->stdoutFd,
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

    if (instance.pid > 0) {
        int status = 0;
        const pid_t waited = ::waitpid(instance.pid, &status, WNOHANG);
        if (waited == 0) {
            errno = 0;
            if (::kill(instance.pid, 0) == 0 || errno == EPERM) {
                alive = true;
            } else if (errno == ESRCH) {
                int reapStatus = 0;
                const pid_t reaped = ::waitpid(instance.pid, &reapStatus, WNOHANG);
                if (reaped == instance.pid || (reaped == -1 && errno == ECHILD)) {
                    instance.pid = -1;
                }
            }
        } else if (waited == instance.pid) {
            instance.pid = -1;
        } else if (waited == -1) {
            if (errno == ECHILD) {
                errno = 0;
                if (::kill(instance.pid, 0) == 0 || errno == EPERM) {
                    alive = true;
                } else {
                    instance.pid = -1;
                }
            }
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
            // Best effort while tearing down server process state.
        }
        instance->client.reset();
    }

    closeFd(instance->stdinFd);
    closeFd(instance->stdoutFd);

    reapIfExited(instance->pid);
    if (instance->pid > 0) {
        (void)::kill(instance->pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTerminateGraceMs);
        while (instance->pid > 0 && std::chrono::steady_clock::now() < deadline) {
            if (reapIfExited(instance->pid)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kTerminationPollMs));
        }
    }

    if (instance->pid > 0) {
        (void)::kill(instance->pid, SIGKILL);
        while (instance->pid > 0) {
            if (reapIfExited(instance->pid)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kTerminationPollMs));
        }
    }

    if (instance->stderrWakePipe[1] >= 0) {
        char wake = 'x';
        (void)::write(instance->stderrWakePipe[1], &wake, 1);
    }

    if (instance->stderrReader.joinable()) {
        instance->stderrReader.request_stop();
        instance->stderrReader.join();
    }

    closePipe(instance->stderrWakePipe);
    closeFd(instance->stderrFd);
}

} // namespace firmius::core
