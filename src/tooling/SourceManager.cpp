#include "tooling/SourceManager.h"

#include <algorithm>
#include <utility>

namespace luna::tooling {
namespace {

struct DecodedScalar {
    uint32_t value;
    size_t bytes;
};

DecodedScalar decodeUtf8(const std::string& text, size_t offset, size_t limit) {
    const auto byte = [&text](size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    const auto continuation = [&byte, limit](size_t index) {
        return index < limit && (byte(index) & 0xc0U) == 0x80U;
    };

    const unsigned char lead = byte(offset);
    if (lead < 0x80U) return {lead, 1};

    if (lead >= 0xc2U && lead <= 0xdfU && continuation(offset + 1)) {
        return {
            static_cast<uint32_t>(((lead & 0x1fU) << 6U) |
                                  (byte(offset + 1) & 0x3fU)),
            2};
    }
    if (lead >= 0xe0U && lead <= 0xefU &&
        continuation(offset + 1) && continuation(offset + 2)) {
        const unsigned char second = byte(offset + 1);
        if ((lead != 0xe0U || second >= 0xa0U) &&
            (lead != 0xedU || second < 0xa0U)) {
            return {
                static_cast<uint32_t>(((lead & 0x0fU) << 12U) |
                                      ((second & 0x3fU) << 6U) |
                                      (byte(offset + 2) & 0x3fU)),
                3};
        }
    }
    if (lead >= 0xf0U && lead <= 0xf4U &&
        continuation(offset + 1) && continuation(offset + 2) &&
        continuation(offset + 3)) {
        const unsigned char second = byte(offset + 1);
        if ((lead != 0xf0U || second >= 0x90U) &&
            (lead != 0xf4U || second < 0x90U)) {
            return {
                static_cast<uint32_t>(((lead & 0x07U) << 18U) |
                                      ((second & 0x3fU) << 12U) |
                                      ((byte(offset + 2) & 0x3fU) << 6U) |
                                      (byte(offset + 3) & 0x3fU)),
                4};
        }
    }

    // Treat malformed input as one replacement character. The lexer remains
    // responsible for diagnosing invalid source bytes; tooling must still be
    // able to navigate the surrounding document deterministically.
    return {0xfffdU, 1};
}

uint32_t utf16Width(uint32_t scalar) {
    return scalar > 0xffffU ? 2U : 1U;
}

} // namespace

SourceDocument::SourceDocument(
    std::string id, std::string text, int64_t version)
    : mId(std::move(id)), mText(std::move(text)), mVersion(version) {
    mLineStarts.push_back(0);
    for (size_t index = 0; index < mText.size(); ++index) {
        if (mText[index] == '\n') mLineStarts.push_back(index + 1);
    }
}

size_t SourceDocument::lineContentEnd(size_t line) const {
    size_t end = line + 1 < mLineStarts.size()
        ? mLineStarts[line + 1] - 1
        : mText.size();
    if (end > mLineStarts[line] && mText[end - 1] == '\r') --end;
    return end;
}

std::optional<size_t> SourceDocument::byteOffset(
    Utf16Position position) const {
    if (position.line >= mLineStarts.size()) return std::nullopt;
    const size_t start = mLineStarts[position.line];
    const size_t end = lineContentEnd(position.line);
    size_t offset = start;
    uint32_t character = 0;
    while (offset < end) {
        if (character == position.character) return offset;
        const auto scalar = decodeUtf8(mText, offset, end);
        const uint32_t width = utf16Width(scalar.value);
        if (character + width > position.character) return std::nullopt;
        character += width;
        offset += scalar.bytes;
    }
    return character == position.character
        ? std::optional<size_t>(end) : std::nullopt;
}

std::optional<Utf16Position> SourceDocument::utf16Position(
    size_t byteOffset) const {
    if (byteOffset > mText.size()) return std::nullopt;
    const auto upper = std::upper_bound(
        mLineStarts.begin(), mLineStarts.end(), byteOffset);
    const size_t line = static_cast<size_t>(
        std::distance(mLineStarts.begin(), upper) - 1);
    const size_t start = mLineStarts[line];
    const size_t end = lineContentEnd(line);
    if (byteOffset > end) return std::nullopt;

    size_t offset = start;
    uint32_t character = 0;
    while (offset < byteOffset) {
        const auto scalar = decodeUtf8(mText, offset, end);
        if (offset + scalar.bytes > byteOffset) return std::nullopt;
        character += utf16Width(scalar.value);
        offset += scalar.bytes;
    }
    return Utf16Position{static_cast<uint32_t>(line), character};
}

std::optional<std::string_view> SourceDocument::lineText(uint32_t line) const {
    if (line >= mLineStarts.size()) return std::nullopt;
    const size_t start = mLineStarts[line];
    return std::string_view(mText.data() + start, lineContentEnd(line) - start);
}

bool SourceManager::open(
    std::string id, std::string text, int64_t version) {
    const std::string key = id;
    return mDocuments.emplace(
        key, SourceDocument(std::move(id), std::move(text), version)).second;
}

bool SourceManager::update(
    const std::string& id, std::string text, int64_t version) {
    auto document = mDocuments.find(id);
    if (document == mDocuments.end() || version <= document->second.version())
        return false;
    document->second = SourceDocument(id, std::move(text), version);
    return true;
}

bool SourceManager::close(const std::string& id) {
    return mDocuments.erase(id) == 1;
}

const SourceDocument* SourceManager::find(const std::string& id) const {
    const auto document = mDocuments.find(id);
    return document == mDocuments.end() ? nullptr : &document->second;
}

} // namespace luna::tooling
