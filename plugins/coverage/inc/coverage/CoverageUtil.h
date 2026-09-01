#pragma once

#include "common/Util.h"

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace coverage_plugin {

// Global point key for one covered line: path hash in the high 32 bits and
// the line number in the low 32 bits, so points are unique across files.
inline std::uint64_t MakePoint(const std::string& path, std::uint32_t line) {
    return (static_cast<std::uint64_t>(common::Fnv1a32(path)) << 32) | line;
}

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

// Packs points into a little-endian byte blob (8 bytes each). Stable across
// the plugin and the standalone settle tool.
std::string PackPoints(const std::vector<std::uint64_t>& points);
std::vector<std::uint64_t> UnpackPoints(const std::string& data);

// Greedy minimal-set selection: repeatedly pick the file that adds the most
// NEW points to *cumulative. *cumulative is both the running covered set and
// the result; it must already hold any previously-covered points (empty for a
// fresh global pass). Returns the selected files in selection order.
std::vector<std::string> SelectGreedyMinSet(
    const std::unordered_map<std::string, std::set<std::uint64_t>>& file_points,
    std::set<std::uint64_t>* cumulative);

// Merges .cov binaries into one cumulative cov plus a cobertura report using
// OpenCppCoverage. Each invocation is bounded to two input artifacts (binary
// tree) to stay under the Windows command-line limit. Fills out_cov/out_xml
// with the merged artifact paths on success.
bool MergeCoverageFiles(const std::string& opencppcoverage,
                        const std::vector<std::string>& cov_files,
                        const std::string& out_cov,
                        const std::string& out_xml);

}  // namespace coverage_plugin
