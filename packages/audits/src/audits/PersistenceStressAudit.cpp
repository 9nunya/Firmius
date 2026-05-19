#include "audits/PersistenceStressAudit.hpp"

#include "persistence/Journaler.hpp"
#include "persistence/ThreadManager.hpp"
#include "utils/PlatformPaths.hpp"
#include "utils/StringUtil.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace firmius::audits {

using namespace firmius::core;
using namespace firmius::shared;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// /proc/self/status + /proc/self/io snapshot on linux (no-op elsewhere).
struct ProcSnapshot {
  long rss_kb = 0;
  long vsz_kb = 0;
  long threads = 0;
  long long rchar = 0;
  long long wchar = 0;
  long long read_bytes = 0;
  long long write_bytes = 0;
};

ProcSnapshot procSnap() {
  ProcSnapshot s;
#ifdef __linux__
  std::ifstream st("/proc/self/status");
  std::string line;
  while (std::getline(st, line)) {
    if (line.rfind("VmRSS:", 0) == 0)
      std::sscanf(line.c_str(), "VmRSS: %ld", &s.rss_kb);
    else if (line.rfind("VmSize:", 0) == 0)
      std::sscanf(line.c_str(), "VmSize: %ld", &s.vsz_kb);
    else if (line.rfind("Threads:", 0) == 0)
      std::sscanf(line.c_str(), "Threads: %ld", &s.threads);
  }
  std::ifstream io("/proc/self/io");
  while (std::getline(io, line)) {
    if (line.rfind("rchar:", 0) == 0)
      std::sscanf(line.c_str(), "rchar: %lld", &s.rchar);
    else if (line.rfind("wchar:", 0) == 0)
      std::sscanf(line.c_str(), "wchar: %lld", &s.wchar);
    else if (line.rfind("read_bytes:", 0) == 0)
      std::sscanf(line.c_str(), "read_bytes: %lld", &s.read_bytes);
    else if (line.rfind("write_bytes:", 0) == 0)
      std::sscanf(line.c_str(), "write_bytes: %lld", &s.write_bytes);
  }
#endif
  return s;
}

void printDelta(const char *label, const ProcSnapshot &a,
                const ProcSnapshot &b) {
  std::cout << "  [" << label << "]"
            << " ΔRSS=" << ((b.rss_kb - a.rss_kb) / 1024.0) << "MB"
            << " peakRSS=" << (b.rss_kb / 1024.0) << "MB"
            << " threads=" << b.threads
            << " Δrchar=" << ((b.rchar - a.rchar) / 1024.0 / 1024.0) << "MB"
            << " Δwchar=" << ((b.wchar - a.wchar) / 1024.0 / 1024.0) << "MB"
            << " Δread_bytes=" << ((b.read_bytes - a.read_bytes) / 1024.0 / 1024.0)
            << "MB"
            << " Δwrite_bytes="
            << ((b.write_bytes - a.write_bytes) / 1024.0 / 1024.0) << "MB"
            << std::endl;
}

