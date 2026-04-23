#ifndef FIRMIUS_CORE_LSP_JSON_RPC_TRANSPORT_HPP
#define FIRMIUS_CORE_LSP_JSON_RPC_TRANSPORT_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <unordered_map>

namespace firmius::core {

class JsonRpcTransport {
public:
    using RequestHandler = std::function<void(const rapidjson::Document& request)>;
    using NotificationHandler = std::function<void(const rapidjson::Document& notification)>;
    using Reader = std::function<std::string(std::chrono::milliseconds timeout)>;
    using Writer = std::function<bool(const std::string& data)>;
    using WakeStop = std::function<void()>;

    JsonRpcTransport(Writer writer, Reader reader, WakeStop wakeStop = {});
    ~JsonRpcTransport();

    JsonRpcTransport(const JsonRpcTransport&) = delete;
    JsonRpcTransport& operator=(const JsonRpcTransport&) = delete;
    JsonRpcTransport(JsonRpcTransport&&) = delete;
    JsonRpcTransport& operator=(JsonRpcTransport&&) = delete;

    void start();
    void stop();
    bool isRunning() const;

    static Reader makeFdReader(int fd);
    static Writer makeFdWriter(int fd);

    rapidjson::Document sendRequest(const std::string& method, rapidjson::Value& params, int timeoutMs = 30000);
    void sendNotification(const std::string& method, rapidjson::Value& params);
    void sendResponse(const rapidjson::Value& id, rapidjson::Value& result);
    void sendError(const rapidjson::Value& id, int code, const std::string& message);

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

    Writer m_writer;
    Reader m_reader;
    WakeStop m_wakeStop;
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
