// =============================================================================
// WindsurfProvider — web-OAuth-driven Windsurf/Codeium chat provider.
//
// Auth strategy
// -------------
// We mirror the official windsurf.vim / windsurf.nvim / JetBrains plugin flow:
//
//   GET https://windsurf.com/profile
//       ?response_type=token
//       &redirect_uri=<URL>
//       &state=<random>
//       &scope=openid+profile+email
//       &redirect_parameters_type=query
//
// The user logs in with Google/GitHub on windsurf.com; the portal redirects to
// `redirect_uri` carrying a Firebase ID token. To honor the user's "no pasting"
// requirement we attempt three capture paths in priority order:
//
//   1. Loopback redirect — we spin up a one-shot HTTP listener on
//      http://127.0.0.1:<random-free-port>/cb and ask the portal to redirect
//      there directly. If the Windsurf IDP allows arbitrary HTTP redirects
//      (most public OAuth IDPs do for loopback), the JWT lands in our process
//      with zero user interaction.
//   2. Clipboard polling — Windsurf's hosted `show-auth-token` page auto-copies
//      the JWT to the system clipboard via JS. We poll xclip / wl-paste in the
//      background and accept the first JWT-shaped clipboard value seen after
//      launch.
//   3. Manual paste — surfaced as a wizard prompt as a final fallback if the
//      portal rejects loopback and no clipboard tool is available.
//
// Once we have the Firebase ID token we exchange it for a long-lived api_key:
//
//   POST https://api.codeium.com/register_user/
//        Content-Type: application/json
//        {"firebase_id_token": "<jwt>"}
//
//   → {"api_key": "<uuid>", "name": "<username>"}
//
// The api_key is stored as the OAuthAccount.accessToken (it doesn't expire).
//
// Streaming chat
// --------------
// We talk gRPC over HTTP/2 directly to https://server.codeium.com (no local
// Windsurf desktop install required). The proto schema mirrors the
// LanguageServerService RawGetChatMessage path used by the upstream
// rsvedant/opencode-windsurf-auth plugin, but routed at the cloud rather than
// the local language_server.
//
// Quota tracking
// --------------
// refreshQuotas() polls Codeium's GetUserStatus + GetUsageLimits cloud
// endpoints every 5 minutes per BaseOAuthProvider's background refresh thread.
// Plan tier ("Free", "Pro", "Pro Ultimate", "Teams", "Enterprise"), prompt
// credits used/limit, flow-action credits used/limit, and reset windows are
// surfaced as QuotaBuckets to Firmius's existing TUI quota panel.
// =============================================================================

#include "providers/WindsurfProvider.hpp"

#include "providers/WindsurfLspManager.hpp"
#include "providers/WindsurfModels.hpp"
#include "utils/GCPHttpClient.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace firmius::provider {

