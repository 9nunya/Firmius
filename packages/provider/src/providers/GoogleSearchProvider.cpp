#include "providers/GoogleSearchProvider.hpp"
#include "providers/AntigravityProvider.hpp"
#include "providers/LLMSearchProviderRegistry.hpp"
#include "utils/GCPHttpClient.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <sstream>
#include <thread>

namespace firmius::provider {

namespace {

// Constants ported from the TypeScript reference implementation
constexpr const char* kSearchModel = "gemini-2.5-flash";
constexpr const char* kAntigravityEndpoint = "https://daily-cloudcode-pa.sandbox.googleapis.com";
constexpr int kSearchTimeoutSeconds = 60;
constexpr int kMaxSearchAttempts = 6;
constexpr int kRetryBaseDelayMs = 350;
constexpr int kRetryMaxDelayMs = 3000;
constexpr int kRetryAccountBackoffSeconds = 25;

// System instruction for search (matches TypeScript SEARCH_SYSTEM_INSTRUCTION)
constexpr const char* kSearchSystemInstruction =
    "You are an expert web search assistant with access to Google Search and URL analysis tools.\n"
    "\n"
    "Your capabilities:\n"
    "- Use google_search to find real-time information from the web\n"
    "- Use url_context to fetch and analyze content from specific URLs when provided\n"
    "\n"
    "Guidelines:\n"
    "- Always provide accurate, well-sourced information\n"
    "- Cite your sources when presenting facts\n"
    "- If analyzing URLs, extract the most relevant information\n"
    "- Be concise but comprehensive in your responses\n"
    "- If information is uncertain or conflicting, acknowledge it\n"
    "- Focus on answering the user's question directly";

// User-Agent matching the TypeScript getAntigravityHeaders browser UA
std::string getSearchUserAgent() {
    // Randomize platform like the TypeScript reference does
    static const std::vector<std::string> platforms = {
        "windows/amd64",
        "darwin/arm64",
        "darwin/amd64",
    };
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, platforms.size() - 1);
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
           "(KHTML, like Gecko) Antigravity/1.18.3 Chrome/138.0.7204.235 "
           "Electron/37.3.1 Safari/537.36";
}

// Generate a unique request ID (matches TypeScript generateRequestId)
std::string generateRequestId() {
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now.time_since_epoch())
                     .count();
    // Convert to base36-like string
    std::ostringstream oss;
    oss << "search-" << std::hex << epoch;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 35);
    for (int i = 0; i < 6; ++i) {
        int v = dis(gen);
        oss << (v < 10 ? static_cast<char>('0' + v) : static_cast<char>('a' + v - 10));
    }
    return oss.str();
}

// Generate a session ID (matches TypeScript getSessionId pattern)
std::string generateSessionId() {
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now.time_since_epoch())
                     .count();
    std::ostringstream oss;
    oss << "search-" << std::hex << epoch << "-session";
    return oss.str();
}

int64_t nowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool containsCaseInsensitive(const std::string& haystack,
                             const std::string& needle) {
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char lhs, char rhs) {
                              return static_cast<char>(std::tolower(static_cast<unsigned char>(lhs))) ==
                                     static_cast<char>(std::tolower(static_cast<unsigned char>(rhs)));
                          });
    return it != haystack.end();
}

bool isRetryableHttpCode(long code) {
    return code == 429 || code == 500 || code == 502 || code == 503 || code == 504;
}

bool isRetryableBodyError(const std::string& body) {
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("error") ||
        !doc["error"].IsObject()) {
        return containsCaseInsensitive(body, "no capacity available") ||
               containsCaseInsensitive(body, "resource_exhausted") ||
               containsCaseInsensitive(body, "unavailable");
    }

    const auto& err = doc["error"];
    if (err.HasMember("code") && err["code"].IsInt() &&
        isRetryableHttpCode(err["code"].GetInt())) {
        return true;
    }
    if (err.HasMember("status") && err["status"].IsString()) {
        const std::string status = err["status"].GetString();
        if (status == "UNAVAILABLE" || status == "RESOURCE_EXHAUSTED") {
            return true;
        }
    }
    if (err.HasMember("message") && err["message"].IsString()) {
        const std::string message = err["message"].GetString();
        return containsCaseInsensitive(message, "no capacity available") ||
               containsCaseInsensitive(message, "resource exhausted") ||
               containsCaseInsensitive(message, "temporarily unavailable");
    }
    return false;
}

