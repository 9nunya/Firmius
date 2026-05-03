// =============================================================================
// WindsurfLspManager
//
// Owns the lifecycle of one or more `language_server_<platform>_<arch>`
// child processes — one per Windsurf account. Each child:
//
//   * Is spawned by Firmius (NOT by the Windsurf IDE), so we control:
//       - which api_key it uses (passed in via stdin as a binary
//         codeium_common_pb.Metadata)
//       - which CSRF token guards its localhost endpoints (we mint it)
//       - which port it listens on (we pick a free ephemeral)
//       - which `--database_dir` it writes to (per-account, isolated)
//   * Talks Connect-RPC over plain HTTP/1.1 on 127.0.0.1:<our_port>.
//   * Survives across multiple chats — we keep the process warm and
//     recycle it on idle / failure / explicit shutdown.
//
// Why this exists
// ---------------
// v1 of WindsurfProvider reused whatever Windsurf-IDE-spawned
// language_server happened to be running. That ALWAYS billed the IDE's
// logged-in account regardless of which Firmius account the user
// selected, because the LSP loads its api_key once from the parent's
// stdin at boot and ignores per-request `Metadata.api_key`. v2 spawns
// our own children so the api_key is OURS.
//
// Why a class and not free functions
// -----------------------------------
// Multiple stateful invariants:
//   1. One process per account, indexed by accountId.
//   2. Each child has a parent_pipe Unix-socket listener we own and that
//      must outlive the child (LSP connects to it on boot and writes
//      progress events).
//   3. Idle eviction is timer-driven, not chat-driven, so the data
//      structure has to be observable from a background reaper.
//   4. Shutdown has to be ordered (SIGTERM → wait → SIGKILL) per child.
//
// Threading
// ---------
// All public methods are thread-safe. ensureRunning() can be called
// concurrently from multiple chat threads; only one spawn will happen
// per accountId.
// =============================================================================

#ifndef FIRMIUS_PROVIDER_WINDSURF_LSP_MANAGER_HPP
#define FIRMIUS_PROVIDER_WINDSURF_LSP_MANAGER_HPP

#include "Context.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace firmius::provider {

class WindsurfLspManager {
public:
  struct Endpoint {
    int pid = 0;
    int port = 0;
    std::string csrf;
    std::string accountId;  // OAuthAccount.identifier
  };

  WindsurfLspManager();
  ~WindsurfLspManager();

  // No copy/move — instances are owned by WindsurfProvider as members.
  WindsurfLspManager(const WindsurfLspManager &) = delete;
  WindsurfLspManager &operator=(const WindsurfLspManager &) = delete;

  // Ensure a healthy language_server child is running for `acc` and
  // return its localhost endpoint. Idempotent; subsequent calls for the
  // same accountId return the live child without re-spawning.
  //
  // Blocks up to ~10s while a fresh child boots and the readiness probe
  // succeeds. Returns false on any spawn / probe failure with a
  // human-readable reason in `outErr`. The caller emits StreamError.
  bool ensureRunning(const firmius::shared::OAuthAccount &acc, Endpoint &out,
                     std::string &outErr);

  // Force-terminate the child for `accountId`. Used after a chat-side
  // failure where we suspect the LSP got into a bad state. The next
  // `ensureRunning` for the same account will spawn fresh.
  void recycle(const std::string &accountId);

  // Send SIGTERM (then SIGKILL after grace period) to every running
  // child. Safe to call multiple times. Called from the destructor and
  // exposed for graceful shutdown sequences.
  void shutdown();

  // For diagnostics / status surface.
  std::vector<Endpoint> liveEndpoints() const;

private:
  struct Child {
    int pid = 0;
    int port = 0;
    std::string csrf;
    std::string accountId;
    std::string apiKey;
    std::filesystem::path binaryPath;
    std::filesystem::path databaseDir;
    std::string parentPipePath;  // Unix-domain-socket path
    int parentPipeListenerFd = -1;
    std::thread parentPipeDrainer;
    std::chrono::steady_clock::time_point lastUsed{};
  };

  // Locate a usable language_server binary. Priority order:
  //   1. $FIRMIUS_WINDSURF_BINARY override (devbox / explicit path).
  //   2. Existing Windsurf install scan (Linux paths first; macOS /
  //      Windows paths once we add them).
  //   3. Firmius cache `~/.firmius/bin/windsurf/...` (for auto-download
  //      to land later — TODO V).
  std::filesystem::path resolveBinary();

  // Allocate an ephemeral 127.0.0.1 port by binding(0)+close(). Race-
  // tolerant for our purposes because Linux doesn't immediately reuse a
  // freshly-released port.
  int allocateEphemeralPort();

  // Build the binary stdin payload — a single `codeium_common_pb.Metadata`
  // proto with `api_key` set to the account's accessToken. The LSP reads
  // this exactly once at boot and uses it as the bearer credential for
  // every outbound call to inference.codeium.com /
  // server.self-serve.windsurf.com.
  std::string buildStdinMetadata(const std::string &apiKey);

  // Listening side-channel that the LSP connects to via
  // --parent_pipe_path. The LSP only PUSHES progress events to it (file
  // indexing, deprecation warnings); we read+drop everything to /dev/null
  // on a detached drainer thread. We have to listen because the LSP boot
  // path appears to require a successful connect before it'll start the
  // gRPC server.
  bool startParentPipe(Child &child);

  // Probe `127.0.0.1:<port>` until StartCascade returns a non-zero HTTP
  // status (any code means "process is up and accepting Connect-RPC"),
  // or `timeout` elapses.
  bool waitForReady(int port, const std::string &csrf,
                    std::chrono::milliseconds timeout);

  // Forks/execs the binary with our argv/env and stdin metadata. On
  // success populates `child.pid` and connects parent pipe. Caller must
  // still wait for readiness via waitForReady().
  bool spawnChild(Child &child, std::string &outErr);

  // SIGTERM -> wait -> SIGKILL. Blocks until the child reaps or we give
  // up. Releases the parent_pipe resources too.
  void killChild(Child &child);

  // Random UUID v4 generator — we use this for both the CSRF and the
  // unique pipe path suffix.
  static std::string makeUuid();

  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<Child>> children_;
  std::filesystem::path cachedBinary_;
};

}  // namespace firmius::provider

#endif  // FIRMIUS_PROVIDER_WINDSURF_LSP_MANAGER_HPP