namespace {

// ---------------------------------------------------------------------------
// Endpoints & constants
// ---------------------------------------------------------------------------

constexpr const char *kAuthPortalUrl = "https://windsurf.com/profile";
constexpr const char *kRegisterUserUrl =
    "https://api.codeium.com/register_user/";
constexpr const char *kUserAgent = "firmius-windsurf/1.0 (linux; x86_64)";

// Endpoints used by the streaming/quota path (will be referenced from the
// gRPC implementation translation unit).
[[maybe_unused]] constexpr const char *kCodeiumServer =
    "https://server.codeium.com";
[[maybe_unused]] constexpr const char *kIdeName = "windsurf";
[[maybe_unused]] constexpr const char *kIdeVersionFallback = "1.13.104";
[[maybe_unused]] constexpr const char *kExtensionVersionFallback = "1.36.4";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string makeUuid() {
  // RFC 4122 v4-ish; not cryptographically strong but unique enough for
  // session ids and conversation ids.
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> dist;
  std::uint64_t a = dist(rng);
  std::uint64_t b = dist(rng);
  // Set version (4) and variant bits.
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
  return std::string(buf);
}

std::string urlEncode(const std::string &s) {
  std::string out;
  out.reserve(s.size() * 3);
  static const char *hex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

bool tryOpenBrowser(const std::string &url) {
  // Linux first (USER's OS). Fall through to xdg-open / open / start for
  // portability. Always background the spawn — we don't want to block the
  // wizard waiting for a browser process.
  std::string cmd;
#if defined(__APPLE__)
  cmd = "open \"" + url + "\" >/dev/null 2>&1 &";
#elif defined(_WIN32)
  cmd = "start \"\" \"" + url + "\"";
#else
  cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
#endif
  return std::system(cmd.c_str()) == 0;
}

std::string trimWhitespace(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.pop_back();
  return s;
}

// JWTs are three base64url segments separated by '.'. Validates only the
// shape, not the signature — enough for clipboard sniffing to dedupe garbage.
bool looksLikeJwt(const std::string &candidate) {
  if (candidate.size() < 32 || candidate.size() > 8192) return false;
  int dots = 0;
  for (char c : candidate) {
    if (c == '.') {
      ++dots;
      continue;
    }
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
      return false;
    }
  }
  return dots == 2;
}

bool looksLikeOAuthToken(const std::string &candidate) {
  if (looksLikeJwt(candidate)) {
    return true;
  }
  if (candidate.size() < 12 || candidate.size() > 8192) {
    return false;
  }
  for (char c : candidate) {
    if (std::isspace(static_cast<unsigned char>(c)) ||
        static_cast<unsigned char>(c) < 0x20) {
      return false;
    }
  }
  return true;
}

// Read first non-empty stdout line from a system command. Returns empty on
// failure. Used to sample clipboard contents.
std::string readSystemOutput(const char *cmd) {
  FILE *p = ::popen(cmd, "r");
  if (!p) return {};
  std::string out;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), p)) {
    out.append(buf);
    if (out.size() > 65536) break; // sanity cap
  }
  ::pclose(p);
  return trimWhitespace(out);
}

std::string readClipboard() {
  // Try, in order: wl-paste (Wayland), xclip (X11), xsel (X11 fallback).
  static const char *kCmds[] = {
      "wl-paste --no-newline 2>/dev/null",
      "xclip -o -selection clipboard 2>/dev/null",
      "xsel --clipboard --output 2>/dev/null",
      nullptr,
  };
  for (const char **c = kCmds; *c; ++c) {
    auto out = readSystemOutput(*c);
    if (!out.empty()) return out;
  }
  return {};
}

// Decode middle (payload) segment of a JWT into UTF-8. Returns empty on
// failure. Only used to fish out email/sub for the account identifier.
std::string base64UrlDecode(const std::string &input) {
  std::string b64 = input;
  for (auto &c : b64) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
  }
  while (b64.size() % 4 != 0) b64.push_back('=');
  auto dec = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  int val = 0, bits = -8;
  for (unsigned char c : b64) {
    if (c == '=') break;
    int d = dec(c);
    if (d < 0) continue;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

// Pull "email" (or fallback "sub") out of a JWT payload.
std::string extractEmailFromJwt(const std::string &jwt) {
  auto first = jwt.find('.');
  if (first == std::string::npos) return {};
  auto second = jwt.find('.', first + 1);
  if (second == std::string::npos) return {};
  std::string payload = jwt.substr(first + 1, second - first - 1);
  std::string decoded = base64UrlDecode(payload);
  if (decoded.empty()) return {};
  rapidjson::Document doc;
  doc.Parse(decoded.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return {};
  if (doc.HasMember("email") && doc["email"].IsString()) {
    return doc["email"].GetString();
  }
  if (doc.HasMember("sub") && doc["sub"].IsString()) {
    return doc["sub"].GetString();
  }
  return {};
}

// ---------------------------------------------------------------------------
// Loopback OAuth listener
//
// One-shot HTTP/1.0 server bound to 127.0.0.1 on an ephemeral port. Accepts a
// single GET, parses ?token= or ?code= or ?id_token= from the query string,
// returns a friendly HTML "you can close this tab" page, and exits.
// ---------------------------------------------------------------------------

class LoopbackOauthListener {
public:
  // Bind on an ephemeral port. Returns false on socket error.
  bool start() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    socklen_t addrlen = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &addrlen) <
        0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    port_ = ntohs(addr.sin_port);
    if (::listen(fd_, 4) < 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    return true;
  }

  int port() const { return port_; }

  // Block up to timeoutMs waiting for a single request; populate outToken on
  // success. Thread-safe with stop() — stop() closes the listening socket
  // which wakes any blocked accept().
  bool waitForToken(int timeoutMs, std::string &outToken,
                    std::string &outState) {
    if (fd_ < 0) return false;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_, &rfds);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int sel = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) return false;

    int client = ::accept(fd_, nullptr, nullptr);
    if (client < 0) return false;

    std::string req;
    char buf[4096];
    // Read request headers (up to first \r\n\r\n or 16k cap).
    while (req.size() < 16384) {
      ssize_t n = ::recv(client, buf, sizeof(buf), 0);
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
      if (req.find("\r\n\r\n") != std::string::npos) break;
    }

    bool ok = parseQueryParams(req, outToken, outState);

    const char *body =
        ok ? "<!doctype html><meta charset=utf-8><title>Firmius "
             "Windsurf</title><style>body{font-family:system-ui;background:#"
             "0a0a0a;color:#e6e6e6;display:grid;place-items:center;height:"
             "100vh;margin:0}div{padding:32px;border:1px solid #2a2a2a;"
             "border-radius:12px;text-align:center}h1{margin:0 0 8px;font-"
             "weight:600}p{margin:8px 0;color:#a0a0a0}</style><div><h1>"
             "Firmius linked.</h1><p>You can close this tab and return to "
             "the terminal.</p></div>"
           : "<!doctype html><meta charset=utf-8><title>Firmius "
             "Windsurf</title><h1>Login failed</h1>"
             "<p>No token in query. Please return to Firmius and try again."
             "</p>";
    std::string resp = "HTTP/1.0 200 OK\r\n";
    resp += "Content-Type: text/html; charset=utf-8\r\n";
    resp += "Content-Length: " + std::to_string(std::strlen(body)) + "\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body;
    ::send(client, resp.data(), resp.size(), 0);
    ::shutdown(client, SHUT_RDWR);
    ::close(client);
    return ok;
  }

  void stop() {
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
      ::close(fd_);
      fd_ = -1;
    }
  }

  ~LoopbackOauthListener() { stop(); }

