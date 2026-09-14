#pragma once

#include "common/Util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coverage_plugin {

// Global point key for one covered line: path hash in the high 32 bits and
// the line number in the low 32 bits, so points are unique across files.
inline std::uint64_t MakePoint(const std::string& path, std::uint32_t line) {
    return (static_cast<std::uint64_t>(common::Fnv1a32(path)) << 32) | line;
}

// Resolves a cobertura filename to the canonical path used for point keys
// (absolute paths are kept, relative ones are joined with the first source
// root). Shared by the client parser and the standalone merge parser so both
// count exactly the same lines.
std::string CanonicalPath(const std::string& filename,
                          const std::vector<std::string>& sources);

// Packs points into a little-endian byte blob (8 bytes each). Stable across
// the plugin and the standalone settle tool.
std::string PackPoints(const std::vector<std::uint64_t>& points);
std::vector<std::uint64_t> UnpackPoints(const std::string& data);

}  // namespace coverage_plugin
