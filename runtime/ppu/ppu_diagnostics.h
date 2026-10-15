#pragma once
#include <stdint.h>
#include <stdio.h>
#include <mutex>
#include <string>
#include <unordered_map>

static inline uint64_t ppu_diag_hit(const std::string& key)
{
    static std::mutex lock;
    static std::unordered_map<std::string, uint64_t> counts;
    std::lock_guard<std::mutex> guard(lock);
    auto found = counts.find(key);
    if (found == counts.end() && counts.size() >= 4096) {
        fprintf(stderr, "[diagnostic] key table full; event is unsampled: %s\n", key.c_str());
        return 1;
    }
    uint64_t count = ++counts[key];
    return count <= 3 || (count & (count - 1)) == 0 ? count : 0;
}
