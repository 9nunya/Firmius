#include "providers/QwenProvider.hpp"
#include "providers/BackoffConstants.hpp"
#include "utils/GCPHttpClient.hpp"
#include "utils/InterruptibleSleep.hpp"
#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
#include <random>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <thread>

namespace firmius::provider {

// Initialize static constants
const std::string QwenProvider::QWEN_OAUTH_CLIENT_ID =
    "f0304373b74a44d2b584a3fb70ca9e56";
const std::string QwenProvider::QWEN_OAUTH_SCOPE =
    "openid profile email model.completion";
const std::string QwenProvider::QWEN_OAUTH_GRANT_TYPE =
    "urn:ietf:params:oauth:grant-type:device_code";
const std::string QwenProvider::QWEN_OAUTH_DEVICE_CODE_ENDPOINT =
    "https://chat.qwen.ai/api/v1/oauth2/device/code";
const std::string QwenProvider::QWEN_OAUTH_TOKEN_ENDPOINT =
    "https://chat.qwen.ai/api/v1/oauth2/token";
const std::string QwenProvider::QWEN_API_BASE_URL = "https://portal.qwen.ai/v1";
const std::string QwenProvider::QWEN_CHAT_ENDPOINT =
    "https://portal.qwen.ai/v1/chat/completions";
const std::string QwenProvider::QWEN_MODELS_ENDPOINT =
    "https://portal.qwen.ai/v1/models";

namespace {

// Stream context for CURL callback
struct StreamContext {
  QwenProvider *provider;
  std::function<void(const StreamEvent &)> *onEvent;
  std::string buffer;
  size_t readOffset = 0;
  std::atomic<bool> *abortSignal;
};

// Generate PKCE code verifier (32 bytes, base64url encoded)
// Following RFC 7636: verifier is base64url(random bytes), no padding
// 32 bytes = 43 base64url characters
std::string generateVerifier() {
  std::vector<uint8_t> bytes(32);
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto &b : bytes)
    b = static_cast<uint8_t>(dist(rd));

  // Use standard base64url encoding (RFC 4648 Section 5)
  static const char *table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  std::string result;
  result.reserve(43);

  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t n = static_cast<uint32_t>(bytes[i]) << 16;
    if (i + 1 < bytes.size())
      n |= static_cast<uint32_t>(bytes[i + 1]) << 8;
    if (i + 2 < bytes.size())
      n |= static_cast<uint32_t>(bytes[i + 2]);

    result.push_back(table[(n >> 18) & 0x3F]);
    result.push_back(table[(n >> 12) & 0x3F]);
    if (i + 1 < bytes.size())
      result.push_back(table[(n >> 6) & 0x3F]);
    if (i + 2 < bytes.size())
      result.push_back(table[n & 0x3F]);
  }

  return result;
}

