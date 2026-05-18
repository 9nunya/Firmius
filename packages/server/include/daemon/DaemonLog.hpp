#pragma once

// Lightweight stderr logger for the firmius daemon and its hot paths.
//
// Goals:
//   * Always-on, low overhead, zero dependencies beyond stdlib.
//   * Single-line monotonic-elapsed prefix so the implicit-spawn path can be
//     diagnosed by redirecting daemon stderr to a file via FIRMIUS_DAEMON_LOG.
//   * Coarse phase markers (FIRMIUS_DLOG_PHASE) and scoped timers
//     (FIRMIUS_DLOG_SCOPE / FIRMIUS_DLOG_PHASE_SCOPE) so we can find slow
//     init steps and slow RPC handlers without bringing up a full tracer.
//
// Verbosity is controlled by the FIRMIUS_DAEMON_VERBOSE env var:
//   * unset / "0"      → only PHASE-level messages and warnings.
//   * "1" / "true"     → also per-RPC and per-step timings (FIRMIUS_DLOG_SCOPE).
//   * "2" / "trace"    → also low-level helpers.
//
// Output destination is normally stderr. Set FIRMIUS_DAEMON_LOG=<path> to
// redirect daemon log lines into a file (recommended for the implicit-spawn
// path, where stderr would otherwise land in the parent TUI's alt-screen).

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace firmius::daemon {

class DaemonLog {
public:
  static DaemonLog &instance() {
    static DaemonLog inst;
    return inst;
  }

  // 0=PHASE only, 1=RPC, 2=trace.
  int verbosity() const { return verbosity_; }

  void writef(const char *level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
      __attribute__((format(printf, 3, 4)))
#endif
  {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    writeRaw(level, buf);
  }

  void writeRaw(const char *level, const char *msg) {
    using namespace std::chrono;
    const double dt =
        duration<double>(steady_clock::now() - startTime_).count();
    char header[96];
#if defined(_WIN32)
    long pid = static_cast<long>(_getpid());
#else
    long pid = static_cast<long>(::getpid());
#endif
    std::snprintf(header, sizeof(header), "[firmiusd %s pid=%ld +%7.3fs]",
                  level ? level : "INFO", pid, dt);
    std::lock_guard<std::mutex> lock(mutex_);
    std::fprintf(stream_, "%s %s\n", header, msg);
    std::fflush(stream_);
  }

  std::chrono::steady_clock::time_point startTime() const { return startTime_; }

private:
  DaemonLog() {
    startTime_ = std::chrono::steady_clock::now();
    if (const char *path = std::getenv("FIRMIUS_DAEMON_LOG")) {
      if (path[0]) {
        FILE *f = std::fopen(path, "a");
        if (f) {
          stream_ = f;
          owned_ = true;
        }
      }
    }
    if (!stream_) stream_ = stderr;

    if (const char *v = std::getenv("FIRMIUS_DAEMON_VERBOSE")) {
      if (std::strcmp(v, "trace") == 0) {
        verbosity_ = 2;
      } else if (std::strcmp(v, "true") == 0) {
        verbosity_ = 1;
      } else {
        verbosity_ = std::atoi(v);
      }
    }
  }

  ~DaemonLog() {
    if (owned_ && stream_) std::fclose(stream_);
  }

  std::mutex mutex_;
  FILE *stream_ = nullptr;
  bool owned_ = false;
  int verbosity_ = 0;
  std::chrono::steady_clock::time_point startTime_;
};

class DaemonLogScope {
public:
  DaemonLogScope(const char *level, std::string label, int min_verbosity)
      : level_(level), label_(std::move(label)),
        start_(std::chrono::steady_clock::now()),
        min_verbosity_(min_verbosity) {
    if (DaemonLog::instance().verbosity() >= min_verbosity_) {
      DaemonLog::instance().writef(level_, "BEGIN %s", label_.c_str());
      verbose_ = true;
    }
  }

  ~DaemonLogScope() {
    using namespace std::chrono;
    const double ms =
        duration<double, std::milli>(steady_clock::now() - start_).count();
    if (verbose_) {
      DaemonLog::instance().writef(level_, "END   %s (%.2f ms)",
                                   label_.c_str(), ms);
    } else if (ms >= 250.0) {
      // Even at quiet verbosity, surface anything suspiciously slow.
      DaemonLog::instance().writef("WARN", "SLOW  %s (%.2f ms)",
                                   label_.c_str(), ms);
    }
  }

private:
  const char *level_;
  std::string label_;
  std::chrono::steady_clock::time_point start_;
  int min_verbosity_;
  bool verbose_ = false;
};

} // namespace firmius::daemon

// Always emitted.
#define FIRMIUS_DLOG_PHASE(msg) \
  ::firmius::daemon::DaemonLog::instance().writef("PHASE", "%s", msg)

#define FIRMIUS_DLOG_PHASEF(fmt, ...) \
  ::firmius::daemon::DaemonLog::instance().writef("PHASE", fmt, __VA_ARGS__)

#define FIRMIUS_DLOG_WARNF(fmt, ...) \
  ::firmius::daemon::DaemonLog::instance().writef("WARN", fmt, __VA_ARGS__)

// Verbosity 1+.
#define FIRMIUS_DLOG_INFOF(fmt, ...)                                       \
  do {                                                                     \
    if (::firmius::daemon::DaemonLog::instance().verbosity() >= 1) {       \
      ::firmius::daemon::DaemonLog::instance().writef("INFO", fmt,         \
                                                       __VA_ARGS__);       \
    }                                                                      \
  } while (0)

// Verbosity 2.
#define FIRMIUS_DLOG_TRACEF(fmt, ...)                                      \
  do {                                                                     \
    if (::firmius::daemon::DaemonLog::instance().verbosity() >= 2) {       \
      ::firmius::daemon::DaemonLog::instance().writef("TRACE", fmt,        \
                                                       __VA_ARGS__);       \
    }                                                                      \
  } while (0)

#define FIRMIUS_DLOG_SCOPE_PASTE2(a, b) a##b
#define FIRMIUS_DLOG_SCOPE_PASTE(a, b) FIRMIUS_DLOG_SCOPE_PASTE2(a, b)
#define FIRMIUS_DLOG_SCOPE_AT(level, label, min_verbosity)                 \
  ::firmius::daemon::DaemonLogScope FIRMIUS_DLOG_SCOPE_PASTE(              \
      _firmius_dlog_scope_, __LINE__)(level, label, min_verbosity)

// Verbosity 1 BEGIN/END; at v0 only warns if >=250ms.
#define FIRMIUS_DLOG_SCOPE(label) FIRMIUS_DLOG_SCOPE_AT("INFO", label, 1)

// Always BEGIN/END at PHASE level (visible at v0 too).
#define FIRMIUS_DLOG_PHASE_SCOPE(label) FIRMIUS_DLOG_SCOPE_AT("PHASE", label, 0)
