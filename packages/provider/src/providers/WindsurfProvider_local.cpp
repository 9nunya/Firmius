// =============================================================================
// WindsurfProvider — local language_server streaming path.
//
// Talks to a running Windsurf `language_server_linux_x64` (or _macos /
// _windows) via Connect-RPC over localhost. The wire format was reverse
// engineered from a live Windsurf 2.0.67 IDE chat capture; full notes in
// `scratch/windsurf_wire/SPEC.md`.
//
// High level flow per chat turn:
//
//   1. Discover the running language_server: parse argv for `--server_port`
//      and `/proc/<pid>/environ` for `WINDSURF_CSRF_TOKEN`.
//   2. Look up (or mint) a `cascade_id` for the caller's threadId. The same
//      cascade_id is reused across model switches *and* across Firmius
//      restarts — this is what preserves prompt-cache hits.
//   3. `InitializeCascadePanelState` once (idempotent on the server side).
//   4. Open a `StreamCascadeReactiveUpdates` server-stream in a worker
//      thread; this is how the LSP delivers assistant text + tool calls +
//      token usage deltas back to us.
//   5. Send the latest user message via `SendUserCascadeMessage`.
//   6. Decode incoming Connect envelopes into Firmius `StreamEvent`s.
//
// Wire layout (Connect-RPC over HTTP/1.1 chunked):
//   POST /exa.language_server_pb.LanguageServerService/<Method>
//   x-codeium-csrf-token: <CSRF from /proc env>
//   content-type: application/proto                  (unary)
//   content-type: application/connect+proto          (streaming)
//   connect-protocol-version: 1
//
//   Streaming envelope (both directions):
//     [1 byte] flags  (0x00 = data, 0x02 = end-of-stream JSON status)
//     [4 bytes BE] payload length
//     [N bytes] protobuf payload
// =============================================================================

#include "providers/WindsurfProvider.hpp"

#include "providers/WindsurfLspManager.hpp"
#include "utils/GCPHttpClient.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace firmius::provider {

namespace {

constexpr const char *kLocalIdeName = "windsurf";
constexpr const char *kLocalIdeVersion = "2.0.67";
constexpr const char *kLocalExtVersion = "1.48.2";
constexpr const char *kLocalUserAgent =
    "firmius-windsurf-local/1.0 (linux; x86_64)";
constexpr const char *kLocalOrigin = "vscode-file://vscode-app";

// ----------------------------------------------------------------------------
// Proto encoding helpers (kept self-contained for this TU; the legacy gRPC TU
// has its own copies in an anonymous namespace and we don't want to fight the
// linker over them).
// ----------------------------------------------------------------------------

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

inline void writeMsg(std::string &out, int field, std::string_view payload) {
  std::uint64_t tag = (static_cast<std::uint64_t>(field) << 3) | 2;
  writeVarint(out, tag);
  writeVarint(out, payload.size());
  out.append(payload.data(), payload.size());
}

inline void writeVar(std::string &out, int field, std::uint64_t v) {
  std::uint64_t tag = (static_cast<std::uint64_t>(field) << 3) | 0;
  writeVarint(out, tag);
  writeVarint(out, v);
}

// Parse a single proto field. Returns the number of bytes consumed and fills
// `outField`/`outWire`/`outBytes`/`outVar`. Returns 0 on parse failure.
struct ProtoField {
  std::uint32_t field = 0;
  std::uint8_t wire = 0;
  std::uint64_t v = 0;
  std::string_view bytes;
};

inline std::size_t readVarint(const std::uint8_t *p, std::size_t n,
                              std::uint64_t &out) {
  std::uint64_t v = 0;
  std::size_t i = 0;
  int shift = 0;
  while (i < n) {
    std::uint8_t b = p[i++];
    v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80)) {
      out = v;
      return i;
    }
    shift += 7;
    if (shift > 63) return 0;
  }
  return 0;
}

inline std::size_t parseField(const std::uint8_t *p, std::size_t n,
                              ProtoField &f) {
  std::size_t off = 0;
  std::uint64_t tag;
  std::size_t k = readVarint(p, n, tag);
  if (k == 0) return 0;
  off += k;
  f.field = static_cast<std::uint32_t>(tag >> 3);
  f.wire = static_cast<std::uint8_t>(tag & 7);
  if (f.wire == 0) {
    k = readVarint(p + off, n - off, f.v);
    if (k == 0) return 0;
    off += k;
  } else if (f.wire == 2) {
    std::uint64_t len;
    k = readVarint(p + off, n - off, len);
    if (k == 0 || off + k + len > n) return 0;
    off += k;
    f.bytes = std::string_view(reinterpret_cast<const char *>(p + off),
                               static_cast<std::size_t>(len));
    off += static_cast<std::size_t>(len);
  } else if (f.wire == 1) {
    if (off + 8 > n) return 0;
    std::memcpy(&f.v, p + off, 8);
    off += 8;
  } else if (f.wire == 5) {
    if (off + 4 > n) return 0;
    std::uint32_t v32;
    std::memcpy(&v32, p + off, 4);
    f.v = v32;
    off += 4;
  } else {
    return 0;  // unknown wire type
  }
  return off;
}

// ----------------------------------------------------------------------------
// Local LSP discovery (Linux only for now).
// ----------------------------------------------------------------------------

struct LocalLspInfo {
  int pid = 0;
  int port = 0;
  std::string csrf;
};

bool readEnviron(int pid, std::vector<std::pair<std::string, std::string>> &out) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/environ",
                  std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  std::string blob = ss.str();
  std::size_t off = 0;
  while (off < blob.size()) {
    std::size_t nul = blob.find('\0', off);
    if (nul == std::string::npos) nul = blob.size();
    std::string kv = blob.substr(off, nul - off);
    auto eq = kv.find('=');
    if (eq != std::string::npos) {
      out.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    }
    off = nul + 1;
  }
  return true;
}

bool readCmdline(int pid, std::vector<std::string> &args) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline",
                  std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  std::string blob = ss.str();
  std::size_t off = 0;
  while (off < blob.size()) {
    std::size_t nul = blob.find('\0', off);
    if (nul == std::string::npos) nul = blob.size();
    if (nul > off) args.emplace_back(blob.substr(off, nul - off));
    off = nul + 1;
  }
  return true;
}

// Returns the LISTEN ports on 127.0.0.1 owned by `pid` by reading
// /proc/<pid>/net/tcp and cross-referencing /proc/<pid>/fd/* inode links.
//
// Format of /proc/<pid>/net/tcp (only LISTEN entries we care about):
//   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
//   12: 0100007F:9C7F 00000000:0000 0A ...                                             ... 12345
//
// `local_address = 0100007F` is 127.0.0.1 (little-endian hex). State `0A` ==
// LISTEN. We collect every (inode, port) tuple matching that, then map back
// to which inode is held by `pid` via fd symlinks.
std::vector<int> listenPortsForPid(int pid) {
  std::vector<int> ports;
  std::ifstream tcp("/proc/" + std::to_string(pid) + "/net/tcp");
  if (!tcp) return ports;
  std::vector<std::pair<std::uint64_t, int>> inodeToPort;
  std::string line;
  std::getline(tcp, line);  // header
  while (std::getline(tcp, line)) {
    // skip leading spaces, eat sl
    std::size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    while (i < line.size() && line[i] != ':') ++i;
    if (i >= line.size()) continue;
    ++i;
    // local_address — until space
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::size_t la = i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::string localAddr = line.substr(la, i - la);
    // rem_address
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    // state
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::string state = i + 2 <= line.size() ? line.substr(i, 2) : "";
    if (state != "0A") continue;
    // local_address must end with ":<port-hex>" and ip prefix == 0100007F
    auto colon = localAddr.find(':');
    if (colon == std::string::npos) continue;
    if (localAddr.substr(0, colon) != "0100007F") continue;
    int port = std::stoi(localAddr.substr(colon + 1), nullptr, 16);
    // After local_address rem_address state we still have 5 tokens before
    // inode: tx_queue:rx_queue, tr:tm->when, retrnsmt, uid, timeout, inode.
    // We're currently parked at the first byte of `state`. Skip 6 tokens
    // (state + 5 trailing) to land at inode.
    int skip = 0;
    while (i < line.size() && skip < 6) {
      while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
      while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
      ++skip;
    }
    std::size_t inStart = i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (inStart >= line.size()) continue;
    std::uint64_t inode = std::strtoull(line.substr(inStart, i - inStart).c_str(), nullptr, 10);
    inodeToPort.emplace_back(inode, port);
  }
  // Now scan /proc/<pid>/fd/* — symlinks like "socket:[<inode>]" — and
  // collect inodes this pid actually has open.
  std::error_code ec;
  std::filesystem::path fdDir("/proc/" + std::to_string(pid) + "/fd");
  if (!std::filesystem::exists(fdDir, ec)) return ports;
  for (const auto &fdEntry : std::filesystem::directory_iterator(fdDir, ec)) {
    auto link = std::filesystem::read_symlink(fdEntry.path(), ec);
    if (ec) { ec.clear(); continue; }
    std::string s = link.string();
    auto open = s.find("socket:[");
    if (open == std::string::npos) continue;
    std::uint64_t inode = std::strtoull(s.c_str() + open + 8, nullptr, 10);
    for (const auto &p : inodeToPort) {
      if (p.first == inode) ports.push_back(p.second);
    }
  }
  std::sort(ports.begin(), ports.end());
  ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
  return ports;
}

