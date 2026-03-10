#include "providers/QwenProvider.hpp"
#include "utils/GCPHttpClient.hpp"
#include "utils/StringUtil.hpp"
#include "utils/TempOAuthServer.hpp"
#include "utils/Logger.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <thread>

namespace firmius::provider {

// Initialize static constants
const std::string QwenProvider::QWEN_OAUTH_CLIENT_ID = "f0304373b74a44d2b584a3fb70ca9e56";
const std::string QwenProvider::QWEN_OAUTH_SCOPE = "openid profile email model.completion";
const std::string QwenProvider::QWEN_OAUTH_GRANT_TYPE = "urn:ietf:params:oauth:grant-type:device_code";
const std::string QwenProvider::QWEN_OAUTH_DEVICE_CODE_ENDPOINT = "https://chat.qwen.ai/api/v1/oauth2/device/code";
const std::string QwenProvider::QWEN_OAUTH_TOKEN_ENDPOINT = "https://chat.qwen.ai/api/v1/oauth2/token";
const std::string QwenProvider::QWEN_API_BASE_URL = "https://portal.qwen.ai/v1";
const std::string QwenProvider::QWEN_CHAT_ENDPOINT = "https://portal.qwen.ai/v1/chat/completions";
const std::string QwenProvider::QWEN_MODELS_ENDPOINT = "https://portal.qwen.ai/v1/models";

namespace {

// Retry configuration
struct RetrySettings {
  static constexpr int BASE_DELAY_MS = 1000;
  static constexpr int MAX_DELAY_MS = 30000;
  static constexpr int MAX_RETRIES = 5;
  static constexpr double JITTER_MIN = 0.5;
  static constexpr double JITTER_MAX = 1.0;
};

// Stream context for CURL callback
struct StreamContext {
  QwenProvider *provider;
  std::function<void(const StreamEvent &)> *onEvent;
  std::string buffer;
  size_t readOffset = 0;
  std::atomic<bool> *abortSignal;
};

// Generate PKCE code verifier (32 bytes, base64url encoded)
std::string generateVerifier() {
  std::vector<uint8_t> bytes(32);
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto &b : bytes)
    b = static_cast<uint8_t>(dist(rd));

  // Base64url encode
  static const char *chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0;
  int valb = -6;
  for (uint8_t c : bytes) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  while (out.size() % 4)
    out.push_back('=');

  // Convert to base64url
  for (char &c : out) {
    if (c == '+')
      c = '-';
    else if (c == '/')
      c = '_';
  }
  while (!out.empty() && out.back() == '=')
    out.pop_back();

