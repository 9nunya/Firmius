// =============================================================================
// WindsurfLspManager — spawns + supervises one language_server_<platform>
// child process per Windsurf account.
//
// THE PURPOSE OF EVERY LINE BELOW:
// We spent the back half of 2026-04-27 reverse-engineering why every
// chat in Firmius was billing the user's IDE-logged-in Windsurf account
// instead of whichever Firmius-OAuth-stored account was selected. The
// answer turned out to be: when v1 talked to a *running* IDE-spawned
// language_server, the LSP cached its api_key from the IDE's stdin at
// boot and silently ignored the per-request `Metadata.api_key` field we
// were so carefully populating. Eight accounts. One quota burning.
//
// The fix is in this file: we spawn the LSP ourselves with our chosen
// api_key, our chosen CSRF, our chosen port, our chosen database
// directory. The LSP becomes a clean per-account inference back-end.
// No more silent quota theft.
// =============================================================================

#include "providers/WindsurfLspManager.hpp"

#include "utils/GCPHttpClient.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <random>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

namespace firmius::provider {

namespace {

// ---------- proto helpers (single bare codeium_common_pb.Metadata) ----------
// Same wire layout as WindsurfProvider_local.cpp::buildMetadata, kept here
// to avoid leaking that TU's anonymous namespace symbols.

inline void writeVarint(std::string &out, std::uint64_t v) {
  while (v > 0x7f) {
    out.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v & 0x7f));
}

inline void writeStr(std::string &out, int field, std::string_view s) {
  std::uint64_t tag = (static_cast<std::uint64_t>(field) << 3) | 2;
  writeVarint(out, tag);
  writeVarint(out, s.size());
  out.append(s.data(), s.size());
}

constexpr const char *kWindsurfVersion = "2.0.67";
constexpr const char *kExtensionVersion = "1.48.2";
constexpr const char *kIdeName = "windsurf";
constexpr const char *kUserAgent =
    "firmius-windsurf-spawner/1.0 (linux; x86_64)";
constexpr const char *kOrigin = "vscode-file://vscode-app";

}  // namespace

// =============================================================================
// Lifecycle
// =============================================================================

WindsurfLspManager::WindsurfLspManager() = default;

WindsurfLspManager::~WindsurfLspManager() {
  shutdown();
}

void WindsurfLspManager::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &[id, child] : children_) {
    if (child) killChild(*child);
  }
  children_.clear();
}

void WindsurfLspManager::recycle(const std::string &accountId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = children_.find(accountId);
  if (it == children_.end()) return;
  if (it->second) killChild(*it->second);
  children_.erase(it);
}

std::vector<WindsurfLspManager::Endpoint>
WindsurfLspManager::liveEndpoints() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Endpoint> out;
  out.reserve(children_.size());
  for (const auto &[id, child] : children_) {
    if (!child) continue;
    out.push_back({child->pid, child->port, child->csrf, child->accountId});
  }
  return out;
}

// =============================================================================
// Discovery: where does the binary live?
// =============================================================================