// Probe a localhost port to confirm it speaks our gRPC service. We send an
// empty POST to RawGetChatMessage with the CSRF; if it's the chat gRPC port
// the server will respond `200` with a grpc-status (not a connection refusal
// or 404). Other ports either reject the path or return 4xx.
bool probeIsChatGrpc(int port, const std::string &csrf) {
  firmius::utils::GCPHttpClient client(kLocalUserAgent);
  client.setContentType("application/proto");
  client.addHeader("connect-protocol-version", "1");
  client.addHeader("x-codeium-csrf-token", csrf);
  client.addHeader("Origin", kLocalOrigin);
  std::string url = "http://127.0.0.1:" + std::to_string(port) +
                    "/exa.language_server_pb.LanguageServerService/StartCascade";
  auto resp = client.post(url, std::string{}, 3);
  return resp.code != 0 && resp.code != 404;
}

bool discoverLocalLsp(LocalLspInfo &out, std::string &outErr) {
#ifndef __linux__
  outErr = "local language_server discovery is only implemented on Linux";
  return false;
#else
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator("/proc", ec)) {
    if (!entry.is_directory()) continue;
    const std::string &name = entry.path().filename().string();
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) {
      continue;
    }
    int pid = std::atoi(name.c_str());
    if (pid <= 0) continue;
    std::vector<std::string> args;
    if (!readCmdline(pid, args) || args.empty()) continue;
    if (args[0].find("language_server_") == std::string::npos) continue;
    if (args[0].find("/windsurf/") == std::string::npos) continue;
    std::vector<std::pair<std::string, std::string>> env;
    if (!readEnviron(pid, env)) continue;
    std::string csrf;
    for (const auto &kv : env) {
      if (kv.first == "WINDSURF_CSRF_TOKEN") {
        csrf = kv.second;
        break;
      }
    }
    if (csrf.empty()) continue;

    // Fast path: argv has --server_port, use it directly.
    int port = 0;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == "--server_port") {
        port = std::atoi(args[i + 1].c_str());
        break;
      }
    }
    // Slow path: random port. Discover from listening sockets owned by pid,
    // then probe each to find the chat gRPC port.
    if (port == 0) {
      auto ports = listenPortsForPid(pid);
      // Skip the extension_server_port (HTML 403 endpoint).
      int extPort = 0;
      for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--extension_server_port") {
          extPort = std::atoi(args[i + 1].c_str());
          break;
        }
      }
      for (int candidate : ports) {
        if (candidate == extPort) continue;
        if (probeIsChatGrpc(candidate, csrf)) {
          port = candidate;
          break;
        }
      }
    }
    if (port == 0) continue;
    out.pid = pid;
    out.port = port;
    out.csrf = std::move(csrf);
    return true;
  }
  outErr = "no running Windsurf language_server with usable gRPC port + "
           "WINDSURF_CSRF_TOKEN found in /proc";
  return false;
#endif
}

// ----------------------------------------------------------------------------
// codeium_common_pb.Metadata builder (modern wire layout, verified live).
// ----------------------------------------------------------------------------

std::string buildMetadata(std::string_view apiKey) {
  std::string out;
  writeStr(out, 1, kLocalIdeName);            // ide_name
  writeStr(out, 2, kLocalExtVersion);         // extension_version
  writeStr(out, 3, apiKey);                   // api_key
  writeStr(out, 4, "en");                     // locale
  writeStr(out, 7, kLocalIdeVersion);         // ide_version
  writeStr(out, 12, kLocalIdeName);           // extension_name
  return out;
}

// ----------------------------------------------------------------------------
// Connect-RPC unary helper. Returns HTTP code + body.
// ----------------------------------------------------------------------------

struct UnaryResult {
  long httpCode = 0;
  std::string body;
  std::string error;
};

UnaryResult connectUnary(int port, const std::string &csrf,
                         const std::string &path, const std::string &body,
                         int timeoutSeconds = 30) {
  firmius::utils::GCPHttpClient client(kLocalUserAgent);
  client.setContentType("application/proto");
  client.addHeader("connect-protocol-version", "1");
  client.addHeader("x-codeium-csrf-token", csrf);
  client.addHeader("Origin", kLocalOrigin);
  client.addHeader("Accept", "*/*");
  std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
  auto resp = client.post(url, body, timeoutSeconds);
  UnaryResult r;
  r.httpCode = resp.code;
  r.body = std::move(resp.body);
  r.error = std::move(resp.error);
  return r;
}

// ----------------------------------------------------------------------------
// Connect-RPC streaming helper. The LSP's streaming responses arrive as a
// chained sequence of 5-byte-prefixed envelopes inside a chunked HTTP/1.1
// response. libcurl handles the chunk decoding for us, so we just slice
// envelopes out of the dechunked body.
// ----------------------------------------------------------------------------

struct StreamCtx {
  std::string buffer;
  std::function<bool(std::uint8_t flags, std::string_view payload)> onEnvelope;
  std::atomic<bool> *abort = nullptr;
  bool stopRequested = false;
};

std::size_t streamWriteCallback(char *ptr, std::size_t size, std::size_t nmemb,
                                void *userdata) {
  auto *ctx = static_cast<StreamCtx *>(userdata);
  if (ctx->abort && ctx->abort->load()) return 0;  // tell curl to abort
  if (ctx->stopRequested) return 0;
  std::size_t total = size * nmemb;
  ctx->buffer.append(ptr, total);
  // Slice out as many complete envelopes as we have.
  while (ctx->buffer.size() >= 5) {
    std::uint8_t flags = static_cast<std::uint8_t>(ctx->buffer[0]);
    std::uint32_t len = (static_cast<std::uint32_t>(
                             static_cast<std::uint8_t>(ctx->buffer[1])) << 24) |
                        (static_cast<std::uint32_t>(
                             static_cast<std::uint8_t>(ctx->buffer[2])) << 16) |
                        (static_cast<std::uint32_t>(
                             static_cast<std::uint8_t>(ctx->buffer[3])) << 8) |
                        static_cast<std::uint32_t>(
                            static_cast<std::uint8_t>(ctx->buffer[4]));
    if (ctx->buffer.size() < 5 + len) break;
    std::string_view payload(ctx->buffer.data() + 5, len);
    bool keepGoing = ctx->onEnvelope(flags, payload);
    ctx->buffer.erase(0, 5 + len);
    if (!keepGoing) {
      ctx->stopRequested = true;
      return 0;  // abort curl
    }
  }
  return total;
}

[[maybe_unused]] bool connectStream(int port, const std::string &csrf, const std::string &path,
                   const std::string &body,
                   std::function<bool(std::uint8_t, std::string_view)> onEnv,
                   std::atomic<bool> *abort, std::string &outErr,
                   int timeoutSeconds = 600) {
  // The request body itself must also be wrapped in a single Connect envelope
  // (1 byte flags + 4 byte BE length + payload). Without this, the server
  // misreads the first 5 bytes as the envelope header and bails with
  // "promised <huge> bytes in enveloped message".
  std::string framed;
  framed.reserve(body.size() + 5);
  framed.push_back(0);  // flags
  std::uint32_t ln = static_cast<std::uint32_t>(body.size());
  framed.push_back(static_cast<char>((ln >> 24) & 0xff));
  framed.push_back(static_cast<char>((ln >> 16) & 0xff));
  framed.push_back(static_cast<char>((ln >> 8) & 0xff));
  framed.push_back(static_cast<char>(ln & 0xff));
  framed.append(body);

  firmius::utils::GCPHttpClient client(kLocalUserAgent);
  client.setContentType("application/connect+proto");
  client.addHeader("connect-protocol-version", "1");
  client.addHeader("x-codeium-csrf-token", csrf);
  client.addHeader("Origin", kLocalOrigin);
  client.addHeader("Accept", "*/*");

  StreamCtx ctx;
  ctx.onEnvelope = std::move(onEnv);
  ctx.abort = abort;
  std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
  auto resp = client.streamPost(url, framed, &streamWriteCallback, &ctx,
                                timeoutSeconds, abort);
  if (resp.code != 200 && !ctx.stopRequested) {
    outErr = "stream HTTP " + std::to_string(resp.code) +
             (resp.error.empty() ? "" : ": " + resp.error);
    return false;
  }
  return true;
}

// ----------------------------------------------------------------------------
// Cascade-management calls.
// ----------------------------------------------------------------------------

bool startCascade(int port, const std::string &csrf, const std::string &apiKey,
                  std::string &outCascadeId, std::string &outErr) {
  std::string body;
  writeMsg(body, 1, buildMetadata(apiKey));
  // Leave fields 4 (source) and 5 (trajectory_type) at default-zero. The
  // server is happy to mint a fresh cascade with UNSPECIFIED enums.
  auto r = connectUnary(port, csrf,
                        "/exa.language_server_pb.LanguageServerService/StartCascade",
                        body);
  if (r.httpCode != 200) {
    outErr = "StartCascade HTTP " + std::to_string(r.httpCode) +
             (r.body.empty() ? "" : ": " + r.body.substr(0, 240));
    return false;
  }
  // Response: StartCascadeResponse { 1 cascade_id (string) }.
  std::size_t off = 0;
  const std::uint8_t *p = reinterpret_cast<const std::uint8_t *>(r.body.data());
  while (off < r.body.size()) {
    ProtoField f;
    auto k = parseField(p + off, r.body.size() - off, f);
    if (k == 0) break;
    off += k;
    if (f.field == 1 && f.wire == 2) {
      outCascadeId.assign(f.bytes.data(), f.bytes.size());
      return true;
    }
  }
  outErr = "StartCascade: no cascade_id in response";
  return false;
}

