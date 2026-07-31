#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

// A dependency-free diagnostic record and renderer shared by every compiler
// pass. Human rendering and machine serialization consume the same data.
namespace diagnostic {

struct Diagnostic {
    std::string severity = "error";
    std::string phase;
    std::string code;
    std::string message;
    std::string file;
    int line = 0;
    int col = 0;
    std::string hint;
    std::string sourceLine;
    std::optional<size_t> startByte;
    std::optional<size_t> endByte;
    int endLine = 0;
    int endCol = 0;
};

// Keep the human-readable message free to evolve while giving users, tests,
// and editor integrations a stable machine-searchable error identity.  The
// final `9999` code in each phase is an intentional catch-all for diagnostics
// that have not yet earned a more specific public category.
inline std::string errorCode(const std::string& phase, const std::string& message) {
    const auto has = [&message](const char* text) {
        return message.find(text) != std::string::npos;
    };
    if (phase == "lex")
        return has("Unexpected character") ? "LEX0001" : "LEX9999";
    if (phase == "parse") {
        if (has("expected") || has("Expected")) return "PAR0001";
        return "PAR9999";
    }
    if (phase == "package") {
        if (has("does not exist") || has("cannot read")) return "PKG0001";
        if (has("contains no .luna")) return "PKG0002";
        if (has("does not match")) return "PKG0003";
        return "PKG9999";
    }
    if (phase == "semantic") {
        if (has("undefined name")) return "SEM0001";
        if (has("Type constraint failed") || has("constraint '"))
            return "SEM0002";
        if (has("may finish without returning")) return "SEM0003";
        if (has("FFI") || has("ABI") || has("extern function")) return "SEM0101";
        if (has("selector") || has("metadata") || has("declaration family"))
            return "SEM0201";
        if (has("trait")) return "SEM0301";
        if (has("kernel")) return "SEM0401";
        return "SEM9999";
    }
    if (phase == "trait") return "TRT0001";
    if (phase == "driver") {
        if (has("runtime library")) return "DRV0001";
        if (has("AOT linker")) return "DRV0002";
        if (has("optimization level")) return "DRV0003";
        return "DRV9999";
    }
    if (phase == "ownership") {
        if (has("control-flow") || has("paths through `if`") || has("loop body changes"))
            return "OWN0201";
        if (has("after move") || has("Use-after-move")) return "OWN0001";
        if (has("after free") || has("Use-after-free")) return "OWN0002";
        if (has("borrow")) return "OWN0003";
        if (has("launch event") || has("in flight") || has("device buffer"))
            return "OWN0101";
        if (has("Linear variable") || has("linear resource")) return "OWN0004";
        return "OWN9999";
    }
    if (phase == "codegen") {
        if (has("invalid host LLVM IR")) return "CGN0001";
        if (has("CUDA") || has("NVPTX")) return "CGN0101";
        if (has("ROCm") || has("AMDGPU") || has("HSA") || has("LLD")) return "CGN0102";
        return "CGN9999";
    }
    return "GEN9999";
}

inline std::string render(const Diagnostic& diagnostic) {
    std::ostringstream out;
    out << diagnostic.severity << '[' << diagnostic.phase << '/'
        << diagnostic.code << "]: " << diagnostic.message;
    if (!diagnostic.file.empty() || diagnostic.line > 0) {
        out << "\n  --> "
            << (diagnostic.file.empty() ? "<input>" : diagnostic.file);
        if (diagnostic.line > 0) {
            out << ':' << diagnostic.line;
            if (diagnostic.col > 0) out << ':' << diagnostic.col;
        }
    }
    if (!diagnostic.sourceLine.empty()) {
        out << "\n   |\n" << diagnostic.line << " | "
            << diagnostic.sourceLine << "\n   | ";
        out << std::string(
            static_cast<size_t>(std::max(0, diagnostic.col - 1)), ' ') << '^';
    }
    if (!diagnostic.hint.empty()) out << "\n  help: " << diagnostic.hint;
    return out.str();
}

inline std::ostream& operator<<(std::ostream& out, const Diagnostic& diagnostic) {
    return out << render(diagnostic);
}

inline std::string quotedToken(const std::string& lexeme) {
    return lexeme.empty() ? "end of file" : "'" + lexeme + "'";
}

// Semantic and ownership passes receive source locations through AST nodes,
// rather than the original input buffer. Cache source lines here so their
// diagnostics can retain the same source excerpt already provided by lexer
// and parser errors. Pseudo-files such as <repl> intentionally have no disk
// lookup and continue to render location-only diagnostics.
inline std::string sourceLineFromFile(const std::string& file, int line) {
    if (file.empty() || line <= 0 || (file.front() == '<' && file.back() == '>'))
        return "";
    static std::unordered_map<std::string, std::vector<std::string>> cache;
    auto cached = cache.find(file);
    if (cached == cache.end()) {
        std::ifstream input(file);
        if (!input) return "";
        std::vector<std::string> lines;
        std::string sourceLine;
        while (std::getline(input, sourceLine)) lines.push_back(std::move(sourceLine));
        cached = cache.emplace(file, std::move(lines)).first;
    }
    const auto& lines = cached->second;
    const size_t index = static_cast<size_t>(line - 1);
    return index < lines.size() ? lines[index] : "";
}

inline std::optional<size_t> byteOffsetFromFile(
    const std::string& file, int line, int col) {
    if (file.empty() || line <= 0 || col <= 0 ||
        (file.front() == '<' && file.back() == '>'))
        return std::nullopt;
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    size_t offset = 0;
    int currentLine = 1;
    char character = '\0';
    while (currentLine < line && input.get(character)) {
        ++offset;
        if (character == '\n') ++currentLine;
    }
    if (currentLine != line) return std::nullopt;
    const size_t columnOffset = static_cast<size_t>(col - 1);
    for (size_t index = 0; index < columnOffset; ++index) {
        if (!input.get(character) || character == '\n') return std::nullopt;
        ++offset;
    }
    return offset;
}

inline size_t highlightedByteLength(const std::string& sourceLine, int col) {
    if (col <= 0) return 0;
    const size_t index = static_cast<size_t>(col - 1);
    if (index >= sourceLine.size()) return 0;
    const unsigned char lead = static_cast<unsigned char>(sourceLine[index]);
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xe0U) == 0xc0U) return 2;
    if ((lead & 0xf0U) == 0xe0U) return 3;
    if ((lead & 0xf8U) == 0xf0U) return 4;
    return 1;
}