private:
  static bool parseQueryParams(const std::string &req, std::string &outToken,
                               std::string &outState) {
    // Request line: "GET /cb?token=...&state=... HTTP/1.1"
    auto sp1 = req.find(' ');
    if (sp1 == std::string::npos) return false;
    auto sp2 = req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    auto qpos = path.find('?');
    if (qpos == std::string::npos) return false;
    std::string query = path.substr(qpos + 1);

    // Sometimes the IDP delivers params in the URL fragment (#...) instead of
    // query (?...). The browser does not send fragments to the server, so the
    // hosted show-auth-token page typically has a JS shim that converts
    // fragment→query before redirecting to the loopback. If the IDP actually
    // gave us a fragment we won't see it here — that path falls through to
    // the clipboard / paste fallback.

    auto get = [&](const std::string &key) -> std::string {
      std::string needle = key + "=";
      auto pos = query.find(needle);
      while (pos != std::string::npos) {
        if (pos == 0 || query[pos - 1] == '&') {
          auto end = query.find('&', pos + needle.size());
          std::string raw =
              query.substr(pos + needle.size(),
                           end == std::string::npos
                               ? std::string::npos
                               : end - pos - needle.size());
          // url-decode
          std::string dec;
          dec.reserve(raw.size());
          for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '+') {
              dec.push_back(' ');
            } else if (raw[i] == '%' && i + 2 < raw.size()) {
              auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
              };
              int hi = hex(raw[i + 1]);
              int lo = hex(raw[i + 2]);
              if (hi < 0 || lo < 0) {
                dec.push_back(raw[i]);
              } else {
                dec.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
              }
            } else {
              dec.push_back(raw[i]);
            }
          }
          return dec;
        }
        pos = query.find(needle, pos + 1);
      }
      return {};
    };

    outState = get("state");
    // Try multiple known param names — Codeium portal uses `token`, but other
    // OAuth flows use `id_token` / `access_token` / `code`.
    for (const char *k : {"token", "id_token", "access_token", "code"}) {
      auto v = get(k);
      if (!v.empty()) {
        outToken = v;
        return looksLikeOAuthToken(v);
      }
    }
    return false;
  }

  int fd_ = -1;
  int port_ = 0;
};

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

