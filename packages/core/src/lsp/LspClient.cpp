#include "lsp/LspClient.hpp"

#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>


#ifdef _WIN32
#include <process.h>
#define GET_PID() _getpid()
#else
#include <unistd.h>
#define GET_PID() getpid()
#endif

#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace firmius::core {

LspClient::LspClient(int stdinFd, int stdoutFd, std::string rootUri, std::string rootPath)
    : m_transport(stdinFd, stdoutFd)
    , m_rootUri(std::move(rootUri))
    , m_rootPath(std::move(rootPath))
{
    // Register notification handler for publishDiagnostics and other server notifications
    m_transport.setNotificationHandler([this](const rapidjson::Document& notification) {
        handleNotification(notification);
    });

    // Register request handler for server-initiated requests
    m_transport.setRequestHandler([this](const rapidjson::Document& request) {
        handleServerRequest(request);
    });

    m_transport.start();
}

LspClient::~LspClient() {
    if (m_initialized.load()) {
        try {
            shutdown();
        } catch (...) {
            // Best effort during destruction
        }
    }
    m_transport.stop();
}

// --- Lifecycle ---

bool LspClient::initialize(int timeoutMs) {
    if (m_initialized.load()) return true;

    try {
        rapidjson::Document params(rapidjson::kObjectType);
        auto& alloc = params.GetAllocator();

        // processId (LSP spec requirement)
        params.AddMember("processId", GET_PID(), alloc);

        // rootUri
        params.AddMember("rootUri", rapidjson::Value(m_rootUri.c_str(), alloc), alloc);

        // rootPath (deprecated but still expected by many servers)
        params.AddMember("rootPath", rapidjson::Value(m_rootPath.c_str(), alloc), alloc);

        // capabilities
        rapidjson::Value capabilities(rapidjson::kObjectType);

        // textDocument capabilities
        rapidjson::Value textDocument(rapidjson::kObjectType);

        // synchronization
        rapidjson::Value synchronization(rapidjson::kObjectType);
        synchronization.AddMember("didSave", true, alloc);
        synchronization.AddMember("dynamicRegistration", false, alloc);
        textDocument.AddMember("synchronization", synchronization, alloc);

        // publishDiagnostics
        rapidjson::Value publishDiagnostics(rapidjson::kObjectType);
        publishDiagnostics.AddMember("versionSupport", true, alloc);
        textDocument.AddMember("publishDiagnostics", publishDiagnostics, alloc);

        // hover
        rapidjson::Value hoverCap(rapidjson::kObjectType);
        hoverCap.AddMember("dynamicRegistration", false, alloc);
        rapidjson::Value hoverContentFormat(rapidjson::kArrayType);
        hoverContentFormat.PushBack("markdown", alloc);
        hoverContentFormat.PushBack("plaintext", alloc);
        hoverCap.AddMember("contentFormat", hoverContentFormat, alloc);
        textDocument.AddMember("hover", hoverCap, alloc);

        // definition
        rapidjson::Value definitionCap(rapidjson::kObjectType);
        definitionCap.AddMember("dynamicRegistration", false, alloc);
        textDocument.AddMember("definition", definitionCap, alloc);

        // references
        rapidjson::Value referencesCap(rapidjson::kObjectType);
        referencesCap.AddMember("dynamicRegistration", false, alloc);
        textDocument.AddMember("references", referencesCap, alloc);

        // implementation
        rapidjson::Value implementationCap(rapidjson::kObjectType);
        implementationCap.AddMember("dynamicRegistration", false, alloc);
        textDocument.AddMember("implementation", implementationCap, alloc);

        // documentSymbol
        rapidjson::Value documentSymbolCap(rapidjson::kObjectType);
        documentSymbolCap.AddMember("dynamicRegistration", false, alloc);
        rapidjson::Value symbolKindCap(rapidjson::kObjectType);
        rapidjson::Value symbolKindValues(rapidjson::kArrayType);
        for (int i = 1; i <= 26; ++i) {
            symbolKindValues.PushBack(i, alloc);
        }
        symbolKindCap.AddMember("valueSet", symbolKindValues, alloc);
        documentSymbolCap.AddMember("symbolKind", symbolKindCap, alloc);
        documentSymbolCap.AddMember("hierarchicalDocumentSymbolSupport", true, alloc);
        textDocument.AddMember("documentSymbol", documentSymbolCap, alloc);

        // callHierarchy
        rapidjson::Value callHierarchyCap(rapidjson::kObjectType);
        callHierarchyCap.AddMember("dynamicRegistration", false, alloc);
        textDocument.AddMember("callHierarchy", callHierarchyCap, alloc);

        capabilities.AddMember("textDocument", textDocument, alloc);

        // workspace capabilities
        rapidjson::Value workspace(rapidjson::kObjectType);

        // workspace/symbol
        rapidjson::Value workspaceSymbolCap(rapidjson::kObjectType);
        workspaceSymbolCap.AddMember("dynamicRegistration", false, alloc);
        workspace.AddMember("symbol", workspaceSymbolCap, alloc);

        // workspace/configuration
        workspace.AddMember("configuration", true, alloc);

        // workspace/didChangeWatchedFiles
        rapidjson::Value watchedFilesCap(rapidjson::kObjectType);
        watchedFilesCap.AddMember("dynamicRegistration", false, alloc);
        workspace.AddMember("didChangeWatchedFiles", watchedFilesCap, alloc);

        // workspace/workspaceFolders
        workspace.AddMember("workspaceFolders", true, alloc);

        capabilities.AddMember("workspace", workspace, alloc);

        // window capabilities
        rapidjson::Value window(rapidjson::kObjectType);
        rapidjson::Value workDoneProgress(rapidjson::kObjectType);
        window.AddMember("workDoneProgress", true, alloc);
        capabilities.AddMember("window", window, alloc);

        params.AddMember("capabilities", capabilities, alloc);

        // workspaceFolders
        rapidjson::Value workspaceFolders(rapidjson::kArrayType);
        rapidjson::Value folder(rapidjson::kObjectType);
        folder.AddMember("uri", rapidjson::Value(m_rootUri.c_str(), alloc), alloc);
        folder.AddMember("name", rapidjson::Value(m_rootPath.c_str(), alloc), alloc);
        workspaceFolders.PushBack(folder, alloc);
        params.AddMember("workspaceFolders", workspaceFolders, alloc);

        // Send initialize request
        rapidjson::Value paramsValue(params, alloc);
        m_transport.sendRequest("initialize", paramsValue, timeoutMs);

        // Send initialized notification
        rapidjson::Document initNotif(rapidjson::kObjectType);
        rapidjson::Value emptyParams(rapidjson::kObjectType);
        m_transport.sendNotification("initialized", emptyParams);

        m_initialized.store(true);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void LspClient::shutdown(int timeoutMs) {
    if (!m_initialized.load()) return;

    try {
        // Send shutdown request
        rapidjson::Document doc(rapidjson::kObjectType);
        rapidjson::Value nullParams(rapidjson::kNullType);
        m_transport.sendRequest("shutdown", nullParams, timeoutMs);
    } catch (...) {
        // Best effort — continue to exit regardless
    }

    // Send exit notification
    try {
        rapidjson::Value emptyParams(rapidjson::kObjectType);
        m_transport.sendNotification("exit", emptyParams);
    } catch (...) {
        // Best effort
    }

    m_initialized.store(false);
}

bool LspClient::isInitialized() const {
    return m_initialized.load();
}

// --- Document Sync ---

void LspClient::openDocument(const std::string& path, const std::string& languageId, const std::string& text) {
    std::string uri = fileUri(path);

    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    rapidjson::Value textDocItem(rapidjson::kObjectType);
    textDocItem.AddMember("uri", rapidjson::Value(uri.c_str(), alloc), alloc);
    textDocItem.AddMember("languageId", rapidjson::Value(languageId.c_str(), alloc), alloc);
    textDocItem.AddMember("version", nextVersion(uri), alloc);
    textDocItem.AddMember("text", rapidjson::Value(text.c_str(), alloc), alloc);

    params.AddMember("textDocument", textDocItem, alloc);

    rapidjson::Value paramsValue(params, alloc);
    m_transport.sendNotification("textDocument/didOpen", paramsValue);

    {
        std::lock_guard<std::mutex> lock(m_docMutex);
        m_openDocuments.insert(uri);
    }
}

void LspClient::changeDocument(const std::string& path, const std::string& text) {
    std::string uri = fileUri(path);

    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    rapidjson::Value textDocId(rapidjson::kObjectType);
    textDocId.AddMember("uri", rapidjson::Value(uri.c_str(), alloc), alloc);
    textDocId.AddMember("version", nextVersion(uri), alloc);

    params.AddMember("textDocument", textDocId, alloc);

    // Full content sync
    rapidjson::Value contentChanges(rapidjson::kArrayType);
    rapidjson::Value change(rapidjson::kObjectType);
    change.AddMember("text", rapidjson::Value(text.c_str(), alloc), alloc);
    contentChanges.PushBack(change, alloc);

    params.AddMember("contentChanges", contentChanges, alloc);

    rapidjson::Value paramsValue(params, alloc);
    m_transport.sendNotification("textDocument/didChange", paramsValue);
}

void LspClient::closeDocument(const std::string& path) {
    std::string uri = fileUri(path);

    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    rapidjson::Value textDocId(rapidjson::kObjectType);
    textDocId.AddMember("uri", rapidjson::Value(uri.c_str(), alloc), alloc);

    params.AddMember("textDocument", textDocId, alloc);

    rapidjson::Value paramsValue(params, alloc);
    m_transport.sendNotification("textDocument/didClose", paramsValue);

    {
        std::lock_guard<std::mutex> lock(m_docMutex);
        m_openDocuments.erase(uri);
    }
}

bool LspClient::touchFile(const std::string& path, const std::string& languageId, bool waitForDiags, int timeoutMs) {
    // Read file content from disk
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    std::string uri = fileUri(path);

    // Check if document is already open
    bool alreadyOpen = false;
    {
        std::lock_guard<std::mutex> lock(m_docMutex);
        alreadyOpen = m_openDocuments.count(uri) > 0;
    }

    // Clear received flag before sending so we can detect fresh diagnostics
    if (waitForDiags) {
        std::lock_guard<std::mutex> lock(m_diagMutex);
        m_diagReceived.erase(uri);
    }

    if (alreadyOpen) {
        changeDocument(path, content);
    } else {
        openDocument(path, languageId, content);
    }

    bool gotDiags = true;
    if (waitForDiags) {
        gotDiags = waitForDiagnostics(uri, timeoutMs);
        // Post-diagnostic settling delay matching proven Python bridge behavior
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    return gotDiags;
}

// --- Diagnostics ---

std::vector<Diagnostic> LspClient::getDiagnostics(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(m_diagMutex);
    auto it = m_diagnostics.find(uri);
    if (it != m_diagnostics.end()) {
        return it->second;
    }
    return {};
}

std::unordered_map<std::string, std::vector<Diagnostic>> LspClient::getAllDiagnostics() const {
    std::lock_guard<std::mutex> lock(m_diagMutex);
    return m_diagnostics;
}

bool LspClient::waitForDiagnostics(const std::string& uri, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_diagMutex);
    return m_diagCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
        return m_diagReceived.count(uri) > 0;
    });
}