std::string randomString(std::mt19937 &rng, size_t len) {
  static const char charset[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz "
      "        \n";
  std::string out;
  out.resize(len);
  for (size_t i = 0; i < len; ++i)
    out[i] = charset[rng() % (sizeof(charset) - 1)];
  return out;
}

// Build a synthetic AgentTurn. Mode controls content shape:
//   0 = light text turn
//   1 = tool-call + result turn (1 tool)
//   2 = thinking + text + tool-call + result + edits (heavy)
AgentTurn makeTurn(std::mt19937 &rng, int idx, int mode) {
  AgentTurn turn;
  turn.turnId = "turn-" + std::to_string(idx);

  Message userMsg;
  userMsg.id = "msg-u-" + std::to_string(idx);
  userMsg.role = Role::User;
  userMsg.content.push_back(TextContent{randomString(rng, 60 + (rng() % 240))});
  turn.messages.push_back(std::move(userMsg));

  Message asst;
  asst.id = "msg-a-" + std::to_string(idx);
  asst.role = Role::Assistant;

  if (mode == 0) {
    asst.content.push_back(
        TextContent{randomString(rng, 200 + (rng() % 800))});
  } else if (mode == 1) {
    const std::string callId = "call-" + std::to_string(idx);
    asst.content.push_back(
        ToolCallContent{callId, "process_execute", "{\"command\":\"ls -R\"}"});
    Message resMsg;
    resMsg.id = "msg-r-" + std::to_string(idx);
    resMsg.role = Role::ToolResult;
    resMsg.content.push_back(ToolResultContent{
        callId, randomString(rng, 400 + (rng() % 2000)), true, "", ""});
    turn.messages.push_back(std::move(resMsg));
  } else {
    asst.content.push_back(
        ThinkingContent{randomString(rng, 200 + (rng() % 600)), ""});
    asst.content.push_back(
        TextContent{randomString(rng, 200 + (rng() % 400))});
    for (int t = 0; t < 3; ++t) {
      const std::string callId =
          "call-" + std::to_string(idx) + "-" + std::to_string(t);
      asst.content.push_back(ToolCallContent{
          callId, t == 0 ? "file_edit"
                         : (t == 1 ? "process_execute" : "file_read"),
          "{\"path\":\"src/demo_" + std::to_string(idx % 32) + ".cpp\"}"});
      Message resMsg;
      resMsg.id = "msg-r-" + std::to_string(idx) + "-" + std::to_string(t);
      resMsg.role = Role::ToolResult;
      resMsg.content.push_back(ToolResultContent{
          callId, randomString(rng, 300 + (rng() % 1500)), true, "", ""});
      turn.messages.push_back(std::move(resMsg));
    }
  }
  turn.metrics.tokens.prompt = 200 + (rng() % 4000);
  turn.metrics.tokens.completion = 100 + (rng() % 3000);
  turn.metrics.tokens.reasoning = rng() % 1000;
  turn.metrics.tokens.total = turn.metrics.tokens.prompt +
                              turn.metrics.tokens.completion +
                              turn.metrics.tokens.reasoning;
  turn.metrics.estimatedCostUsd = 0.0001 * turn.metrics.tokens.total;
  turn.messages.insert(turn.messages.begin() + 1, std::move(asst));
  return turn;
}

void summarizeLatencies(const std::string &label, std::vector<double> &lat_ms) {
  if (lat_ms.empty()) {
    std::cout << "  " << label << ": no samples" << std::endl;
    return;
  }
  std::sort(lat_ms.begin(), lat_ms.end());
  auto pct = [&](double p) {
    size_t idx = static_cast<size_t>(
        std::min<double>(lat_ms.size() - 1, lat_ms.size() * p));
    return lat_ms[idx];
  };
  double sum = 0;
  for (double v : lat_ms)
    sum += v;
  std::cout << "  " << label << " n=" << lat_ms.size()
            << "  avg=" << std::fixed << std::setprecision(3)
            << (sum / lat_ms.size()) << "ms"
            << "  p50=" << pct(0.50) << "ms"
            << "  p95=" << pct(0.95) << "ms"
            << "  p99=" << pct(0.99) << "ms"
            << "  max=" << lat_ms.back() << "ms" << std::endl;
}

// Stand up a scratch FIRMIUS_HOME under parent (or temp) and return its path.
std::string mintScratchHome(const std::string &parent) {
  std::filesystem::path parentPath = parent.empty()
      ? std::filesystem::temp_directory_path()
      : std::filesystem::path(parent);
  std::filesystem::create_directories(parentPath);
  auto home = parentPath / ("firmius_persist_" + firmius::shared::StringUtil::generateUuid().substr(0, 12));
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(home / ".firmius/threads");
  ::setenv("HOME", home.string().c_str(), 1);
  return home.string();
}

long long fileSize(const std::filesystem::path &p) {
  std::error_code ec;
  auto s = std::filesystem::file_size(p, ec);
  return ec ? -1 : static_cast<long long>(s);
}

} // namespace

std::string PersistenceStressAudit::getId() const {
  return "persistence_stress";
}

std::string PersistenceStressAudit::getDescription() const {
  return "Stress test Journaler/ThreadManager with 30k-turn threads, "
         "multi-agent fleet writes, tool-call-heavy turns, cold reload, "
         "memory + disk I/O monitoring (mock FIRMIUS_HOME)";
}

