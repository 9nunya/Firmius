#include "utils/GCPHttpClient.hpp"
#include <iostream>
#include <atomic>
#include <thread>
#include <future>
#include <cstdio>
#include <map>
#include <algorithm>
#include <mutex>

namespace firmius::utils {

namespace {
struct CurlRuntime;
void curlShareLock(CURL*, curl_lock_data, curl_lock_access, void* userdata);
void curlShareUnlock(CURL*, curl_lock_data, void* userdata);

struct CurlRuntime {
    std::mutex shareMutex;
    CURLSH* share = nullptr;
    bool globalInitialized = false;

    CurlRuntime() {
        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            return;
        }
        globalInitialized = true;
        share = curl_share_init();
        if (!share) {
            return;
        }

        curl_share_setopt(share, CURLSHOPT_LOCKFUNC, curlShareLock);
        curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, curlShareUnlock);
        curl_share_setopt(share, CURLSHOPT_USERDATA, this);
        curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }

    ~CurlRuntime() {
        if (share) {
            curl_share_cleanup(share);
            share = nullptr;
        }
        if (globalInitialized) {
            curl_global_cleanup();
        }
    }

    CurlRuntime(const CurlRuntime&) = delete;
    CurlRuntime& operator=(const CurlRuntime&) = delete;
};

void curlShareLock(CURL*, curl_lock_data, curl_lock_access, void* userdata) {
    static_cast<CurlRuntime*>(userdata)->shareMutex.lock();
}

void curlShareUnlock(CURL*, curl_lock_data, void* userdata) {
    static_cast<CurlRuntime*>(userdata)->shareMutex.unlock();
}

CurlRuntime& curlRuntime() {
    static CurlRuntime runtime;
    return runtime;
}
} // namespace

GCPHttpClient::GCPHttpClient(std::string userAgent) : userAgent_(std::move(userAgent)) {
    (void)curlRuntime();
}

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

static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(buffer, size * nitems);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        
        name.erase(0, name.find_first_not_of(" \t\r\n"));
        name.erase(name.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        
        for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        
        (*headers)[name] = value;
    }
    return size * nitems;
}

namespace {

firmius::utils::GCPHttpClient::Response performInterruptibleTransfer(
    CURL* curl,
    struct curl_slist* headers,
    const std::string& url,
    const std::string& body,
    size_t (*writeCallback)(char*, size_t, size_t, void*),
    void* userdata,
    int timeoutSeconds,
    std::atomic<bool>* abortSignal,
    std::map<std::string, std::string>* responseHeaders) {
    firmius::utils::GCPHttpClient::Response resp;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, userdata);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, responseHeaders);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);

    FILE* devnull = fopen("/dev/null", "w");
    if (devnull) {
        curl_easy_setopt(curl, CURLOPT_STDERR, devnull);
    }

    CURLM* multi = curl_multi_init();
    if (!multi) {
        if (devnull) {
            fclose(devnull);
        }
        resp.error = "Failed to initialize CURL multi handle";
        return resp;
    }

    CURLMcode multiCode = curl_multi_add_handle(multi, curl);
    if (multiCode != CURLM_OK) {
        curl_multi_cleanup(multi);
        if (devnull) {
            fclose(devnull);
        }
        resp.error = "Failed to add CURL handle to multi interface";
        return resp;
    }

    int stillRunning = 0;
    multiCode = curl_multi_perform(multi, &stillRunning);
    CURLcode resultCode = CURLE_OK;
    if (multiCode != CURLM_OK) {
        resultCode = CURLE_RECV_ERROR;
    }

    while (resultCode == CURLE_OK && stillRunning > 0) {
        if (abortSignal && abortSignal->load()) {
            resultCode = CURLE_ABORTED_BY_CALLBACK;
            break;
        }

        int numFds = 0;
        multiCode = curl_multi_wait(multi, nullptr, 0, 20, &numFds);
        if (multiCode != CURLM_OK) {
            resultCode = CURLE_RECV_ERROR;
            break;
        }

        if (abortSignal && abortSignal->load()) {
            resultCode = CURLE_ABORTED_BY_CALLBACK;
            break;
        }

        multiCode = curl_multi_perform(multi, &stillRunning);
        if (multiCode != CURLM_OK) {
            resultCode = CURLE_RECV_ERROR;
            break;
        }
    }

    if (resultCode == CURLE_OK) {
        int messagesLeft = 0;
        while (CURLMsg* msg = curl_multi_info_read(multi, &messagesLeft)) {
            if (msg->msg == CURLMSG_DONE) {
                resultCode = msg->data.result;
            }
        }
    }

    if (resultCode != CURLE_OK) {
        if (abortSignal && abortSignal->load()) {
            resp.error = "Request interrupted by user";
        } else {
            resp.error = curl_easy_strerror(resultCode);
        }
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.code);
    }

    if (responseHeaders) {
        resp.headers = *responseHeaders;
    }

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    if (devnull) {
        fclose(devnull);
    }
    return resp;
}

} // namespace

GCPHttpClient::Response GCPHttpClient::post(const std::string& url, const std::string& body, int timeoutSeconds) {
    CURL* curl = curl_easy_init();
    if (!curl) return {0, "", "Failed to initialize CURL", {}};
    if (curlRuntime().share) curl_easy_setopt(curl, CURLOPT_SHARE, curlRuntime().share);

    Response resp;
    struct curl_slist* headers = prepareHeaders();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stringWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
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
    if (!curl) return {0, "", "Failed to initialize CURL", {}};
    if (curlRuntime().share) curl_easy_setopt(curl, CURLOPT_SHARE, curlRuntime().share);

    Response resp;
    struct curl_slist* headers = prepareHeaders();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stringWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
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
    CURL* curl = curl_easy_init();
    if (!curl) {
        return {0, "", "Failed to initialize CURL", {}};
    }
    if (curlRuntime().share) curl_easy_setopt(curl, CURLOPT_SHARE, curlRuntime().share);

    std::map<std::string, std::string> responseHeaders;
    struct curl_slist* headers = prepareHeaders();
    Response resp = performInterruptibleTransfer(
        curl, headers, url, body, writeCallback, userdata, timeoutSeconds,
        abortSignal, &responseHeaders);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

} // namespace firmius::utils