// SHA256 for PKCE code challenge
std::vector<uint8_t> sha256(const std::string &input) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

  struct Context {
    uint8_t data[64];
    uint32_t state[8];
    uint64_t bitlen = 0;
    size_t datalen = 0;
  };

  auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };

  Context ctx;
  ctx.datalen = 0;
  ctx.bitlen = 0;
  ctx.state[0] = 0x6a09e667;
  ctx.state[1] = 0xbb67ae85;
  ctx.state[2] = 0x3c6ef372;
  ctx.state[3] = 0xa54ff53a;
  ctx.state[4] = 0x510e527f;
  ctx.state[5] = 0x9b05688c;
  ctx.state[6] = 0x1f83d9ab;
  ctx.state[7] = 0x5be0cd19;

  std::vector<uint8_t> hashOut(32);

  auto transform = [&](const uint8_t data[]) {
    uint32_t m[64];
    for (uint32_t i = 0, j = 0; i < 16; ++i, j += 4) {
      m[i] = (static_cast<uint32_t>(data[j]) << 24) |
             (static_cast<uint32_t>(data[j + 1]) << 16) |
             (static_cast<uint32_t>(data[j + 2]) << 8) |
             (static_cast<uint32_t>(data[j + 3]));
    }
    for (uint32_t i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
      uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
      m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    uint32_t a = ctx.state[0];
    uint32_t b = ctx.state[1];
    uint32_t c = ctx.state[2];
    uint32_t d = ctx.state[3];
    uint32_t e = ctx.state[4];
    uint32_t f = ctx.state[5];
    uint32_t g = ctx.state[6];
    uint32_t h = ctx.state[7];

    for (uint32_t i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t temp1 = h + S1 + ch + k[i] + m[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = S0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
    ctx.state[4] += e;
    ctx.state[5] += f;
    ctx.state[6] += g;
    ctx.state[7] += h;
  };

  for (size_t i = 0; i < input.size(); ++i) {
    ctx.data[ctx.datalen] = static_cast<uint8_t>(input[i]);
    ctx.datalen++;
    if (ctx.datalen == 64) {
      transform(ctx.data);
      ctx.bitlen += 512;
      ctx.datalen = 0;
    }
  }

  uint32_t i = ctx.datalen;
  if (ctx.datalen < 56) {
    ctx.data[i++] = 0x80;
    while (i < 56)
      ctx.data[i++] = 0x00;
  } else {
    ctx.data[i++] = 0x80;
    while (i < 64)
      ctx.data[i++] = 0x00;
    transform(ctx.data);
    std::fill(std::begin(ctx.data), std::end(ctx.data), 0);
  }

  ctx.bitlen += ctx.datalen * 8;
  ctx.data[63] = static_cast<uint8_t>(ctx.bitlen);
  ctx.data[62] = static_cast<uint8_t>(ctx.bitlen >> 8);
  ctx.data[61] = static_cast<uint8_t>(ctx.bitlen >> 16);
  ctx.data[60] = static_cast<uint8_t>(ctx.bitlen >> 24);
  ctx.data[59] = static_cast<uint8_t>(ctx.bitlen >> 32);
  ctx.data[58] = static_cast<uint8_t>(ctx.bitlen >> 40);
  ctx.data[57] = static_cast<uint8_t>(ctx.bitlen >> 48);
  ctx.data[56] = static_cast<uint8_t>(ctx.bitlen >> 56);
  transform(ctx.data);

  for (i = 0; i < 4; ++i) {
    hashOut[i] = (ctx.state[0] >> (24 - i * 8)) & 0x000000ff;
    hashOut[i + 4] = (ctx.state[1] >> (24 - i * 8)) & 0x000000ff;
    hashOut[i + 8] = (ctx.state[2] >> (24 - i * 8)) & 0x000000ff;
    hashOut[i + 12] = (ctx.state[3] >> (24 - i * 8)) & 0x000000ff;
    hashOut[i + 16] = (ctx.state[4] >> (24 - i * 8)) & 0x000000ff;
    hashOut[i + 20] = (ctx.state[5] >> (24 - i * 8)) & 0x000000ff;
    hashOut[i + 24] = (ctx.state[6] >> (24 - i * 8)) & 0x000000ff;
    hashOut[i + 28] = (ctx.state[7] >> (24 - i * 8)) & 0x000000ff;
  }

  return hashOut;
}

// Generate code challenge from verifier
// Following RFC 7636: challenge is base64url(SHA256(verifier)), no padding
// SHA256 = 32 bytes = 43 base64url characters
std::string generateCodeChallenge(const std::string &verifier) {
  auto hash = sha256(verifier);

  // Use standard base64url encoding (RFC 4648 Section 5)
  static const char *table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  std::string result;
  result.reserve(43);

  for (size_t i = 0; i < hash.size(); i += 3) {
    uint32_t n = static_cast<uint32_t>(hash[i]) << 16;
    if (i + 1 < hash.size())
      n |= static_cast<uint32_t>(hash[i + 1]) << 8;
    if (i + 2 < hash.size())
      n |= static_cast<uint32_t>(hash[i + 2]);

    result.push_back(table[(n >> 18) & 0x3F]);
    result.push_back(table[(n >> 12) & 0x3F]);
    if (i + 1 < hash.size())
      result.push_back(table[(n >> 6) & 0x3F]);
    if (i + 2 < hash.size())
      result.push_back(table[n & 0x3F]);
  }

  return result;
}

// URL encode for form data
std::string urlEncode(const std::string &value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;
  for (unsigned char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ||
        c == '_' || c == '~') {
      escaped << c;
    } else {
      escaped << '%' << std::uppercase << std::setw(2) << static_cast<int>(c)
              << std::nouppercase;
    }
  }
  return escaped.str();
}

// Convert object to URL-encoded form data
std::string objectToUrlEncoded(const std::map<std::string, std::string> &data) {
  std::vector<std::string> pairs;
  for (const auto &[key, value] : data) {
    pairs.push_back(urlEncode(key) + "=" + urlEncode(value));
  }
  std::string result;
  for (size_t i = 0; i < pairs.size(); ++i) {
    if (i > 0)
      result += "&";
    result += pairs[i];
  }
  return result;
}

int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::uint64_t nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string roleToString(firmius::shared::Role role) {
  switch (role) {
  case firmius::shared::Role::System:
    return "system";
  case firmius::shared::Role::User:
    return "user";
  case firmius::shared::Role::Assistant:
    return "assistant";
  case firmius::shared::Role::ToolResult:
    return "tool";
  case firmius::shared::Role::Error:
    return "system";
  }
  return "user";
}

} // namespace

// ============================================================================
// QwenOAuthWizard - Interactive OAuth Device Flow
// ============================================================================

class QwenOAuthWizard : public OAuthWizard {
public:
  explicit QwenOAuthWizard(QwenProvider *provider) : provider_(provider) {
    // Generate PKCE verifier and challenge
    verifier_ = generateVerifier();
    challenge_ = generateCodeChallenge(verifier_);

    // Build device authorization request
    std::map<std::string, std::string> bodyData = {
        {"client_id", QwenProvider::QWEN_OAUTH_CLIENT_ID},
        {"scope", QwenProvider::QWEN_OAUTH_SCOPE},
        {"code_challenge", challenge_},
        {"code_challenge_method", "S256"},
    };

    firmius::utils::GCPHttpClient client;
    client.setContentType("application/x-www-form-urlencoded");

    auto resp = client.post(QwenProvider::QWEN_OAUTH_DEVICE_CODE_ENDPOINT,
                            objectToUrlEncoded(bodyData));

    if (resp.code != 200) {
      prompt_ = "Failed to initiate OAuth: HTTP " + std::to_string(resp.code);
      return;
    }

    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
      prompt_ = "Failed to parse OAuth response";
      return;
    }