std::filesystem::path WindsurfLspManager::resolveBinary() {
  if (!cachedBinary_.empty()) return cachedBinary_;

  // 1. Explicit override — preferred for devboxes and CI.
  if (const char *override = std::getenv("FIRMIUS_WINDSURF_BINARY")) {
    std::filesystem::path p{override};
    if (std::filesystem::exists(p)) {
      cachedBinary_ = p;
      return cachedBinary_;
    }
  }

  // 2. Reuse an installed Windsurf's binary. We do NOT depend on the IDE
  //    being *running*; we just need the on-disk binary. Cross-platform
  //    candidates listed in priority order; first-existing wins.
  const std::vector<std::filesystem::path> candidates = {
      // Linux x86_64 (Arch / Ubuntu / system-wide install)
      "/usr/share/windsurf/resources/app/extensions/windsurf/bin/"
      "language_server_linux_x64",
      "/opt/Windsurf/resources/app/extensions/windsurf/bin/"
      "language_server_linux_x64",
      // Linux ARM64
      "/usr/share/windsurf/resources/app/extensions/windsurf/bin/"
      "language_server_linux_arm",
      // macOS application bundle
      "/Applications/Windsurf.app/Contents/Resources/app/extensions/windsurf/"
      "bin/language_server_macos_x64",
      "/Applications/Windsurf.app/Contents/Resources/app/extensions/windsurf/"
      "bin/language_server_macos_arm",
      // Windows install (handled cosmetically here; spawn path is Linux
      // for now, see WindowsSpawner v3 RFC).
      // TODO: %LOCALAPPDATA%/Programs/Windsurf/resources/app/extensions/
      //       windsurf/bin/language_server_windows_x64.exe
  };
  for (const auto &c : candidates) {
    if (std::filesystem::exists(c)) {
      cachedBinary_ = c;
      return cachedBinary_;
    }
  }

  // 3. Firmius cache (auto-download lands here in a future patch).
  if (const char *home = std::getenv("HOME")) {
    std::filesystem::path cached = std::filesystem::path{home} /
                                   ".firmius" / "bin" / "windsurf" /
                                   "language_server_linux_x64";
    if (std::filesystem::exists(cached)) {
      cachedBinary_ = cached;
      return cachedBinary_;
    }
  }

  return {};
}

// =============================================================================
// Helpers
// =============================================================================

int WindsurfLspManager::allocateEphemeralPort() {
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(s);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
    ::close(s);
    return 0;
  }
  int port = ntohs(addr.sin_port);
  ::close(s);
  return port;
}

std::string WindsurfLspManager::makeUuid() {
  // v4-ish: 32 hex digits with dashes at canonical positions. We lean on
  // std::random_device; it's good enough for a CSRF and a pipe path.
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> dist;
  std::uint64_t a = dist(rng);
  std::uint64_t b = dist(rng);
  // Force version 4 + variant 10
  a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xFFFF),
                static_cast<unsigned>(a & 0xFFFF),
                static_cast<unsigned>(b >> 48),
                static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
  return std::string{buf};
}

std::string
WindsurfLspManager::buildStdinMetadata(const std::string &apiKey) {
  std::string out;
  // Field layout VERIFIED LIVE in scratch/windsurf_wire/SPEC.md against the
  // codeium_common_pb.Metadata wire bytes from a real IDE chat capture.
  writeStr(out, 1, kIdeName);            // ide_name
  writeStr(out, 2, kExtensionVersion);   // extension_version
  writeStr(out, 3, apiKey);              // api_key  ← OURS, not the IDE's
  writeStr(out, 4, "en");                // locale
  writeStr(out, 7, kWindsurfVersion);    // ide_version
  writeStr(out, 12, kIdeName);           // extension_name
  return out;
}

// =============================================================================
// Parent pipe (Unix domain socket the LSP connects to on boot)
// =============================================================================

bool WindsurfLspManager::startParentPipe(Child &child) {
  // Build a unique socket path under /tmp. The IDE uses
  // `/tmp/server_<random>` and the LSP doesn't seem to care about the
  // name beyond uniqueness.
  child.parentPipePath = "/tmp/firmius_lsp_" + makeUuid().substr(0, 16);
  ::unlink(child.parentPipePath.c_str());

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, child.parentPipePath.c_str(),
               sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return false;
  }
  if (::listen(fd, 4) < 0) {
    ::close(fd);
    ::unlink(child.parentPipePath.c_str());
    return false;
  }
  child.parentPipeListenerFd = fd;

  // Drain on a detached thread. We do NOT join — the thread exits when
  // the listener fd closes (during killChild).
  child.parentPipeDrainer = std::thread([fd]() {
    for (;;) {
      sockaddr_un peer{};
      socklen_t len = sizeof(peer);
      int conn = ::accept(fd, reinterpret_cast<sockaddr *>(&peer), &len);
      if (conn < 0) {
        if (errno == EBADF || errno == EINVAL) return;
        if (errno == EINTR) continue;
        return;
      }
      // Drain bytes to /dev/null until peer disconnects, on a sub-thread
      // so additional connects (rare) aren't blocked.
      std::thread([conn]() {
        char buf[4096];
        while (::read(conn, buf, sizeof(buf)) > 0) { /* discard */ }
        ::close(conn);
      }).detach();
    }
  });
  return true;
}

