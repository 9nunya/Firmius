#include "hosts/DockerHost.hpp"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <iomanip>

namespace firmius::core {

using namespace firmius::shared;

/**
 * @file DockerHost.cpp
 * @brief Implementation of the Docker sandboxed execution host.
 */

namespace {
/**
 * @brief CURL write callback for simple string responses.
 */
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

/**
 * @brief Parses the Docker multiplexing stream format.
 */
void parseDockerStream(const std::string& raw, std::string& stdoutData, std::string& stderrData) {
    if (raw.empty()) return;
    const char* p = raw.data();
    size_t remaining = raw.size();
    
    // Check if it's a multiplexed stream (at least 8 bytes and type is 1 or 2)
    if (remaining < 8 || (p[0] != 1 && p[0] != 2)) {
        stdoutData = raw;
        return;
    }

    while (remaining >= 8) {
        uint8_t type = p[0];
        uint32_t size = 0;
        size |= (uint8_t)p[4] << 24;
        size |= (uint8_t)p[5] << 16;
        size |= (uint8_t)p[6] << 8;
        size |= (uint8_t)p[7];
        
        p += 8;
        remaining -= 8;
        
        // Safety guard: if size is unrealistic or larger than remaining buffer, fallback
        if (size > remaining || size > 1024 * 1024 * 16) { 
            stdoutData.append(p, remaining);
            break;
        }
        
        if (type == 1) stdoutData.append(p, size);
        else if (type == 2) stderrData.append(p, size);
        
        p += size;
        remaining -= size;
    }
}

/**
 * @brief Context for background stream monitoring.
 */
struct StreamContext {
    std::function<void(const std::string&, bool isError)>* callback;
    std::mutex* mutex;
    std::string* stdoutBuf;
    std::string* stderrBuf;
    std::string partial;
};

/**
 * @brief CURL write callback for real-time streaming data.
 */
size_t streamingWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamContext*>(userdata);
    size_t total = size * nmemb;
    ctx->partial.append(ptr, total);

    while (ctx->partial.size() >= 8) {
        const char* p = ctx->partial.data();
        uint8_t type = p[0];
        uint32_t payloadSize = 0;
        payloadSize |= (uint8_t)p[4] << 24;
        payloadSize |= (uint8_t)p[5] << 16;
        payloadSize |= (uint8_t)p[6] << 8;
        payloadSize |= (uint8_t)p[7];

        if (payloadSize > 1024 * 1024 * 16) {
            std::lock_guard<std::mutex> lock(*ctx->mutex);
            ctx->stdoutBuf->append(ctx->partial);
            ctx->partial.clear();
            break;
        }

        if (ctx->partial.size() < 8 + payloadSize) break;

        std::string chunk(ctx->partial.data() + 8, payloadSize);
        ctx->partial.erase(0, 8 + payloadSize);

        std::lock_guard<std::mutex> lock(*ctx->mutex);
        if (type == 1) {
            ctx->stdoutBuf->append(chunk);
            if (*ctx->callback) (*ctx->callback)(chunk, false);
        } else if (type == 2) {
            ctx->stderrBuf->append(chunk);
            if (*ctx->callback) (*ctx->callback)(chunk, true);
        }
    }
    return total;
}

/**
 * @brief Encodes binary data to base64.
 */
std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

} // namespace

/**
 * @brief Handle for a process running inside a Docker container.
 */
class DockerHostProcess : public IHostProcess {
public:
    DockerHostProcess(const std::string& containerId, const std::string& execId)
        : containerId(containerId), execId(execId) {
        startTime = std::chrono::steady_clock::now();
        streamThread = std::thread(&DockerHostProcess::streamLoop, this);
    }

    ~DockerHostProcess() override {
        if (streamThread.joinable()) streamThread.join();
    }

    void onOutput(std::function<void(const std::string&, bool isError)> cb) override {
        std::lock_guard<std::mutex> lock(callbackMutex);
        callback = cb;
    }

    ProcessResult wait() override {
        if (streamThread.joinable()) streamThread.join();
        auto end = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double, std::milli>(end - startTime).count();
        return {exitCode, stdoutBuffer, stderrBuffer, duration};
    }

    ProcessSnapshot inspect() const override {
        std::lock_guard<std::mutex> lock(callbackMutex);
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(now - startTime).count();
        return {
            !finished.load(),
            exitCode,
            stdoutBuffer,
            stderrBuffer,
            elapsed
        };
    }

    void kill() override {}

    bool isRunning() override {
        return !finished;
    }

private:
    void streamLoop() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            finished = true;
            return;
        }

        std::string url = "http://localhost/v1.44/exec/" + execId + "/start";
        std::string body = R"({"Detach":false,"Tty":false})";

        struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

        StreamContext ctx{&callback, &callbackMutex, &stdoutBuffer, &stderrBuffer, ""};

        curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, "/var/run/docker.sock");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamingWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);

        curl_easy_perform(curl);
        curl_slist_free_all(headers);

        curl_easy_reset(curl);
        std::string inspectRes;
        curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, "/var/run/docker.sock");
        curl_easy_setopt(curl, CURLOPT_URL, ("http://localhost/v1.44/exec/" + execId + "/json").c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &inspectRes);
        curl_easy_perform(curl);

        rapidjson::Document inspectDoc;
        inspectDoc.Parse(inspectRes.c_str());
        if (inspectDoc.HasMember("ExitCode") && !inspectDoc["ExitCode"].IsNull()) {
            exitCode = inspectDoc["ExitCode"].GetInt();
        }

        curl_easy_cleanup(curl);
        finished = true;
    }

    std::string containerId;
    std::string execId;
    std::function<void(const std::string&, bool isError)> callback;
    mutable std::mutex callbackMutex;
    mutable std::string stdoutBuffer;
    mutable std::string stderrBuffer;
    std::thread streamThread;
    std::atomic<bool> finished{false};
    int exitCode = -1;
    std::chrono::steady_clock::time_point startTime;
};