bool isRetryableParsedError(const std::string& error) {
    return containsCaseInsensitive(error, "no capacity available") ||
           containsCaseInsensitive(error, "resource exhausted") ||
           containsCaseInsensitive(error, "unavailable") ||
           containsCaseInsensitive(error, "deadline exceeded") ||
           containsCaseInsensitive(error, "timeout");
}

int computeRetryDelayMs(int attemptIndex) {
    const int boundedAttempt = std::max(0, attemptIndex);
    const int backoff = std::min(kRetryMaxDelayMs, kRetryBaseDelayMs * (1 << std::min(boundedAttempt, 4)));
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> jitter(0, 175);
    return backoff + jitter(rng);
}

void markAccountTemporarilyRateLimited(AntigravityProvider* provider,
                                       OAuthAccount account,
                                       int backoffSeconds) {
    account.rateLimited = true;
    account.backoffUntil = nowEpochSeconds() + backoffSeconds;
    provider->updateAccount(account);
}

// Parse the Antigravity search response (matches TypeScript parseSearchResponse)
SearchResult parseSearchResponse(const std::string& responseBody) {
    SearchResult result;

    rapidjson::Document doc;
    doc.Parse(responseBody.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        result.error = "Invalid JSON response from search API";
        return result;
    }

    // Check for top-level error
    if (doc.HasMember("error") && doc["error"].IsObject()) {
        const auto& err = doc["error"];
        std::string msg = "Search API error";
        if (err.HasMember("message") && err["message"].IsString()) {
            msg += ": " + std::string(err["message"].GetString());
        }
        result.error = msg;
        return result;
    }

    // Navigate to response.candidates[0]
    if (!doc.HasMember("response") || !doc["response"].IsObject()) {
        result.error = "No response in search result";
        return result;
    }
    const auto& response = doc["response"];

    if (!response.HasMember("candidates") || !response["candidates"].IsArray() ||
        response["candidates"].Empty()) {
        // Check for response-level error
        if (response.HasMember("error") && response["error"].IsObject()) {
            const auto& rErr = response["error"];
            if (rErr.HasMember("message") && rErr["message"].IsString()) {
                result.error = "Error: " + std::string(rErr["message"].GetString());
            } else {
                result.error = "Unknown search error";
            }
        } else {
            result.error = "No candidates in search response";
        }
        return result;
    }

    const auto& candidate = response["candidates"][0];
    if (!candidate.IsObject()) {
        result.error = "Invalid candidate in search response";
        return result;
    }

    // Extract text content from candidate.content.parts
    if (candidate.HasMember("content") && candidate["content"].IsObject()) {
        const auto& content = candidate["content"];
        if (content.HasMember("parts") && content["parts"].IsArray()) {
            std::vector<std::string> textParts;
            for (const auto& part : content["parts"].GetArray()) {
                if (part.IsObject() && part.HasMember("text") && part["text"].IsString()) {
                    textParts.push_back(part["text"].GetString());
                }
            }
            // Join text parts
            for (size_t i = 0; i < textParts.size(); ++i) {
                if (i > 0) result.formatted_text += "\n";
                result.formatted_text += textParts[i];
            }
        }
    }

    // Extract grounding metadata (sources and queries)
    if (candidate.HasMember("groundingMetadata") && candidate["groundingMetadata"].IsObject()) {
        const auto& gm = candidate["groundingMetadata"];

        // webSearchQueries -> queries_used
        if (gm.HasMember("webSearchQueries") && gm["webSearchQueries"].IsArray()) {
            for (const auto& q : gm["webSearchQueries"].GetArray()) {
                if (q.IsString()) {
                    result.queries_used.push_back(q.GetString());
                }
            }
        }

        // groundingChunks -> sources
        if (gm.HasMember("groundingChunks") && gm["groundingChunks"].IsArray()) {
            for (const auto& chunk : gm["groundingChunks"].GetArray()) {
                if (chunk.IsObject() && chunk.HasMember("web") && chunk["web"].IsObject()) {
                    const auto& web = chunk["web"];
                    std::string url, title;
                    if (web.HasMember("uri") && web["uri"].IsString()) {
                        url = web["uri"].GetString();
                    }
                    if (web.HasMember("title") && web["title"].IsString()) {
                        title = web["title"].GetString();
                    }
                    if (!url.empty() && !title.empty()) {
                        result.sources.push_back({url, title});
                    }
                }
            }
        }
    }

    // Extract urlContextMetadata -> urlsRetrieved (stored in sources for now)
    if (candidate.HasMember("urlContextMetadata") && candidate["urlContextMetadata"].IsObject()) {
        const auto& ucm = candidate["urlContextMetadata"];
        if (ucm.HasMember("url_metadata") && ucm["url_metadata"].IsArray()) {
            for (const auto& meta : ucm["url_metadata"].GetArray()) {
                if (meta.IsObject() && meta.HasMember("retrieved_url") &&
                    meta["retrieved_url"].IsString()) {
                    std::string url = meta["retrieved_url"].GetString();
                    std::string status = "UNKNOWN";
                    if (meta.HasMember("url_retrieval_status") &&
                        meta["url_retrieval_status"].IsString()) {
                        status = meta["url_retrieval_status"].GetString();
                    }
                    // Add to sources with status as title prefix
                    result.sources.push_back({url, "[" + status + "] " + url});
                }
            }
        }
    }

    return result;
}