std::string jsonString(const rapidjson::Document &d) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  d.Accept(w);
  return std::string(buf.GetString(), buf.GetSize());
}

std::string jsonStringMember(const rapidjson::Value &v,
                             std::initializer_list<const char *> keys) {
  if (!v.IsObject()) return {};
  for (const char *k : keys) {
    if (v.HasMember(k) && v[k].IsString()) {
      return v[k].GetString();
    }
  }
  return {};
}

std::int64_t jsonIntMember(const rapidjson::Value &v,
                           std::initializer_list<const char *> keys,
                           std::int64_t fallback = 0) {
  if (!v.IsObject()) return fallback;
  for (const char *k : keys) {
    if (!v.HasMember(k)) continue;
    const auto &m = v[k];
    if (m.IsInt64()) return m.GetInt64();
    if (m.IsInt()) return m.GetInt();
    if (m.IsString()) {
      try {
        return std::stoll(m.GetString());
      } catch (...) {
      }
    }
  }
  return fallback;
}

} // namespace

// ===========================================================================
// WindsurfProvider — boilerplate
// ===========================================================================

WindsurfProvider::WindsurfProvider() : BaseOAuthProvider(kProviderId) {
  loadModelCache();
  loadCascadeMap();
}

WindsurfProvider::~WindsurfProvider() {
  shutdownRequested_.store(true);
  std::lock_guard<std::mutex> lock(discoveryMutex_);
  if (discoveryThread_.joinable()) {
    discoveryThread_.join();
  }
}

// ---------------------------------------------------------------------------
// Token exchange — POST /register_user/ → {api_key, name}
// ---------------------------------------------------------------------------

bool WindsurfProvider::exchangeFirebaseIdToken(
    const std::string &firebaseIdToken,
    firmius::shared::OAuthAccount &outAcc, std::string &outError) {
  rapidjson::Document req;
  req.SetObject();
  auto &alloc = req.GetAllocator();
  req.AddMember("firebase_id_token",
                rapidjson::Value(firebaseIdToken.c_str(), alloc), alloc);

  firmius::utils::GCPHttpClient client(kUserAgent);
  client.setContentType("application/json");
  auto resp = client.post(kRegisterUserUrl, jsonString(req), 30);

  if (resp.code != 200) {
    outError = "register_user HTTP " + std::to_string(resp.code) +
               (resp.body.empty() ? "" : ": " + resp.body);
    return false;
  }

  rapidjson::Document doc;
  doc.Parse(resp.body.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    outError = "register_user: malformed response: " + resp.body;
    return false;
  }

  std::string apiKey = jsonStringMember(doc, {"api_key", "apiKey"});
  if (apiKey.empty()) {
    outError = "register_user: missing api_key in response: " + resp.body;
    return false;
  }
  std::string name = jsonStringMember(doc, {"name", "displayName"});
  std::string email = extractEmailFromJwt(firebaseIdToken);

  // accessToken = api_key (long-lived, no refresh needed).
  // refreshToken = the original Firebase JWT (kept so we can re-call
  //                register_user later if the account is ever wiped).
  outAcc.accessToken = apiKey;
  outAcc.refreshToken = firebaseIdToken;
  outAcc.tokenExpiration = nowSeconds() + 3600 * 24 * 365 * 10; // 10y far future
  outAcc.identifier = !email.empty() ? email
                      : !name.empty() ? name
                                      : apiKey.substr(0, 8);
  if (!email.empty()) outAcc.metadata["email"] = email;
  if (!name.empty()) outAcc.metadata["name"] = name;
  outAcc.metadata["addedAt"] = std::to_string(nowSeconds());
  return true;
}

// ===========================================================================
// OAuth wizard
// ===========================================================================

namespace {

class WindsurfOAuthWizard : public OAuthWizard {
public:
  explicit WindsurfOAuthWizard(WindsurfProvider *provider)
      : provider_(provider), state_(makeUuid()) {
    // Start the loopback listener BEFORE building the URL so we know the port.
    loopbackOk_ = listener_.start();

    std::string redirectUri;
    if (loopbackOk_) {
      redirectUri =
          "http://127.0.0.1:" + std::to_string(listener_.port()) + "/cb";
    } else {
      // Fallback to the hosted token-display page; clipboard / paste path
      // will catch the token.
      redirectUri = "show-auth-token";
    }

    authUrl_ = std::string(kAuthPortalUrl) +
               "?response_type=token"
               "&redirect_uri=" + urlEncode(redirectUri) +
               "&state=" + state_ +
               "&scope=openid+profile+email"
               "&redirect_parameters_type=query";

    // Background capture thread races loopback + clipboard polling.
    captureThread_ = std::thread([this]() { runCapture(); });
  }