// =============================================================================
// Spawn
// =============================================================================

bool WindsurfLspManager::spawnChild(Child &child, std::string &outErr) {
  if (!startParentPipe(child)) {
    outErr = "failed to bind parent_pipe socket";
    return false;
  }

  // Build argv. We mirror the Windsurf VSCode extension's argv almost
  // exactly so the binary doesn't take any unhappy boot path. The single
  // important deviation is `--server_port`: we pin it instead of using
  // `--random_port`, because we need a known address up front.
  std::vector<std::string> argvHolder = {
      child.binaryPath.string(),
      "--api_server_url", "https://server.self-serve.windsurf.com",
      "--run_child",
      "--enable_lsp",
      "--ide_name", kIdeName,
      "--server_port", std::to_string(child.port),
      "--inference_api_server_url", "https://inference.codeium.com",
      "--database_dir", child.databaseDir.string(),
      "--enable_index_service",
      "--enable_local_search",
      "--search_max_workspace_file_count", "5000",
      "--indexed_files_retention_period_days", "30",
      "--workspace_id", "firmius_default",
      "--codeium_dir", ".firmius/windsurf",
      "--extensions_dir", "/tmp/firmius_windsurf_extensions",
      "--parent_pipe_path", child.parentPipePath,
      "--windsurf_version", kWindsurfVersion,
      "--stdin_initial_metadata",
  };
  std::vector<char *> argv;
  argv.reserve(argvHolder.size() + 1);
  for (auto &s : argvHolder) argv.push_back(s.data());
  argv.push_back(nullptr);

  int stdinPipe[2];
  if (::pipe(stdinPipe) < 0) {
    outErr = "pipe() failed";
    return false;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(stdinPipe[0]);
    ::close(stdinPipe[1]);
    outErr = "fork() failed";
    return false;
  }
  if (pid == 0) {
    // ---------- child ----------
    ::dup2(stdinPipe[0], STDIN_FILENO);
    ::close(stdinPipe[0]);
    ::close(stdinPipe[1]);
    // Don't leak the parent_pipe listener fd; the LSP opens its own
    // client socket to that path.
    if (child.parentPipeListenerFd >= 0) {
      ::close(child.parentPipeListenerFd);
    }
    // Redirect stdout/stderr to a per-account log file so we can debug.
    // The LSP is chatty (sentry breadcrumbs, file-indexing progress,
    // etc.) but the alternative — /dev/null — left us flying blind when
    // the child silently failed to bootstrap. Path mirrors database_dir
    // structure: ~/.firmius/windsurf/by-account/<id>/lsp.log
    std::filesystem::path logPath = child.databaseDir / "lsp.log";
    int logFd = ::open(logPath.c_str(),
                       O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (logFd >= 0) {
      ::dup2(logFd, STDOUT_FILENO);
      ::dup2(logFd, STDERR_FILENO);
      ::close(logFd);
    } else {
      int devnull = ::open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        ::close(devnull);
      }
    }
    // Inject CSRF into the env. The LSP looks for WINDSURF_CSRF_TOKEN at
    // boot and uses it to authenticate every inbound request.
    ::setenv("WINDSURF_CSRF_TOKEN", child.csrf.c_str(), 1);
    ::execv(argv[0], argv.data());
    // execv failed
    ::_exit(127);
  }
  // ---------- parent ----------
  ::close(stdinPipe[0]);
  child.pid = pid;

  // Push the binary-encoded Metadata into stdin and close so the LSP
  // sees EOF on its initial-metadata read.
  std::string meta = buildStdinMetadata(child.apiKey);
  std::size_t written = 0;
  while (written < meta.size()) {
    ssize_t n = ::write(stdinPipe[1], meta.data() + written,
                        meta.size() - written);
    if (n <= 0) break;
    written += n;
  }
  ::close(stdinPipe[1]);
  return true;
}

// =============================================================================
// Readiness probe
// =============================================================================