// Format the search result for display (matches TypeScript formatSearchResult)
std::string formatSearchResult(const SearchResult& result) {
    std::ostringstream oss;

    oss << "## Search Results\n\n";
    oss << result.formatted_text;
    oss << "\n";

    if (!result.sources.empty()) {
        oss << "\n### Sources\n";
        for (const auto& [url, title] : result.sources) {
            oss << "- [" << title << "](" << url << ")\n";
        }
    }

    if (!result.queries_used.empty()) {
        oss << "\n### Search Queries Used\n";
        for (const auto& q : result.queries_used) {
            oss << "- \"" << q << "\"\n";
        }
    }

    return oss.str();
}

} // namespace

GoogleSearchProvider::GoogleSearchProvider(AntigravityProvider* antigravity)
    : antigravity_(antigravity) {
    // Auto-register with the singleton registry
    LLMSearchProviderRegistry::instance().registerProvider(
        std::shared_ptr<LLMSearchProvider>(this, [](LLMSearchProvider*) {
            // Non-owning deleter - the AntigravityProvider owns the lifecycle
            // through composition or the application manages it
        }));
}

std::string GoogleSearchProvider::name() const {
    return "google-search";
}

bool GoogleSearchProvider::isAvailable() const {
    if (!antigravity_) {
        return false;
    }
    return antigravity_->isConfigured();
}