  ~WindsurfOAuthWizard() override {
    stopRequested_.store(true);
    listener_.stop();
    if (captureThread_.joinable()) {
      captureThread_.join();
    }
  }

  std::optional<WizardPrompt> nextPrompt() override {
    if (tokenReady_.load()) {
      return std::nullopt;
    }
    if (state_machine_ == State::OpeningBrowser) {
      WizardPrompt p;
      std::string lines =
          "Linking Windsurf — opening your browser to sign in.\n\n"
          "If the browser doesn't open automatically, copy this URL:\n\n" +
          authUrl_ +
          "\n\nAfter you log in, Firmius will pick up the token automatically "
          "(loopback or clipboard). If neither works, paste the JWT here.";
      p.message = lines;
      p.allowFreeformInput = true;
      p.allowEmptyInput = true;
      p.placeholder = "Paste JWT here (or just wait for auto-capture)";
      p.submitLabel = "Open browser / Wait / Submit";
      // Trigger browser open lazily — first nextPrompt() call.
      if (!browserOpened_.exchange(true)) {
        tryOpenBrowser(authUrl_);
      }
      return p;
    }
    if (state_machine_ == State::Done) {
      return std::nullopt;
    }
    WizardPrompt p;
    p.message = errorMessage_.empty() ? "Working..." : errorMessage_;
    p.allowFreeformInput = false;
    p.allowEmptyInput = true;
    p.submitLabel = "Wait";
    return p;
  }

  void submitAnswer(const std::string &answer) override {
    std::string trimmed = trimWhitespace(answer);
    if (!trimmed.empty() && looksLikeOAuthToken(trimmed)) {
      // User pasted a token manually — short-circuit the capture race.
      capturedToken_ = trimmed;
      tokenReady_.store(true);
    }
    // Intentionally non-blocking: OAuthWizardModal runs this on the UI thread.
    // The capture thread continues in the background; completion is observed via
    // isComplete() polling and finalizeExchange().
  }

  bool isComplete() const override { return tokenReady_.load(); }

  bool finalizeExchange(std::string &outErrorMessage) override {
    if (!tokenReady_.load() || capturedToken_.empty()) {
      outErrorMessage = errorMessage_.empty()
                            ? "Windsurf login was not completed."
                            : errorMessage_;
      return false;
    }

    firmius::shared::OAuthAccount acc;
    if (!provider_->exchangeFirebaseIdToken(capturedToken_, acc,
                                            outErrorMessage)) {
      return false;
    }
    provider_->addAccount(acc);
    successEmail_ = acc.metadata.count("email") ? acc.metadata.at("email")
                                                : acc.identifier;

    // Eager discovery is intentionally NOT done on a detached thread here:
    // tools that exit immediately after wizard completion (e.g. firmius_audit
    // --audit oauth_wizard) would have the global libcurl state torn down
    // while the detached thread is mid-request, causing a SIGSEGV in libcurl.
    // The BaseOAuthProvider background refresh loop (5-min cadence) and lazy
    // listModels() will populate the cache without this risk.

    state_machine_ = State::Done;
    return true;
  }

  std::string getFinalMessage() const override {
    if (successEmail_.empty()) {
      return "Successfully linked Windsurf account.";
    }
    return "Successfully linked Windsurf account: " + successEmail_;
  }

private:
  enum class State { OpeningBrowser, Working, Done };