// --- Semantic Requests ---

rapidjson::Document LspClient::hover(const std::string& uri, Position pos, int timeoutMs) {
    auto params = makeTextDocumentPositionParams(uri, pos);
    rapidjson::Value paramsValue(params, params.GetAllocator());
    return m_transport.sendRequest("textDocument/hover", paramsValue, timeoutMs);
}

rapidjson::Document LspClient::definition(const std::string& uri, Position pos, int timeoutMs) {
    auto params = makeTextDocumentPositionParams(uri, pos);
    rapidjson::Value paramsValue(params, params.GetAllocator());
    return m_transport.sendRequest("textDocument/definition", paramsValue, timeoutMs);
}

rapidjson::Document LspClient::references(const std::string& uri, Position pos, bool includeDeclaration, int timeoutMs) {
    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    rapidjson::Value textDocId(rapidjson::kObjectType);
    textDocId.AddMember("uri", rapidjson::Value(uri.c_str(), alloc), alloc);
    params.AddMember("textDocument", textDocId, alloc);

    rapidjson::Value position(rapidjson::kObjectType);
    position.AddMember("line", pos.line, alloc);
    position.AddMember("character", pos.character, alloc);
    params.AddMember("position", position, alloc);

    rapidjson::Value context(rapidjson::kObjectType);
    context.AddMember("includeDeclaration", includeDeclaration, alloc);
    params.AddMember("context", context, alloc);

    rapidjson::Value paramsValue(params, alloc);
    return m_transport.sendRequest("textDocument/references", paramsValue, timeoutMs);
}

