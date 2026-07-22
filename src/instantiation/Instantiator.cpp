#include "Instantiator.h"

#include <cstdint>
#include <iomanip>
#include <sstream>

namespace luna::instantiation {

namespace {

void appendPart(std::string& output, const std::string& part) {
    output += std::to_string(part.size());
    output += ':';
    output += part;
    output += ';';
}

uint64_t stableHash(const std::string& value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

void Instantiator::reset() {
    mEntries.clear();
}

const Entry& Instantiator::begin(const Request& request) {
    const auto key = keyFor(request);
    auto [iterator, inserted] = mEntries.emplace(key, Entry{});
    auto& entry = iterator->second;
    if (inserted) {
        entry.key = key;
        entry.instanceId = instanceIdFor(request);
        entry.state = State::InProgress;
    }
    if (!request.requestedBy.empty())
        entry.requestSites.push_back(request.requestedBy);
    return entry;
}

bool Instantiator::complete(const std::string& key) {
    auto iterator = mEntries.find(key);
    if (iterator == mEntries.end() || iterator->second.state != State::InProgress)
        return false;
    iterator->second.state = State::Ready;
    return true;
}

bool Instantiator::fail(const std::string& key, std::string reason) {
    auto iterator = mEntries.find(key);
    if (iterator == mEntries.end()) return false;
    iterator->second.state = State::Failed;
    iterator->second.failure = std::move(reason);
    return true;
}

const Entry* Instantiator::lookup(const Request& request) const {
    return lookupKey(keyFor(request));
}

const Entry* Instantiator::lookupKey(const std::string& key) const {
    auto iterator = mEntries.find(key);
    return iterator == mEntries.end() ? nullptr : &iterator->second;
}

std::string Instantiator::keyFor(const Request& request) {
    std::string key;
    appendPart(key, request.genericDeclarationId);
    key += "T";
    for (const auto& argument : request.typeArguments) appendPart(key, argument);
    key += "V";
    for (const auto& argument : request.valueArguments) appendPart(key, argument);
    key += "M";
    for (const auto& argument : request.metadataArguments) appendPart(key, argument);
    return key;
}

std::string Instantiator::instanceIdFor(const Request& request) {
    std::ostringstream output;
    output << "__moon_inst_" << std::hex
           << std::setw(16) << std::setfill('0') << stableHash(keyFor(request));
    return output.str();
}

} // namespace luna::instantiation
