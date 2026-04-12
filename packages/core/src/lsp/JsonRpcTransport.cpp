#include "lsp/JsonRpcTransport.hpp"
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/error/en.h>

namespace firmius::core {

JsonRpcTransport::JsonRpcTransport(int stdinWriteFd, int stdoutReadFd)
    : m_writeFd(stdinWriteFd), m_readFd(stdoutReadFd) {
    if (::pipe(m_shutdownPipe) != 0) {
        throw std::runtime_error("JsonRpcTransport: failed to create shutdown pipe: " +
                                 std::string(std::strerror(errno)));
    }
}

JsonRpcTransport::~JsonRpcTransport() {
    stop();
    if (m_shutdownPipe[0] >= 0) ::close(m_shutdownPipe[0]);
    if (m_shutdownPipe[1] >= 0) ::close(m_shutdownPipe[1]);
}

void JsonRpcTransport::start() {
    if (m_running.exchange(true)) return;
    m_readerThread = std::jthread([this](std::stop_token) {
        readerLoop();
    });
}

void JsonRpcTransport::stop() {
    if (!m_running.exchange(false)) return;

    // Wake up poll() in the reader loop
    if (m_shutdownPipe[1] >= 0) {
        char c = 'x';
        (void)::write(m_shutdownPipe[1], &c, 1);
    }

    if (m_readerThread.joinable()) {
        m_readerThread.request_stop();
        m_readerThread.join();
    }

    // Reject any remaining pending requests
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
        m_pendingRequests[id] = PendingSlot{
            std::move(promise),
            std::chrono::steady_clock::now()
        };
    }

    if (!writeMessage(doc)) {
        // Remove the pending slot and throw
        std::lock_guard lock(m_pendingMutex);
        m_pendingRequests.erase(id);
        throw std::runtime_error("JsonRpcTransport: failed to write request: " + method);
    }

    // Wait for response with timeout
    auto status = future.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status == std::future_status::timeout) {
        // Clean up the pending slot
        std::lock_guard lock(m_pendingMutex);
        m_pendingRequests.erase(id);
        throw std::runtime_error("JSON-RPC request timed out after " +
                                 std::to_string(timeoutMs) + "ms: " + method);
    }

    // May throw if the promise was broken (e.g. transport stopped)
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

    std::string content = buffer.GetString();
    std::string header = "Content-Length: " + std::to_string(content.length()) + "\r\n\r\n";
    std::string fullMessage = header + content;

    std::lock_guard lock(m_writeMutex);

    const char* data = fullMessage.c_str();
    size_t remaining = fullMessage.length();

    while (remaining > 0) {
        ssize_t written = ::write(m_writeFd, data, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

void JsonRpcTransport::readerLoop() {
    std::string buffer;
    char chunk[8192];

    struct pollfd fds[2];
    fds[0].fd = m_readFd;
    fds[0].events = POLLIN;
    fds[1].fd = m_shutdownPipe[0];
    fds[1].events = POLLIN;

    while (m_running) {
        int ret = ::poll(fds, 2, 500); // 500ms poll timeout for periodic check
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Shutdown signal
        if (fds[1].revents & POLLIN) {
            break;
        }

        if (ret == 0) continue; // poll timeout, loop and check m_running

        if (!(fds[0].revents & POLLIN)) continue;

        ssize_t bytesRead = ::read(m_readFd, chunk, sizeof(chunk));
        if (bytesRead <= 0) {
            // Pipe closed or error
            break;
        }

        buffer.append(chunk, static_cast<size_t>(bytesRead));

        // Process all complete messages in buffer
        while (true) {
            // Find Content-Length header
            auto headerPos = buffer.find("Content-Length: ");
            if (headerPos == std::string::npos) {
                // Discard any garbage before a valid header
                buffer.clear();
                break;
            }

            // Discard anything before the header
            if (headerPos > 0) {
                buffer.erase(0, headerPos);
                headerPos = 0;
            }

            auto endHeader = buffer.find("\r\n\r\n", headerPos);
            if (endHeader == std::string::npos) break; // incomplete header

            // Parse content length
            size_t contentLength = 0;
            try {
                std::string lenStr = buffer.substr(16, endHeader - 16);
                // Handle potential other headers - find just the number
                auto firstNewline = lenStr.find("\r\n");
                if (firstNewline != std::string::npos) {
                    lenStr = lenStr.substr(0, firstNewline);
                }
                contentLength = std::stoul(lenStr);
            } catch (...) {
                // Malformed header, discard and try to recover
                buffer.erase(0, endHeader + 4);
                continue;
            }

            size_t messageStart = endHeader + 4;
            if (buffer.length() < messageStart + contentLength) break; // incomplete body

            std::string message = buffer.substr(messageStart, contentLength);
            buffer.erase(0, messageStart + contentLength);

            rapidjson::Document doc;
            doc.Parse(message.c_str(), message.length());

            if (doc.HasParseError()) {
                continue; // Skip malformed JSON
            }

            handleMessage(std::move(doc));
        }
    }

    m_running = false;
    rejectAllPending("Transport reader stopped");
}

void JsonRpcTransport::handleMessage(rapidjson::Document&& doc) {
    if (doc.HasMember("id")) {
        if (doc.HasMember("method")) {
            // Server-initiated request
            std::lock_guard lock(m_handlerMutex);
            if (m_requestHandler) {
                m_requestHandler(doc);
            }
        } else {
            // Response to our request
            int id = -1;
            if (doc["id"].IsInt()) {
                id = doc["id"].GetInt();
            } else if (doc["id"].IsString()) {
                try {
                    id = std::stoi(doc["id"].GetString());
                } catch (...) {
                    return; // Invalid ID format
                }
            }

            if (id < 0) return;

            std::lock_guard lock(m_pendingMutex);
            auto it = m_pendingRequests.find(id);
            if (it != m_pendingRequests.end()) {
                try {
                    it->second.promise.set_value(std::move(doc));
                } catch (...) {
                    // Promise may have already been satisfied or broken
                }
                m_pendingRequests.erase(it);
            }
        }
    } else if (doc.HasMember("method")) {
        // Notification
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
            slot.promise.set_exception(
                std::make_exception_ptr(std::runtime_error(reason)));
        } catch (...) {
            // Promise may have already been satisfied
        }
    }
    m_pendingRequests.clear();
}

} // namespace firmius::core
