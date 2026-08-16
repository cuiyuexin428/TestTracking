#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace coverage_plugin {

// Storage interface for per-wave coverage staging and selected sets. The
// client writes per-file point sets; the scheduler reads them at settlement.
// Current implementation persists to files; a PostgreSQL/ODBC implementation
// can replace it without touching the plugin logic.
class CoverageStore {
public:
    virtual ~CoverageStore() = default;

    // Reads plugin config (e.g. store location / connection string).
    virtual bool init(const std::string& config) = 0;

    // Client side: persists one file's covered point set into the wave
    // staging. Points must be globally unique (see CoverageUtil::MakePoint).
    virtual void saveFileCoverage(const std::string& run_id, int wave,
                                  const std::string& file,
                                  const std::vector<std::uint64_t>& points) = 0;

    // Client side: persists one file's OpenCppCoverage .cov binary (optional
    // artifact kept for audit and alternative total-coverage computation).
    virtual void saveFileCov(const std::string& run_id, int wave, const std::string& file,
                             const std::string& cov_data, std::int64_t size_bytes) = 0;

    // Scheduler side: loads all files' coverage point sets of a wave.
    virtual std::unordered_map<std::string, std::set<std::uint64_t>> loadWave(
        const std::string& run_id, int wave) = 0;

    // Removes the wave staging after settlement.
    virtual void clearWave(const std::string& run_id, int wave) = 0;

    // Appends files to the run's selected set.
    virtual void appendSelected(const std::string& run_id,
                                const std::vector<std::string>& files) = 0;

    // Scheduler side: persists the cumulative point set after each wave
    // settlement so the next wave starts from the same coverage.
    virtual void saveCumulative(const std::string& run_id, int wave,
                                const std::vector<std::uint64_t>& points) = 0;

    // Scheduler side: upserts the run summary (total/selected/cumulative).
    virtual void saveRunSummary(const std::string& run_id, int total_files,
                                int selected_files, std::int64_t cumulative_points) = 0;
};

// Creates the default (file-based) store implementation.
// Selects the backend from config: "store_type" == "postgres" uses libpq,
// anything else uses local files.
std::unique_ptr<CoverageStore> CreateCoverageStore(const std::string& config);

}  // namespace coverage_plugin
