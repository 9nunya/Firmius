#include "utils/GCPHttpClient.hpp"
#include <iostream>

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
    headers = curl_slist_append(headers, ("User-Agent: " + userAgent_).c_str());
    
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
                                                int timeoutSeconds) {
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

} // namespace firmius::utils