shared::AuditResult
PersistenceStressAudit::run(const std::vector<std::string> &args) {
  AuditResult result;
  result.auditId = getId();
  result.passed = true;

  int turns = 30000;
  int agents = 5;
  int multiagent_turns_each = 3000;
  int heavy_turns = 1000;
  bool keep_home = false;
  std::string scratch_parent;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto &a = args[i];
    auto next = [&](int def) {
      if (i + 1 < args.size()) {
        try {
          return std::stoi(args[++i]);
        } catch (...) {
        }
      }
      return def;
    };
    if (a == "--turns")
      turns = next(turns);
    else if (a == "--agents")
      agents = next(agents);
    else if (a == "--multiagent-turns")
      multiagent_turns_each = next(multiagent_turns_each);
    else if (a == "--heavy-turns")
      heavy_turns = next(heavy_turns);
    else if (a == "--keep-home")
      keep_home = true;
    else if (a == "--scratch-parent" && i + 1 < args.size())
      scratch_parent = args[++i];
  }

  const std::string saved_home = PlatformPaths::userHomeDir().string();
  std::string scratchHome;
  try {
    scratchHome = mintScratchHome(scratch_parent);
  } catch (const std::exception &e) {
    std::cerr << "Failed to mint scratch home: " << e.what() << std::endl;
    result.passed = false;
    result.exitCode = 1;
    return result;
  }
  const std::string threadsBase = scratchHome + "/.firmius/threads";
  const std::string dbPath = threadsBase + "/firmius_threads.db";

  std::cout << "═══ PersistenceStressAudit ═══" << std::endl;
  std::cout << "  scratchHome=" << scratchHome << std::endl;
  std::cout << "  turns=" << turns << "  agents=" << agents
            << "  multiagent_turns_each=" << multiagent_turns_each
            << "  heavy_turns=" << heavy_turns << std::endl;

  const auto base_snap = procSnap();

  // ───────────────────────────────────────────────────────────────────
  // Phase A: Bulk rewrite (single rewriteJournal of N turns).
  // ───────────────────────────────────────────────────────────────────
  std::cout << "\n── Phase A: Bulk rewriteJournal (" << turns << " turns) ──"
            << std::endl;
  std::string bulkThreadId;
  {
    ThreadManager tm(threadsBase);
    ThreadMetadata meta;
    meta.title = "Bulk rewrite stress " + std::to_string(turns);
    bulkThreadId = tm.createThread(meta);
    const std::string agentId = "agent-bulk";
    std::map<std::string, AgentManifestEntry> manifest;
    manifest[agentId] = {"aster", "", "aster", "Lead", true};
    tm.writeAgentManifest(bulkThreadId, manifest);

    std::vector<AgentTurn> history;
    history.reserve(turns);
    std::mt19937 rng(0xC0FFEE);
    auto t_gen = Clock::now();
    for (int i = 0; i < turns; ++i)
      history.push_back(makeTurn(rng, i, i % 3));
    std::cout << "  generated history in " << msSince(t_gen) << "ms"
              << std::endl;

    const auto pre = procSnap();
    auto t_rw = Clock::now();
    {
      Journaler journal(bulkThreadId, agentId);
      journal.rewriteJournal(history); // synchronous: enqueues op
    } // dtor flushes queue + joins worker
    const double rw_ms = msSince(t_rw);
    const auto post = procSnap();
    const long long db_size = fileSize(dbPath);
    std::cout << "  rewriteJournal+flush: " << rw_ms << "ms  ("
              << (turns * 1000.0 / rw_ms) << " turns/s)" << std::endl;
    std::cout << "  db_size_after_bulk=" << (db_size / 1024.0 / 1024.0) << "MB"
              << std::endl;
    printDelta("bulk-write", pre, post);
  }

  // ───────────────────────────────────────────────────────────────────
  // Phase B: Cold reload (loadAgentHistory of the bulk thread).
  // ───────────────────────────────────────────────────────────────────
  std::cout << "\n── Phase B: Cold reload via ThreadManager.loadAgentHistory ──"
            << std::endl;
  {
    const auto pre = procSnap();
    auto t_ld = Clock::now();
    ThreadManager tm(threadsBase);
    AgentHistory loaded = tm.loadAgentHistory(bulkThreadId, "agent-bulk");
    const double ld_ms = msSince(t_ld);
    const auto post = procSnap();
    std::cout << "  loadAgentHistory: " << ld_ms << "ms  loaded_turns="
              << loaded.turns.size() << "  ("
              << (loaded.turns.size() * 1000.0 / ld_ms) << " turns/s)"
              << std::endl;
    printDelta("cold-reload", pre, post);
  }

  // ───────────────────────────────────────────────────────────────────
  // Phase C: Streaming append (Journaler::appendTurn one-by-one).
  // ───────────────────────────────────────────────────────────────────
  const int stream_turns = std::min(turns, 5000);
  std::cout << "\n── Phase C: Streaming appendTurn (" << stream_turns
            << " single-turn ops) ──" << std::endl;
  {
    ThreadManager tm(threadsBase);
    ThreadMetadata meta;
    meta.title = "Streaming append stress";
    const std::string sid = tm.createThread(meta);
    std::map<std::string, AgentManifestEntry> manifest;
    manifest["agent-stream"] = {"aster", "", "aster", "Lead", true};
    tm.writeAgentManifest(sid, manifest);

    std::mt19937 rng(0x5EED);
    std::vector<double> enqueue_lat;
    enqueue_lat.reserve(stream_turns);

    const auto pre = procSnap();
    auto t_total = Clock::now();
    {
      Journaler journal(sid, "agent-stream");
      for (int i = 0; i < stream_turns; ++i) {
        AgentTurn turn = makeTurn(rng, i, i % 3);
        auto t0 = Clock::now();
        journal.appendTurn(turn);
        enqueue_lat.push_back(msSince(t0));
      }
      auto t_dtor = Clock::now();
      // dtor will block until worker drains
      (void)t_dtor;
    }
    const double total_ms = msSince(t_total);
    const auto post = procSnap();
    summarizeLatencies("appendTurn(enqueue) latency", enqueue_lat);
    std::cout << "  total wall (incl. dtor flush): " << total_ms << "ms  ("
              << (stream_turns * 1000.0 / total_ms) << " turns/s effective)"
              << std::endl;
    printDelta("stream-append", pre, post);
  }

  // ───────────────────────────────────────────────────────────────────
  // Phase D: Multi-agent fleet (N agents writing in parallel to one thread).
  // ───────────────────────────────────────────────────────────────────
  std::cout << "\n── Phase D: Multi-agent fleet (" << agents << " agents x "
            << multiagent_turns_each << " turns concurrent) ──" << std::endl;
  {
    ThreadManager tm(threadsBase);
    ThreadMetadata meta;
    meta.title = "Fleet contention stress";
    const std::string fid = tm.createThread(meta);
    std::map<std::string, AgentManifestEntry> manifest;
    for (int a = 0; a < agents; ++a) {
      const std::string id = "fleet-" + std::to_string(a);
      manifest[id] = {"aster", a == 0 ? "" : "fleet-0", "agent",
                      "Worker " + std::to_string(a), true};
    }
    tm.writeAgentManifest(fid, manifest);

    std::atomic<int> total_writes{0};
    std::vector<std::thread> threads;
    std::vector<std::vector<double>> per_agent_lat(agents);
    const auto pre = procSnap();
    auto t_par = Clock::now();
    for (int a = 0; a < agents; ++a) {
      threads.emplace_back([&, a] {
        std::mt19937 rng(0xA11CE0 + a);
        const std::string id = "fleet-" + std::to_string(a);
        Journaler journal(fid, id);
        per_agent_lat[a].reserve(multiagent_turns_each);
        for (int i = 0; i < multiagent_turns_each; ++i) {
          AgentTurn turn = makeTurn(rng, a * 1000000 + i, (i % 2) + 1);
          auto t0 = Clock::now();
          journal.appendTurn(turn);
          per_agent_lat[a].push_back(msSince(t0));
          total_writes.fetch_add(1, std::memory_order_relaxed);
        }
        // dtor flushes
      });
    }
    for (auto &th : threads)
      th.join();
    const double par_ms = msSince(t_par);
    const auto post = procSnap();

    std::vector<double> all_lat;
    for (auto &v : per_agent_lat)
      all_lat.insert(all_lat.end(), v.begin(), v.end());
    summarizeLatencies("fleet appendTurn(enqueue) latency", all_lat);
    std::cout << "  total wall: " << par_ms << "ms  total_writes="
              << total_writes.load() << "  effective_throughput="
              << (total_writes.load() * 1000.0 / par_ms) << " turns/s"
              << std::endl;
    std::cout << "  db_size_after_fleet="
              << (fileSize(dbPath) / 1024.0 / 1024.0) << "MB" << std::endl;
    printDelta("fleet-write", pre, post);

    // Cold reload of fleet thread per-agent
    std::cout << "  cold reload of fleet thread (per-agent loadAgentHistory):"
              << std::endl;
    auto t_rl = Clock::now();
    long long total_loaded = 0;
    for (int a = 0; a < agents; ++a) {
      const std::string id = "fleet-" + std::to_string(a);
      auto t_a = Clock::now();
      AgentHistory h = tm.loadAgentHistory(fid, id);
      const double ms = msSince(t_a);
      total_loaded += static_cast<long long>(h.turns.size());
      std::cout << "    agent=" << id << " turns=" << h.turns.size()
                << " load=" << ms << "ms" << std::endl;
    }
    std::cout << "  fleet aggregate cold-reload: " << msSince(t_rl)
              << "ms  total_turns=" << total_loaded << std::endl;
  }

  // ───────────────────────────────────────────────────────────────────
  // Phase E: Heavy tool-call turns (mode=2 only) for write+read latency.
  // ───────────────────────────────────────────────────────────────────
  std::cout << "\n── Phase E: Heavy tool-call turns (" << heavy_turns
            << " turns, 3 tool calls + thinking + text each) ──" << std::endl;
  {
    ThreadManager tm(threadsBase);
    ThreadMetadata meta;
    meta.title = "Heavy tool-call stress";
    const std::string hid = tm.createThread(meta);
    std::map<std::string, AgentManifestEntry> manifest;
    manifest["agent-heavy"] = {"aster", "", "aster", "Lead", true};
    tm.writeAgentManifest(hid, manifest);

    std::vector<AgentTurn> history;
    history.reserve(heavy_turns);
    std::mt19937 rng(0xDEADBEEF);
    for (int i = 0; i < heavy_turns; ++i)
      history.push_back(makeTurn(rng, i, 2));

    const auto pre = procSnap();
    auto t_w = Clock::now();
    {
      Journaler journal(hid, "agent-heavy");
      journal.rewriteJournal(history);
    }
    const double w_ms = msSince(t_w);
    const auto post_w = procSnap();
    std::cout << "  bulk write: " << w_ms << "ms  ("
              << (heavy_turns * 1000.0 / w_ms) << " turns/s)" << std::endl;
    printDelta("heavy-write", pre, post_w);

    auto t_r = Clock::now();
    AgentHistory loaded = tm.loadAgentHistory(hid, "agent-heavy");
    const double r_ms = msSince(t_r);
    const auto post_r = procSnap();
    long long total_msgs = 0;
    long long total_parts = 0;
    for (const auto &t : loaded.turns) {
      total_msgs += static_cast<long long>(t.messages.size());
      for (const auto &m : t.messages)
        total_parts += static_cast<long long>(m.content.size());
    }
    std::cout << "  cold reload: " << r_ms << "ms  turns=" << loaded.turns.size()
              << "  total_messages=" << total_msgs
              << "  total_parts=" << total_parts << std::endl;
    printDelta("heavy-read", post_w, post_r);
  }

  const auto end_snap = procSnap();
  std::cout << "\n── Final ──" << std::endl;
  printDelta("audit-total", base_snap, end_snap);
  std::cout << "  final db size = " << (fileSize(dbPath) / 1024.0 / 1024.0)
            << "MB" << std::endl;

  // ───────────────────────────────────────────────────────────────────
  // Cleanup
  // ───────────────────────────────────────────────────────────────────
  if (!saved_home.empty())
    ::setenv("HOME", saved_home.c_str(), 1);
  if (!keep_home) {
    std::error_code ec;
    std::filesystem::remove_all(scratchHome, ec);
    if (ec)
      std::cerr << "warning: failed to remove " << scratchHome << ": "
                << ec.message() << std::endl;
  } else {
    std::cout << "  scratch home retained at " << scratchHome << std::endl;
  }

  return result;
}

} // namespace firmius::audits
