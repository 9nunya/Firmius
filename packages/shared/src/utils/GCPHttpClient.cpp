#include "utils/GCPHttpClient.hpp"
#include <iostream>
#include <atomic>
#include <thread>
#include <future>

namespace firmius::utils {

GCPHttpClient::GCPHttpClient(std::string userAgent) : userAgent_(std::move(userAgent)) {}

GCPHttpClient::~GCPHttpClient() {}

void GCPHttpClient::setBearerToken(const std::string& token) {
    bearerToken_ = token;
}

void GCPHttpClient::setContentType(const std::string& type) {
    contentType_ = type;
}

void GCPHttpClient::addHeader(const std::string& name, const std::string& value) {
    customHeaders_[name] = value;
}

void GCPHttpClient::clearHeaders() {
    customHeaders_.clear();
    bearerToken_.reset();
}

struct curl_slist* GCPHttpClient::prepareHeaders() {
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Content-Type: " + contentType_).c_str());
    if (bearerToken_) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + *bearerToken_).c_str());
    }
    bool hasCustomUserAgent = false;
    for (const auto& [name, value] : customHeaders_) {
        std::string lowerName = name;
        for (auto& c : lowerName)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (lowerName == "user-agent") {
            hasCustomUserAgent = true;
            break;
        }
    }
    if (!hasCustomUserAgent) {
        headers = curl_slist_append(headers, ("User-Agent: " + userAgent_).c_str());
    }
    
    for (const auto& [name, value] : customHeaders_) {
        headers = curl_slist_append(headers, (name + ": " + value).c_str());
    }
    return headers;
}

size_t GCPHttpClient::stringWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

/**
 * @brief Abort callback for CURL - called periodically during transfers.
 * @param clientp Pointer to std::atomic<bool>* abort signal
 * @return 1 to abort, 0 to continue
 */
static int abortCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* abortSignal = static_cast<std::atomic<bool>*>(clientp);
    return (abortSignal && abortSignal->load()) ? 1 : 0;
}

GCPHttpClient::Response GCPHttpClient::post(const std::string& url, const std::string& body, int timeoutSeconds) {
    CURL* curl = curl_easy_init();
    if (!curl) return {0, "", "Failed to initialize CURL"};

    Response resp;
    struct curl_slist* headers = prepareHeaders();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stringWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.error = curl_easy_strerror(res);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

GCPHttpClient::Response GCPHttpClient::get(const std::string& url, int timeoutSeconds) {
    CURL* curl = curl_easy_init();
    if (!curl) return {0, "", "Failed to initialize CURL"};

    Response resp;
    struct curl_slist* headers = prepareHeaders();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stringWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.error = curl_easy_strerror(res);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

GCPHttpClient::Response GCPHttpClient::streamPost(const std::string& url,
                                                const std::string& body,
                                                size_t (*writeCallback)(char*, size_t, size_t, void*),
                                                void* userdata,
                                                int timeoutSeconds,
                                                std::atomic<bool>* abortSignal) {
    // If no abort signal, use simple synchronous call
    if (!abortSignal) {
        CURL* curl = curl_easy_init();
        if (!curl) return {0, "", "Failed to initialize CURL"};

        Response resp;
        struct curl_slist* headers = prepareHeaders();

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, userdata);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            resp.error = curl_easy_strerror(res);
        } else {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.code);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return resp;
    }

    // Run CURL in a separate thread for immediate interrupt support
    struct ThreadData {
        CURL* curl = nullptr;
        struct curl_slist* headers = nullptr;
        std::promise<Response> promise;
        std::atomic<bool>* abortSignal;
        int timeoutSeconds;
    };

    auto threadFunc = [](ThreadData* data) {
        Response resp;
        
        // Check if already aborted before starting
        if (data->abortSignal && data->abortSignal->load()) {
            resp.error = "Request aborted before start";
            resp.code = 0;
            data->promise.set_value(resp);
            if (data->headers) curl_slist_free_all(data->headers);
            if (data->curl) curl_easy_cleanup(data->curl);
            delete data;
            return;
        }

        // Set up abort callback with periodic checking
        std::atomic<bool> localAbort{false};
        auto checkAbortCallback = [](void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            auto* localFlag = static_cast<std::atomic<bool>*>(clientp);
            return localFlag && localFlag->load() ? 1 : 0;
        };

        curl_easy_setopt(data->curl, CURLOPT_XFERINFOFUNCTION, checkAbortCallback);
        curl_easy_setopt(data->curl, CURLOPT_XFERINFODATA, &localAbort);
        curl_easy_setopt(data->curl, CURLOPT_NOPROGRESS, 0L);

        // Monitor abort signal in a separate thread
        std::thread monitorThread([data, &localAbort]() {
            while (!localAbort.load()) {
                if (data->abortSignal && data->abortSignal->load()) {
                    localAbort.store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

        CURLcode res = curl_easy_perform(data->curl);
        localAbort.store(true); // Stop monitor thread
        monitorThread.join();

        if (res != CURLE_OK) {
            // Check if it was due to abort
            if (data->abortSignal && data->abortSignal->load()) {
                resp.error = "Request interrupted by user";
            } else {
                resp.error = curl_easy_strerror(res);
            }
        } else {
            curl_easy_getinfo(data->curl, CURLINFO_RESPONSE_CODE, &resp.code);
        }

        data->promise.set_value(resp);
        if (data->headers) curl_slist_free_all(data->headers);
        if (data->curl) curl_easy_cleanup(data->curl);
        delete data;
    };

    ThreadData* data = new ThreadData();
    data->curl = curl_easy_init();
    if (!data->curl) {
        Response resp{0, "", "Failed to initialize CURL"};
        delete data;
        return resp;
    }

    data->headers = prepareHeaders();
    data->abortSignal = abortSignal;
    data->timeoutSeconds = timeoutSeconds;

    curl_easy_setopt(data->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(data->curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(data->curl, CURLOPT_HTTPHEADER, data->headers);
    curl_easy_setopt(data->curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(data->curl, CURLOPT_WRITEDATA, userdata);
    curl_easy_setopt(data->curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));
    curl_easy_setopt(data->curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(data->curl, CURLOPT_TCP_KEEPALIVE, 1L);

    std::thread curlThread(threadFunc, data);
    curlThread.detach();

    // Wait for result
    auto future = data->promise.get_future();
    future.wait();
    return future.get();
}

} // namespace firmius::utils
