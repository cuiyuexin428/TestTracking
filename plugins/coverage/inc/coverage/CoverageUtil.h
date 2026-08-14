#pragma once

#include <cstdint>
#include <string>

namespace coverage_plugin {

inline std::uint64_t Fnv1a64(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::uint32_t Fnv1a32(const std::string& text) {
    std::uint32_t hash = 2166136261U;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619U;
    }
    return hash;
}

// Global point key for one covered line: path hash in the high 32 bits and
// the line number in the low 32 bits, so points are unique across files.
inline std::uint64_t MakePoint(const std::string& path, std::uint32_t line) {
    return (static_cast<std::uint64_t>(Fnv1a32(path)) << 32) | line;
}

}  // namespace coverage_plugin