    if (doc.HasMember("device_code") && doc["device_code"].IsString()) {
      deviceCode_ = doc["device_code"].GetString();
    }
    if (doc.HasMember("user_code") && doc["user_code"].IsString()) {
      userCode_ = doc["user_code"].GetString();
    }
    if (doc.HasMember("verification_uri") &&
        doc["verification_uri"].IsString()) {
      verificationUri_ = doc["verification_uri"].GetString();
    }
    if (doc.HasMember("verification_uri_complete") &&
        doc["verification_uri_complete"].IsString()) {
      verificationUriComplete_ = doc["verification_uri_complete"].GetString();
    }
    if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
      expiresIn_ = doc["expires_in"].GetInt();
    }

    if (deviceCode_.empty() || userCode_.empty()) {
      prompt_ = "Invalid OAuth response from Qwen";
      return;
    }

    prompt_ = "Qwen OAuth Authorization\n\n"
              "1. Open this URL in your browser:\n"
              "   " +
              verificationUriComplete_ +
              "\n\n"
              "2. Enter the code: " +
              userCode_ +
              "\n\n"
              "3. Press Enter after completing authorization...";

    // Start polling in background thread
    pollingThread_ = std::thread([this]() { pollForToken(); });
  }

  ~QwenOAuthWizard() override {
    if (pollingThread_.joinable()) {
      pollingThread_.join();
    }
  }

  std::optional<WizardPrompt> nextPrompt() override {
    if (!promptShown_) {
      promptShown_ = true;
      return WizardPrompt{prompt_, false};
    }
    return std::nullopt;
  }

  void submitAnswer(const std::string &) override {
    // User pressed Enter - just wait for polling to complete if not already
    if (pollingThread_.joinable()) {
      pollingThread_.join();
    }
  }

  bool isComplete() const override {
    return isComplete_.load() || tokenReceived_.load();
  }

  bool finalizeExchange(std::string &outErrorMessage) override {
    if (!tokenReceived_.load()) {
      outErrorMessage = "OAuth authorization not completed: " + prompt_;
      return false;
    }

    // Build account from token response
    OAuthAccount acc;
    acc.accessToken = accessToken_;
    acc.refreshToken = refreshToken_;
    acc.tokenExpiration = tokenExpiration_;
    
    // Use email as identifier, or generate from token if email extraction failed
    if (email_.empty()) {
      // Generate identifier from access token prefix
      acc.identifier = accessToken_.substr(0, std::min(size_t(12), accessToken_.size()));
    } else {
      acc.identifier = email_;
    }

    provider_->addAccount(acc);
    return true;
  }

  std::string getFinalMessage() const override {
    return "Successfully authenticated with Qwen Code!";
  }

private:
  void pollForToken() {
    std::map<std::string, std::string> bodyData = {
        {"grant_type", QwenProvider::QWEN_OAUTH_GRANT_TYPE},
        {"client_id", QwenProvider::QWEN_OAUTH_CLIENT_ID},
        {"device_code", deviceCode_},
        {"code_verifier", verifier_},
    };

    firmius::utils::GCPHttpClient client;
    client.setContentType("application/x-www-form-urlencoded");

    std::uint64_t startTime = nowMs();
    int interval = 5000; // 5 seconds initial interval
    const std::uint64_t timeoutMs =
        static_cast<std::uint64_t>(expiresIn_) * 1000 - 3000;

    while ((nowMs() - startTime) < timeoutMs) {
      auto resp = client.post(QwenProvider::QWEN_OAUTH_TOKEN_ENDPOINT,
                              objectToUrlEncoded(bodyData));

      if (resp.code == 200) {
        rapidjson::Document doc;
        doc.Parse(resp.body.c_str());
        if (!doc.HasParseError() && doc.IsObject()) {
          if (doc.HasMember("access_token") && doc["access_token"].IsString()) {
            accessToken_ = doc["access_token"].GetString();
          }
          if (doc.HasMember("refresh_token") &&
              doc["refresh_token"].IsString()) {
            refreshToken_ = doc["refresh_token"].GetString();
          }
          if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
            tokenExpiration_ = nowSeconds() + doc["expires_in"].GetInt();
          }
          if (doc.HasMember("scope") && doc["scope"].IsString()) {
            scope_ = doc["scope"].GetString();
          }

          if (!accessToken_.empty()) {
            // Fetch user email from token
            fetchUserEmail();
            tokenReceived_.store(true);
            isComplete_.store(true);
            return;
          }
        }
      } else if (resp.code == 400) {
        // Check for specific OAuth errors
        rapidjson::Document doc;
        doc.Parse(resp.body.c_str());
        if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("error") &&
            doc["error"].IsString()) {
          std::string error = doc["error"].GetString();

          if (error == "authorization_pending") {
            // User hasn't authorized yet, continue polling
          } else if (error == "slow_down") {
            // Server asks us to slow down
            interval += 5000;
          } else if (error == "expired_token") {
            prompt_ = "OAuth code expired. Please try again.";
            return;
          } else if (error == "access_denied") {
            prompt_ = "Authorization denied. Please try again.";
            return;
          }
        }
      }

      // Wait before next poll
      std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    }

    prompt_ = "OAuth authorization timed out";
  }

  void fetchUserEmail() {
    // Try to extract email from JWT access token
    auto firstDot = accessToken_.find('.');
    if (firstDot != std::string::npos) {
      auto secondDot = accessToken_.find('.', firstDot + 1);
      if (secondDot != std::string::npos) {
        std::string payload =
            accessToken_.substr(firstDot + 1, secondDot - firstDot - 1);

        // Base64url decode
        std::string b64 = payload;
        for (char &c : b64) {
          if (c == '-')
            c = '+';
          else if (c == '_')
            c = '/';
        }
        while (b64.size() % 4 != 0)
          b64.push_back('=');

        // Simple base64 decode
        std::string decoded;
        int val = 0, valb = -8;
        for (unsigned char c : b64) {
          if (c == '=')
            break;
          int d = (c >= 'A' && c <= 'Z')   ? c - 'A'
                  : (c >= 'a' && c <= 'z') ? c - 'a' + 26
                  : (c >= '0' && c <= '9') ? c - '0' + 52
                  : (c == '+')             ? 62
                  : (c == '/')             ? 63
                                           : -1;
          if (d == -1)
            continue;
          val = (val << 6) + d;
          valb += 6;
          if (valb >= 0) {
            decoded.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
          }
        }

        // Parse JSON for email
        rapidjson::Document doc;
        doc.Parse(decoded.c_str());
        if (!doc.HasParseError() && doc.IsObject()) {
          if (doc.HasMember("email") && doc["email"].IsString()) {
            email_ = doc["email"].GetString();
          } else if (doc.HasMember("preferred_username") &&
                     doc["preferred_username"].IsString()) {
            email_ = doc["preferred_username"].GetString();
          }
        }
      }
    }

    // If no email from JWT, try userinfo endpoint
    if (email_.empty()) {
      firmius::utils::GCPHttpClient client;
      client.setBearerToken(accessToken_);
      auto resp = client.get("https://chat.qwen.ai/api/v1/oauth2/userinfo", 10);
      if (resp.code == 200) {
        rapidjson::Document doc;
        doc.Parse(resp.body.c_str());
        if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("email") &&
            doc["email"].IsString()) {
          email_ = doc["email"].GetString();
        }
      }
    }
  }

  QwenProvider *provider_;
  std::string prompt_;
  bool promptShown_ = false;
  std::atomic<bool> isComplete_ = false;
  std::atomic<bool> tokenReceived_ = false;

  std::string verifier_;
  std::string challenge_;
  std::string deviceCode_;
  std::string userCode_;
  std::string verificationUri_;
  std::string verificationUriComplete_;
  int expiresIn_ = 300;

  std::string accessToken_;
  std::string refreshToken_;
  int64_t tokenExpiration_ = 0;
  std::string scope_;
  std::string email_;

  std::thread pollingThread_;
};

