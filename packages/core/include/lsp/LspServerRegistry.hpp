#ifndef FIRMIUS_CORE_LSPSERVERREGISTRY_HPP
#define FIRMIUS_CORE_LSPSERVERREGISTRY_HPP

#include <string>
#include <vector>
#include <mutex>
#include <lsp/LspServerSpec.hpp>

namespace firmius::core {

class LspServerRegistry {
public:
    static LspServerRegistry& instance();

    const LspServerSpec* findByPath(const std::string& filePath) const;
    const LspServerSpec* findByExtension(const std::string& ext) const;
    const LspServerSpec* findById(const std::string& id) const;
    void registerCustomSpec(LspServerSpec spec);
    std::vector<std::string> listIds() const;

    // Walk up from startPath checking for marker files/dirs,
    // return first dir containing a marker or startPath itself
    static std::string detectRoot(const std::string& startPath,
                                  const std::vector<std::string>& markers);

private:
    LspServerRegistry();
    ~LspServerRegistry() = default;

    LspServerRegistry(const LspServerRegistry&) = delete;
    LspServerRegistry& operator=(const LspServerRegistry&) = delete;

    void registerBuiltinSpecs();

    std::vector<LspServerSpec> specs_;
    mutable std::mutex mutex_;
};

} // namespace firmius::core

#endif // FIRMIUS_CORE_LSPSERVERREGISTRY_HPP
