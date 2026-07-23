#pragma once

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// A dependency-free renderer shared by every compiler pass. Error containers
// remain strings for compatibility, but their presentation is now consistent.
namespace diagnostic {

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

inline std::string format(const std::string& phase, const std::string& message,
                          const std::string& file = "", int line = 0, int col = 0,
                          const std::string& hint = "",
    const std::string& sourceLine = "") {
    std::ostringstream out;
    out << "error[" << phase << '/' << errorCode(phase, message) << "]: " << message;
    if (!file.empty() || line > 0) {
        out << "\n  --> " << (file.empty() ? "<input>" : file);
        if (line > 0) {
            out << ':' << line;
            if (col > 0) out << ':' << col;
        }
    }
    if (!sourceLine.empty()) {
        out << "\n   |\n" << line << " | " << sourceLine << "\n   | ";
        out << std::string(static_cast<size_t>(std::max(0, col - 1)), ' ') << '^';
    }
    if (!hint.empty()) out << "\n  help: " << hint;
    return out.str();
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

} // namespace diagnostic