// ============================================================================
// QwenProvider Implementation
// ============================================================================

QwenProvider::QwenProvider() : BaseOAuthProvider("qwen") {}

std::map<std::string, ModelInfo> QwenProvider::getStaticModels() {
  std::vector<ModelVariant> coderVariants = {
      {"low", "{\"effort\":\"low\"}"},
      {"medium", "{\"effort\":\"medium\"}"},
      {"high", "{\"effort\":\"high\"}"},
      {"max", "{\"effort\":\"max\"}"},
  };

  std::vector<ModelVariant> noVariants = {};

  return {
      {"qwen3.5-plus",
       {.id = "qwen3.5-plus",
        .provider = "qwen",
        .contextWindow = 1048576, // 1M tokens
        .modalities = {"text", "image"},
        .variants = noVariants,
        .supportsReasoning = true,
        .pricePer1MInput = 0.0,
        .pricePer1MOutput = 0.0,
        .pricePer1MCacheRead = 0.0,
        .pricePer1MCacheWrite = 0.0}},
      {"qwen3-coder-plus",
       {.id = "qwen3-coder-plus",
        .provider = "qwen",
        .contextWindow = 1048576, // 1M tokens
        .modalities = {"text"},
        .variants = coderVariants,
        .supportsReasoning = false,
        .pricePer1MInput = 0.0,
        .pricePer1MOutput = 0.0,
        .pricePer1MCacheRead = 0.0,
        .pricePer1MCacheWrite = 0.0}},
      {"qwen3-coder-flash",
       {.id = "qwen3-coder-flash",
        .provider = "qwen",
        .contextWindow = 1048576, // 1M tokens
        .modalities = {"text"},
        .variants = coderVariants,
        .supportsReasoning = false,
        .pricePer1MInput = 0.0,
        .pricePer1MOutput = 0.0,
        .pricePer1MCacheRead = 0.0,
        .pricePer1MCacheWrite = 0.0}},
      {"coder-model",
       {.id = "coder-model",
        .provider = "qwen",
        .contextWindow = 1048576, // Auto-routed to Qwen 3.5 Plus
        .modalities = {"text", "image"},
        .variants = noVariants,
        .supportsReasoning = false,
        .pricePer1MInput = 0.0,
        .pricePer1MOutput = 0.0,
        .pricePer1MCacheRead = 0.0,
        .pricePer1MCacheWrite = 0.0}},
      {"vision-model",
       {.id = "vision-model",
        .provider = "qwen",
        .contextWindow = 131072, // 128K tokens
        .modalities = {"text", "image"},
        .variants = noVariants,
        .supportsReasoning = false,
        .pricePer1MInput = 0.0,
        .pricePer1MOutput = 0.0,
        .pricePer1MCacheRead = 0.0,
        .pricePer1MCacheWrite = 0.0}},
  };
}

std::vector<ModelInfo> QwenProvider::listModels() {
  std::vector<ModelInfo> result;
  for (const auto &[id, info] : getStaticModels()) {
    result.push_back(info);
  }
  return result;
}

ModelInfo QwenProvider::getModelInfo(const std::string &modelId) {
  auto models = getStaticModels();
  auto it = models.find(modelId);
  if (it != models.end()) {
    return it->second;
  }
  // Default fallback
  return {.id = modelId,
          .provider = "qwen",
          .contextWindow = 131072,
          .modalities = {"text"},
          .variants = {},
          .supportsReasoning = false,
          .pricePer1MInput = 0.0,
          .pricePer1MOutput = 0.0,
          .pricePer1MCacheRead = 0.0,
          .pricePer1MCacheWrite = 0.0};
}

std::unique_ptr<OAuthWizard> QwenProvider::beginConnectionWizard() {
  return std::make_unique<QwenOAuthWizard>(this);
}