inline Diagnostic format(const std::string& phase, const std::string& message,
                         const std::string& file = "", int line = 0, int col = 0,
                         const std::string& hint = "",
                         const std::string& sourceLine = "") {
    Diagnostic result;
    result.phase = phase;
    result.code = errorCode(phase, message);
    result.message = message;
    result.file = file;
    result.line = line;
    result.col = col;
    result.hint = hint;
    result.sourceLine = sourceLine;
    result.startByte = byteOffsetFromFile(file, line, col);
    if (result.startByte) {
        const size_t byteLength = highlightedByteLength(sourceLine, col);
        result.endByte = *result.startByte + byteLength;
        result.endLine = line;
        result.endCol = col + static_cast<int>(byteLength);
    }
    return result;
}

inline std::string jsonEscape(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20U) {
                    result += "\\u00";
                    result += hex[(character >> 4U) & 0x0fU];
                    result += hex[character & 0x0fU];
                } else {
                    result += static_cast<char>(character);
                }
        }
    }
    return result;
}

inline std::string normalizedPath(const std::string& file) {
    if (file.empty() || (file.front() == '<' && file.back() == '>')) return file;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(file, error);
    return error ? file : absolute.lexically_normal().string();
}

inline std::string toJson(const Diagnostic& diagnostic) {
    std::ostringstream out;
    out << "{\"protocol\":\"luna.diagnostic\",\"version\":1,"
        << "\"kind\":\"diagnostic\",\"severity\":\""
        << jsonEscape(diagnostic.severity) << "\",\"phase\":\""
        << jsonEscape(diagnostic.phase) << "\",\"code\":\""
        << jsonEscape(diagnostic.code) << "\",\"message\":\""
        << jsonEscape(diagnostic.message) << "\",\"primary\":";
    if (diagnostic.startByte && diagnostic.endByte && !diagnostic.file.empty()) {
        out << "{\"path\":\"" << jsonEscape(normalizedPath(diagnostic.file))
            << "\",\"start\":{\"byte\":" << *diagnostic.startByte
            << ",\"line\":" << diagnostic.line
            << ",\"column\":" << diagnostic.col
            << "},\"end\":{\"byte\":" << *diagnostic.endByte
            << ",\"line\":" << diagnostic.endLine
            << ",\"column\":" << diagnostic.endCol << "}}";
    } else {
        out << "null";
    }
    out << ",\"labels\":[],\"notes\":[";
    if (!diagnostic.hint.empty())
        out << '\"' << jsonEscape(diagnostic.hint) << '\"';
    out << "],\"fixes\":[]}";
    return out.str();
}

} // namespace diagnostic
