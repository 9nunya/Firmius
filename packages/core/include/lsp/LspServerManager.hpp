#ifndef FIRMIUS_CORE_LSP_SERVER_MANAGER_HPP
#define FIRMIUS_CORE_LSP_SERVER_MANAGER_HPP

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::shared {
class IHost;
}

namespace firmius::core {

class LspClient;
struct LspServerSpec;

class LspServerManager {
public:
    static LspServerManager& instance();
    ~LspServerManager();

    LspServerManager(const LspServerManager&) = delete;
    LspServerManager& operator=(const LspServerManager&) = delete;

    LspClient* getOrCreateServer(const LspServerSpec& spec,
                                 const std::string& projectRoot,
                                 int initTimeoutMs = 45000);
    void releaseServer(const std::string& specId, const std::string& projectRoot);
    void shutdownAll();
    void shutdownServer(const std::string& specId, const std::string& projectRoot);
    bool isServerHealthy(const std::string& specId, const std::string& projectRoot);
    size_t activeServerCount() const;
    std::vector<std::string> activeServerIds() const;
    std::vector<std::string> getServerStderr(const std::string& specId,
                                             const std::string& projectRoot,
                                             size_t maxLines = 20) const;

    void setHostForTesting(std::shared_ptr<firmius::shared::IHost> host);

private:
    struct ServerInstance;

    LspServerManager() = default;

    static std::string canonicalizeProjectRoot(const std::string& projectRoot);
    static std::string makePoolKey(const std::string& specId, const std::string& canonicalProjectRoot);
    static std::string shellCommandForArgs(const std::vector<std::string>& command);
    static std::string jdtlsDataRoot();
    static std::string projectNameForPath(const std::string& path);

    std::unique_ptr<ServerInstance> spawnServer(const LspServerSpec& spec,
                                                const std::string& canonicalProjectRoot,
                                                int initTimeoutMs) const;
    bool refreshHealthLocked(ServerInstance& instance);
    std::unique_ptr<ServerInstance> extractServerLocked(const std::string& poolKey);
    void shutdownServerInstance(std::unique_ptr<ServerInstance> instance) const;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<ServerInstance>> m_pool;
    std::shared_ptr<firmius::shared::IHost> m_host;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_LSP_SERVER_MANAGER_HPP
