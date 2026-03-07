#include "hosts/DockerHost.hpp"
#include "utils/StringUtil.hpp"
#include <cstdlib>
#include <curl/easy.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <iostream>
#include <chrono>
#include <thread>
#include <optional>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <iomanip>
#include <sstream>

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

int abortCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* flag = static_cast<std::atomic<bool>*>(clientp);
    return flag->load() ? 1 : 0;
}

/**
 * @brief Parses the Docker multiplexing stream format.
 */
[[maybe_unused]] void parseDockerStream(const std::string& raw, std::string& stdoutData, std::string& stderrData) {
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
        ProcessResult res;
        res.exitCode = exitCode;
        res.stdoutData = stdoutBuffer;
        res.stderrData = stderrBuffer;
        res.durationMs = duration;
        res.finishReason = shared::ProcessFinishReason::Natural;
        return res;
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

    void kill() override {
        killed = true;
    }

    void write(const std::string& data) override {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stdinQueue.push(data);
        }
        if (activeCurl) {
            curl_easy_pause(activeCurl, CURLPAUSE_CONT);
        }
    }

    bool isRunning() override {
        return !finished;
    }

    std::string getSystemId() const override {
        return execId;
    }

private:
    static size_t readCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<DockerHostProcess*>(userdata);
        std::lock_guard<std::mutex> lock(self->queueMutex);
        if (self->stdinQueue.empty()) {
            return CURL_READFUNC_PAUSE;
        }
        std::string& front = self->stdinQueue.front();
        size_t toCopy = std::min(size * nmemb, front.size());
        memcpy(ptr, front.data(), toCopy);
        if (toCopy == front.size()) {
            self->stdinQueue.pop();
        } else {
            front.erase(0, toCopy);
        }
        return toCopy;
    }

    void streamLoop() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            finished = true;
            return;
        }
        activeCurl = curl;

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
        
        // Stdin support - only enable upload if we actually have data to send
        // If AttachStdin is true, we should handle the stream properly
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, readCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, this);
        // Do NOT use CURLOPT_UPLOAD here as it forces a PUT/upload-style POST
        // Docker expects a streaming POST body for stdin.
        // We'll use CHUNKED encoding if we want to stream data later.

        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &killed);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);

        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        activeCurl = nullptr;

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
    std::atomic<bool> killed{false};
    std::atomic<CURL*> activeCurl{nullptr};
    std::queue<std::string> stdinQueue;
    std::mutex queueMutex;
    int exitCode = -1;
    std::chrono::steady_clock::time_point startTime;
};

DockerHost::DockerHost(const std::string& id) {
    if (id.empty()) {
        containerId = "firmius-sandbox-" + StringUtil::generateUuid();
    } else {
        containerId = id;
    }
    curl = curl_easy_init();
    if (!curl) throw std::runtime_error("CURL init failed");
}

DockerHost::~DockerHost() {
    cleanup();
    curl_easy_cleanup(curl);
}

std::string DockerHost::init() {
    containerId = "firmius-sandbox-" + StringUtil::generateUuid();
    
    std::string createCommand = "docker create --name '"
        + containerId + "' firmius-sandbox:latest tail -f /dev/null > /dev/null 2>&1";
    int createResult = system(createCommand.c_str());
    if (createResult != 0) {
        throw std::runtime_error("Failed to create Docker container: " + containerId);
    }
    
    std::string startCommand = "docker start '" + containerId + "' > /dev/null 2>&1";
    int startResult = system(startCommand.c_str());
    if (startResult != 0) {
        throw std::runtime_error("Failed to start Docker container: " + containerId);
    }
    
    containerIds.push_back(containerId);
    return containerId;
}
void DockerHost::destroy() {}

void DockerHost::cleanup() {
    for (const auto& id : containerIds) {
        try {
            request("DELETE", "/containers/" + id + "?force=true", "");
        } catch (...) {}
    }
    containerIds.clear();
    
    std::lock_guard<std::mutex> lock(bgMutex);
    for (auto& [id, proc] : backgroundProcesses) {
        if (proc) proc->kill();
    }
    backgroundProcesses.clear();
}

