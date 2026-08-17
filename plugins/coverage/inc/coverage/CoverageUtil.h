#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

// Runs a command line and returns its process exit code. On Windows the
// process is created without a console window; -1 means the process could
// not be started.
int RunCommand(const std::string& command);

// Resolves a cobertura filename to the canonical path used for point keys
// (absolute paths are kept, relative ones are joined with the first source
// root). Shared by the client parser and the scheduler merge parser so both
// count exactly the same lines.
std::string CanonicalPath(const std::string& filename,
                          const std::vector<std::string>& sources);

// Parses the totals of a merged cobertura report produced by
// OpenCppCoverage. OpenCppCoverage exports one <class> per compilation unit,
// so template/header lines are repeated; the counts are deduplicated by
// (canonical file, line) to match the plugin's point-set semantics. Returns
// false when the file is missing or malformed.
bool ParseCoverageTotals(const std::string& xml_path, std::int64_t* covered_lines,
                         std::int64_t* total_lines);

}  // namespace coverage_plugin