  void runCapture() {
    constexpr int kTotalTimeoutMs = 5 * 60 * 1000; // 5 minutes total
    constexpr int kPollIntervalMs = 600;
    auto start = std::chrono::steady_clock::now();
    std::string lastClip;

    while (!stopRequested_.load() && !tokenReady_.load()) {
      // Path A: loopback accept (quick, non-blocking timeout per spin).
      if (loopbackOk_) {
        std::string token, gotState;
        if (listener_.waitForToken(kPollIntervalMs, token, gotState)) {
          if (looksLikeOAuthToken(token)) {
            // Validate state if portal echoed it; otherwise trust loopback.
            if (gotState.empty() || gotState == state_) {
              capturedToken_ = token;
              tokenReady_.store(true);
              return;
            }
          }
        }
      } else {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kPollIntervalMs));
      }

      // Path B: clipboard polling. Skip the very first sample to avoid
      // capturing a JWT that was already in the clipboard pre-launch.
      std::string clip = readClipboard();
      if (!clip.empty() && clip != lastClip) {
        lastClip = clip;
        if (clipboardSeeded_ && looksLikeOAuthToken(clip)) {
          capturedToken_ = clip;
          tokenReady_.store(true);
          return;
        }
        clipboardSeeded_ = true;
      }

      auto elapsed = std::chrono::steady_clock::now() - start;
      if (elapsed >
          std::chrono::milliseconds(kTotalTimeoutMs)) {
        errorMessage_ = "Login timed out after 5 minutes.";
        return;
      }
    }
  }

  WindsurfProvider *provider_;
  std::string state_;
  std::string authUrl_;
  std::string capturedToken_;
  std::string errorMessage_;
  std::string successEmail_;
  LoopbackOauthListener listener_;
  bool loopbackOk_ = false;
  bool clipboardSeeded_ = false;
  std::atomic<bool> browserOpened_{false};
  std::atomic<bool> tokenReady_{false};
  std::atomic<bool> stopRequested_{false};
  std::thread captureThread_;
  State state_machine_ = State::OpeningBrowser;
};

} // namespace

std::unique_ptr<OAuthWizard> WindsurfProvider::beginConnectionWizard() {
  return std::make_unique<WindsurfOAuthWizard>(this);
}

bool WindsurfProvider::refreshAccessToken(firmius::shared::OAuthAccount &acc) {
  // Codeium api_keys do not expire. If for some reason a request returned
  // 401, re-exchange the cached Firebase JWT (acc.refreshToken) for a fresh
  // api_key. This handles edge cases like server-side key revocation.
  if (acc.refreshToken.empty()) return true;
  std::string err;
  firmius::shared::OAuthAccount fresh = acc;
  if (!exchangeFirebaseIdToken(acc.refreshToken, fresh, err)) {
    return false;
  }
  acc.accessToken = fresh.accessToken;
  acc.tokenExpiration = fresh.tokenExpiration;
  saveAccounts();
  return true;
}

// ===========================================================================
// Model cache (static fallback ⊕ cloud discovery)
// ===========================================================================

std::filesystem::path WindsurfProvider::modelCachePath() const {
  const char *home = std::getenv("HOME");
  if (!home || !*home) home = "/tmp";
  std::filesystem::path dir =
      std::filesystem::path(home) / ".firmius" / "cache";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir / "windsurf-models.json";
}