void DockerHost::setUser(const std::string& user) {
    currentUser = user;
}

std::string DockerHost::request(const std::string& method, const std::string& url, const std::string& body) {
    std::lock_guard<std::mutex> lock(requestMutex);
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
    auto res = exec("cat -- " + StringUtil::shellEscape(path));
    if (res.exitCode != 0) throw std::runtime_error("Failed to read file in Docker: " + res.stderrData);
    return std::vector<uint8_t>(res.stdoutData.begin(), res.stdoutData.end());
}

void DockerHost::writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::string b64 = base64_encode(data);
    auto parentPath = path.substr(0, path.rfind('/'));
    std::string command = "mkdir -p " + StringUtil::shellEscape(parentPath)
        + " && printf '%s' " + StringUtil::shellEscape(b64)
        + " | base64 -d > " + StringUtil::shellEscape(path);
    auto res = exec(command);
    if (res.exitCode != 0) {
        throw std::runtime_error("Failed to write file in Docker: " + res.stderrData);
    }
}

bool DockerHost::exists(const std::string& path) {
    auto res = exec("test -e " + StringUtil::shellEscape(path));
    return res.exitCode == 0;
}

std::vector<FileInfo> DockerHost::listDir(const std::string& path) {
    auto res = exec("find " + StringUtil::shellEscape(path)
        + " -maxdepth 1 -mindepth 1 -printf '%f\\t%s\\t%Y\\t%T@\\n'");
    if (res.exitCode != 0) {
        throw std::runtime_error("listDir failed in Docker: " + res.stderrData);
    }
    std::vector<FileInfo> entries;
    std::istringstream stream(res.stdoutData);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        auto parts = StringUtil::split(line, '\t');
        if (parts.size() < 4) continue;
        FileInfo info;
        info.name = parts[0];
        // Construct full path — path + "/" + name
        info.path = path;
        if (!info.path.empty() && info.path.back() != '/') info.path += '/';
        info.path += info.name;
        info.size = std::stoull(parts[1]);
        // find -printf %Y: f=file, d=directory, l=symlink
        info.isDirectory = (parts[2] == "d");
        info.isSymlink = (parts[2] == "l");
        // %T@ is seconds.fractional since epoch
        double epochSec = std::stod(parts[3]);
        info.modifiedMs = static_cast<int64_t>(epochSec * 1000.0);
        entries.push_back(std::move(info));
    }
    return entries;
}

FileInfo DockerHost::stat(const std::string& path) {
    auto res = exec("stat -c '%n\\t%s\\t%F\\t%Y' " + StringUtil::shellEscape(path));
    if (res.exitCode != 0) {
        throw std::runtime_error("stat failed in Docker: " + res.stderrData);
    }
    std::string output = StringUtil::trim(res.stdoutData);
    auto parts = StringUtil::split(output, '\t');
    if (parts.size() < 4) {
        throw std::runtime_error("Unexpected stat output: " + output);
    }
    FileInfo info;
    info.path = parts[0];
    auto slashPos = info.path.rfind('/');
    info.name = (slashPos != std::string::npos) ? info.path.substr(slashPos + 1) : info.path;
    info.size = std::stoull(parts[1]);
    // stat -c %F: "regular file", "directory", "symbolic link", etc.
    info.isDirectory = (parts[2] == "directory");
    info.isSymlink = (parts[2] == "symbolic link");
    info.modifiedMs = std::stoll(parts[3]) * 1000;
    return info;
}

