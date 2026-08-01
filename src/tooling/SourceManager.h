#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace luna::tooling {

// LSP positions are zero-based and count UTF-16 code units, independently of
// the compiler's UTF-8 byte-oriented source spans.
struct Utf16Position {
    uint32_t line = 0;
    uint32_t character = 0;

    bool operator==(const Utf16Position& other) const {
        return line == other.line && character == other.character;
    }
};

class SourceDocument {
public:
    SourceDocument(std::string id, std::string text, int64_t version);

    const std::string& id() const { return mId; }
    const std::string& text() const { return mText; }
    int64_t version() const { return mVersion; }
    size_t lineCount() const { return mLineStarts.size(); }

    std::optional<size_t> byteOffset(Utf16Position position) const;
    std::optional<Utf16Position> utf16Position(size_t byteOffset) const;
    std::optional<std::string_view> lineText(uint32_t line) const;

private:
    size_t lineContentEnd(size_t line) const;

    std::string mId;
    std::string mText;
    int64_t mVersion = 0;
    std::vector<size_t> mLineStarts;
};

class SourceManager {
public:
    bool open(std::string id, std::string text, int64_t version);
    bool update(const std::string& id, std::string text, int64_t version);
    bool close(const std::string& id);

    const SourceDocument* find(const std::string& id) const;
    size_t documentCount() const { return mDocuments.size(); }

private:
    std::unordered_map<std::string, SourceDocument> mDocuments;
};

} // namespace luna::tooling
