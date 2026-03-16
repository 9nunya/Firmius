#pragma once

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <optional>
#include <atomic>
#include <curl/curl.h>

namespace firmius::utils {

/**
 * @brief Encapsulates CURL operations for Google Cloud Platform (GCP) and Antigravity APIs.
 */
class GCPHttpClient {
public:
    struct Response {
        long code = 0;
        std::string body;
        std::string error;
        std::map<std::string, std::string> headers;
    };

    explicit GCPHttpClient(std::string userAgent = "antigravity/1.18.3 linux/x86_64");
    ~GCPHttpClient();

    // Disable copy
    GCPHttpClient(const GCPHttpClient&) = delete;
    GCPHttpClient& operator=(const GCPHttpClient&) = delete;

    void setBearerToken(const std::string& token);
    void setContentType(const std::string& type);
    void addHeader(const std::string& name, const std::string& value);
    void clearHeaders();

    Response post(const std::string& url, const std::string& body, int timeoutSeconds = 30);
    Response get(const std::string& url, int timeoutSeconds = 30);

    /**
     * @brief Performs a streaming POST request with interrupt support.
     * @param writeCallback Standard CURL write callback
     * @param userdata Data passed to the callback
     * @param abortSignal Optional atomic bool to abort the request immediately
     * 
     * This method runs CURL in a separate thread to support immediate interruption.
     * When abortSignal is set, the CURL handle is cancelled immediately.
     */
    Response streamPost(const std::string& url,
                       const std::string& body,
                       size_t (*writeCallback)(char*, size_t, size_t, void*),
                       void* userdata,
                       int timeoutSeconds = 300,
                       std::atomic<bool>* abortSignal = nullptr);

private:
    std::string userAgent_;
    std::optional<std::string> bearerToken_;
    std::string contentType_ = "application/json";
    std::map<std::string, std::string> customHeaders_;

    struct curl_slist* prepareHeaders();
    static size_t stringWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
};

} // namespace firmius::utils