SearchResult GoogleSearchProvider::search(const std::string& query,
                                           const std::vector<std::string>& urls) {
    SearchResult errorResult;

    if (!antigravity_) {
        errorResult.error = "AntigravityProvider not available";
        return errorResult;
    }

    // Build prompt with optional URLs
    std::string prompt = query;
    if (!urls.empty()) {
        prompt += "\n\nURLs to analyze:\n";
        for (const auto& url : urls) {
            prompt += url + "\n";
        }
    }

    const std::string url = std::string(kAntigravityEndpoint) + "/v1internal:generateContent";
    const auto accounts = antigravity_->getAccounts();
    const int accountCount = static_cast<int>(accounts.size());
    const int maxAttempts = std::max(2, std::min(kMaxSearchAttempts, accountCount * 2));

    std::string lastRetryableError;

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        // Ask Antigravity for an account that has quota for the exact search model.
        auto optAccount = antigravity_->getAvailableAccount(std::string(kSearchModel));
        if (!optAccount.has_value()) {
            if (!lastRetryableError.empty()) {
                errorResult.error = "Search retries exhausted after temporary provider errors. " +
                                    lastRetryableError;
            } else {
                errorResult.error = "No authenticated Antigravity accounts available for " +
                                    std::string(kSearchModel);
            }
            return errorResult;
        }

        OAuthAccount account = *optAccount;

        // Build the search request body following the TypeScript reference pattern.
        rapidjson::Document doc;
        doc.SetObject();
        auto& a = doc.GetAllocator();

        doc.AddMember("model", rapidjson::Value(kSearchModel, a), a);
        doc.AddMember("project", rapidjson::Value(
                    antigravity_->resolveProjectIdForAccount(account, false).c_str(), a), a);
        doc.AddMember("userAgent", rapidjson::Value("antigravity", a), a);
        doc.AddMember("requestId", rapidjson::Value(generateRequestId().c_str(), a), a);

        rapidjson::Value request(rapidjson::kObjectType);
        request.AddMember("model", rapidjson::Value(kSearchModel, a), a);
        request.AddMember("sessionId", rapidjson::Value(generateSessionId().c_str(), a), a);

        rapidjson::Value systemInstruction(rapidjson::kObjectType);
        rapidjson::Value sysParts(rapidjson::kArrayType);
        rapidjson::Value sysPart(rapidjson::kObjectType);
        sysPart.AddMember("text", rapidjson::Value(kSearchSystemInstruction, a), a);
        sysParts.PushBack(sysPart, a);
        systemInstruction.AddMember("parts", sysParts, a);
        request.AddMember("systemInstruction", systemInstruction, a);

        rapidjson::Value contents(rapidjson::kArrayType);
        rapidjson::Value contentTurn(rapidjson::kObjectType);
        contentTurn.AddMember("role", rapidjson::Value("user", a), a);
        rapidjson::Value contentParts(rapidjson::kArrayType);
        rapidjson::Value contentPart(rapidjson::kObjectType);
        contentPart.AddMember("text", rapidjson::Value(prompt.c_str(), a), a);
        contentParts.PushBack(contentPart, a);
        contentTurn.AddMember("parts", contentParts, a);
        contents.PushBack(contentTurn, a);
        request.AddMember("contents", contents, a);

        rapidjson::Value tools(rapidjson::kArrayType);
        rapidjson::Value googleSearchTool(rapidjson::kObjectType);
        rapidjson::Value googleSearchObj(rapidjson::kObjectType);
        googleSearchTool.AddMember("googleSearch", googleSearchObj, a);
        tools.PushBack(googleSearchTool, a);

        if (!urls.empty()) {
            rapidjson::Value urlContextTool(rapidjson::kObjectType);
            rapidjson::Value urlContextObj(rapidjson::kObjectType);
            urlContextTool.AddMember("urlContext", urlContextObj, a);
            tools.PushBack(urlContextTool, a);
        }
        request.AddMember("tools", tools, a);

        rapidjson::Value genConfig(rapidjson::kObjectType);
        genConfig.AddMember("temperature", 0.0, a);
        genConfig.AddMember("topP", 1.0, a);
        request.AddMember("generationConfig", genConfig, a);

        doc.AddMember("request", request, a);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        utils::GCPHttpClient client(getSearchUserAgent());
        client.setBearerToken(account.accessToken);
        client.setContentType("application/json");

        auto response = client.post(url, buffer.GetString(), kSearchTimeoutSeconds);

        if (response.code == 200) {
            SearchResult result = parseSearchResponse(response.body);
            if (!result.error.has_value()) {
                result.formatted_text = formatSearchResult(result);
                return result;
            }

            if (!isRetryableParsedError(*result.error) &&
                !isRetryableBodyError(response.body)) {
                return result;
            }

            lastRetryableError = *result.error;
        } else if (response.code == 0) {
            lastRetryableError = "Search request failed: " + response.error;
        } else if (isRetryableHttpCode(response.code) || isRetryableBodyError(response.body)) {
            lastRetryableError = "Search API returned HTTP " + std::to_string(response.code) +
                                 ": " + response.body;
        } else {
            errorResult.error = "Search API returned HTTP " + std::to_string(response.code) +
                               ": " + response.body;
            return errorResult;
        }

        markAccountTemporarilyRateLimited(antigravity_, account,
                                          kRetryAccountBackoffSeconds + attempt * 5);

        if (attempt + 1 < maxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(computeRetryDelayMs(attempt)));
        }
    }

    errorResult.error = "Search retries exhausted. " +
                       (lastRetryableError.empty() ? "No successful search response received."
                                                    : lastRetryableError);
    return errorResult;
}

} // namespace firmius::provider
