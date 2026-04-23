#include <gtest/gtest.h>

#include "lsp/JsonRpcTransport.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace firmius::core;

namespace {

std::string serializeJson(const rapidjson::Value& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return std::string(buffer.GetString(), buffer.GetSize());
}

class InMemoryByteChannel {
public:
    bool write(const std::string& data) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;
            }
            buffer_ += data;
        }
        cv_.notify_all();
        return true;
    }

    std::string read(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [&]() { return closed_ || !buffer_.empty(); });
        if (buffer_.empty()) {
            return {};
        }
        std::string out = std::move(buffer_);
        buffer_.clear();
        return out;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string buffer_;
    bool closed_ = false;
};

void writeFramedJson(InMemoryByteChannel& channel, const std::string& jsonPayload) {
    const std::string header =
        "Content-Length: " + std::to_string(jsonPayload.size()) + "\r\n\r\n";
    if (!channel.write(header + jsonPayload)) {
        throw std::runtime_error("write failed");
    }
}

std::optional<std::string> tryReadFramedJson(InMemoryByteChannel& channel,
                                             std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string buffer;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const std::string chunk = channel.read(remaining);
        if (chunk.empty()) {
            return std::nullopt;
        }
        buffer += chunk;

        const auto headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            continue;
        }

        constexpr size_t kHeaderPrefixLen = 16;
        if (buffer.rfind("Content-Length: ", 0) != 0) {
            throw std::runtime_error("missing Content-Length header");
        }

        const std::string lenStr = buffer.substr(kHeaderPrefixLen, headerEnd - kHeaderPrefixLen);
        const size_t contentLen = std::stoul(lenStr);
        const size_t bodyOffset = headerEnd + 4;
        if (buffer.size() < bodyOffset + contentLen) {
            continue;
        }

        return buffer.substr(bodyOffset, contentLen);
    }

    return std::nullopt;
}

rapidjson::Document parseJson(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse(text.c_str(), text.size());
    if (doc.HasParseError()) {
        throw std::runtime_error("json parse error");
    }
    return doc;
}

class JsonRpcTransportFixture : public ::testing::Test {
protected:
    void SetUp() override {
        transport_ = std::make_unique<JsonRpcTransport>(
            [this](const std::string& data) { return clientToServer_.write(data); },
            [this](std::chrono::milliseconds timeout) { return serverToClient_.read(timeout); },
            [this]() { serverToClient_.close(); });
        transport_->start();
    }

    void TearDown() override {
        if (transport_) {
            transport_->stop();
            transport_.reset();
        }
        clientToServer_.close();
        serverToClient_.close();
    }

    InMemoryByteChannel clientToServer_;
    InMemoryByteChannel serverToClient_;
    std::unique_ptr<JsonRpcTransport> transport_;
};

TEST_F(JsonRpcTransportFixture, FramesContentLengthAndSendsNotificationWithoutId) {
    rapidjson::Document params;
    params.SetObject();
    params.AddMember("value", 7, params.GetAllocator());

    transport_->sendNotification("notify/test", params);

    const auto payload = tryReadFramedJson(clientToServer_, std::chrono::milliseconds(1000));
    ASSERT_TRUE(payload.has_value());

    const rapidjson::Document outbound = parseJson(*payload);
    ASSERT_TRUE(outbound.IsObject());
    EXPECT_TRUE(outbound.HasMember("jsonrpc"));
    EXPECT_STREQ(outbound["jsonrpc"].GetString(), "2.0");
    EXPECT_TRUE(outbound.HasMember("method"));
    EXPECT_STREQ(outbound["method"].GetString(), "notify/test");
    EXPECT_TRUE(outbound.HasMember("params"));
    EXPECT_FALSE(outbound.HasMember("id"));
}

TEST_F(JsonRpcTransportFixture, CompletesRequestResponseCycle) {
    std::thread server([&]() {
        const auto reqPayload = tryReadFramedJson(clientToServer_, std::chrono::milliseconds(1000));
        ASSERT_TRUE(reqPayload.has_value());
        rapidjson::Document request = parseJson(*reqPayload);

        ASSERT_TRUE(request.HasMember("id"));
        ASSERT_TRUE(request.HasMember("method"));
        ASSERT_STREQ(request["method"].GetString(), "sum");

        rapidjson::Document response;
        response.SetObject();
        auto& alloc = response.GetAllocator();
        response.AddMember("jsonrpc", "2.0", alloc);
        response.AddMember("id", request["id"].GetInt(), alloc);

        rapidjson::Value result(rapidjson::kObjectType);
        result.AddMember("total", 3, alloc);
        response.AddMember("result", result, alloc);

        writeFramedJson(serverToClient_, serializeJson(response));
    });

    rapidjson::Document params;
    params.SetObject();
    params.AddMember("a", 1, params.GetAllocator());
    params.AddMember("b", 2, params.GetAllocator());

    rapidjson::Document response = transport_->sendRequest("sum", params, 1000);

    ASSERT_TRUE(response.HasMember("result"));
    ASSERT_TRUE(response["result"].IsObject());
    EXPECT_EQ(response["result"]["total"].GetInt(), 3);

    server.join();
}