bool initCascadePanel(int port, const std::string &csrf,
                      const std::string &apiKey, std::string &outErr) {
  std::string body;
  writeMsg(body, 1, buildMetadata(apiKey));
  writeVar(body, 3, 1);  // workspace_trusted = true
  auto r = connectUnary(
      port, csrf,
      "/exa.language_server_pb.LanguageServerService/InitializeCascadePanelState",
      body);
  if (r.httpCode != 200) {
    outErr = "InitializeCascadePanelState HTTP " + std::to_string(r.httpCode) +
             (r.body.empty() ? "" : ": " + r.body.substr(0, 240));
    return false;
  }
  return true;
}

// SendUserCascadeMessageRequest with the full CascadeConfig the language
// server actually requires for a headless turn. Verified against
// dwgx/WindsurfAPI@2026-04-28 src/windsurf.js and the proto descriptors
// embedded in language_server_linux_x64.
//
// CascadeConfig wire layout (exa.cortex_pb.CascadeConfig):
//   1  planner_config             = CascadeConversationalPlannerConfig
//      └─ inside CascadeConversationalPlannerConfig:
//         2  conversational      (CascadeConversational; sets planner_mode +
//                                  section overrides)
//         6  max_output_tokens   (int32)
//         15 requested_model_deprecated  (ModelOrAlias { 1 model = enum })
//         34 plan_model_uid              (string, modern uid path)
//         35 requested_model_uid         (string, modern uid path)
//         1  plan_model_deprecated       (Model enum varint, legacy path)
//   5  memory_config              ({ 1 enabled = false })
//   7  brain_config               (mostly empty, with seeded sub-flags)
//
// Free-tier / fresh accounts report "user status is nil" if only the modern
// uid fields are set, so we always populate BOTH the deprecated enum fields
// AND the modern UID fields when both are known.
// Build the system-prompt preamble that teaches the model our tool-call
// protocol. Returns empty string when `tools` is empty. Wire format mirrors
// dwgx/WindsurfAPI's TOOL_PROTOCOL_SYSTEM_HEADER + per-tool blocks.
std::string buildToolPreamble(const std::vector<firmius::provider::ToolDefinition> &tools) {
  if (tools.empty()) return {};
  std::string out;
  out +=
      "You have access to the following functions. To invoke a function, "
      "emit a block in this EXACT format:\n\n"
      "<tool_call>{\"name\":\"<function_name>\",\"arguments\":{...}}</tool_call>\n\n"
      "Rules:\n"
      "1. Each <tool_call>...</tool_call> block must fit on ONE line "
      "(no line breaks inside the JSON).\n"
      "2. \"arguments\" must be a JSON object matching the function's "
      "parameter schema.\n"
      "3. You MAY emit MULTIPLE <tool_call> blocks if the request requires "
      "calling several functions in parallel. Emit ALL needed calls "
      "consecutively, then STOP generating.\n"
      "4. After emitting the last <tool_call> block, STOP. Do not write "
      "any explanation after it. The caller executes the functions and "
      "returns results wrapped in <tool_result tool_call_id=\"...\">"
      "...</tool_result> tags in the next user turn.\n"
      "5. NEVER say \"I don't have access to tools\". The functions listed "
      "below ARE your available tools.\n"
      "6. When a function is relevant to the user's request, prefer "
      "calling it over answering from memory.\n\n"
      "Available functions:\n";
  for (const auto &t : tools) {
    out += "\n### " + t.name + "\n";
    if (!t.description.empty()) out += t.description + "\n";
    if (!t.inputSchema.empty()) {
      out += "Parameters:\n```json\n" + t.inputSchema + "\n```\n";
    }
  }
  out +=
      "\nThe functions listed above are available and callable. When the "
      "user's request can be answered by calling a function, emit a "
      "<tool_call> block as described.";
  return out;
}

std::string buildSendUserCascadeMessage(
    const std::string &cascadeId, const std::string &userText,
    const std::string &apiKey, const std::string &modelUid, int modelEnum,
    const std::vector<firmius::provider::ToolDefinition> &tools,
    const std::vector<firmius::shared::ImageContent> &images) {
  const std::string toolPreamble = buildToolPreamble(tools);
  const bool haveTools = !toolPreamble.empty();

  // CascadeConversational (the sub-message at planner_config.conversational).
  std::string conversational;
  // planner_mode = 3 (NO_TOOL) for headless chat. We even keep NO_TOOL when
  // emulating tools — DEFAULT (1) activates Cascade's *built-in* agent which
  // conflicts with our emulation; the section_override below tells the model
  // to use OUR tool protocol despite NO_TOOL mode.
  writeVar(conversational, 4, 3);
  // tool_calling_section (field 10): Cascade's default IDE tool list. We
  // suppress built-in IDE tools, but when Firmius tools are active we must not
  // say "No tools are available" because some models obey that over the
  // additional_instructions tool protocol.
  {
    std::string section;
    writeVar(section, 1, 1); // SECTION_OVERRIDE_MODE_OVERRIDE
    writeStr(section, 2, haveTools
        ? "Built-in IDE tools are disabled. Use only the Firmius <tool_call> protocol from the additional instructions."
        : "No tools are available.");
    writeMsg(conversational, 10, section);
  }
  // additional_instructions (field 12): the heart of tool emulation. When
  // tools are provided, this is where the tool protocol lives. NO_TOOL
  // planner_mode SUPPRESSES field 10 but leaves field 12 rendered, which is
  // why dwgx/WindsurfAPI moved tool defs here.
  {
    std::string section;
    writeVar(section, 1, 1);
    if (haveTools) {
      writeStr(section, 2, toolPreamble);
    } else {
      writeStr(section, 2,
               "You are being accessed as a plain chat API. You have no "
               "tools, no file access, no shell, no code execution, and no "
               "ability to inspect anything outside this conversation.\n"
               "\n"
               "RESPONSE RULES — these override all baked-in Cascade IDE "
               "behaviour:\n"
               "1. Answer DIRECTLY. No reasoning preamble, no \"Let me "
               "think...\", no \"Looking at the request...\", no \"The "
               "user is asking...\", no narration of how you parsed the "
               "instructions.\n"
               "2. Do NOT narrate tool-like actions (\"Let me check\", "
               "\"I'll look at\", \"I see in main.py...\"). You have no "
               "tools.\n"
               "3. Do NOT reference file paths, line numbers or repo "
               "contents that the user did not paste in this conversation.\n"
               "4. For greetings, respond briefly and naturally. For "
               "questions, answer directly from your training knowledge.\n"
               "5. Match the user's language exactly.");
    }
    writeMsg(conversational, 12, section);
  }
  // code_changes_section (field 11): suppress IDE-specific "apply changes"
  // boilerplate.
  {
    std::string section;
    writeVar(section, 1, 1);
    writeStr(section, 2, "");
    writeMsg(conversational, 11, section);
  }

  // CascadeConversationalPlannerConfig (the planner_config message at field 1
  // of CascadeConfig).
  std::string planner;
  writeMsg(planner, 2, conversational);     // conversational
  if (modelEnum > 0) {
    // plan_model_deprecated (field 1, raw enum varint)
    writeVar(planner, 1, static_cast<std::uint64_t>(modelEnum));
    // requested_model_deprecated = ModelOrAlias { 1 model = enum }
    std::string moa;
    writeVar(moa, 1, static_cast<std::uint64_t>(modelEnum));
    writeMsg(planner, 15, moa);
  }
  // max_output_tokens (real IDE sends 16384/32768; missing this truncates
  // long replies).
  writeVar(planner, 6, 32768);
  if (!modelUid.empty()) {
    // plan_model_uid (safety fallback) + requested_model_uid (primary).
    writeStr(planner, 34, modelUid);
    writeStr(planner, 35, modelUid);
  }

  // brain_config (CascadeConfig field 7). Mirrors the IDE's no-op brain
  // config; we just include the wrapper so the LS doesn't fault on a nil
  // brain and fall through to a buggy default.
  std::string brain;
  writeVar(brain, 1, 1);                            // some "enabled" flag
  // Nested empty message at field 6 — opaque "executor" placeholder.
  std::string brainExecutor;
  writeMsg(brain, 6, brainExecutor);

  // memory_config (CascadeConfig field 5). enabled=false suppresses LS
  // injecting the user's stored Cascade memories into responses.
  std::string memory;
  writeVar(memory, 1, 0);                           // enabled = false

  // Final CascadeConfig.
  std::string cascadeConfig;
  writeMsg(cascadeConfig, 1, planner);              // planner_config
  writeMsg(cascadeConfig, 5, memory);               // memory_config
  writeMsg(cascadeConfig, 7, brain);                // brain_config

  // SendUserCascadeMessageRequest wire layout:
  //   1 cascade_id (string)
  //   2 items (repeated TextOrScopeItem { 1 text })
  //   3 metadata (Metadata)
  //   5 cascade_config (CascadeConfig)
  //   6 images (repeated ImageData { 1 base64_data, 2 mime_type })
  std::string item;
  writeStr(item, 1, userText);                      // TextOrScopeItem.text
  std::string body;
  writeStr(body, 1, cascadeId);
  writeMsg(body, 2, item);
  writeMsg(body, 3, buildMetadata(apiKey));
  writeMsg(body, 5, cascadeConfig);

  // Attach images. ImageContent.url may be a data URI ("data:image/png;
  // base64,XXX") or a raw base64 payload. Strip the prefix when present
  // and pass through the inferred mime type.
  for (const auto &img : images) {
    std::string data = img.url;
    std::string mime = img.mediaType;
    if (data.rfind("data:", 0) == 0) {
      auto comma = data.find(',');
      if (comma != std::string::npos) {
        std::string header = data.substr(5, comma - 5);
        auto semi = header.find(';');
        std::string detectedMime = (semi != std::string::npos)
                                       ? header.substr(0, semi)
                                       : header;
        if (mime.empty() && !detectedMime.empty()) mime = detectedMime;
        data = data.substr(comma + 1);
      }
    }
    if (mime.empty()) mime = "image/png";
    std::string imageData;
    writeStr(imageData, 1, data);
    writeStr(imageData, 2, mime);
    writeMsg(body, 6, imageData);
  }
  return body;
}

