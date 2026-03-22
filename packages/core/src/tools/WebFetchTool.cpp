#include "tools/WebFetchTool.hpp"
#include "IAgent.hpp"
#include "utils/StringUtil.hpp"
#include <curl/curl.h>
#include <rapidjson/document.h>
#include <vector>

namespace firmius::core {

namespace {
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

int progressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    auto* ctx = static_cast<shared::ToolContext*>(clientp);
    if (ctx && ctx->cancelRequested()) {
        return 1;
    }
    return 0;
}
}

shared::ToolResult WebFetchTool::execute(const WebFetchInput& input, shared::ToolContext& ctx) {
    CURL* curl = curl_easy_init();
    if (!curl) return shared::ToolResult::fail("CURL init failed");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, input.url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Firmius/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return shared::ToolResult::fail(std::string("Fetch failed: ") + curl_easy_strerror(res));
    }

    if (httpCode >= 400) {
        return shared::ToolResult::fail("HTTP Error: " + std::to_string(httpCode));
    }

    std::string markdown = shared::StringUtil::htmlToMarkdown(response);
    size_t size = markdown.size();

    rapidjson::Document doc;
    doc.SetObject();
    auto& a = doc.GetAllocator();
    doc.AddMember("size", static_cast<uint64_t>(size), a);

    if (size > 100000) {
        std::string fileName = "/tmp/firmius_fetch_" + shared::StringUtil::generateUuid() + ".md";
        std::vector<uint8_t> data(markdown.begin(), markdown.end());
        ctx.agent.getPermissions()->validatePathAccess(
            fileName, shared::AccessMode::WRITE);
        ctx.host.writeFile(fileName, data);
        doc.AddMember("redirected_to", rapidjson::Value(fileName.c_str(), a).Move(), a);
        doc.AddMember("content", "Content too large, saved to file.", a);
        doc.AddMember("instruction", "The content was too large for a single turn. Please use 'file_read' with 'start_line' and 'end_line' to inspect specific sections of the file, or 'grep' to search for keywords within it.", a);
    } else {
        doc.AddMember("content", rapidjson::Value(markdown.c_str(), a).Move(), a);
    }

    return shared::ToolResult::ok(doc);
}

}
