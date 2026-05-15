#include "lsp/JsonRpcTransport.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#endif

#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>

namespace firmius::core {

JsonRpcTransport::JsonRpcTransport(Writer writer, Reader reader, WakeStop wakeStop)
    : m_writer(std::move(writer)), m_reader(std::move(reader)), m_wakeStop(std::move(wakeStop)) {
    if (!m_writer || !m_reader) {
        throw std::runtime_error("JsonRpcTransport: reader and writer are required");
    }
}

JsonRpcTransport::~JsonRpcTransport() {
    stop();
}

JsonRpcTransport::Reader JsonRpcTransport::makeFdReader(int fd) {
#if defined(_WIN32)
    (void)fd;
    throw std::runtime_error("JsonRpcTransport::makeFdReader is not supported on Windows");
#else
    return [fd](std::chrono::milliseconds timeout) -> std::string {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN | POLLHUP | POLLERR;

        const int ret = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (ret < 0) {
            if (errno == EINTR) {
                return {};
            }
            throw std::runtime_error("JsonRpcTransport: poll failed: " + std::string(std::strerror(errno)));
        }
        if (ret == 0) {
            return {};
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            throw std::runtime_error("JsonRpcTransport: poll signaled socket error");
        }
        if (!(pfd.revents & (POLLIN | POLLHUP))) {
            return {};
        }

        char chunk[8192];
        const ssize_t bytesRead = ::read(fd, chunk, sizeof(chunk));
        if (bytesRead <= 0) {
            throw std::runtime_error("JsonRpcTransport: channel closed");
        }
        return std::string(chunk, static_cast<size_t>(bytesRead));
    };
#endif
}

JsonRpcTransport::Writer JsonRpcTransport::makeFdWriter(int fd) {
    return [fd](const std::string& data) -> bool {
        const char* ptr = data.data();
        size_t remaining = data.size();
        while (remaining > 0) {
#if defined(_WIN32)
            (void)fd;
            return false;
#else
            const ssize_t written = ::write(fd, ptr, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (written == 0) {
                return false;
            }
            ptr += written;
            remaining -= static_cast<size_t>(written);
#endif
        }
        return true;
    };
}

void JsonRpcTransport::start() {
    if (m_running.exchange(true)) return;
    m_readerThread = std::jthread([this](std::stop_token) {
        readerLoop();
    });
}

void JsonRpcTransport::stop() {
    if (!m_running.exchange(false)) return;

    if (m_wakeStop) {
        m_wakeStop();
    }

    if (m_readerThread.joinable()) {
        m_readerThread.request_stop();
        if (m_readerThread.get_id() != std::this_thread::get_id()) {
            m_readerThread.join();
        }
    }

    rejectAllPending("Transport stopped");
}

bool JsonRpcTransport::isRunning() const {
    return m_running;
}

rapidjson::Document JsonRpcTransport::sendRequest(const std::string& method,
                                                  rapidjson::Value& params,
                                                  int timeoutMs) {
    if (!m_running) {
        throw std::runtime_error("JsonRpcTransport: not running, cannot send request: " + method);
    }

    int id = m_nextId++;

    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    doc.AddMember("jsonrpc", "2.0", allocator);
    doc.AddMember("id", id, allocator);
    doc.AddMember("method", rapidjson::Value(method.c_str(), allocator).Move(), allocator);
    doc.AddMember("params", params, allocator);

    std::promise<rapidjson::Document> promise;
    auto future = promise.get_future();

    {
        std::lock_guard lock(m_pendingMutex);
        m_pendingRequests[id] = PendingSlot{std::move(promise), std::chrono::steady_clock::now()};
    }

    if (!writeMessage(doc)) {
        std::lock_guard lock(m_pendingMutex);
        m_pendingRequests.erase(id);
        throw std::runtime_error("JsonRpcTransport: failed to write request: " + method);
    }

    const auto status = future.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status == std::future_status::timeout) {
        std::lock_guard lock(m_pendingMutex);
        m_pendingRequests.erase(id);
        throw std::runtime_error("JSON-RPC request timed out after " + std::to_string(timeoutMs) + "ms: " + method);
    }

    return future.get();
}

void JsonRpcTransport::sendNotification(const std::string& method, rapidjson::Value& params) {
    if (!m_running) return;

    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    doc.AddMember("jsonrpc", "2.0", allocator);
    doc.AddMember("method", rapidjson::Value(method.c_str(), allocator).Move(), allocator);
    doc.AddMember("params", params, allocator);

    writeMessage(doc);
}

void JsonRpcTransport::sendResponse(const rapidjson::Value& id, rapidjson::Value& result) {
    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    doc.AddMember("jsonrpc", "2.0", allocator);
    doc.AddMember("id", rapidjson::Value().CopyFrom(id, allocator), allocator);
    doc.AddMember("result", result, allocator);

    writeMessage(doc);
}

void JsonRpcTransport::sendError(const rapidjson::Value& id, int code, const std::string& message) {
    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    doc.AddMember("jsonrpc", "2.0", allocator);
    doc.AddMember("id", rapidjson::Value().CopyFrom(id, allocator), allocator);

    rapidjson::Value error(rapidjson::kObjectType);
    error.AddMember("code", code, allocator);
    error.AddMember("message", rapidjson::Value(message.c_str(), allocator).Move(), allocator);
    doc.AddMember("error", error, allocator);

    writeMessage(doc);
}

void JsonRpcTransport::setRequestHandler(RequestHandler handler) {
    std::lock_guard lock(m_handlerMutex);
    m_requestHandler = std::move(handler);
}

void JsonRpcTransport::setNotificationHandler(NotificationHandler handler) {
    std::lock_guard lock(m_handlerMutex);
    m_notificationHandler = std::move(handler);
}

bool JsonRpcTransport::writeMessage(const rapidjson::Document& doc) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    const std::string content = buffer.GetString();
    const std::string header = "Content-Length: " + std::to_string(content.length()) + "\r\n\r\n";