bool QwenProvider::refreshAccessToken(OAuthAccount &acc) {
  std::map<std::string, std::string> bodyData = {
      {"grant_type", "refresh_token"},
      {"client_id", QWEN_OAUTH_CLIENT_ID},
      {"refresh_token", acc.refreshToken},
  };

  firmius::utils::GCPHttpClient client;
  client.setContentType("application/x-www-form-urlencoded");

  auto resp =
      client.post(QWEN_OAUTH_TOKEN_ENDPOINT, objectToUrlEncoded(bodyData));

  if (resp.code == 200) {
    rapidjson::Document doc;
    doc.Parse(resp.body.c_str());
    if (!doc.HasParseError() && doc.IsObject()) {
      bool updated = false;
      if (doc.HasMember("access_token") && doc["access_token"].IsString()) {
        acc.accessToken = doc["access_token"].GetString();
        updated = true;
      }
      if (doc.HasMember("refresh_token") && doc["refresh_token"].IsString()) {
        acc.refreshToken = doc["refresh_token"].GetString();
      }
      if (doc.HasMember("expires_in") && doc["expires_in"].IsInt()) {
        acc.tokenExpiration = nowSeconds() + doc["expires_in"].GetInt();
      }
      if (updated) {
        saveAccounts();
        return true;
      }
    }
  }

  return false;
}

void QwenProvider::refreshQuotas() {
  // Qwen OAuth free tier has no quota API endpoint.
  // We don't track quotas - let the API tell us when we're rate limited.
  
  // If no accounts loaded, can't refresh
  if (accounts_.empty()) {
    return;
  }

  int64_t now = nowSeconds();
  bool needsSave = false;
  
  for (auto &acc : accounts_) {
    // Refresh token if expired
    if (isTokenExpired(acc)) {
      if (refreshAccessToken(acc)) {
        needsSave = true;
      }
    }
    
    // Clear any stale rate-limiting from previous sessions
    if (acc.rateLimited && now > acc.backoffUntil) {
      acc.rateLimited = false;
      needsSave = true;
    }
    acc.backoffUntil = 0;
    acc.lastQuotaRefresh = now;
  }
  
  // Only save if something actually changed (token refreshed or rate limit cleared)
  if (needsSave) {
    saveAccounts();
  }
}

std::map<std::string, std::vector<QuotaBucket>>
QwenProvider::getAllQuotas() const {
  std::map<std::string, std::vector<QuotaBucket>> result;

  for (const auto &acc : accounts_) {
    std::vector<QuotaBucket> buckets;

    // Check each model's quota
    std::vector<std::string> models = {"coder-model", "qwen3-coder-plus",
                                       "qwen3-coder-flash", "qwen3.5-plus",
                                       "vision-model"};

    for (const auto &model : models) {
      std::string quotaKey = "quota:" + model;
      std::string resetKey = "quota_reset:" + model;

      float remaining = 1.0f; // Default to available
      if (acc.metadata.count(quotaKey)) {
        try {
          remaining = std::stof(acc.metadata.at(quotaKey));
        } catch (...) {
          remaining = 1.0f;
        }
      }

      std::string resetTime;
      if (acc.metadata.count(resetKey)) {
        resetTime = acc.metadata.at(resetKey);
      }

      buckets.push_back(QuotaBucket{model, remaining, resetTime});
    }

    result[acc.getIdentifier()] = buckets;
  }

  return result;
}

size_t QwenProvider::sseWriteCallback(char *ptr, size_t size, size_t nmemb,
                                      void *userdata) {
  auto *ctx = static_cast<StreamContext *>(userdata);
  if (ctx->abortSignal && ctx->abortSignal->load()) {
    return 0;
  }

  ctx->buffer.append(ptr, size * nmemb);

  // Process complete lines
  size_t newlinePos;
  while ((newlinePos = ctx->buffer.find('\n', ctx->readOffset)) !=
         std::string::npos) {
    std::string line =
        ctx->buffer.substr(ctx->readOffset, newlinePos - ctx->readOffset);
    ctx->readOffset = newlinePos + 1;

    // Remove trailing \r if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Skip empty lines and comments
    if (line.empty() || line[0] == ':') {
      continue;
    }

    // Parse SSE data
    std::string data = line.substr(0, 6) == "data: " ? line.substr(6) : line;
    if (data == "[DONE]") {
      continue;
    }

    ctx->provider->processSSELine(data, *(ctx->onEvent));
  }

  // Prevent buffer growth
  if (ctx->readOffset > 1024 * 1024) {
    ctx->buffer.erase(0, ctx->readOffset);
    ctx->readOffset = 0;
  }

  return size * nmemb;
}

