#ifndef FIRMIUS_CORE_LSP_PROTOCOL_HPP
#define FIRMIUS_CORE_LSP_PROTOCOL_HPP

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <map>


namespace firmius::core {

// --- Enums ---

enum class DiagnosticSeverity {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4
};

enum class SymbolKind {
    File = 1, Module = 2, Namespace = 3, Package = 4, Class = 5, Method = 6,
    Property = 7, Field = 8, Constructor = 9, Enum = 10, Interface = 11,
    Function = 12, Variable = 13, Constant = 14, String = 15, Number = 16,
    Boolean = 17, Array = 18, Object = 19, Key = 20, Null = 21, EnumMember = 22,
    Struct = 23, Event = 24, Operator = 25, TypeParameter = 26
};

// --- Structs ---

struct Position {
    int line;      // 0-based
    int character; // 0-based

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("line");
        writer.Int(line);
        writer.Key("character");
        writer.Int(character);
        writer.EndObject();
    }
};

struct Range {
    Position start;
    Position end;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("start");
        start.Serialize(writer);
        writer.Key("end");
        end.Serialize(writer);
        writer.EndObject();
    }
};

struct Location {
    std::string uri;
    Range range;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("uri");
        writer.String(uri.c_str());
        writer.Key("range");
        range.Serialize(writer);
        writer.EndObject();
    }
};

struct TextDocumentIdentifier {
    std::string uri;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("uri");
        writer.String(uri.c_str());
        writer.EndObject();
    }
};

struct TextDocumentPositionParams {
    TextDocumentIdentifier textDocument;
    Position position;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("textDocument");
        textDocument.Serialize(writer);
        writer.Key("position");
        position.Serialize(writer);
        writer.EndObject();
    }
};

struct Diagnostic {
    Range range;
    std::optional<DiagnosticSeverity> severity;
    std::optional<std::variant<int, std::string>> code;
    std::optional<std::string> source;
    std::string message;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("range");
        range.Serialize(writer);
        if (severity) {
            writer.Key("severity");
            writer.Int(static_cast<int>(*severity));
        }
        if (code) {
            writer.Key("code");
            if (std::holds_alternative<int>(*code)) {
                writer.Int(std::get<int>(*code));
            } else {
                writer.String(std::get<std::string>(*code).c_str());
            }
        }
        if (source) {
            writer.Key("source");
            writer.String(source->c_str());
        }
        writer.Key("message");
        writer.String(message.c_str());
        writer.EndObject();
    }
};

struct DocumentSymbol {
    std::string name;
    std::optional<std::string> detail;
    SymbolKind kind;
    bool deprecated = false;
    Range range;
    Range selectionRange;
    std::vector<DocumentSymbol> children;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("name");
        writer.String(name.c_str());
        if (detail) {
            writer.Key("detail");
            writer.String(detail->c_str());
        }
        writer.Key("kind");
        writer.Int(static_cast<int>(kind));
        if (deprecated) {
            writer.Key("deprecated");
            writer.Bool(deprecated);
        }
        writer.Key("range");
        range.Serialize(writer);
        writer.Key("selectionRange");
        selectionRange.Serialize(writer);
        if (!children.empty()) {
            writer.Key("children");
            writer.StartArray();
            for (const auto& child : children) {
                child.Serialize(writer);
            }
            writer.EndArray();
        }
        writer.EndObject();
    }
};

struct SymbolInformation {
    std::string name;
    SymbolKind kind;
    bool deprecated = false;
    Location location;
    std::optional<std::string> containerName;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("name");
        writer.String(name.c_str());
        writer.Key("kind");
        writer.Int(static_cast<int>(kind));
        if (deprecated) {
            writer.Key("deprecated");
            writer.Bool(deprecated);
        }
        writer.Key("location");
        location.Serialize(writer);
        if (containerName) {
            writer.Key("containerName");
            writer.String(containerName->c_str());
        }
        writer.EndObject();
    }
};

struct Hover {
    std::variant<std::string, std::vector<std::string>> contents;
    std::optional<Range> range;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("contents");
        if (std::holds_alternative<std::string>(contents)) {
            writer.String(std::get<std::string>(contents).c_str());
        } else {
            writer.StartArray();
            for (const auto& s : std::get<std::vector<std::string>>(contents)) {
                writer.String(s.c_str());
            }
            writer.EndArray();
        }
        if (range) {
            writer.Key("range");
            range->Serialize(writer);
        }
        writer.EndObject();
    }
};

struct CallHierarchyItem {
    std::string name;
    SymbolKind kind;
    std::optional<std::string> detail;
    std::string uri;
    Range range;
    Range selectionRange;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("name");
        writer.String(name.c_str());
        writer.Key("kind");
        writer.Int(static_cast<int>(kind));
        if (detail) {
            writer.Key("detail");
            writer.String(detail->c_str());
        }
        writer.Key("uri");
        writer.String(uri.c_str());
        writer.Key("range");
        range.Serialize(writer);
        writer.Key("selectionRange");
        selectionRange.Serialize(writer);
        writer.EndObject();
    }
};

struct DiagnosticSummary {
    int files = 0;
    int errors = 0;
    int warnings = 0;
    int infos = 0;
    int hints = 0;

    template <typename Writer>
    void Serialize(Writer& writer) const {
        writer.StartObject();
        writer.Key("files");
        writer.Int(files);
        writer.Key("errors");
        writer.Int(errors);
        writer.Key("warnings");
        writer.Int(warnings);
        writer.Key("infos");
        writer.Int(infos);
        writer.Key("hints");
        writer.Int(hints);
        writer.EndObject();
    }
};

// --- Utilities ---

inline std::string fileUri(const std::string& path) {
    if (path.starts_with("file://")) return path;
    return "file://" + path;
}

inline std::string pathFromUri(const std::string& uri) {
    if (uri.starts_with("file://")) {
        return uri.substr(7);
    }
    return uri;
}

inline std::string severityName(int severity) {
    switch (static_cast<DiagnosticSeverity>(severity)) {
        case DiagnosticSeverity::Error: return "Error";
        case DiagnosticSeverity::Warning: return "Warning";
        case DiagnosticSeverity::Information: return "Information";
        case DiagnosticSeverity::Hint: return "Hint";
        default: return "Unknown";
    }
}

inline Position toZeroBased(int oneLine, int oneChar) {
    return {oneLine - 1, oneChar - 1};
}

inline std::pair<int, int> toOneBased(const Position& pos) {
    return {pos.line + 1, pos.character + 1};
}

} // namespace firmius::core

#endif // FIRMIUS_CORE_LSP_PROTOCOL_HPP