rapidjson::Document LspClient::implementation(const std::string& uri, Position pos, int timeoutMs) {
    auto params = makeTextDocumentPositionParams(uri, pos);
    rapidjson::Value paramsValue(params, params.GetAllocator());
    return m_transport.sendRequest("textDocument/implementation", paramsValue, timeoutMs);
}

rapidjson::Document LspClient::documentSymbol(const std::string& uri, int timeoutMs) {
    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    rapidjson::Value textDocId(rapidjson::kObjectType);
    textDocId.AddMember("uri", rapidjson::Value(uri.c_str(), alloc), alloc);
    params.AddMember("textDocument", textDocId, alloc);

    rapidjson::Value paramsValue(params, alloc);
    return m_transport.sendRequest("textDocument/documentSymbol", paramsValue, timeoutMs);
}

rapidjson::Document LspClient::workspaceSymbol(const std::string& query, int timeoutMs) {
    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    params.AddMember("query", rapidjson::Value(query.c_str(), alloc), alloc);

    rapidjson::Value paramsValue(params, alloc);
    return m_transport.sendRequest("workspace/symbol", paramsValue, timeoutMs);
}

rapidjson::Document LspClient::prepareCallHierarchy(const std::string& uri, Position pos, int timeoutMs) {
    auto params = makeTextDocumentPositionParams(uri, pos);
    rapidjson::Value paramsValue(params, params.GetAllocator());
    return m_transport.sendRequest("textDocument/prepareCallHierarchy", paramsValue, timeoutMs);
}

