#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace luna::instantiation {

struct Request {
    std::string genericDeclarationId;
    std::vector<std::string> typeArguments;
    std::vector<std::string> valueArguments;
    std::vector<std::string> metadataArguments;
    std::string requestedBy;
};

enum class State {
    New,
    InProgress,
    Ready,
    Failed,
};

struct Entry {
    std::string key;
    std::string instanceId;
    State state = State::New;
    std::vector<std::string> requestSites;
    std::string failure;
};

class Instantiator {
public:
    void reset();

    // begin() provides deterministic caching and recursion detection. The
    // frontend-specific declaration cloner supplies the concrete body, then
    // calls complete() or fail().
    const Entry& begin(const Request& request);
    bool complete(const std::string& key);
    bool fail(const std::string& key, std::string reason);
    const Entry* lookup(const Request& request) const;
    const Entry* lookupKey(const std::string& key) const;

    static std::string keyFor(const Request& request);
    static std::string instanceIdFor(const Request& request);

private:
    std::unordered_map<std::string, Entry> mEntries;
};

} // namespace luna::instantiation