TEST_F(JsonRpcTransportFixture, TimesOutWhenNoResponseArrives) {
    std::thread server([&]() {
        const auto reqPayload = tryReadFramedJson(clientToServer_, std::chrono::milliseconds(1000));
        ASSERT_TRUE(reqPayload.has_value());
        (void)parseJson(*reqPayload);
    });

    rapidjson::Document params;
    params.SetObject();

    EXPECT_THROW(
        {
            try {
                (void)transport_->sendRequest("never-respond", params, 80);
            } catch (const std::runtime_error& e) {
                EXPECT_NE(std::string(e.what()).find("timed out"), std::string::npos);
                throw;
            }
        },
        std::runtime_error);

    server.join();
}

TEST_F(JsonRpcTransportFixture, RoutesConcurrentRequestResponsesById) {
    bool serverThreadOk = true;
    std::thread server([&]() {
        try {
            std::map<std::string, int> idsByMethod;

            for (int i = 0; i < 2; ++i) {
                const auto reqPayload = tryReadFramedJson(clientToServer_, std::chrono::milliseconds(1000));
                if (!reqPayload.has_value()) {
                    serverThreadOk = false;
                    return;
                }
                rapidjson::Document request = parseJson(*reqPayload);
                if (!request.HasMember("method") || !request.HasMember("id")) {
                    serverThreadOk = false;
                    return;
                }
                idsByMethod[request["method"].GetString()] = request["id"].GetInt();
            }

            if (idsByMethod.count("first") == 0 || idsByMethod.count("second") == 0) {
                serverThreadOk = false;
                return;
            }

            auto sendResponseFor = [&](const std::string& method) {
                rapidjson::Document response;
                response.SetObject();
                auto& alloc = response.GetAllocator();
                response.AddMember("jsonrpc", "2.0", alloc);
                response.AddMember("id", idsByMethod.at(method), alloc);

                rapidjson::Value result(rapidjson::kObjectType);
                result.AddMember("servedMethod", rapidjson::Value(method.c_str(), alloc).Move(), alloc);
                response.AddMember("result", result, alloc);
                writeFramedJson(serverToClient_, serializeJson(response));
            };

            sendResponseFor("second");
            sendResponseFor("first");
        } catch (...) {
            serverThreadOk = false;
        }
    });

    auto call = [&](const std::string& method) {
        rapidjson::Document params;
        params.SetObject();
        params.AddMember("payload", 1, params.GetAllocator());
        return transport_->sendRequest(method, params, 1000);
    };

    auto firstFuture = std::async(std::launch::async, [&]() { return call("first"); });
    auto secondFuture = std::async(std::launch::async, [&]() { return call("second"); });

    rapidjson::Document firstResp;
    rapidjson::Document secondResp;
    std::exception_ptr firstErr;
    std::exception_ptr secondErr;

    try {
        firstResp = firstFuture.get();
    } catch (...) {
        firstErr = std::current_exception();
    }
    try {
        secondResp = secondFuture.get();
    } catch (...) {
        secondErr = std::current_exception();
    }

    server.join();

    ASSERT_EQ(firstErr, nullptr) << "first request failed";
    ASSERT_EQ(secondErr, nullptr) << "second request failed";
    ASSERT_TRUE(serverThreadOk);
    ASSERT_TRUE(firstResp.HasMember("result"));
    ASSERT_TRUE(secondResp.HasMember("result"));
    ASSERT_TRUE(firstResp["result"].HasMember("servedMethod"));
    ASSERT_TRUE(secondResp["result"].HasMember("servedMethod"));
    EXPECT_STREQ(firstResp["result"]["servedMethod"].GetString(), "first");
    EXPECT_STREQ(secondResp["result"]["servedMethod"].GetString(), "second");
}

TEST_F(JsonRpcTransportFixture, InvokesServerInitiatedRequestHandlerCallback) {
    std::promise<rapidjson::Document> seenPromise;
    auto seenFuture = seenPromise.get_future();

    transport_->setRequestHandler([&](const rapidjson::Document& request) {
        rapidjson::Document copy;
        copy.CopyFrom(request, copy.GetAllocator());
        seenPromise.set_value(std::move(copy));
    });

    rapidjson::Document serverRequest;
    serverRequest.SetObject();
    auto& alloc = serverRequest.GetAllocator();
    serverRequest.AddMember("jsonrpc", "2.0", alloc);
    serverRequest.AddMember("id", 91, alloc);
    serverRequest.AddMember("method", "server/request", alloc);
    rapidjson::Value params(rapidjson::kObjectType);
    params.AddMember("k", "v", alloc);
    serverRequest.AddMember("params", params, alloc);

    writeFramedJson(serverToClient_, serializeJson(serverRequest));

    ASSERT_EQ(seenFuture.wait_for(std::chrono::milliseconds(1000)), std::future_status::ready);
    rapidjson::Document observed = seenFuture.get();
    ASSERT_TRUE(observed.HasMember("method"));
    ASSERT_TRUE(observed.HasMember("id"));
    EXPECT_STREQ(observed["method"].GetString(), "server/request");
    EXPECT_EQ(observed["id"].GetInt(), 91);
}

TEST_F(JsonRpcTransportFixture, HandlesChannelCloseGracefully) {
    serverToClient_.close();

    rapidjson::Document params;
    params.SetObject();
    EXPECT_THROW((void)transport_->sendRequest("after-close", params, 100), std::runtime_error);

    EXPECT_NO_THROW(transport_->sendNotification("after-close-notify", params));
}

} // namespace