ProcessResult DockerHost::exec(const std::string& command, const std::string& cwd, const std::map<std::string, std::string>& env, std::optional<std::chrono::milliseconds> timeout) {
    if (!timeout.has_value()) {
        auto proc = spawn(command, cwd, env);
        auto res = proc->wait();
        res.finishReason = ProcessFinishReason::Natural;
        return res;
    }
    
    auto proc = spawn(command, cwd, env);
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + *timeout;
    
    while (true) {
        if (!proc->isRunning()) {
            auto res = proc->wait();
            res.finishReason = ProcessFinishReason::Natural;
            return res;
        }
        
        auto snapshot = proc->inspect();
        auto now = std::chrono::steady_clock::now();
        
        if (now >= deadline) {
            auto elapsed = std::chrono::duration<double, std::milli>(now - start).count();
            std::string bgId = StringUtil::generateUuid();
            registerBackgroundProcess(bgId, std::move(proc));
            
            ProcessResult partial;
            partial.exitCode = -1;
            partial.stdoutData = snapshot.stdoutData;
            partial.stderrData = snapshot.stderrData;
            partial.durationMs = elapsed;
            partial.finishReason = ProcessFinishReason::Timeout;
            partial.backgroundProcessId = bgId;
            
            return partial;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
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
    d.AddMember("AttachStdin", false, a);
    d.AddMember("AttachStdout", true, a);
    d.AddMember("AttachStderr", true, a);
    if (!cwd.empty()) d.AddMember("WorkingDir", rapidjson::Value(cwd.c_str(), a), a);

    rapidjson::Value envArray(rapidjson::kArrayType);
    for (const auto& [k, v] : env) {
        envArray.PushBack(rapidjson::Value((k + "=" + v).c_str(), a), a);
    }
    d.AddMember("Env", envArray, a);

    if (!currentUser.empty() && currentUser != "root") {
        d.AddMember("User", rapidjson::Value(currentUser.c_str(), a), a);
    }

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

void DockerHost::registerBackgroundProcess(const std::string& id, std::unique_ptr<IHostProcess> proc) {
    std::lock_guard<std::mutex> lock(bgMutex);
    backgroundProcesses[id] = std::move(proc);
}

ProcessSnapshot DockerHost::inspectBackgroundProcess(const std::string& id) {
    std::lock_guard<std::mutex> lock(bgMutex);
    auto it = backgroundProcesses.find(id);
    if (it == backgroundProcesses.end()) {
        throw std::runtime_error("Background process not found: " + id);
    }
    auto snapshot = it->second->inspect();
    if (!snapshot.running) {
        backgroundProcesses.erase(it);
    }
    return snapshot;
}

void DockerHost::writeToBackgroundProcess(const std::string& id, const std::string& data) {
    std::lock_guard<std::mutex> lock(bgMutex);
    auto it = backgroundProcesses.find(id);
    if (it == backgroundProcesses.end()) {
        throw std::runtime_error("Background process not found: " + id);
    }
    it->second->write(data);
}

void DockerHost::killBackgroundProcess(const std::string& id) {
    std::lock_guard<std::mutex> lock(bgMutex);
    auto it = backgroundProcesses.find(id);
    if (it != backgroundProcesses.end()) {
        it->second->kill();
    } else {
        throw std::runtime_error("Background process not found: " + id);
    }
}

std::vector<ContainerInfo> DockerHost::listContainersWithLabel(const std::string& label) {
    std::vector<ContainerInfo> result;

    // Initialize libcurl for this static method
    CURL* curl = curl_easy_init();
    if (!curl) {
        return result;
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, "/var/run/docker.sock");
    curl_easy_setopt(curl, CURLOPT_URL, "http://localhost/containers/json?all=true");
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return result;
    }

    // Parse response JSON
    rapidjson::Document doc;
    doc.Parse(response.c_str());
    if (!doc.IsArray()) {
        return result;
    }

    for (auto& containerObj : doc.GetArray()) {
        if (!containerObj.IsObject()) continue;

        ContainerInfo info;
        if (containerObj.HasMember("Id") && containerObj["Id"].IsString()) {
            info.id = containerObj["Id"].GetString();
        }

        // Extract labels
        if (containerObj.HasMember("Labels") && containerObj["Labels"].IsObject()) {
            const auto& labelsObj = containerObj["Labels"];
            for (auto it = labelsObj.MemberBegin(); it != labelsObj.MemberEnd(); ++it) {
                if (it->name.IsString() && it->value.IsString()) {
                    info.labels[it->name.GetString()] = it->value.GetString();
                }
            }
        }

        // Only include if the container has the specified label
        if (info.labels.count(label)) {
            result.push_back(std::move(info));
        }
    }

    return result;
}

} // namespace firmius::core