// GetCascadeTrajectoryStepsRequest:
//   1 cascade_id (string), 2 step_offset (uint32, optional)
std::string buildGetTrajectoryStepsRequest(const std::string &cascadeId,
                                           std::uint32_t stepOffset = 0) {
  std::string body;
  writeStr(body, 1, cascadeId);
  if (stepOffset > 0) writeVar(body, 2, stepOffset);
  return body;
}

// One decoded CortexTrajectoryStep entry. Field IDs verified against
// exa.cortex_pb.CortexTrajectoryStep + the dwgx/WindsurfAPI parser.
struct TrajectoryStep {
  int type = 0;            // CortexTrajectoryStepType
  int status = 0;          // CortexTrajectoryStepStatus
  std::string responseText;
  std::string modifiedText;
  std::string thinking;
  std::string errorText;
  std::uint64_t inputTokens = 0;
  std::uint64_t outputTokens = 0;
  std::uint64_t cacheReadTokens = 0;
  std::uint64_t cacheWriteTokens = 0;
};

// Parse GetCascadeTrajectoryStepsResponse → repeated CortexTrajectoryStep.
//
// Top-level response: field 1 (repeated) = trajectory_steps.
// CortexTrajectoryStep:
//   1  type (enum)
//   4  status (enum)
//   5  metadata (CortexStepMetadata)
//        └── 9 model_usage (ModelUsageStats { 2 input, 3 output, 4 cw, 5 cr })
//   20 planner_response (CortexStepPlannerResponse)
//        └── 1 response, 3 thinking, 8 modified_response
//   24 error_message (CortexStepErrorMessage { 3 details })
//   31 error (CortexErrorDetails { 1 user_msg, 2 short, 3 full })
std::vector<TrajectoryStep> parseTrajectorySteps(std::string_view buf) {
  std::vector<TrajectoryStep> out;
  std::size_t off = 0;
  const auto *base = reinterpret_cast<const std::uint8_t *>(buf.data());

  auto parseSubFields = [](std::string_view src,
                           auto &&visitor) {
    std::size_t o = 0;
    const auto *b = reinterpret_cast<const std::uint8_t *>(src.data());
    while (o < src.size()) {
      ProtoField pf{};
      std::size_t consumed = parseField(b + o, src.size() - o, pf);
      if (consumed == 0) break;
      visitor(pf);
      o += consumed;
    }
  };

  while (off < buf.size()) {
    ProtoField step{};
    std::size_t consumed = parseField(base + off, buf.size() - off, step);
    if (consumed == 0) break;
    off += consumed;
    if (step.field != 1 || step.wire != 2) continue;

    TrajectoryStep entry;
    parseSubFields(step.bytes, [&](const ProtoField &f) {
      switch (f.field) {
      case 1:
        if (f.wire == 0) entry.type = static_cast<int>(f.v);
        break;
      case 4:
        if (f.wire == 0) entry.status = static_cast<int>(f.v);
        break;
      case 5:
        if (f.wire == 2) {
          // CortexStepMetadata.model_usage (field 9)
          parseSubFields(f.bytes, [&](const ProtoField &mf) {
            if (mf.field == 9 && mf.wire == 2) {
              parseSubFields(mf.bytes, [&](const ProtoField &uf) {
                if (uf.wire != 0) return;
                switch (uf.field) {
                case 2: entry.inputTokens = uf.v; break;
                case 3: entry.outputTokens = uf.v; break;
                case 4: entry.cacheWriteTokens = uf.v; break;
                case 5: entry.cacheReadTokens = uf.v; break;
                default: break;
                }
              });
            }
          });
        }
        break;
      case 20:
        if (f.wire == 2) {
          parseSubFields(f.bytes, [&](const ProtoField &pf) {
            if (pf.wire != 2) return;
            switch (pf.field) {
            case 1:
              entry.responseText.assign(pf.bytes.data(), pf.bytes.size());
              break;
            case 3:
              entry.thinking.assign(pf.bytes.data(), pf.bytes.size());
              break;
            case 8:
              entry.modifiedText.assign(pf.bytes.data(), pf.bytes.size());
              break;
            default:
              break;
            }
          });
        }
        break;
      case 24:
      case 31:
        if (f.wire == 2 && entry.errorText.empty()) {
          auto walkError = [&](std::string_view src) {
            parseSubFields(src, [&](const ProtoField &ef) {
              if (ef.wire != 2) return;
              if (ef.field >= 1 && ef.field <= 3 &&
                  entry.errorText.empty()) {
                entry.errorText.assign(ef.bytes.data(), ef.bytes.size());
              }
            });
          };
          if (f.field == 24) {
            parseSubFields(f.bytes, [&](const ProtoField &ef) {
              if (ef.field == 3 && ef.wire == 2) {
                walkError(ef.bytes);
              }
            });
          } else {
            walkError(f.bytes);
          }
        }
        break;
      default:
        break;
      }
    });

    out.push_back(std::move(entry));
  }
  return out;
}

// StreamCascadeReactiveUpdatesRequest. Two fields, no metadata:
//   field 1 (varint) = 1   (subscribe-to-deltas flag)
//   field 2 (string) = cascade_id
[[maybe_unused]] std::string buildStreamRequest(const std::string &cascadeId) {
  std::string body;
  writeVar(body, 1, 1);
  writeStr(body, 2, cascadeId);
  return body;
}

// ----------------------------------------------------------------------------
// Envelope / CascadeReactiveUpdate decoder.
//
// Each envelope payload is a CascadeReactiveUpdate message. The fields we
// care about (extracted by recursive walk):
//   - assistant text content       → emit TextChunk
//   - tool_call entries            → emit ToolCall
//   - reasoning_content            → emit ThinkingChunk
//   - response_statistics metric   → emit AgentMetrics (with cacheRead)
//
// The wire schema is wide; rather than encode every field here we walk the
// tree looking for marker substrings ("input_tokens", "output_tokens",
// "cached_input_tokens") attached to fixed32 values.
// ----------------------------------------------------------------------------

struct DecodedAssistantState {
  std::string text;
  std::string reasoning;
  std::vector<firmius::shared::ToolCall> toolCalls;
  std::uint32_t inputTokens = 0;
  std::uint32_t outputTokens = 0;
  std::uint32_t cacheRead = 0;
  std::uint32_t cacheWrite = 0;
  bool turnFinished = false;
};

bool isPrintableUtf8(std::string_view s, double minRatio = 0.85) {
  if (s.empty()) return false;
  std::size_t printable = 0;
  for (unsigned char c : s) {
    if (c == '\n' || c == '\t' || (c >= 0x20 && c <= 0x7e)) ++printable;
    else if (c >= 0x80) ++printable;  // assume utf-8 continuation
  }
  return printable >= static_cast<std::size_t>(s.size() * minRatio);
}

// Path-aware proto walker. The visitor receives the full ancestry stack of
// field numbers from the root down to the current field, so we can match on
// the EXACT proto path (e.g. assistant_message.text =
// `[3, 2, 2, 20, 3]` for `update.trajectory.trajectory_data.assistant_message.text`)
// instead of relying on weak heuristics that pick up workspace metadata.
using PathVisitor =
    std::function<void(const std::vector<std::uint32_t> &path,
                       const ProtoField &f)>;

void walkProtoPath(std::string_view buf, std::vector<std::uint32_t> &path,
                   const PathVisitor &visit) {
  const std::uint8_t *p = reinterpret_cast<const std::uint8_t *>(buf.data());
  std::size_t off = 0;
  while (off < buf.size()) {
    ProtoField f;
    auto k = parseField(p + off, buf.size() - off, f);
    if (k == 0) break;
    off += k;
    path.push_back(f.field);
    visit(path, f);
    if (f.wire == 2 && path.size() < 12) {
      walkProtoPath(f.bytes, path, visit);
    }
    path.pop_back();
  }
}

// Match a path SUFFIX. Returns true if `tail` is a tail of `path`.
bool pathEndsWith(const std::vector<std::uint32_t> &path,
                  std::initializer_list<std::uint32_t> tail) {
  if (path.size() < tail.size()) return false;
  auto pi = path.end() - tail.size();
  auto ti = tail.begin();
  for (; ti != tail.end(); ++pi, ++ti) {
    if (*pi != *ti) return false;
  }
  return true;
}

