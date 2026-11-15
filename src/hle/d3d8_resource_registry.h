#pragma once
#include <stdint.h>
#include <stddef.h>
#include <unordered_map>

struct HleResourceKey {
    uint32_t type, data, format, size;
    bool operator==(const HleResourceKey& b) const {
        return type == b.type && data == b.data && format == b.format && size == b.size;
    }
};
inline HleResourceKey hle_resource_key(uint32_t common, uint32_t data,
                                       uint32_t format = 0, uint32_t size = 0) {
    uint32_t type = common & 0x70000;
    bool pixel = type == 0x40000 || type == 0x50000;
    return {type, data, pixel ? format : 0, pixel ? size : 0};
}
// External synchronization; no guest pointers are retained/dereferenced here.
class HleResourceRegistry {
    struct Entry { HleResourceKey key; uint64_t identity; };
    std::unordered_map<uint32_t, Entry> entries;
    size_t capacity;
    uint64_t next = 0;
public:
    uint64_t retired = 0, evicted = 0;
    explicit HleResourceRegistry(size_t limit = 8192) : capacity(limit ? limit : 1) {}
    uint64_t identify(uint32_t object, HleResourceKey key, bool renew = false) {
        if (!object) return 0;
        auto it = entries.find(object);
        if (it != entries.end() && it->second.key == key && !renew) return it->second.identity;
        if (it == entries.end() && entries.size() >= capacity) { entries.erase(entries.begin()); ++evicted; }
        uint64_t id = ++next;
        entries[object] = {key, id};
        return id;
    }
    void retire(uint32_t object) { if (entries.erase(object)) ++retired; }
    size_t live() const { return entries.size(); }
    uint64_t created() const { return next; }
};