    std::lock_guard lock(m_writeMutex);
    return m_writer(header + content);
}

void JsonRpcTransport::readerLoop() {
    std::string buffer;

    try {
        while (m_running) {
            std::string chunk = m_reader(std::chrono::milliseconds(500));
            if (!m_running) {
                break;
            }
            if (chunk.empty()) {
                continue;
            }

            buffer.append(chunk);

            while (true) {
                auto headerPos = buffer.find("Content-Length: ");
                if (headerPos == std::string::npos) {
                    buffer.clear();
                    break;
                }

                if (headerPos > 0) {
                    buffer.erase(0, headerPos);
                    headerPos = 0;
                }

                const auto endHeader = buffer.find("\r\n\r\n", headerPos);
                if (endHeader == std::string::npos) {
                    break;
                }

                size_t contentLength = 0;
                try {
                    std::string lenStr = buffer.substr(16, endHeader - 16);
                    const auto firstNewline = lenStr.find("\r\n");
                    if (firstNewline != std::string::npos) {
                        lenStr = lenStr.substr(0, firstNewline);
                    }
                    contentLength = std::stoul(lenStr);
                } catch (...) {
                    buffer.erase(0, endHeader + 4);
                    continue;
                }

                const size_t messageStart = endHeader + 4;
                if (buffer.length() < messageStart + contentLength) {
                    break;
                }

                const std::string message = buffer.substr(messageStart, contentLength);
                buffer.erase(0, messageStart + contentLength);

                rapidjson::Document doc;
                doc.Parse(message.c_str(), message.length());
                if (doc.HasParseError()) {
                    continue;
                }

                handleMessage(std::move(doc));
            }
        }
    } catch (...) {
    }

    m_running = false;
    rejectAllPending("Transport reader stopped");
}

void JsonRpcTransport::handleMessage(rapidjson::Document&& doc) {
    if (doc.HasMember("id")) {
        if (doc.HasMember("method")) {
            std::lock_guard lock(m_handlerMutex);
            if (m_requestHandler) {
                m_requestHandler(doc);
            }
        } else {
            int id = -1;
            if (doc["id"].IsInt()) {
                id = doc["id"].GetInt();
            } else if (doc["id"].IsString()) {
                try {
                    id = std::stoi(doc["id"].GetString());
                } catch (...) {
                    return;
                }
            }

            if (id < 0) return;

            std::lock_guard lock(m_pendingMutex);
            auto it = m_pendingRequests.find(id);
            if (it != m_pendingRequests.end()) {
                try {
                    it->second.promise.set_value(std::move(doc));
                } catch (...) {
                }
                m_pendingRequests.erase(it);
            }
        }
    } else if (doc.HasMember("method")) {
        std::lock_guard lock(m_handlerMutex);
        if (m_notificationHandler) {
            m_notificationHandler(doc);
        }
    }
}

void JsonRpcTransport::rejectAllPending(const std::string& reason) {
    std::lock_guard lock(m_pendingMutex);
    for (auto& [id, slot] : m_pendingRequests) {
        try {
            slot.promise.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
        } catch (...) {
        }
    }
    m_pendingRequests.clear();
}

} // namespace firmius::core
