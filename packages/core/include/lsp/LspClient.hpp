#ifndef FIRMIUS_CORE_LSPCLIENT_HPP
#define FIRMIUS_CORE_LSPCLIENT_HPP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <rapidjson/fwd.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "JsonRpcTransport.hpp"
#include "LspProtocol.hpp"

namespace firmius::core {

class LspClient {
public:
    LspClient(JsonRpcTransport::Writer writer, JsonRpcTransport::Reader reader,
              std::string rootUri, std::string rootPath);
    ~LspClient();

    LspClient(const LspClient&) = delete;
    LspClient& operator=(const LspClient&) = delete;

    bool initialize(int timeoutMs);
    void shutdown(int timeoutMs = 2000);
    bool isInitialized() const;

    void openDocument(const std::string& path, const std::string& languageId, const std::string& text);
    void changeDocument(const std::string& path, const std::string& text);
    void closeDocument(const std::string& path);
    bool touchFile(const std::string& path, const std::string& languageId, bool waitForDiagnostics, int timeoutMs);

    std::vector<Diagnostic> getDiagnostics(const std::string& uri) const;
    std::unordered_map<std::string, std::vector<Diagnostic>> getAllDiagnostics() const;
    bool waitForDiagnostics(const std::string& uri, int timeoutMs);

    rapidjson::Document hover(const std::string& uri, Position pos, int timeoutMs);
    rapidjson::Document definition(const std::string& uri, Position pos, int timeoutMs);
    rapidjson::Document references(const std::string& uri, Position pos, bool includeDeclaration, int timeoutMs);
    rapidjson::Document implementation(const std::string& uri, Position pos, int timeoutMs);
    rapidjson::Document documentSymbol(const std::string& uri, int timeoutMs);
    rapidjson::Document workspaceSymbol(const std::string& query, int timeoutMs);
    rapidjson::Document prepareCallHierarchy(const std::string& uri, Position pos, int timeoutMs);
    rapidjson::Document incomingCalls(const rapidjson::Value& item, int timeoutMs);
    rapidjson::Document outgoingCalls(const rapidjson::Value& item, int timeoutMs);

private:
    void handleNotification(const rapidjson::Document& notification);
    void handleServerRequest(const rapidjson::Document& request);

    rapidjson::Document makeTextDocumentPositionParams(const std::string& uri, Position pos);
    int nextVersion(const std::string& uri);

    JsonRpcTransport m_transport;
    std::string m_rootUri;
    std::string m_rootPath;
    std::atomic<bool> m_initialized{false};

    mutable std::mutex m_docMutex;
    std::unordered_set<std::string> m_openDocuments;
    std::unordered_map<std::string, int> m_documentVersions;
    std::unordered_map<std::string, std::string> m_documentContents;

    mutable std::mutex m_diagMutex;
    std::unordered_map<std::string, std::vector<Diagnostic>> m_diagnostics;
    std::condition_variable m_diagCondition;
    std::unordered_set<std::string> m_diagReceived;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_LSPCLIENT_HPP
