#include <lsp/LspServerRegistry.hpp>
#include <filesystem>

namespace firmius::core {

LspServerRegistry& LspServerRegistry::instance() {
    static LspServerRegistry registry;
    return registry;
}

LspServerRegistry::LspServerRegistry() {
    registerBuiltinSpecs();
}

void LspServerRegistry::registerBuiltinSpecs() {
    // 1. Python
    {
        LspServerSpec spec;
        spec.id = "python";
        spec.extensions = {".py", ".pyi"};
        spec.markers = {"pyproject.toml", "setup.py", "setup.cfg", "requirements.txt", ".git"};
        spec.commands = {
            {"basedpyright-langserver", "--stdio"},
            {"pyright-langserver", "--stdio"},
            {"uvx", "--from", "basedpyright", "basedpyright-langserver", "--stdio"}
        };
        spec.defaultLanguageId = "python";
        specs_.push_back(std::move(spec));
    }

    // 2. Clangd (C/C++)
    {
        LspServerSpec spec;
        spec.id = "clangd";
        spec.extensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"};
        spec.markers = {"compile_commands.json", "compile_flags.txt", ".clangd", ".git"};
        spec.commands = {
            {"clangd", "--background-index", "--clang-tidy"}
        };
        spec.languageIds = {
            {".c", "c"}
        };
        spec.defaultLanguageId = "cpp";
        specs_.push_back(std::move(spec));
    }

    // 3. Rust
    {
        LspServerSpec spec;
        spec.id = "rust";
        spec.extensions = {".rs"};
        spec.markers = {"Cargo.toml", "rust-project.json", ".git"};
        spec.commands = {
            {"rust-analyzer"}
        };
        spec.defaultLanguageId = "rust";
        specs_.push_back(std::move(spec));
    }

    // 4. Go
    {
        LspServerSpec spec;
        spec.id = "go";
        spec.extensions = {".go"};
        spec.markers = {"go.work", "go.mod", ".git"};
        spec.commands = {
            {"gopls"}
        };
        spec.defaultLanguageId = "go";
        specs_.push_back(std::move(spec));
    }

    // 5. TypeScript/JavaScript
    {
        LspServerSpec spec;
        spec.id = "typescript";
        spec.extensions = {".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".mts", ".cts"};
        spec.markers = {"tsconfig.json", "jsconfig.json", "package.json",
                        "pnpm-lock.yaml", "package-lock.json", "bun.lockb", "bun.lock", ".git"};
        spec.commands = {
            {"typescript-language-server", "--stdio"},
            {"npx", "--yes", "typescript-language-server", "--stdio"}
        };
        spec.languageIds = {
            {".ts", "typescript"},
            {".tsx", "typescriptreact"},
            {".js", "javascript"},
            {".jsx", "javascriptreact"},
            {".mjs", "javascript"},
            {".cjs", "javascript"},
            {".mts", "typescript"},
            {".cts", "typescript"}
        };
        spec.defaultLanguageId = "typescript";
        specs_.push_back(std::move(spec));
    }

    // 6. Java
    {
        LspServerSpec spec;
        spec.id = "java";
        spec.extensions = {".java"};
        spec.markers = {"pom.xml", "build.gradle", "settings.gradle", ".git"};
        spec.commands = {
            {"jdtls"}
        };
        spec.defaultLanguageId = "java";
        specs_.push_back(std::move(spec));
    }

    // 7. Bash/Shell
    {
        LspServerSpec spec;
        spec.id = "bash";
        spec.extensions = {".sh", ".bash", ".zsh", ".ksh"};
        spec.markers = {".git"};
        spec.commands = {
            {"bash-language-server", "start"},
            {"npx", "--yes", "bash-language-server", "start"}
        };
        spec.defaultLanguageId = "shellscript";
        specs_.push_back(std::move(spec));
    }
}

const LspServerSpec* LspServerRegistry::findByPath(const std::string& filePath) const {
    auto ext = std::filesystem::path(filePath).extension().string();
    if (ext.empty()) {
        return nullptr;
    }
    return findByExtension(ext);
}

const LspServerSpec* LspServerRegistry::findByExtension(const std::string& ext) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& spec : specs_) {
        for (const auto& e : spec.extensions) {
            if (e == ext) {
                return &spec;
            }
        }
    }
    return nullptr;
}

const LspServerSpec* LspServerRegistry::findById(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& spec : specs_) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

void LspServerRegistry::registerCustomSpec(LspServerSpec spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    spec.isCustom = true;
    // Replace if already exists with the same id
    for (auto& existing : specs_) {
        if (existing.id == spec.id) {
            existing = std::move(spec);
            return;
        }
    }
    specs_.push_back(std::move(spec));
}

std::vector<std::string> LspServerRegistry::listIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(specs_.size());
    for (const auto& spec : specs_) {
        ids.push_back(spec.id);
    }
    return ids;
}

std::string LspServerRegistry::detectRoot(const std::string& startPath,
                                           const std::vector<std::string>& markers) {
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::path current = fs::canonical(startPath, ec);
    if (ec) {
        // If canonical fails, use the path as-is
        current = fs::path(startPath);
    }

    // If startPath is a file, begin from its parent directory
    if (fs::is_regular_file(current, ec)) {
        current = current.parent_path();
    }

    // Remember the starting directory for fallback
    std::string startDir = current.string();

    // Walk up the directory tree
    while (true) {
        for (const auto& marker : markers) {
            fs::path candidate = current / marker;
            if (fs::exists(candidate, ec)) {
                return current.string();
            }
        }

        fs::path parent = current.parent_path();
        if (parent == current) {
            // Reached filesystem root
            break;
        }
        current = parent;
    }

    return startDir;
}

} // namespace firmius::core