void QwenProvider::processSSELine(
    const std::string &line,
    std::function<void(const StreamEvent &)> &onEvent) {
  if (line.empty()) {
    return;
  }

  rapidjson::Document d;
  d.Parse(line.c_str());
  if (d.HasParseError() || !d.IsObject()) {
    return;
  }

  // Parse usage metadata - Qwen may send usage only in final chunk or not at
  // all for free tier
  if (d.HasMember("usage") && !d["usage"].IsNull()) {
    const auto &usage = d["usage"];
    AgentMetrics metrics;

    if (usage.HasMember("prompt_tokens") && usage["prompt_tokens"].IsUint()) {
      metrics.tokens.prompt = usage["prompt_tokens"].GetUint();
      metrics.tokens.contextSize = metrics.tokens.prompt;
    }
    if (usage.HasMember("completion_tokens") &&
        usage["completion_tokens"].IsUint()) {
      metrics.tokens.completion = usage["completion_tokens"].GetUint();
    }
    if (usage.HasMember("total_tokens") && usage["total_tokens"].IsUint()) {
      metrics.tokens.total = usage["total_tokens"].GetUint();
    }
    // Qwen may send reasoning_tokens separately
    if (usage.HasMember("prompt_tokens_details") &&
        usage["prompt_tokens_details"].IsObject()) {
      const auto &details = usage["prompt_tokens_details"];
      if (details.HasMember("cached_tokens") &&
          details["cached_tokens"].IsUint()) {
        metrics.tokens.cacheRead = details["cached_tokens"].GetUint();
      }
    }
    if (usage.HasMember("completion_tokens_details") &&
        usage["completion_tokens_details"].IsObject()) {
      const auto &details = usage["completion_tokens_details"];
      if (details.HasMember("reasoning_tokens") &&
          details["reasoning_tokens"].IsUint()) {
        metrics.tokens.reasoning = details["reasoning_tokens"].GetUint();
      }
    }

    // Only emit if we have actual token data
    if (metrics.tokens.prompt > 0 || metrics.tokens.completion > 0) {
      onEvent(metrics);
    }
  }

  // Parse choices
  if (d.HasMember("choices") && d["choices"].IsArray()) {
    for (const auto &choice : d["choices"].GetArray()) {
      if (!choice.IsObject()) {
        continue;
      }

      // Check for finish reason
      if (choice.HasMember("finish_reason") &&
          choice["finish_reason"].IsString()) {
        std::string reason = choice["finish_reason"].GetString();
        if (reason == "stop") {
          onEvent(StreamDone{StopReason::Stop});
        } else if (reason == "tool_calls") {
          onEvent(StreamDone{StopReason::ToolUse});
        } else if (reason == "length") {
          onEvent(StreamDone{StopReason::MaxTokens});
        } else if (reason == "content_filter") {
          onEvent(StreamDone{StopReason::ContentFilter});
        }
      }

      // Parse delta
      if (choice.HasMember("delta") && choice["delta"].IsObject()) {
        const auto &delta = choice["delta"];

        // Content text
        if (delta.HasMember("content") && delta["content"].IsString()) {
          std::string content = delta["content"].GetString();
          if (!content.empty()) {
            onEvent(TextChunk{content});
          }
        }

        // Reasoning/thinking content
        if (delta.HasMember("reasoning_content") &&
            delta["reasoning_content"].IsString()) {
          std::string reasoning = delta["reasoning_content"].GetString();
          if (!reasoning.empty()) {
            onEvent(ThinkingChunk{reasoning, ""});
          }
        }

        // Tool calls
        if (delta.HasMember("tool_calls") && delta["tool_calls"].IsArray()) {
          for (const auto &tc : delta["tool_calls"].GetArray()) {
            if (!tc.IsObject()) {
              continue;
            }

            ToolCallChunk chunk;

            if (tc.HasMember("index") && tc["index"].IsUint()) {
              chunk.index = tc["index"].GetUint();
            }
            if (tc.HasMember("id") && tc["id"].IsString()) {
              chunk.id = tc["id"].GetString();
            }

            if (tc.HasMember("function") && tc["function"].IsObject()) {
              const auto &func = tc["function"];
              if (func.HasMember("name") && func["name"].IsString()) {
                chunk.nameDelta = func["name"].GetString();
              }
              if (func.HasMember("arguments") && func["arguments"].IsString()) {
                chunk.argsDelta = func["arguments"].GetString();
              }
            }

            onEvent(chunk);
          }
        }
      }
    }
  }
}