  return out;
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
std::string generateCodeChallenge(const std::string &verifier) {
  auto hash = sha256(verifier);

  // Base64url encode
  static const char *chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0;
  int valb = -6;
  for (uint8_t c : hash) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  while (out.size() % 4)
    out.push_back('=');

  // Convert to base64url
  for (char &c : out) {
    if (c == '+')
      c = '-';
    else if (c == '/')
      c = '_';
  }
  while (!out.empty() && out.back() == '=')
    out.pop_back();

  return out;
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
      escaped << '%' << std::uppercase << std::setw(2)
              << static_cast<int>(c) << std::nouppercase;
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

    auto resp = client.post(
        QwenProvider::QWEN_OAUTH_DEVICE_CODE_ENDPOINT,
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
              "   " + verificationUriComplete_ +
              "\n\n"
              "2. Enter the code: " + userCode_ +
              "\n\n"
              "3. Press Enter after completing authorization...";

    // Start polling in background thread
    pollingThread_ = std::thread([this]() {
      pollForToken();
    });
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
    return isComplete_ || tokenReceived_; 
  }

  bool finalizeExchange(std::string &outErrorMessage) override {
    if (!tokenReceived_) {
      outErrorMessage = "OAuth authorization not completed: " + prompt_;
      return false;
    }

    // Build account from token response
    OAuthAccount acc;
    acc.accessToken = accessToken_;
    acc.refreshToken = refreshToken_;
    acc.tokenExpiration = tokenExpiration_;
    acc.email = email_;

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
    const std::uint64_t timeoutMs = static_cast<std::uint64_t>(expiresIn_) * 1000 - 3000;

    while ((nowMs() - startTime) < timeoutMs) {
      auto resp = client.post(QwenProvider::QWEN_OAUTH_TOKEN_ENDPOINT,
                              objectToUrlEncoded(bodyData));

      if (resp.code == 200) {
        rapidjson::Document doc;
        doc.Parse(resp.body.c_str());
        if (!doc.HasParseError() && doc.IsObject()) {
          if (doc.HasMember("access_token") &&
              doc["access_token"].IsString()) {
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
            tokenReceived_ = true;
            isComplete_ = true;
            return;
          }
        }
      } else if (resp.code == 400) {
        // Check for specific OAuth errors
        rapidjson::Document doc;
        doc.Parse(resp.body.c_str());
        if (!doc.HasParseError() && doc.IsObject() &&
            doc.HasMember("error") && doc["error"].IsString()) {
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
                                           : (c == '/') ? 63
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
      auto resp =
          client.get("https://chat.qwen.ai/api/v1/oauth2/userinfo", 10);
      if (resp.code == 200) {
        rapidjson::Document doc;
        doc.Parse(resp.body.c_str());
        if (!doc.HasParseError() && doc.IsObject() &&
            doc.HasMember("email") && doc["email"].IsString()) {
          email_ = doc["email"].GetString();
        }
      }
    }
  }

  QwenProvider *provider_;
  std::string prompt_;
  bool promptShown_ = false;
  bool isComplete_ = false;
  bool tokenReceived_ = false;

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
  // Qwen OAuth is free tier, no quota tracking needed
  // But we can still update token expiration
  int64_t now = nowSeconds();
  for (auto &acc : accounts_) {
    if (isTokenExpired(acc)) {
      refreshAccessToken(acc);
    }
    acc.lastQuotaRefresh = now;
  }
  saveAccounts();
}

std::map<std::string, std::vector<QuotaBucket>>
QwenProvider::getAllQuotas() const {
  // Qwen OAuth is free tier, return empty quotas
  return {};
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
    std::string line = ctx->buffer.substr(ctx->readOffset, newlinePos - ctx->readOffset);
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

  // Parse usage metadata
  if (d.HasMember("usage")) {
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

    onEvent(metrics);
  }

  // Parse choices
  if (d.HasMember("choices") && d["choices"].IsArray()) {
    for (const auto &choice : d["choices"].GetArray()) {
      if (!choice.IsObject()) {
        continue;
      }

      // Check for finish reason
      if (choice.HasMember("finish_reason") && choice["finish_reason"].IsString()) {
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
            onEvent(ThinkingChunk{reasoning});
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
              if (func.HasMember("arguments") &&
                  func["arguments"].IsString()) {
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

void QwenProvider::executeStreamRequest(
    OAuthAccount &acc, const AgentHistory &history,
    const ProviderOptions &opts,
    std::function<void(const StreamEvent &)> &onEvent) {
  // Build request body (OpenAI-compatible format)
  rapidjson::Document d;
  d.SetObject();
  auto &a = d.GetAllocator();

  // Model
  std::string modelId = opts.modelId.empty() ? "qwen3.5-plus" : opts.modelId;
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

      // Build content array
      rapidjson::Value content(rapidjson::kArrayType);
      for (const auto &part : msg.content) {
        if (auto *txt = std::get_if<firmius::shared::TextContent>(&part)) {
          rapidjson::Value item(rapidjson::kObjectType);
          item.AddMember("type", "text", a);
          item.AddMember("text",
                         rapidjson::Value(txt->text.c_str(), a), a);
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
        // Tool calls and results handled separately in OpenAI format
      }

      // Add content to message
      if (content.Size() > 0) {
        message.AddMember("content", content, a);
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
      function.AddMember("name",
                         rapidjson::Value(tool.name.c_str(), a), a);
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
    d.AddMember("max_tokens",
                rapidjson::Value(opts.maxTokens.value()), a);
  }

  // Stop sequences
  if (!opts.stop.empty()) {
    rapidjson::Value stop(rapidjson::kArrayType);
    for (const auto &s : opts.stop) {
      stop.PushBack(rapidjson::Value(s.c_str(), a), a);
    }
    d.AddMember("stop", stop, a);
  }

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

  auto resp = client.streamPost(QWEN_CHAT_ENDPOINT, body, sseWriteCallback,
                                &ctx);

  // Handle response
  if (resp.code == 200) {
    auto endMs = nowMs();
    if (metricsReceived) {
      capturedMetrics.timing.startMs = startMs;
      capturedMetrics.timing.firstTokenMs = firstTokenEmitted ? firstTokenMs : 0;
      capturedMetrics.timing.endMs = endMs;
      onEvent(capturedMetrics);
    }
    return;
  }

  // Error handling
  std::string errMsg = "API error: " + std::to_string(resp.code);
  if (!ctx.buffer.empty()) {
    errMsg += "\n" + ctx.buffer;
  }

  if (resp.code == 401 || resp.code == 403) {
    // Token expired or invalid - mark for refresh
    acc.rateLimited = true;
    acc.backoffUntil = nowSeconds() + 60;
    onEvent(StreamError{
        "Authentication failed. Token may be expired.",
        static_cast<int>(resp.code), acc.email});
  } else if (resp.code == 429) {
    // Rate limited
    int backoff = 60;
    markAccountRateLimited(acc, backoff);
    onEvent(StreamRetrying{1, 5, 429, backoff * 1000,
                           "Rate limited", acc.email});
  } else if (resp.code >= 500) {
    // Server error
    onEvent(StreamError{"Server error: " + std::to_string(resp.code),
                        static_cast<int>(resp.code), acc.email});
  } else {
    // Other errors
    onEvent(StreamError{errMsg, static_cast<int>(resp.code), acc.email});
  }
}

void QwenProvider::stream(const AgentHistory &history,
                          const ProviderOptions &opts,
                          std::function<void(const StreamEvent &)> onEvent) {
  int accountRetries = 0;
  std::string lastError;
  std::string lastAccountEmail;
  int maxRetries = std::max(5, static_cast<int>(accounts_.size()) * 3);

  while (accountRetries < maxRetries) {
    auto optAcc = getAvailableAccount(opts.modelId);
    if (!optAcc) {
      // All accounts rate-limited
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
      if (waitSec > 120) {
        waitSec = 120;
      }

      if (waitSec > 0) {
        onEvent(StreamRetrying{accountRetries + 1, maxRetries, 429,
                               static_cast<int>(waitSec * 1000),
                               "All accounts rate-limited", lastAccountEmail});
        std::this_thread::sleep_for(std::chrono::seconds(waitSec));

        // Clear expired backoffs
        int64_t nowAfter = nowSeconds();
        for (auto &a : accounts_) {
          if (a.rateLimited && nowAfter > a.backoffUntil) {
            a.rateLimited = false;
          }
        }
        accountRetries++;
        continue;
      }

      if (!lastError.empty()) {
        onEvent(StreamError{lastError, -1, lastAccountEmail});
      } else {
        onEvent(StreamError{"No accounts available.", -1, lastAccountEmail});
      }
      return;
    }

    OAuthAccount &acc = *optAcc.value();
    lastAccountEmail = acc.email;

    if (accountRetries > 0) {
      onEvent(StreamAccountSwitched{acc.email});
    }

    // Retry loop for this account
    for (int retryAttempt = 0; retryAttempt < 4; ++retryAttempt) {
      if (retryAttempt > 0) {
        int delay = (1 << (retryAttempt - 1)) * 1000;
        onEvent(StreamRetrying{retryAttempt, 4, 0, delay,
                               "Connection error", acc.email});
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      }

      executeStreamRequest(acc, history, opts, onEvent);
      return; // Success or terminal error
    }

    accountRetries++;
  }

  if (!lastError.empty()) {
    onEvent(StreamError{lastError, -1, lastAccountEmail});
  } else {
    onEvent(StreamError{"Exhausted retries.", -1, lastAccountEmail});
  }
}

void QwenProvider::generateSummary(
    const std::string &modelId, const AgentHistory &history,
    const std::string &, std::function<void(const StreamEvent &)> onEvent,
    std::atomic<bool> *abortSignal) {
  ProviderOptions opts;
  opts.modelId = modelId.empty() ? "qwen3.5-plus" : modelId;
  opts.temperature = 0.7f;
  opts.maxTokens = 16384;
  opts.abortSignal = abortSignal;
  stream(history, opts, onEvent);
}

} // namespace firmius::provider