[[maybe_unused]] void decodeUpdate(std::string_view payload, DecodedAssistantState &state) {
  // Wire-format anchors (verified live 2026-04-27 against the captured
  // pcap; see `scratch/windsurf_wire/SPEC.md`):
  //
  //   CascadeReactiveUpdate
  //     field 3 = trajectory (Trajectory)
  //       field 2 = trajectory_data (TrajectoryData repeated)
  //         field 20 = assistant_message (AssistantMessage)
  //           field 3 = text content (string)         ← what we want
  //           field 4 = signature blob (bytes)         ← skip
  //           field 6 = bot id (string)                ← skip
  //           field 7 = tool_call (ToolCall, repeated)
  //             field 1 = tool_use_id "toolu_..."
  //             field 2 = name "run_command"
  //             field 3 = arguments_json
  //         field 35 = response_statistics (UI-facing token meters)
  //           field 2 = metric (repeated)
  //             field 5 = metric_id "input_tokens" / "output_tokens" /
  //                       "cached_input_tokens"
  //             field 4 = metric_data
  //               field 2 = value (fixed32 IEEE-754 float, NOT varint!)
  //
  // The walker tracks the full path stack so we never confuse e.g. a
  // workspace metadata `field 3` string at depth 4 with the assistant
  // message body at the SAME depth — paths disambiguate.
  std::vector<std::uint32_t> path;
  std::string lastMetricId;
  std::vector<firmius::shared::ToolCall> pendingTools;
  std::string toolUseId, toolName, toolArgs;
  std::uint32_t lastSeenToolField = 0;

  walkProtoPath(payload, path, [&](const std::vector<std::uint32_t> &p,
                                   const ProtoField &f) {
    // ---- Assistant text body ----
    // Path tails: [..., 20, 3] for text, [..., 20, 3] *again* for reasoning
    // — same field, but reasoning blocks are commonly long-form prose.
    // Different model families plant the user-visible reply in different
    // sub-fields; we accept the largest field-3 string under field-20 as a
    // working approximation until we have per-model anchors.
    if (f.wire == 2 && pathEndsWith(p, {20, 3}) && isPrintableUtf8(f.bytes) &&
        f.bytes.size() > state.text.size() && f.bytes.size() > 1) {
      state.text.assign(f.bytes.data(), f.bytes.size());
    }

    // ---- Tool call assembly ----
    // Path tail: [..., 20, 7] enters a single ToolCall message; the inner
    // fields are siblings.
    if (f.wire == 2 && pathEndsWith(p, {20, 7})) {
      // New tool call started — flush any previous pending tool.
      if (!toolUseId.empty()) {
        firmius::shared::ToolCall tc;
        tc.id = std::move(toolUseId);
        tc.name = std::move(toolName);
        tc.args = std::move(toolArgs);
        state.toolCalls.push_back(std::move(tc));
      }
      toolUseId.clear();
      toolName.clear();
      toolArgs.clear();
      lastSeenToolField = 7;
      return;
    }
    if (lastSeenToolField == 7 && f.wire == 2 && isPrintableUtf8(f.bytes)) {
      if (f.field == 1) toolUseId.assign(f.bytes.data(), f.bytes.size());
      else if (f.field == 2) toolName.assign(f.bytes.data(), f.bytes.size());
      else if (f.field == 3) toolArgs.assign(f.bytes.data(), f.bytes.size());
    }

    // ---- Token usage metrics (response_statistics) ----
    // metric_id sits at [..., 35, 2, 5]; the matching value is at
    // [..., 35, 2, 4, 2] as a little-endian IEEE-754 float32. We capture
    // the ID and pair it with the next fixed32 we see at the value path.
    if (f.wire == 2 && f.field == 5 && pathEndsWith(p, {35, 2, 5}) &&
        isPrintableUtf8(f.bytes)) {
      lastMetricId.assign(f.bytes.data(), f.bytes.size());
    }
    if (f.wire == 5 && pathEndsWith(p, {35, 2, 4, 2}) && !lastMetricId.empty()) {
      float v;
      std::memcpy(&v, &f.v, 4);
      auto count = static_cast<std::uint32_t>(v < 0 ? 0 : v);
      if (lastMetricId == "input_tokens") state.inputTokens = count;
      else if (lastMetricId == "output_tokens") state.outputTokens = count;
      else if (lastMetricId == "cached_input_tokens" ||
               lastMetricId == "cache_read_tokens") state.cacheRead = count;
      else if (lastMetricId == "cache_creation_tokens") state.cacheWrite = count;
      lastMetricId.clear();
    }
  });

  // Flush any tail tool call.
  if (!toolUseId.empty()) {
    firmius::shared::ToolCall tc;
    tc.id = std::move(toolUseId);
    tc.name = std::move(toolName);
    tc.args = std::move(toolArgs);
    state.toolCalls.push_back(std::move(tc));
  }
}

}  // anonymous namespace

// =============================================================================
// Cascade-id persistence
// =============================================================================

std::filesystem::path WindsurfProvider::cascadeMapPath() const {
  const char *home = std::getenv("HOME");
  std::filesystem::path base = home ? home : ".";
  return base / ".firmius" / "windsurf_cascades.json";
}

void WindsurfProvider::loadCascadeMap() {
  std::lock_guard<std::mutex> lock(cascadeMutex_);
  cascadeByThread_.clear();
  auto p = cascadeMapPath();
  if (!std::filesystem::exists(p)) return;
  std::ifstream f(p);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  rapidjson::Document doc;
  doc.Parse(ss.str().c_str());
  if (!doc.IsObject()) return;
  // Schema versions in the wild:
  //   v0: { "<threadId>": "<cascade_id>" }
  //   v1: { "<threadId>": { "cascade": "...", "csrf": "<lsp-csrf>" } }
  //   v2: { "<threadId>": { "cascade": "...", "csrf": "...",
  //                         "stepOffset": 123 } }
  // v0/v1 entries have no reliable offset. On reuse, streamChatLocal snapshots
  // the existing trajectory length before sending the next user message so old
  // assistant/tool steps are not replayed as fresh output.
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    if (it->value.IsString()) {
      cascadeByThread_[it->name.GetString()] = {it->value.GetString(),
                                                std::string{}, 0};
    } else if (it->value.IsObject() && it->value.HasMember("cascade") &&
               it->value["cascade"].IsString()) {
      std::string csrf;
      if (it->value.HasMember("csrf") && it->value["csrf"].IsString()) {
        csrf = it->value["csrf"].GetString();
      }
      std::uint32_t stepOffset = 0;
      if (it->value.HasMember("stepOffset") && it->value["stepOffset"].IsUint()) {
        stepOffset = it->value["stepOffset"].GetUint();
      }
      cascadeByThread_[it->name.GetString()] = {
          it->value["cascade"].GetString(), csrf, stepOffset};
    }
  }
}

void WindsurfProvider::saveCascadeMap() const {
  std::lock_guard<std::mutex> lock(cascadeMutex_);
  auto p = cascadeMapPath();
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  for (const auto &kv : cascadeByThread_) {
    rapidjson::Value k(kv.first.c_str(), alloc);
    rapidjson::Value entry(rapidjson::kObjectType);
    entry.AddMember("cascade",
                    rapidjson::Value(kv.second.cascadeId.c_str(), alloc),
                    alloc);
    entry.AddMember("csrf",
                    rapidjson::Value(kv.second.csrf.c_str(), alloc), alloc);
    entry.AddMember("stepOffset", kv.second.stepOffset, alloc);
    doc.AddMember(k, entry, alloc);
  }
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  doc.Accept(w);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (f) f.write(sb.GetString(), sb.GetSize());
}

WindsurfProvider::CascadePersist
WindsurfProvider::getCascadeForThread(const std::string &t,
                                      const std::string &currentCsrf) {
  std::lock_guard<std::mutex> lock(cascadeMutex_);
  auto it = cascadeByThread_.find(t);
  if (it == cascadeByThread_.end()) return {};
  // Stale entry from a previous LSP boot — the LSP doesn't know this
  // cascade_id, so reusing it makes SendUserCascadeMessage 200 into the
  // void and StreamCascadeReactiveUpdates produces zero envelopes. Drop it
  // and let the caller mint a fresh one.
  if (!it->second.csrf.empty() && it->second.csrf != currentCsrf) {
    cascadeByThread_.erase(it);
    return {};
  }
  return it->second;
}

void WindsurfProvider::setCascadeForThread(const std::string &t,
                                           const std::string &c,
                                           const std::string &csrf,
                                           std::uint32_t stepOffset) {
  {
    std::lock_guard<std::mutex> lock(cascadeMutex_);
    cascadeByThread_[t] = {c, csrf, stepOffset};
  }
  saveCascadeMap();
}

void WindsurfProvider::setCascadeStepOffsetForThread(
    const std::string &t, std::uint32_t stepOffset) {
  {
    std::lock_guard<std::mutex> lock(cascadeMutex_);
    auto it = cascadeByThread_.find(t);
    if (it == cascadeByThread_.end()) return;
    it->second.stepOffset = stepOffset;
  }
  saveCascadeMap();
}

// =============================================================================
// streamChatLocal — public entry point (called from stream() in
// WindsurfProvider_grpc.cpp).
// =============================================================================

