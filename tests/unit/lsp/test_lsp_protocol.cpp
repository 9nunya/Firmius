#include "lsp/LspProtocol.hpp"

#include <gtest/gtest.h>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <string>

using namespace firmius::core;

namespace {

TEST(LspProtocolTest, FileUriPathFromUriRoundtripPreservesSpacesAndUnicode) {
    const std::string path = "/tmp/space dir/ユニコード/测试 file.cpp";

    const std::string uri = fileUri(path);
    EXPECT_EQ(uri, "file://" + path);
    EXPECT_EQ(pathFromUri(uri), path);
}

TEST(LspProtocolTest, FileUriAndPathFromUriHandleAlreadyConvertedAndPlainInput) {
    const std::string uri = "file:///tmp/already uri.cpp";
    EXPECT_EQ(fileUri(uri), uri);

    const std::string plainPath = "/tmp/plain.cpp";
    EXPECT_EQ(pathFromUri(plainPath), plainPath);
}

TEST(LspProtocolTest, OneBasedAndZeroBasedConversions) {
    const Position zeroStart = toZeroBased(1, 1);
    EXPECT_EQ(zeroStart.line, 0);
    EXPECT_EQ(zeroStart.character, 0);

    const Position zeroConverted = toZeroBased(42, 17);
    EXPECT_EQ(zeroConverted.line, 41);
    EXPECT_EQ(zeroConverted.character, 16);

    const auto oneBased = toOneBased(Position{41, 16});
    EXPECT_EQ(oneBased.first, 42);
    EXPECT_EQ(oneBased.second, 17);
}

TEST(LspProtocolTest, SeverityNameCoversKnownAndUnknownValues) {
    EXPECT_EQ(severityName(1), "Error");
    EXPECT_EQ(severityName(2), "Warning");
    EXPECT_EQ(severityName(3), "Information");
    EXPECT_EQ(severityName(4), "Hint");

    EXPECT_EQ(severityName(0), "Unknown");
    EXPECT_EQ(severityName(5), "Unknown");
    EXPECT_EQ(severityName(-1), "Unknown");
}

TEST(LspProtocolTest, PositionSerializationSmokeCheck) {
    const Position position{3, 14};

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    position.Serialize(writer);

    EXPECT_EQ(std::string(buffer.GetString()), R"({"line":3,"character":14})");
}

TEST(LspProtocolTest, RangeSerializationSmokeCheck) {
    const Range range{{1, 2}, {3, 4}};

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    range.Serialize(writer);

    EXPECT_EQ(std::string(buffer.GetString()),
              R"({"start":{"line":1,"character":2},"end":{"line":3,"character":4}})");
}

} // namespace