void WindsurfProvider::loadModelCache() {
  std::lock_guard<std::recursive_mutex> lock(modelMutex_);
  models_.clear();

  // Seed with the static fallback so we can resolve names before any cloud
  // discovery has run.
  for (const auto &info : windsurf::listAllModels()) {
    auto resolved = windsurf::resolveModel(info.id);
    if (!resolved) continue;
    CachedModel cm;
    cm.enumValue = resolved->enumValue;
    cm.canonicalId = info.id;
    cm.displayName = info.id;
    cm.contextWindow = info.contextWindow;
    cm.maxOutput = info.maxOutputTokens;
    cm.supportsReasoning = info.supportsReasoning;
    for (const auto &v : info.variants) cm.variants.push_back(v.variantName);
    cm.fromDiscovery = false;
    models_.push_back(std::move(cm));
  }

  // Merge in persisted discovery results.
  auto path = modelCachePath();
  if (!std::filesystem::exists(path)) return;
  std::ifstream ifs(path);
  if (!ifs) return;
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  rapidjson::Document doc;
  doc.Parse(content.c_str());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("models") ||
      !doc["models"].IsArray()) {
    return;
  }
  for (const auto &m : doc["models"].GetArray()) {
    if (!m.IsObject()) continue;
    CachedModel cm;
    cm.enumValue = static_cast<int>(jsonIntMember(m, {"enumValue"}));
    cm.canonicalId = jsonStringMember(m, {"canonicalId", "id"});
    cm.displayName = jsonStringMember(m, {"displayName", "name"});
    cm.contextWindow =
        static_cast<std::uint32_t>(jsonIntMember(m, {"contextWindow"}, 200000));
    cm.maxOutput =
        static_cast<std::uint32_t>(jsonIntMember(m, {"maxOutput"}, 8192));
    cm.supportsReasoning =
        m.HasMember("supportsReasoning") && m["supportsReasoning"].IsBool() &&
        m["supportsReasoning"].GetBool();
    if (m.HasMember("variants") && m["variants"].IsArray()) {
      for (const auto &v : m["variants"].GetArray()) {
        if (v.IsString()) cm.variants.push_back(v.GetString());
      }
    }
    cm.fromDiscovery = true;

    // Note: we intentionally accept entries with enumValue == 0 here.
    // Newer Windsurf models (e.g. Claude Opus 4.7) are returned by
    // ApiServerService.GetCascadeModelConfigs without a `model_or_alias`
    // submessage, so they have no enum mapping yet. We still want them
    // selectable in the picker keyed by their canonical id (model_uid).
    if (cm.canonicalId.empty()) continue;

    // Pull the optional pricing / modality fields written by
    // saveModelCache() so they survive across launches.
    if (m.HasMember("supportsImages") && m["supportsImages"].IsBool()) {
      cm.supportsImages = m["supportsImages"].GetBool();
    }
    if (m.HasMember("creditMultiplier") && m["creditMultiplier"].IsNumber()) {
      cm.creditMultiplier = m["creditMultiplier"].GetDouble();
    }
    if (m.HasMember("pricePer1MInput") && m["pricePer1MInput"].IsNumber()) {
      cm.pricePer1MInput = m["pricePer1MInput"].GetDouble();
    }
    if (m.HasMember("pricePer1MOutput") && m["pricePer1MOutput"].IsNumber()) {
      cm.pricePer1MOutput = m["pricePer1MOutput"].GetDouble();
    }
    if (m.HasMember("pricePer1MCacheRead") &&
        m["pricePer1MCacheRead"].IsNumber()) {
      cm.pricePer1MCacheRead = m["pricePer1MCacheRead"].GetDouble();
    }
    if (m.HasMember("pricePer1MCacheWrite") &&
        m["pricePer1MCacheWrite"].IsNumber()) {
      cm.pricePer1MCacheWrite = m["pricePer1MCacheWrite"].GetDouble();
    }

    // Replace any matching static entry with the discovery one.
    auto it = std::find_if(models_.begin(), models_.end(),
                           [&](const CachedModel &x) {
                             return x.canonicalId == cm.canonicalId;
                           });
    if (it != models_.end()) {
      *it = std::move(cm);
    } else {
      models_.push_back(std::move(cm));
    }
  }
}

void WindsurfProvider::saveModelCache() const {
  std::lock_guard<std::recursive_mutex> lock(modelMutex_);
  // Persist only discovery-derived entries — fallback can always be rebuilt.
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();
  rapidjson::Value arr(rapidjson::kArrayType);
  for (const auto &cm : models_) {
    if (!cm.fromDiscovery) continue;
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("enumValue", cm.enumValue, alloc);
    obj.AddMember("canonicalId",
                  rapidjson::Value(cm.canonicalId.c_str(), alloc), alloc);
    obj.AddMember("displayName",
                  rapidjson::Value(cm.displayName.c_str(), alloc), alloc);
    obj.AddMember("contextWindow",
                  static_cast<std::int64_t>(cm.contextWindow), alloc);
    obj.AddMember("maxOutput", static_cast<std::int64_t>(cm.maxOutput),
                  alloc);
    obj.AddMember("supportsReasoning", cm.supportsReasoning, alloc);
    obj.AddMember("supportsImages", cm.supportsImages, alloc);
    obj.AddMember("creditMultiplier", cm.creditMultiplier, alloc);
    obj.AddMember("pricePer1MInput", cm.pricePer1MInput, alloc);
    obj.AddMember("pricePer1MOutput", cm.pricePer1MOutput, alloc);
    obj.AddMember("pricePer1MCacheRead", cm.pricePer1MCacheRead, alloc);
    obj.AddMember("pricePer1MCacheWrite", cm.pricePer1MCacheWrite, alloc);
    rapidjson::Value vars(rapidjson::kArrayType);
    for (const auto &v : cm.variants) {
      vars.PushBack(rapidjson::Value(v.c_str(), alloc), alloc);
    }
    obj.AddMember("variants", vars, alloc);
    arr.PushBack(obj, alloc);
  }
  doc.AddMember("models", arr, alloc);
  doc.AddMember("savedAt", static_cast<std::int64_t>(nowSeconds()), alloc);

  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
  doc.Accept(writer);

  auto path = modelCachePath();
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  if (!ofs) return;
  ofs.write(buf.GetString(), static_cast<std::streamsize>(buf.GetSize()));
}

