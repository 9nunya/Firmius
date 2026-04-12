#ifndef FIRMIUS_CORE_LSP_JSON_RPC_TRANSPORT_HPP
#define FIRMIUS_CORE_LSP_JSON_RPC_TRANSPORT_HPP

#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <future>
#include <atomic>
#include <chrono>
#include <rapidjson/document.h>

namespace firmius::core {

class JsonRpcTransport {
public:
    using RequestHandler = std::function<void(const rapidjson::Document& request)>;
    using NotificationHandler = std::function<void(const rapidjson::Document& notification)>;

    JsonRpcTransport(int stdinWriteFd, int stdoutReadFd);
    ~JsonRpcTransport();

    JsonRpcTransport(const JsonRpcTransport&) = delete;
    JsonRpcTransport& operator=(const JsonRpcTransport&) = delete;

    // Lifecycle
    void start();
    void stop();
    bool isRunning() const;

    // Synchronous request with timeout — throws on timeout or transport error
    rapidjson::Document sendRequest(const std::string& method, rapidjson::Value& params, int timeoutMs = 30000);
    void sendNotification(const std::string& method, rapidjson::Value& params);
    void sendResponse(const rapidjson::Value& id, rapidjson::Value& result);
    void sendError(const rapidjson::Value& id, int code, const std::string& message);

    // Handlers for server-initiated messages
    void setRequestHandler(RequestHandler handler);
    void setNotificationHandler(NotificationHandler handler);

private:
    struct PendingSlot {
        std::promise<rapidjson::Document> promise;
        std::chrono::steady_clock::time_point created;
    };

    void readerLoop();
    bool writeMessage(const rapidjson::Document& doc);
    void handleMessage(rapidjson::Document&& doc);
    void rejectAllPending(const std::string& reason);

    int m_writeFd;
    int m_readFd;
    int m_shutdownPipe[2]{-1, -1};
    std::atomic<bool> m_running{false};
    std::atomic<int> m_nextId{1};

    std::jthread m_readerThread;

    mutable std::mutex m_writeMutex;

    mutable std::mutex m_pendingMutex;
    std::unordered_map<int, PendingSlot> m_pendingRequests;

    RequestHandler m_requestHandler;
    NotificationHandler m_notificationHandler;
    mutable std::mutex m_handlerMutex;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_LSP_JSON_RPC_TRANSPORT_HPP