bool WindsurfLspManager::waitForReady(int port, const std::string &csrf,
                                      std::chrono::milliseconds timeout) {
  using clock = std::chrono::steady_clock;
  auto deadline = clock::now() + timeout;
  std::string url = "http://127.0.0.1:" + std::to_string(port) +
                    "/exa.language_server_pb.LanguageServerService/StartCascade";
  while (clock::now() < deadline) {
    firmius::utils::GCPHttpClient client(kUserAgent);
    client.setContentType("application/proto");
    client.addHeader("connect-protocol-version", "1");
    client.addHeader("x-codeium-csrf-token", csrf);
    client.addHeader("Origin", kOrigin);
    auto resp = client.post(url, std::string{}, 2);
    // Any HTTP code from the LSP means it's accepting connections; an
    // empty StartCascade body returns 200 with a grpc-message envelope
    // OR 400, both fine.
    if (resp.code != 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// =============================================================================
// Termination
// =============================================================================

void WindsurfLspManager::killChild(Child &child) {
  if (child.pid > 0) {
    ::kill(child.pid, SIGTERM);
    // Give it ~3 s to exit cleanly. Then SIGKILL the holdouts.
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      pid_t r = ::waitpid(child.pid, &status, WNOHANG);
      if (r == child.pid || r == -1) {
        child.pid = 0;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (child.pid > 0) {
      ::kill(child.pid, SIGKILL);
      int status = 0;
      ::waitpid(child.pid, &status, 0);
      child.pid = 0;
    }
  }
  if (child.parentPipeListenerFd >= 0) {
    ::shutdown(child.parentPipeListenerFd, SHUT_RDWR);
    ::close(child.parentPipeListenerFd);
    child.parentPipeListenerFd = -1;
  }
  if (!child.parentPipePath.empty()) {
    ::unlink(child.parentPipePath.c_str());
    child.parentPipePath.clear();
  }
  if (child.parentPipeDrainer.joinable()) {
    child.parentPipeDrainer.detach();
  }
}

// =============================================================================
// Public: ensureRunning
// =============================================================================

bool WindsurfLspManager::ensureRunning(
    const firmius::shared::OAuthAccount &acc, Endpoint &out,
    std::string &outErr) {
  std::lock_guard<std::mutex> lock(mutex_);

  const std::string &id = acc.identifier.empty()
                              ? acc.accessToken.substr(0, 12)
                              : acc.identifier;

  // Fast path: live + healthy child for this account.
  auto it = children_.find(id);
  if (it != children_.end() && it->second && it->second->pid > 0) {
    auto &c = *it->second;
    out = {c.pid, c.port, c.csrf, c.accountId};
    c.lastUsed = std::chrono::steady_clock::now();
    return true;
  }

  // Slow path: spawn fresh.
  auto child = std::make_unique<Child>();
  child->accountId = id;
  child->apiKey = acc.accessToken;
  child->csrf = makeUuid();
  child->binaryPath = resolveBinary();
  if (child->binaryPath.empty()) {
    outErr = "could not locate language_server binary; install Windsurf "
             "(any version) or set FIRMIUS_WINDSURF_BINARY to the path";
    return false;
  }
  // Per-account database dir — keeps account A's index out of account B's
  // file space. We DO persist it across runs so the workspace index stays
  // warm.
  const char *home = std::getenv("HOME");
  std::filesystem::path base = home ? home : "/tmp";
  child->databaseDir = base / ".firmius" / "windsurf" / "by-account" / id;
  std::error_code ec;
  std::filesystem::create_directories(child->databaseDir, ec);

  child->port = allocateEphemeralPort();
  if (child->port == 0) {
    outErr = "failed to allocate ephemeral 127.0.0.1 port";
    return false;
  }

  if (!spawnChild(*child, outErr)) return false;

  if (!waitForReady(child->port, child->csrf, std::chrono::seconds(10))) {
    outErr = "language_server failed to become ready within 10s";
    killChild(*child);
    return false;
  }

  child->lastUsed = std::chrono::steady_clock::now();
  out = {child->pid, child->port, child->csrf, child->accountId};
  children_.emplace(id, std::move(child));
  return true;
}

}  // namespace firmius::provider