rapidjson::Document LspClient::incomingCalls(const rapidjson::Value& item, int timeoutMs) {
    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    rapidjson::Value itemCopy(item, alloc);
    params.AddMember("item", itemCopy, alloc);

    rapidjson::Value paramsValue(params, alloc);
    return m_transport.sendRequest("callHierarchy/incomingCalls", paramsValue, timeoutMs);
}

rapidjson::Document LspClient::outgoingCalls(const rapidjson::Value& item, int timeoutMs) {
    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    rapidjson::Value itemCopy(item, alloc);
    params.AddMember("item", itemCopy, alloc);

    rapidjson::Value paramsValue(params, alloc);
    return m_transport.sendRequest("callHierarchy/outgoingCalls", paramsValue, timeoutMs);
}

// --- Notification Handler ---

void LspClient::handleNotification(const rapidjson::Document& notification) {
    if (!notification.HasMember("method") || !notification["method"].IsString()) return;

    std::string method = notification["method"].GetString();

    if (method == "textDocument/publishDiagnostics") {
        if (!notification.HasMember("params") || !notification["params"].IsObject()) return;
        const auto& params = notification["params"];

        if (!params.HasMember("uri") || !params["uri"].IsString()) return;
        std::string uri = params["uri"].GetString();

        std::vector<Diagnostic> diagnostics;

        if (params.HasMember("diagnostics") && params["diagnostics"].IsArray()) {
            const auto& diagArray = params["diagnostics"];
            for (rapidjson::SizeType i = 0; i < diagArray.Size(); ++i) {
                const auto& d = diagArray[i];
                Diagnostic diag;

                // Parse range
                if (d.HasMember("range") && d["range"].IsObject()) {
                    const auto& range = d["range"];
                    if (range.HasMember("start") && range["start"].IsObject()) {
                        diag.range.start.line = range["start"]["line"].GetInt();
                        diag.range.start.character = range["start"]["character"].GetInt();
                    }
                    if (range.HasMember("end") && range["end"].IsObject()) {
                        diag.range.end.line = range["end"]["line"].GetInt();
                        diag.range.end.character = range["end"]["character"].GetInt();
                    }
                }

                // Parse severity
                if (d.HasMember("severity") && d["severity"].IsInt()) {
                    diag.severity = static_cast<DiagnosticSeverity>(d["severity"].GetInt());
                }

                // Parse code
                if (d.HasMember("code")) {
                    if (d["code"].IsInt()) {
                        diag.code = d["code"].GetInt();
                    } else if (d["code"].IsString()) {
                        diag.code = std::string(d["code"].GetString());
                    }
                }

                // Parse source
                if (d.HasMember("source") && d["source"].IsString()) {
                    diag.source = d["source"].GetString();
                }

                // Parse message
                if (d.HasMember("message") && d["message"].IsString()) {
                    diag.message = d["message"].GetString();
                }

                diagnostics.push_back(std::move(diag));
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_diagMutex);
            m_diagnostics[uri] = std::move(diagnostics);
            m_diagReceived.insert(uri);
        }
        m_diagCondition.notify_all();
    }
    // Other notifications (progress, etc.) are intentionally ignored
}