// ---------------------------------------------------------------------------
// listModels / getModelInfo
// ---------------------------------------------------------------------------

std::vector<firmius::shared::ModelInfo> WindsurfProvider::listModels() {
  // Return the current static/cached catalog only. Discovery is handled by
  // discoverModels() on the caller's background worker so we do not create a
  // second unmanaged libcurl thread from inside listModels().
  std::lock_guard<std::recursive_mutex> lock(modelMutex_);
  std::vector<firmius::shared::ModelInfo> out;
  out.reserve(models_.size());
  for (const auto &cm : models_) {
    firmius::shared::ModelInfo info;
    info.id = cm.canonicalId;
    info.provider = kProviderId;
    info.contextWindow = cm.contextWindow;
    info.maxOutputTokens = cm.maxOutput;
    info.modalities = {"text"};
    if (cm.supportsImages) info.modalities.push_back("image");
    info.supportsReasoning = cm.supportsReasoning;
    info.pricePer1MInput = cm.pricePer1MInput;
    info.pricePer1MOutput = cm.pricePer1MOutput;
    info.pricePer1MCacheRead = cm.pricePer1MCacheRead;
    info.pricePer1MCacheWrite = cm.pricePer1MCacheWrite;
    for (const auto &v : cm.variants) {
      firmius::shared::ModelVariant mv;
      mv.variantName = v;
      info.variants.push_back(std::move(mv));
    }
    out.push_back(std::move(info));
  }
  return out;
}

void WindsurfProvider::discoverModels(
    std::function<void(const firmius::shared::ModelInfo &)> onModel) {
  auto current = listModels();
  for (const auto &model : current) {
    onModel(model);
  }

  auto accounts = getAccounts();
  if (accounts.empty()) {
    return;
  }

  try {
    fetchAndMergeModels(accounts.front());
  } catch (...) {
    return;
  }

  auto refreshed = listModels();
  for (const auto &model : refreshed) {
    onModel(model);
  }
}

firmius::shared::ModelInfo
WindsurfProvider::getModelInfo(const std::string &modelId) {
  // Strip variant suffix when looking up info.
  auto resolved = windsurf::resolveModel(modelId);
  std::string canonical = resolved ? resolved->canonicalId : modelId;
  std::lock_guard<std::recursive_mutex> lock(modelMutex_);
  for (const auto &cm : models_) {
    if (cm.canonicalId == canonical) {
      firmius::shared::ModelInfo info;
      info.id = cm.canonicalId;
      info.provider = kProviderId;
      info.contextWindow = cm.contextWindow;
      info.maxOutputTokens = cm.maxOutput;
      info.modalities = {"text"};
      info.supportsReasoning = cm.supportsReasoning;
      for (const auto &v : cm.variants) {
        firmius::shared::ModelVariant mv;
        mv.variantName = v;
        info.variants.push_back(std::move(mv));
      }
      return info;
    }
  }
  // Unknown — return permissive defaults.
  firmius::shared::ModelInfo info;
  info.id = modelId;
  info.provider = kProviderId;
  info.contextWindow = 200000;
  info.maxOutputTokens = 8192;
  info.modalities = {"text"};
  return info;
}

// ===========================================================================
// Model discovery & quota / streaming impls live in WindsurfProvider_grpc.cpp
// (split out for build-time + readability).
// ===========================================================================

} // namespace firmius::provider