bool QwenProvider::executeStreamRequest(
    OAuthAccount &acc, const AgentHistory &history, const ProviderOptions &opts,
    std::function<void(const StreamEvent &)> &onEvent) {
  // Build request body (OpenAI-compatible format)
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();

  // Model
  std::string modelId = opts.modelId.empty() ? "coder-model" : opts.modelId;
  d.AddMember("model", rapidjson::Value(modelId.c_str(), a), a);

  // Stream
  d.AddMember("stream", true, a);

  // Messages
  rapidjson::Value messages(rapidjson::kArrayType);
  for (const auto &turn : history.turns) {
    for (const auto &msg : turn.messages) {
      if (msg.role == firmius::shared::Role::Error) {
        continue;
      }

      rapidjson::Value message(rapidjson::kObjectType);
      message.AddMember("role",
                        rapidjson::Value(roleToString(msg.role).c_str(), a), a);

      // Build content array for text/images
      rapidjson::Value content(rapidjson::kArrayType);
      for (const auto &part : msg.content) {
        if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
          rapidjson::Value item(rapidjson::kObjectType);
          item.AddMember("type", "text", a);
          item.AddMember("text", rapidjson::Value(txt->text.c_str(), a), a);
          content.PushBack(item, a);
        } else if (auto *img =
                       std::get_if<firmius::shared::ImageContent>(&part)) {
          rapidjson::Value item(rapidjson::kObjectType);
          item.AddMember("type", "image_url", a);
          rapidjson::Value imageUrl(rapidjson::kObjectType);
          imageUrl.AddMember("url", rapidjson::Value(img->url.c_str(), a), a);
          item.AddMember("image_url", imageUrl, a);
          content.PushBack(item, a);
        }
      }

      // Add content to message if not empty
      if (content.Size() > 0) {
        message.AddMember("content", content, a);
      }

      // Handle tool calls (assistant message with tool_calls)
      for (const auto &part : msg.content) {
        if (auto *call = std::get_if<firmius::shared::ToolCallContent>(&part)) {
          // Add tool_calls array to the message
          if (!message.HasMember("tool_calls")) {
            rapidjson::Value toolCalls(rapidjson::kArrayType);
            message.AddMember("tool_calls", toolCalls, a);
          }

          rapidjson::Value toolCall(rapidjson::kObjectType);
          toolCall.AddMember("id", rapidjson::Value(call->id.c_str(), a), a);
          toolCall.AddMember("type", "function", a);

          rapidjson::Value function(rapidjson::kObjectType);
          function.AddMember("name", rapidjson::Value(call->name.c_str(), a),
                             a);
          function.AddMember("arguments",
                             rapidjson::Value(call->args.c_str(), a), a);
          toolCall.AddMember("function", function, a);

          message["tool_calls"].PushBack(toolCall, a);
        }
      }

      // Handle tool results (tool role message)
      for (const auto &part : msg.content) {
        if (auto *result =
                std::get_if<firmius::shared::ToolResultContent>(&part)) {
          // Tool results are sent as separate messages with role="tool"
          // But since we're iterating by message, we need to handle this
          // differently For now, add result as content if this is a tool result
          // message
          if (msg.role == firmius::shared::Role::ToolResult) {
            if (!message.HasMember("content") ||
                message["content"].Size() == 0) {
              rapidjson::Value resultContent(rapidjson::kArrayType);
              rapidjson::Value item(rapidjson::kObjectType);
              item.AddMember("type", "text", a);
              item.AddMember("text",
                             rapidjson::Value(result->result.c_str(), a), a);
              resultContent.PushBack(item, a);
              message.AddMember("content", resultContent, a);
            }
          }
          // Add tool_call_id for tool result messages
          if (!result->toolCallId.empty()) {
            message.AddMember("tool_call_id",
                              rapidjson::Value(result->toolCallId.c_str(), a),
                              a);
          }
        }
      }

      messages.PushBack(message, a);
    }
  }
  d.AddMember("messages", messages, a);

  // Tools
  if (!opts.tools.empty()) {
    rapidjson::Value tools(rapidjson::kArrayType);
    for (const auto &tool : opts.tools) {
      rapidjson::Value toolObj(rapidjson::kObjectType);
      toolObj.AddMember("type", "function", a);

      rapidjson::Value function(rapidjson::kObjectType);
      function.AddMember("name", rapidjson::Value(tool.name.c_str(), a), a);
      function.AddMember("description",
                         rapidjson::Value(tool.description.c_str(), a), a);

      rapidjson::Document schemaDoc;
      schemaDoc.Parse(tool.inputSchema.c_str());
      if (!schemaDoc.HasParseError() && schemaDoc.IsObject()) {
        rapidjson::Value params;
        params.CopyFrom(schemaDoc, a);
        function.AddMember("parameters", params, a);
      } else {
        rapidjson::Value params(rapidjson::kObjectType);
        function.AddMember("parameters", params, a);
      }

      toolObj.AddMember("function", function, a);
      tools.PushBack(toolObj, a);
    }
    d.AddMember("tools", tools, a);
  }

  // Temperature
  d.AddMember("temperature", opts.temperature, a);

  // Max tokens
  if (opts.maxTokens.has_value()) {
    d.AddMember("max_tokens", rapidjson::Value(opts.maxTokens.value()), a);
  }

  // Stop sequences
  if (!opts.stop.empty()) {
    rapidjson::Value stop(rapidjson::kArrayType);
    for (const auto &s : opts.stop) {
      stop.PushBack(rapidjson::Value(s.c_str(), a), a);
    }
    d.AddMember("stop", stop, a);
  }

  // Request usage metadata in streaming response (OpenAI-compatible)
  rapidjson::Value streamOptions(rapidjson::kObjectType);
  streamOptions.AddMember("include_usage", true, a);
  d.AddMember("stream_options", streamOptions, a);

  // Serialize to JSON string
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  std::string body = buffer.GetString();

  // Make HTTP request
  firmius::utils::GCPHttpClient client;
  client.setBearerToken(acc.accessToken);
  client.setContentType("application/json");

  std::uint64_t startMs = nowMs();
  std::uint64_t firstTokenMs = 0;
  bool firstTokenEmitted = false;
  bool metricsReceived = false;
  AgentMetrics capturedMetrics;

  auto wrappedOnEvent = [&](const StreamEvent &ev) {
    if (std::holds_alternative<TextChunk>(ev)) {
      if (!firstTokenEmitted) {
        firstTokenMs = nowMs();
        firstTokenEmitted = true;
      }
      onEvent(ev);
    } else if (std::holds_alternative<ThinkingChunk>(ev)) {
      if (!firstTokenEmitted) {
        firstTokenMs = nowMs();
        firstTokenEmitted = true;
      }
      onEvent(ev);
    } else if (auto *met = std::get_if<AgentMetrics>(&ev)) {
      capturedMetrics = *met;
      metricsReceived = true;
    } else {
      onEvent(ev);
    }
  };

  std::function<void(const StreamEvent &)> wrappedFn = wrappedOnEvent;
  StreamContext ctx{this, &wrappedFn, "", 0, opts.abortSignal};

  // 30 second timeout for Qwen API
  auto resp =
      client.streamPost(QWEN_CHAT_ENDPOINT, body, sseWriteCallback, &ctx, 30);

  // Handle response
  if (resp.code == 200) {
    auto endMs = nowMs();
    if (metricsReceived) {
      capturedMetrics.timing.startMs = startMs;
      capturedMetrics.timing.firstTokenMs =
          firstTokenEmitted ? firstTokenMs : 0;
      capturedMetrics.timing.endMs = endMs;
      onEvent(capturedMetrics);
    }
    return true; // Success
  }

  // Error handling
  std::string errMsg = "API error: " + std::to_string(resp.code);
  if (!ctx.buffer.empty()) {
    errMsg += "\n" + ctx.buffer;
  }

  if (resp.code == 401 || resp.code == 403) {
    // Token expired or invalid - mark for refresh
    acc.rateLimited = true;
    acc.backoffUntil =
        nowSeconds() + firmius::shared::BackoffConstants::MAX_BACKOFF;
    onEvent(StreamError{"Authentication failed. Token may be expired.",
                        static_cast<int>(resp.code), acc.getIdentifier()});
  } else if (resp.code == 429 ||
             (resp.code == 400 &&
              errMsg.find("insufficient_quota") != std::string::npos)) {
    // Rate limited or quota exhausted - IMMEDIATE account switch
    // Qwen has no real quota API, so we don't mark quotas as 0
    // Just rely on retry/account switching logic
    
    std::string quotaError =
        "Quota exhausted (2k/day free limit). Switching to next account...";
    onEvent(StreamError{quotaError, static_cast<int>(resp.code),
                        acc.getIdentifier()});
  } else if (resp.code >= 500 || resp.code == 0) {
    // Server error or timeout - DON'T mark as exhausted, just fail this attempt
    // The retry logic will try other accounts
    onEvent(StreamError{resp.code == 0
                            ? "Request timeout"
                            : "Server error: " + std::to_string(resp.code),
                        static_cast<int>(resp.code), acc.getIdentifier()});
  } else {
    // Other errors (4xx) - don't mark as rate-limited, but fail this attempt
    onEvent(
        StreamError{errMsg, static_cast<int>(resp.code), acc.getIdentifier()});
  }
  return false; // Failed
}

