#include "tooling/SourceManager.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main() {
    luna::tooling::SourceManager sources;
    const std::string id = "file:///workspace/main.luna";
    const std::string text = "A\xf0\x9f\x98\x80\xe4\xb8\xad\nx\r\n";

    if (!expect(sources.open(id, text, 1), "initial open failed") ||
        !expect(!sources.open(id, text, 1), "duplicate open was accepted") ||
        !expect(sources.documentCount() == 1, "unexpected document count"))
        return 1;

    const auto* document = sources.find(id);
    if (!expect(document != nullptr, "opened document is missing") ||
        !expect(document->lineCount() == 3, "line index is incorrect") ||
        !expect(document->lineText(0) == std::optional<std::string_view>(
            "A\xf0\x9f\x98\x80\xe4\xb8\xad"),
            "UTF-8 line content is incorrect") ||
        !expect(document->lineText(1) == std::optional<std::string_view>("x"),
            "CRLF line content is incorrect"))
        return 2;

    const auto afterAscii = document->byteOffset({0, 1});
    const auto insideSurrogatePair = document->byteOffset({0, 2});
    const auto afterEmoji = document->byteOffset({0, 3});
    const auto afterChinese = document->byteOffset({0, 4});
    if (!expect(afterAscii == std::optional<size_t>(1),
                "ASCII UTF-16 conversion failed") ||
        !expect(!insideSurrogatePair,
                "position inside a surrogate pair was accepted") ||
        !expect(afterEmoji == std::optional<size_t>(5),
                "non-BMP UTF-16 conversion failed") ||
        !expect(afterChinese == std::optional<size_t>(8),
                "multibyte UTF-8 conversion failed") ||
        !expect(document->utf16Position(5) ==
                    std::optional<luna::tooling::Utf16Position>({0, 3}),
                "byte-to-UTF-16 conversion failed") ||
        !expect(!document->utf16Position(3),
                "position inside a UTF-8 scalar was accepted") ||
        !expect(document->byteOffset({1, 1}) == std::optional<size_t>(10),
                "CRLF line offset is incorrect"))
        return 3;

    if (!expect(!sources.update(id, "stale", 1),
                "stale document update was accepted") ||
        !expect(sources.update(id, "let value = 1;\n", 2),
                "new document update was rejected"))
        return 4;

    document = sources.find(id);
    if (!expect(document != nullptr && document->version() == 2,
                "updated document version is incorrect") ||
        !expect(document->text() == "let value = 1;\n",
                "updated in-memory text is incorrect") ||
        !expect(sources.close(id), "document close failed") ||
        !expect(sources.find(id) == nullptr, "closed document remains visible") ||
        !expect(!sources.close(id), "duplicate close was accepted"))
        return 5;

    return 0;
}
