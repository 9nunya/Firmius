#include "tools/WebFetchTool.hpp"
#include "IAgent.hpp"
#include "ITool.hpp"
#include "environment/PermissionSuggestionEngine.hpp"
#include "harness/Harness.hpp"
#include "utils/SpillIfLarge.hpp"
#include "utils/StringUtil.hpp"
#include <curl/curl.h>
#include <sstream>
#include <curl/easy.h>
#include <rapidjson/document.h>
#include <regex>
#include <vector>

namespace firmius::core {

namespace {
size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  if (!userdata) return 0;
  auto *s = static_cast<std::string *>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

/// Pull out scheme + host from a URL. Cheap regex; doesn't need full
/// RFC 3986. Sets host="" / scheme="" on parse failure — caller
/// handles fallback gracefully.
void parseUrl(const std::string &url, std::string &scheme, std::string &host) {
  static const std::regex pattern(R"(^([a-zA-Z][a-zA-Z0-9+.-]*)://([^/:?#]+))");
  std::smatch m;
  if (std::regex_search(url, m, pattern)) {
    scheme = m[1];
    host = m[2];
  }
}

/// Throw if policy says no. Triggers the escalation flow on Ask.
void gateNetworkFetch(shared::ToolContext &ctx, const std::string &url) {
  PolicyRequest req;
  req.category = kCatNetworkFetch;
  req.url = url;
  parseUrl(url, req.scheme, req.host);
  req.toolName = "WebFetch";

  auto eval = Harness::instance().policyEngine().evaluate(req);
  if (eval.decision == PolicyDecision::Allow) return;
  if (eval.decision == PolicyDecision::Deny) {
    throw std::runtime_error("Network fetch denied by policy: " + url);
  }
  // Ask: build escalation + suggestions.
  shared::PermissionEscalationRequest esc;
  const auto &actx = ctx.agent.getContext();
  esc.threadId = actx.history ? actx.history->threadId : "";
  esc.agentId = actx.identity.id;
  esc.toolName = req.toolName;
  esc.requestType = shared::PermissionRequestType::Read;
  esc.title = "Allow network fetch?";
  esc.message = "Approve fetching " + url;
  esc.severity = shared::CommandSeverity::LOW;
  esc.allowAlways = true;
  esc.category = req.category;
  esc.url = req.url;
  esc.host = req.host;
  esc.scheme = req.scheme;

  shared::CommandIntent dummy;
  auto suggestions = PermissionSuggestionEngine::generate(req, dummy);
  auto response = Harness::instance().requestPermissionEscalationWithSuggestions(
      std::move(esc), std::move(suggestions));
  if (response == shared::PermissionResponse::Deny) {
    throw std::runtime_error("Network fetch denied: " + url);
  }
}
} // namespace

shared::ToolResult WebFetchTool::execute(const WebFetchInput &input,
                                         shared::ToolContext &ctx) {
  gateNetworkFetch(ctx, input.url);

  CURL *curl = curl_easy_init();
  if (!curl)
    return shared::ToolResult::fail("CURL init failed");

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, input.url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Firmius/1.0");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

  CURLcode res = curl_easy_perform(curl);

  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    return shared::ToolResult::fail(std::string("Fetch failed: ") +
                                    curl_easy_strerror(res));
  }

  if (httpCode >= 400) {
    return shared::ToolResult::fail("HTTP Error: " + std::to_string(httpCode));
  }
  // Convert HTML to a safe text representation. std::regex-based htmlToMarkdown
  // causes stack overflow on complex HTML (e.g. GitHub pages) due to
  // catastrophic backtracking. For responses above a safe threshold, use a
  // simple non-regex tag stripper instead.
  constexpr size_t kSafeHtmlThreshold = 50000;
  std::string markdown;
  if (response.size() > kSafeHtmlThreshold) {
    // Fast non-regex HTML tag stripping for large documents.
    std::string stripped;
    stripped.reserve(response.size());
    bool inTag = false;
    for (char c : response) {
      if (c == '<') {
        inTag = true;
      } else if (c == '>') {
        inTag = false;
      } else if (!inTag) {
        stripped.push_back(c);
      }
    }
    // Collapse whitespace.
    markdown.reserve(stripped.size());
    bool prevWasSpace = false;
    for (char c : stripped) {
      bool isSpace = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
      if (isSpace) {
        if (!prevWasSpace) {
          markdown.push_back(' ');
        }
        prevWasSpace = true;
      } else {
        markdown.push_back(c);
        prevWasSpace = false;
      }
    }
  } else {
    markdown = shared::StringUtil::htmlToMarkdown(response);
  }

  // Token-waste pass 4: route via the unified spillIfLarge helper.
  // Threshold lowered from 100 KB to 32 KB — 100 KB of markdown is
  // ~25k tokens, which dominates context for content the model rarely
  // needs in full (it usually wants to grep/skim the doc).
  constexpr std::size_t kFetchSpillThreshold = 32 * 1024;
  constexpr std::size_t kFetchTailBytes = 4 * 1024;
  auto spill = shared::utils::spillIfLarge(markdown, kFetchSpillThreshold,
                                           "firmius_fetch", kFetchTailBytes);

  rapidjson::Document doc;
  doc.SetObject();
  auto &a = doc.GetAllocator();

  if (spill.spilled) {
    std::ostringstream prose;
    prose << "Fetched " << spill.totalBytes << " bytes from " << input.url
          << "; full content saved to " << spill.refPath
          << " (showing last " << spill.tail.size() << " B).";
    const std::string proseStr = prose.str();
    doc.AddMember(
        "result",
        rapidjson::Value(proseStr.c_str(),
                         static_cast<rapidjson::SizeType>(proseStr.size()),
                         a).Move(),
        a);
    doc.AddMember(
        "tail",
        rapidjson::Value(spill.tail.c_str(),
                         static_cast<rapidjson::SizeType>(spill.tail.size()),
                         a).Move(),
        a);
    doc.AddMember("ref",
                  rapidjson::Value(spill.refPath.c_str(), a).Move(), a);
    doc.AddMember("size", static_cast<uint64_t>(spill.totalBytes), a);
  } else {
    doc.AddMember("content", rapidjson::Value(markdown.c_str(), a).Move(), a);
    doc.AddMember("size", static_cast<uint64_t>(markdown.size()), a);
  }
  return shared::ToolResult::ok(doc);
}

} // namespace firmius::core