// --- Server Request Handler ---

void LspClient::handleServerRequest(const rapidjson::Document& request) {
    if (!request.HasMember("method") || !request["method"].IsString()) return;
    if (!request.HasMember("id")) return;

    std::string method = request["method"].GetString();
    const auto& id = request["id"];

    if (method == "workspace/configuration") {
        // Respond with empty settings array (one empty object per requested item)
        rapidjson::Value result(rapidjson::kArrayType);
        rapidjson::Document doc;
        auto& alloc = doc.GetAllocator();

        // Count requested items
        int count = 1;
        if (request.HasMember("params") && request["params"].IsObject()
            && request["params"].HasMember("items") && request["params"]["items"].IsArray()) {
            count = static_cast<int>(request["params"]["items"].Size());
        }

        for (int i = 0; i < count; ++i) {
            rapidjson::Value empty(rapidjson::kObjectType);
            result.PushBack(empty, alloc);
        }

        m_transport.sendResponse(id, result);
    } else if (method == "workspace/workspaceFolders") {
        rapidjson::Document doc;
        auto& alloc = doc.GetAllocator();

        rapidjson::Value result(rapidjson::kArrayType);
        rapidjson::Value folder(rapidjson::kObjectType);
        folder.AddMember("uri", rapidjson::Value(m_rootUri.c_str(), alloc), alloc);
        folder.AddMember("name", rapidjson::Value(m_rootPath.c_str(), alloc), alloc);
        result.PushBack(folder, alloc);

        m_transport.sendResponse(id, result);
    } else if (method == "window/workDoneProgress/create") {
        // Accept — respond with null
        rapidjson::Value nullResult(rapidjson::kNullType);
        m_transport.sendResponse(id, nullResult);
    } else if (method == "client/registerCapability") {
        // Accept — respond with null
        rapidjson::Value nullResult(rapidjson::kNullType);
        m_transport.sendResponse(id, nullResult);
    } else if (method == "client/unregisterCapability") {
        // Accept — respond with null
        rapidjson::Value nullResult(rapidjson::kNullType);
        m_transport.sendResponse(id, nullResult);
    } else {
        // Unknown server request — respond with null to avoid hanging the server
        rapidjson::Value nullResult(rapidjson::kNullType);
        m_transport.sendResponse(id, nullResult);
    }
}

// --- Internal Helpers ---

rapidjson::Document LspClient::makeTextDocumentPositionParams(const std::string& uri, Position pos) {
    rapidjson::Document params(rapidjson::kObjectType);
    auto& alloc = params.GetAllocator();

    rapidjson::Value textDocId(rapidjson::kObjectType);
    textDocId.AddMember("uri", rapidjson::Value(uri.c_str(), alloc), alloc);
    params.AddMember("textDocument", textDocId, alloc);

    rapidjson::Value position(rapidjson::kObjectType);
    position.AddMember("line", pos.line, alloc);
    position.AddMember("character", pos.character, alloc);
    params.AddMember("position", position, alloc);

    return params;
}

int LspClient::nextVersion(const std::string& uri) {
    std::lock_guard<std::mutex> lock(m_docMutex);
    return ++m_documentVersions[uri];
}

} // namespace firmius::core