bool WindsurfProvider::streamChatLocal(
    const firmius::shared::OAuthAccount &acc, const std::string &modelUid,
    int modelEnum,
    const firmius::shared::AgentHistory &history, const ProviderOptions &opts,
    std::function<void(const firmius::shared::StreamEvent &)> onEvent) {
  const bool debug = std::getenv("FIRMIUS_WINDSURF_DEBUG") != nullptr;

  // 1) Acquire a language_server child for THIS account.
  //
  // v2 spawn path (default): we own the child, we wrote `acc.accessToken`
  // into its stdin, so every outbound call from the LSP carries OUR
  // api_key. The user's IDE quota is never touched regardless of which
  // account they happen to be logged into in the Windsurf desktop app.
  //
  // v1 reuse path (FIRMIUS_WINDSURF_REUSE_IDE=1): keep the original
  // /proc-scan behaviour for debugging. Dangerous in production because
  // it leaks the IDE's api_key onto our requests; behind an env knob.
  std::string err;
  WindsurfLspManager::Endpoint ep;
  bool useReuse = false;
  if (const char *reuse = std::getenv("FIRMIUS_WINDSURF_REUSE_IDE")) {
    useReuse = (reuse[0] != '\0' && reuse[0] != '0');
  }

  LocalLspInfo lsp;  // legacy carrier struct, populated either way
  if (useReuse) {
    if (!discoverLocalLsp(lsp, err)) {
      onEvent(firmius::shared::StreamError{err, 0, acc.identifier});
      onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
      return false;
    }
  } else {
    {
      std::lock_guard<std::mutex> lk(lspManagerMutex_);
      if (!lspManager_) {
        lspManager_ = std::make_unique<WindsurfLspManager>();
      }
    }
    if (!lspManager_->ensureRunning(acc, ep, err)) {
      onEvent(firmius::shared::StreamError{
          "Failed to spawn Windsurf language_server: " + err, 0,
          acc.identifier});
      onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
      return false;
    }
    lsp.pid = ep.pid;
    lsp.port = ep.port;
    lsp.csrf = ep.csrf;
  }
  if (debug) {
    std::fprintf(stderr,
                 "[windsurf-local] lsp pid=%d port=%d csrf=%.8s...\n",
                 lsp.pid, lsp.port, lsp.csrf.c_str());
  }

  // 2) Resolve cascade_id and trajectory offset for this thread.
  //
  // Reuse if (a) we've seen this threadId before AND (b) the stored entry
  // belongs to the SAME LSP boot (csrf matches). When (b) fails we silently
  // remint — the LSP forgot the cascade across its restart and reusing
  // would hang the stream silently.
  //
  // When reusing a cascade, never poll from step 0 again: the trajectory
  // contains prior assistant/tool steps. Replaying them makes old output look
  // like the answer to the new prompt and can instantly re-execute old tools.
  auto cascade = getCascadeForThread(history.threadId, lsp.csrf);
  std::string cascadeId = cascade.cascadeId;
  std::uint32_t trajectoryStepOffset = cascade.stepOffset;
  if (cascadeId.empty()) {
    if (!startCascade(lsp.port, lsp.csrf, acc.accessToken, cascadeId, err)) {
      onEvent(firmius::shared::StreamError{err, 0, acc.identifier});
      onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
      return false;
    }
    setCascadeForThread(history.threadId, cascadeId, lsp.csrf, 0);
    trajectoryStepOffset = 0;
    if (debug) {
      std::fprintf(stderr, "[windsurf-local] minted cascade_id=%s for thread=%s\n",
                   cascadeId.c_str(), history.threadId.c_str());
    }
  } else {
    // Backfill legacy v0/v1 cascade entries that do not have a persisted
    // offset. Snapshot the pre-existing trajectory before sending the new
    // user message, then poll from that absolute boundary.
    if (trajectoryStepOffset == 0) {
      auto resumeBody = buildGetTrajectoryStepsRequest(cascadeId, 0);
      auto resumeResp = connectUnary(
          lsp.port, lsp.csrf,
          "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectorySteps",
          resumeBody);
      if (resumeResp.httpCode == 200) {
        auto existingSteps = parseTrajectorySteps(resumeResp.body);
        trajectoryStepOffset = static_cast<std::uint32_t>(existingSteps.size());
        setCascadeStepOffsetForThread(history.threadId, trajectoryStepOffset);
      }
    }
    if (debug) {
      std::fprintf(stderr,
                   "[windsurf-local] reusing cascade_id=%s for thread=%s "
                   "from step_offset=%u (prompt cache should hit)\n",
                   cascadeId.c_str(), history.threadId.c_str(),
                   trajectoryStepOffset);
    }
  }

  // 3) Init panel state (idempotent, cheap).
  if (!initCascadePanel(lsp.port, lsp.csrf, acc.accessToken, err)) {
    if (debug) std::fprintf(stderr, "[windsurf-local] init panel: %s\n", err.c_str());
    // Non-fatal — proceed.
  }

  // 4) Build the user-message text + collect images.
  //
  // The Windsurf Cascade trajectory is useful for prompt-cache continuity but
  // is not a reliable substitute for the caller-visible AgentHistory. We still
  // serialize prior Firmius turns into the latest prompt so the model sees the
  // actual user task across turns. The persisted stepOffset prevents old
  // trajectory outputs from being replayed as fresh assistant text.
  //
  // History flattening:
  //   - Walk turns in order.
  //   - For each message, render content parts into a wrapper block.
  //   - The LATEST user message becomes the active prompt; everything before
  //     it is wrapped in <human>/<assistant>/<tool_result> tags.
  std::string userText;
  std::vector<firmius::shared::ImageContent> images;

  auto renderToolResult =
      [](const firmius::shared::ToolResultContent &tr) -> std::string {
    std::string out = "<tool_result tool_call_id=\"" + tr.toolCallId + "\"";
    if (!tr.success) out += " success=\"false\"";
    out += ">\n" + tr.result + "\n</tool_result>";
    return out;
  };
  auto renderToolCall =
      [](const firmius::shared::ToolCallContent &tc) -> std::string {
    // Re-canonicalise prior assistant tool calls so the model recognizes its
    // own format if it scans the history.
    std::string args = tc.args.empty() ? "{}" : tc.args;
    return "<tool_call>{\"name\":\"" + tc.name +
           "\",\"arguments\":" + args + "}</tool_call>";
  };
  auto messageToText = [&](const firmius::shared::Message &m) -> std::string {
    std::string buf;
    for (const auto &p : m.content) {
      if (auto *t = std::get_if<firmius::shared::TextContent>(&p)) {
        buf += t->text;
      } else if (auto *tk = std::get_if<firmius::shared::ThinkingContent>(&p)) {
        // Skip thinking blocks in replay — they're internal.
        (void)tk;
      } else if (auto *tc =
                     std::get_if<firmius::shared::ToolCallContent>(&p)) {
        if (!buf.empty()) buf += "\n";
        buf += renderToolCall(*tc);
      } else if (auto *tr =
                     std::get_if<firmius::shared::ToolResultContent>(&p)) {
        if (!buf.empty()) buf += "\n";
        buf += renderToolResult(*tr);
      } else if (auto *img =
                     std::get_if<firmius::shared::ImageContent>(&p)) {
        // Defer image collection to the LATEST user message only; older
        // images become text placeholders.
        (void)img;
      }
    }
    return buf;
  };

  // Find latest user message. Anything after it (usually ToolResult messages
  // from the just-executed tool batch) is the active follow-up payload for
  // this provider turn.
  std::vector<std::pair<firmius::shared::Role, std::string>> historyBlocks;
  std::vector<std::pair<firmius::shared::Role, std::string>> trailingBlocks;
  const firmius::shared::Message *latestUserMessage = nullptr;
  std::string latestUser;
  bool foundLatest = false;
  for (auto turnIt = history.turns.rbegin();
       turnIt != history.turns.rend() && !foundLatest; ++turnIt) {
    for (auto msgIt = turnIt->messages.rbegin();
         msgIt != turnIt->messages.rend(); ++msgIt) {
      if (msgIt->role == firmius::shared::Role::User && !foundLatest) {
        // This is the latest user message — pull its text and images.
        latestUserMessage = &(*msgIt);
        for (const auto &p : msgIt->content) {
          if (auto *t = std::get_if<firmius::shared::TextContent>(&p)) {
            latestUser += t->text;
          } else if (auto *img =
                         std::get_if<firmius::shared::ImageContent>(&p)) {
            images.push_back(*img);
          }
        }
        foundLatest = true;
      }
    }
  }
  if (!foundLatest) {
    onEvent(firmius::shared::StreamError{
        "Windsurf local: no user text content in history.", 0, acc.identifier});
    onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
    return false;
  }

  // Walk history forward. Messages before the latest user become replay
  // history for fresh cascades. ToolResult/System messages after the latest
  // user are active follow-up input, because Agent.cpp appends tool results
  // as separate turns and immediately calls the provider again with no new
  // user message. Assistant messages after the latest user are already present
  // in the reused Cascade trajectory, so do not echo them back as user text.
  bool passedLatestUser = false;
  for (const auto &turn : history.turns) {
    for (const auto &m : turn.messages) {
      if (&m == latestUserMessage) {
        passedLatestUser = true;
        continue;
      }
      if (passedLatestUser && m.role == firmius::shared::Role::Assistant) {
        continue;
      }
      auto text = messageToText(m);
      if (passedLatestUser) {
        trailingBlocks.emplace_back(m.role, std::move(text));
      } else {
        historyBlocks.emplace_back(m.role, std::move(text));
      }
    }
  }

  auto renderBlocks = [](const std::vector<std::pair<firmius::shared::Role, std::string>> &blocks) {
    std::string out;
    for (const auto &[role, text] : blocks) {
      if (text.empty()) continue;
      const char *tag = nullptr;
      switch (role) {
      case firmius::shared::Role::User:      tag = "human";     break;
      case firmius::shared::Role::Assistant: tag = "assistant"; break;
      case firmius::shared::Role::System:    tag = "system";    break;
      case firmius::shared::Role::ToolResult: tag = nullptr;    break;
      default: tag = nullptr; break;
      }
      if (tag) {
        out += "<";
        out += tag;
        out += ">\n" + text + "\n</";
        out += tag;
        out += ">\n\n";
      } else {
        out += text + "\n\n";
      }
    }
    return out;
  };
  const std::string trailingText = renderBlocks(trailingBlocks);

  const bool hasHistoryContext = !historyBlocks.empty();
  std::string historyPrefix;
  if (hasHistoryContext) {
    historyPrefix +=
        "The following is a multi-turn conversation. You MUST remember "
        "and use all information from prior turns.\n\n";
    historyPrefix += renderBlocks(historyBlocks);
  }

  if (!trailingText.empty()) {
    userText = historyPrefix +
               "<human>\n" + latestUser + "\n</human>\n\n" +
               trailingText +
               "Continue the task using the tool results above. If more "
               "information is needed, call another tool; otherwise answer "
               "the user directly.";
  } else if (hasHistoryContext) {
    userText = historyPrefix +
               "<human>\n" + latestUser + "\n</human>";
  } else {
    userText = latestUser;
  }

  // 5) Send the user message.
  //
  // We used to subscribe to StreamCascadeReactiveUpdates BEFORE sending and
  // wait for envelopes. That works only when an Extension server is attached
  // (the IDE) — headless our spawned LSP accepts the subscription but never
  // pushes updates. Instead we follow dwgx/WindsurfAPI's verified-working
  // pattern: send the message, then poll GetCascadeTrajectorySteps every
  // 250ms and emit deltas off the trajectory until the final step's status
  // becomes terminal (>= DONE) or no growth is observed for several seconds.
  auto sendBody = buildSendUserCascadeMessage(cascadeId, userText,
                                              acc.accessToken, modelUid,
                                              modelEnum, opts.tools, images);
  auto sendResp = connectUnary(
      lsp.port, lsp.csrf,
      "/exa.language_server_pb.LanguageServerService/SendUserCascadeMessage",
      sendBody);
  if (sendResp.httpCode != 200) {
    onEvent(firmius::shared::StreamError{
        "SendUserCascadeMessage HTTP " + std::to_string(sendResp.httpCode) +
            (sendResp.body.empty() ? "" : ": " + sendResp.body.substr(0, 240)),
        static_cast<int>(sendResp.httpCode), acc.identifier});
    onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
    return false;
  }
  if (debug) {
    std::fprintf(stderr,
                 "[windsurf-local] SendUserCascadeMessage 200 (cascade=%s "
                 "model=%s text=%zuB)\n",
                 cascadeId.c_str(), modelUid.c_str(), userText.size());
  }

  // 6) Poll GetCascadeTrajectorySteps for assistant deltas.
  //
  // Per-step state we need to track:
  //   - per-step text cursor: how many bytes of `responseText` we've emitted
  //   - per-step thinking cursor
  //   - per-step toolCalls already surfaced (dedupe by id)
  //   - per-step usage (we sum across steps at the end)
  //
  // Termination:
  //   - status >= 3 (DONE) on the latest step  → finalize
  //   - no growth (no new step, no text/thinking growth, no new tool call)
  //     for ~12s after we've already seen text → assume the LS is stuck or
  //     the model finished without flipping the status. Bail out cleanly.
  //   - hard ceiling 180s.
  std::map<std::size_t, std::size_t> textCursor;
  std::map<std::size_t, std::size_t> thinkCursor;
  std::set<std::string> emittedToolIds;
  std::uint64_t totalInput = 0, totalOutput = 0,
                totalCacheRead = 0, totalCacheWrite = 0;
  std::string lastFullText;

  // Streaming `<tool_call>` parser state — one buffer per step. Holds any
  // tail bytes that COULD be the start of an open-tag so we don't leak
  // partial `<tool_ca` to the text channel and then re-emit the same bytes
  // when the rest arrives. Closed `<tool_call>{...}</tool_call>` blocks are
  // parsed and emitted as ToolCall events; text outside the markers passes
  // through normally.
  struct ToolParser {
    std::string buffer; // unflushed tail (may contain partial open tag)
    bool inCall = false; // currently between <tool_call> and </tool_call>
  };
  std::map<std::size_t, ToolParser> toolParsers;
  std::set<std::string> emittedToolCallIds;
  int toolCallSerial = 0;
  auto findOpenOrTail = [](const std::string &s) -> std::size_t {
    // Returns the offset where it's no longer SAFE to flush as plain text,
    // i.e. the start of a complete or partial `<tool_call>` open tag.
    static const std::string kOpen = "<tool_call>";
    auto pos = s.find(kOpen);
    if (pos != std::string::npos) return pos;
    // Check for any prefix of the open tag at the end of s.
    for (std::size_t prefix = std::min<std::size_t>(s.size(), kOpen.size() - 1);
         prefix > 0; --prefix) {
      if (s.compare(s.size() - prefix, prefix, kOpen, 0, prefix) == 0) {
        return s.size() - prefix;
      }
    }
    return s.size();
  };
  auto extractName = [](const std::string &json) -> std::string {
    // crude key:"value" extractor — enough for our `name` field.
    auto pos = json.find("\"name\"");
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    auto end = pos + 1;
    while (end < json.size() && json[end] != '"') {
      if (json[end] == '\\' && end + 1 < json.size()) ++end;
      ++end;
    }
    return json.substr(pos + 1, end - pos - 1);
  };
  auto extractArgs = [](const std::string &json) -> std::string {
    // Find the "arguments" key; the value is a JSON object — copy it
    // verbatim by tracking brace depth from the next `{`.
    auto k = json.find("\"arguments\"");
    if (k == std::string::npos) return "{}";
    auto colon = json.find(':', k);
    if (colon == std::string::npos) return "{}";
    auto open = json.find('{', colon);
    if (open == std::string::npos) return "{}";
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (std::size_t i = open; i < json.size(); ++i) {
      char c = json[i];
      if (esc) { esc = false; continue; }
      if (c == '\\' && inStr) { esc = true; continue; }
      if (c == '"') { inStr = !inStr; continue; }
      if (inStr) continue;
      if (c == '{') ++depth;
      if (c == '}') {
        --depth;
        if (depth == 0) return json.substr(open, i - open + 1);
      }
    }
    return "{}";
  };
  // Per-step parsed tool call id+index. We allocate these when an open tag is
  // seen, but only emit tool events after a complete name+arguments body.
  struct PendingToolCall {
    std::string id;
    std::uint32_t index = 0;
    bool open = false;
  };
  std::map<std::size_t, PendingToolCall> pendingTC;
  std::map<std::size_t, std::uint32_t> nextToolIndex;

  auto feedToolParser = [&](std::size_t stepIdx, std::string_view delta) {
    auto &tp = toolParsers[stepIdx];
    tp.buffer.append(delta.data(), delta.size());
    static const std::string kOpen = "<tool_call>";
    static const std::string kClose = "</tool_call>";
    while (true) {
      if (tp.inCall) {
        auto closeIdx = tp.buffer.find(kClose);
        if (closeIdx == std::string::npos) return;
        std::string body = tp.buffer.substr(0, closeIdx);
        tp.buffer.erase(0, closeIdx + kClose.size());
        tp.inCall = false;
        std::string name = extractName(body);
        std::string args = extractArgs(body);
        auto &pending = pendingTC[stepIdx];
        if (!name.empty()) {
          // Emit a final ToolCallChunk carrying the full name + args
          // (clients that consume chunks see the assembled payload here).
          onEvent(firmius::shared::ToolCallChunk{pending.id, pending.index,
                                                 name, args});
          // And the canonical `ToolCall` for executors.
          firmius::shared::ToolCall call;
          call.id = pending.id;
          call.index = pending.index;
          call.name = std::move(name);
          call.args = std::move(args);
          if (!emittedToolCallIds.count(call.id)) {
            emittedToolCallIds.insert(call.id);
            onEvent(call);
          }
        }
        pending.open = false;
        continue;
      }
      auto openIdx = tp.buffer.find(kOpen);
      if (openIdx != std::string::npos) {
        // Flush text BEFORE the open tag.
        if (openIdx > 0) {
          onEvent(firmius::shared::TextChunk{tp.buffer.substr(0, openIdx)});
        }
        tp.buffer.erase(0, openIdx + kOpen.size());
        tp.inCall = true;
        // Mint an id for this tool call, but do not emit a ToolCallChunk yet.
        // If the model never closes the XML block, an early nameless chunk
        // becomes an unresolved tool call and Agent.cpp rejects the turn as
        // "missing tool name". Emulated tool calls are only executable once
        // we have parsed a complete name+arguments body.
        auto &pending = pendingTC[stepIdx];
        pending.id =
            "call_" + std::to_string(toolCallSerial++) + "_" +
            std::to_string(static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()));
        pending.index = nextToolIndex[stepIdx]++;
        pending.open = true;
        continue;
      }
      // No open tag visible. Find the longest prefix of `<tool_call>` at
      // the end of the buffer; flush everything before it as text and hold
      // the rest until the next delta.
      std::size_t safe = findOpenOrTail(tp.buffer);
      if (safe > 0) {
        onEvent(firmius::shared::TextChunk{tp.buffer.substr(0, safe)});
        tp.buffer.erase(0, safe);
      }
      return;
    }
  };

  auto pollStart = std::chrono::steady_clock::now();
  auto lastGrowth = pollStart;
  bool sawText = false;
  bool sawDoneStatus = false;
  std::string finalErrorText;
  int pollCount = 0;
  std::size_t lastStepCount = 0;
  std::vector<TrajectoryStep> lastSteps;

  while (true) {
    if (opts.abortSignal && opts.abortSignal->load()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (opts.abortSignal && opts.abortSignal->load()) break;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - pollStart)
                       .count();
    auto sinceGrowth = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - lastGrowth)
                           .count();
    if (elapsed > 180000) {
      finalErrorText = "Cascade poll timed out after 180s";
      break;
    }
    if (sawText && sinceGrowth > 12000) {
      // Model returned text but never flipped status to DONE. Treat as
      // a clean finish — better than hanging the user.
      sawDoneStatus = true;
      break;
    }
    if (!sawText && elapsed > 60000) {
      finalErrorText = "Cascade planner stalled — no output after 60s";
      break;
    }

    auto stepsBody = buildGetTrajectoryStepsRequest(cascadeId, trajectoryStepOffset);
    auto stepsResp = connectUnary(
        lsp.port, lsp.csrf,
        "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectorySteps",
        stepsBody);
    pollCount++;
    if (stepsResp.httpCode != 200) {
      // Non-fatal; the LS sometimes returns 503 mid-warmup. Skip & retry.
      if (debug) {
        std::fprintf(stderr,
                     "[windsurf-local] poll %d: HTTP %ld (skipping)\n",
                     pollCount, stepsResp.httpCode);
      }
      continue;
    }

    auto steps = parseTrajectorySteps(stepsResp.body);
    if (debug && pollCount <= 2) {
      std::fprintf(stderr,
                   "[windsurf-local] poll %d body=%zuB steps=%zu\n",
                   pollCount, stepsResp.body.size(), steps.size());
      // Hex dump first 512 bytes of body for proto inspection.
      std::fprintf(stderr, "  hex:");
      for (std::size_t i = 0; i < std::min<std::size_t>(stepsResp.body.size(), 512); ++i) {
        if (i % 32 == 0) std::fprintf(stderr, "\n   ");
        std::fprintf(stderr, " %02x",
                     static_cast<unsigned char>(stepsResp.body[i]));
      }
      std::fprintf(stderr, "\n");
      for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto &s = steps[i];
        std::fprintf(stderr,
                     "  step[%zu] type=%d status=%d text=%zuB think=%zuB "
                     "mod=%zuB err=%s\n",
                     i, s.type, s.status, s.responseText.size(),
                     s.thinking.size(), s.modifiedText.size(),
                     s.errorText.empty() ? "-"
                                         : s.errorText.substr(0, 80).c_str());
      }
    }
    if (steps.size() != lastStepCount) {
      lastStepCount = steps.size();
      lastGrowth = now;
    }

    for (std::size_t i = 0; i < steps.size(); ++i) {
      const auto &s = steps[i];

      // ---- Error step (CORTEX_STEP_TYPE_ERROR_MESSAGE = 17) ----
      if (s.type == 17 && !s.errorText.empty()) {
        finalErrorText = s.errorText;
        sawDoneStatus = true;
        break;
      }

      // ---- Tool calls (none expected in NO_TOOL mode but parse anyway) ----
      // (The current parseTrajectorySteps doesn't yet pull tool_call_proposal
      // / tool_call_choice — left as a TODO; chat-only models don't need it.)

      // ---- Text delta (responseText) — routed through tool-call parser.
      // The parser strips `<tool_call>...</tool_call>` blocks from the text
      // stream and surfaces them as ToolCall events. Anything else passes
      // through as TextChunk.
      const std::string &live = s.responseText;
      auto prev = textCursor[i];
      if (live.size() > prev) {
        std::string_view delta(live);
        delta.remove_prefix(prev);
        textCursor[i] = live.size();
        lastGrowth = now;
        sawText = true;
        feedToolParser(i, delta);
      }

      // ---- Thinking delta ----
      const std::string &think = s.thinking;
      auto prevT = thinkCursor[i];
      if (think.size() > prevT) {
        std::string_view delta(think);
        delta.remove_prefix(prevT);
        thinkCursor[i] = think.size();
        lastGrowth = now;
        onEvent(firmius::shared::ThinkingChunk{std::string(delta), std::string{}});
      }

      // ---- Usage (overwrite to latest, sum across steps at end) ----
      if (s.inputTokens || s.outputTokens || s.cacheReadTokens ||
          s.cacheWriteTokens) {
        // Track latest per-step values (they grow monotonically).
        // We just add into running totals at the end of the loop.
      }
    }

    if (!finalErrorText.empty()) break;

    // Save the latest snapshot so the post-loop topup has steps regardless
    // of which exit branch we took (allDone, warm-stall, cold-stall, etc.).
    lastSteps = steps;

    // We DON'T early-exit on `steps.back().status >= 3`. The first steps in
    // the trajectory (PLAN_INPUT, prompt inputs etc.) arrive synchronously
    // at DONE status before the assistant has even started planning. The
    // assistant's planner_response is appended as a *later* step that
    // transitions PENDING → ACTIVE → DONE. When ALL steps are DONE *and*
    // we have already seen text, that's a strong signal the planner step
    // is finished — fall out and let the post-loop topup run once.
    if (sawText && !steps.empty()) {
      bool allDone = true;
      for (const auto &s : steps) {
        if (s.status < 3) { allDone = false; break; }
      }
      if (allDone) {
        sawDoneStatus = true;
        break;
      }
    }
  }

  // Post-loop finalisation. Runs on every non-error exit (allDone,
  // warm-stall, cold-stall, hard-ceiling). Does ONE last poll to catch
  // any responseText growth that arrived after the last in-loop poll, and
  // tops up with modifiedText (LS post-pass rewrite — usually a polished,
  // longer version of responseText) so we don't truncate mid-sentence.
  if (finalErrorText.empty() && (sawText || sawDoneStatus)) {
    auto finalBody = buildGetTrajectoryStepsRequest(cascadeId, trajectoryStepOffset);
    auto finalResp = connectUnary(
        lsp.port, lsp.csrf,
        "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectorySteps",
        finalBody);
    if (finalResp.httpCode == 200) {
      auto finalStepsParsed = parseTrajectorySteps(finalResp.body);
      if (!finalStepsParsed.empty()) lastSteps = finalStepsParsed;
    }
    for (std::size_t i = 0; i < lastSteps.size(); ++i) {
      const auto &s = lastSteps[i];
      // Trail responseText if any new bytes arrived since the last poll
      // (model may have appended more text right before flipping to DONE).
      if (!s.responseText.empty() &&
          s.responseText.size() > textCursor[i]) {
        std::string_view delta(s.responseText);
        delta.remove_prefix(textCursor[i]);
        textCursor[i] = s.responseText.size();
        if (!delta.empty()) feedToolParser(i, delta);
      }
      // modifiedText is the LS's post-pass rewrite (markdown fixups,
      // citations, tool-result folding). Only emit it when it is a strict
      // extension of responseText; if it rewrote the prefix, suffix slicing
      // would splice unrelated bytes onto already-streamed text.
      if (!s.modifiedText.empty() && s.modifiedText.size() > textCursor[i] &&
          s.modifiedText.rfind(s.responseText, 0) == 0) {
        std::string_view delta(s.modifiedText);
        delta.remove_prefix(textCursor[i]);
        textCursor[i] = s.modifiedText.size();
        if (!delta.empty()) feedToolParser(i, delta);
      }
    }
    for (const auto &s : lastSteps) {
      totalInput += s.inputTokens;
      totalOutput += s.outputTokens;
      totalCacheRead += s.cacheReadTokens;
      totalCacheWrite += s.cacheWriteTokens;
    }
    setCascadeStepOffsetForThread(
        history.threadId,
        trajectoryStepOffset + static_cast<std::uint32_t>(lastSteps.size()));
  }

  // 7) Emit final error / usage / done.
  if (!finalErrorText.empty()) {
    onEvent(firmius::shared::StreamError{
        finalErrorText, 0, acc.identifier});
    onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Error});
    if (debug) {
      std::fprintf(stderr, "[windsurf-local] turn errored: %s\n",
                   finalErrorText.c_str());
    }
    return false;
  }

  if (totalInput || totalOutput || totalCacheRead) {
    firmius::shared::AgentMetrics m;
    m.tokens.prompt = totalInput;
    m.tokens.completion = totalOutput;
    m.tokens.cacheRead = totalCacheRead;
    m.tokens.cacheWrite = totalCacheWrite;
    m.tokens.total = totalInput + totalOutput;
    onEvent(m);
  }
  onEvent(firmius::shared::StreamDone{firmius::shared::StopReason::Stop});
  if (debug) {
    std::fprintf(stderr,
                 "[windsurf-local] turn done. polls=%d done_status=%d\n",
                 pollCount, sawDoneStatus ? 1 : 0);
  }
  return true;
}

}  // namespace firmius::provider
