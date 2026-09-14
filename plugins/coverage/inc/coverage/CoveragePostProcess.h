#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Only linked into the standalone post-processor, never the plugin DLL.
namespace coverage_plugin {

// Parses the totals of a merged cobertura report produced by
// OpenCppCoverage. OpenCppCoverage exports one <class> per compilation unit,
// so template/header lines are repeated; the counts are deduplicated by
// (canonical file, line) to match the plugin's point-set semantics. Returns
// false when the file is missing or malformed.
bool ParseCoverageTotals(const std::string& xml_path, std::int64_t* covered_lines,
                         std::int64_t* total_lines);

// Greedy cover selection, not a guaranteed global minimum: pick the most
// NEW points to *cumulative. *cumulative is both the running covered set and
// the result; it must already hold any previously-covered points (empty for a
// fresh global pass). Ties use the file name for reproducible results.
// Returns the selected files in selection order.
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