void QwenProvider::stream(const AgentHistory &history,
                          const ProviderOptions &opts,
                          std::function<void(const StreamEvent &)> onEvent) {
  // Refresh quotas first to reset any stale quota="0" markers
  // Since Qwen has no dynamic quota API, we assume 100% available
  refreshQuotas();

  // Retry schedule: 1s -> 2s -> 3s -> 4s -> 5s (fail fast, switch accounts)
  static const int retryDelays[] = {1, 2, 3, 4, 5};
  static const int numRetries = sizeof(retryDelays) / sizeof(retryDelays[0]);

  int accountSwitchCount = 0;
  std::string lastError;
  std::string lastAccountEmail;
  int maxAccountSwitches = std::max(3, static_cast<int>(accounts_.size()));

  // Try each account with retries
  while (accountSwitchCount < maxAccountSwitches) {
    auto optAcc = getAvailableAccount(opts.modelId);
    if (!optAcc) {
      // All accounts rate-limited - wait for earliest to unlock
      int64_t now = nowSeconds();
      int64_t earliestUnlock = 0;
      for (const auto &a : accounts_) {
        if (a.rateLimited) {
          if (earliestUnlock == 0 || a.backoffUntil < earliestUnlock) {
            earliestUnlock = a.backoffUntil;
          }
        }
      }

      int64_t waitSec = (earliestUnlock > now) ? (earliestUnlock - now) : 0;
      if (waitSec > 0 && waitSec <= 120) {
        onEvent(StreamRetrying{
            1, numRetries, 429, static_cast<int>(waitSec * 1000),
            "All accounts rate-limited, waiting", lastAccountEmail});
        std::this_thread::sleep_for(std::chrono::seconds(waitSec));

        // Clear expired backoffs
        int64_t nowAfter = nowSeconds();
        for (auto &a : accounts_) {
          if (a.rateLimited && nowAfter > a.backoffUntil) {
            a.rateLimited = false;
          }
        }
        continue;
      }

      // No accounts available
      if (!lastError.empty()) {
        onEvent(StreamError{lastError, -1, lastAccountEmail});
      } else {
        onEvent(StreamError{"No accounts available.", -1, lastAccountEmail});
      }
      return;
    }

    OAuthAccount &acc = *optAcc.value();

    if (accountSwitchCount > 0) {
      onEvent(StreamAccountSwitched{acc.getIdentifier()});
    }
    lastAccountEmail = acc.getIdentifier();

    // Retry loop for this account (retries happen within same turn)
    bool accountFailed = false;
    bool wasQuotaError = false;

    for (int retryIdx = 0; retryIdx < numRetries; ++retryIdx) {
      if (retryIdx > 0) {
        int delaySec = retryDelays[retryIdx - 1];
        onEvent(StreamRetrying{retryIdx, numRetries, 0, delaySec * 1000,
                               "Retrying request", acc.getIdentifier()});
        // Use interruptible sleep to allow immediate cancellation
        if (!interruptibleSleep(std::chrono::seconds(delaySec),
                                opts.abortSignal)) {
          // Interrupted during retry delay
          return;
        }
      }

      // Try the request
      if (executeStreamRequest(acc, history, opts, onEvent)) {
        // Success! Update lastUsedIndex_ so next request starts from this
        // account
        {
          std::lock_guard<std::recursive_mutex> lock(accountsMutex_);
          for (size_t i = 0; i < accounts_.size(); i++) {
            if (&accounts_[i] == &acc) {
              lastUsedIndex_ = static_cast<int>(i);
              saveAccounts();
              break;
            }
          }
        }
        return; // Success!
      }

      // Check if this was a quota exhaustion error (429 or insufficient_quota)
      if (wasQuotaError) {
        // Quota exhausted - switch accounts immediately, no more retries
        accountSwitchCount++;
        break;
      }

      // For other errors (timeout, server error), retry a few times then switch
      // After 2 retries, switch to next account
      if (retryIdx >= 2) {
        accountFailed = true;
        accountSwitchCount++;
        break;
      }
    }

    // All retries exhausted for this account
    if (accountFailed) {
      if (wasQuotaError) {
        continue; // Try next account
      }
      // For non-quota errors, we already switched accounts in the loop
      continue;
    }

    // Non-rate-limit failure after all retries
    lastError = "Exhausted all retries";
    onEvent(StreamError{lastError, -1, lastAccountEmail});
    return;
  }

  // All accounts exhausted
  onEvent(StreamError{"All accounts rate-limited or exhausted", -1,
                      lastAccountEmail});
}

void QwenProvider::generateSummary(
    const std::string &modelId, const AgentHistory &history,
    const std::string &, std::function<void(const StreamEvent &)> onEvent,
    std::atomic<bool> *abortSignal) {
  ProviderOptions opts;
  opts.modelId = modelId.empty() ? "coder-model" : modelId;
  opts.temperature = 0.7f;
  opts.maxTokens = 16384;
  opts.abortSignal = abortSignal;
  stream(history, opts, onEvent);
}

} // namespace firmius::provider