DockerHost::DockerHost(const std::string& containerId) : containerId(containerId) {
    curl = curl_easy_init();
    if (!curl) throw std::runtime_error("CURL init failed");
}

DockerHost::~DockerHost() {
    curl_easy_cleanup(curl);
}

void DockerHost::init() {}
void DockerHost::destroy() {}

std::string DockerHost::request(const std::string& method, const std::string& url, const std::string& body) {
    std::string response;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, "/var/run/docker.sock");
    curl_easy_setopt(curl, CURLOPT_URL, ("http://localhost/v1.44" + url).c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    
    struct curl_slist* headers = nullptr;
    if (!body.empty()) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (headers) curl_slist_free_all(headers);

    if (res != CURLE_OK) throw std::runtime_error(std::string("Docker request failed: ") + curl_easy_strerror(res));
    return response;
}

std::vector<uint8_t> DockerHost::readFile(const std::string& path) {
    auto res = exec("cat " + path);
    if (res.exitCode != 0) throw std::runtime_error("Failed to read file in Docker: " + res.stderrData);
    return std::vector<uint8_t>(res.stdoutData.begin(), res.stdoutData.end());
}

void DockerHost::writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::string b64 = base64_encode(data);
    std::string command = "echo '" + b64 + "' | base64 -d > " + path;
    auto res = exec(command);
    if (res.exitCode != 0) {
        throw std::runtime_error("Failed to write file in Docker: " + res.stderrData);
    }
}

bool DockerHost::exists(const std::string& path) {
    auto res = exec("test -e " + path);
    return res.exitCode == 0;
}

ProcessResult DockerHost::exec(const std::string& command, const std::string& cwd, const std::map<std::string, std::string>& env) {
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    
    rapidjson::Value cmdArray(rapidjson::kArrayType);
    cmdArray.PushBack("sh", a);
    cmdArray.PushBack("-c", a);
    cmdArray.PushBack(rapidjson::Value(command.c_str(), a), a);
    d.AddMember("Cmd", cmdArray, a);
    d.AddMember("AttachStdout", true, a);
    d.AddMember("AttachStderr", true, a);
    if (!cwd.empty()) d.AddMember("WorkingDir", rapidjson::Value(cwd.c_str(), a), a);
    
    rapidjson::Value envArray(rapidjson::kArrayType);
    for (const auto& [k, v] : env) {
        envArray.PushBack(rapidjson::Value((k + "=" + v).c_str(), a), a);
    }
    d.AddMember("Env", envArray, a);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    auto start = std::chrono::steady_clock::now();
    std::string createRes = request("POST", "/containers/" + containerId + "/exec", buffer.GetString());
    
    rapidjson::Document resDoc;
    resDoc.Parse(createRes.c_str());
    if (!resDoc.HasMember("Id")) throw std::runtime_error("Failed to create exec: " + createRes);
    std::string execId = resDoc["Id"].GetString();

    std::string startRes = request("POST", "/exec/" + execId + "/start", R"({"Detach":false,"Tty":false})");
    
    auto end = std::chrono::steady_clock::now();
    
    ProcessResult result;
    parseDockerStream(startRes, result.stdoutData, result.stderrData);
    
    std::string inspectRes = request("GET", "/exec/" + execId + "/json");
    rapidjson::Document inspectDoc;
    inspectDoc.Parse(inspectRes.c_str());
    if (inspectDoc.HasMember("ExitCode") && !inspectDoc["ExitCode"].IsNull()) {
        result.exitCode = inspectDoc["ExitCode"].GetInt();
    }
    
    result.durationMs = std::chrono::duration<double, std::milli>(end - start).count();
    return result;
}

std::unique_ptr<IHostProcess> DockerHost::spawn(const std::string& command, const std::string& cwd, const std::map<std::string, std::string>& env) {
    rapidjson::Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    
    rapidjson::Value cmdArray(rapidjson::kArrayType);
    cmdArray.PushBack("sh", a);
    cmdArray.PushBack("-c", a);
    cmdArray.PushBack(rapidjson::Value(command.c_str(), a), a);
    d.AddMember("Cmd", cmdArray, a);
    d.AddMember("AttachStdout", true, a);
    d.AddMember("AttachStderr", true, a);
    if (!cwd.empty()) d.AddMember("WorkingDir", rapidjson::Value(cwd.c_str(), a), a);
    
    rapidjson::Value envArray(rapidjson::kArrayType);
    for (const auto& [k, v] : env) {
        envArray.PushBack(rapidjson::Value((k + "=" + v).c_str(), a), a);
    }
    d.AddMember("Env", envArray, a);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);

    std::string createRes = request("POST", "/containers/" + containerId + "/exec", buffer.GetString());
    
    rapidjson::Document resDoc;
    resDoc.Parse(createRes.c_str());
    if (!resDoc.HasMember("Id")) throw std::runtime_error("Failed to spawn exec: " + createRes);
    std::string execId = resDoc["Id"].GetString();

    return std::make_unique<DockerHostProcess>(containerId, execId);
}

} // namespace firmius::core
