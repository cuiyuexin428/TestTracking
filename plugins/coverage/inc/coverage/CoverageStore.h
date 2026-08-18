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
// Implementations: FileCoverageStore (all platforms) and PostgresCoverageStore
// (Windows + libpq). Both implement the same interface, keeping plugin logic backend-independent.
class CoverageStore {
public:
    virtual ~CoverageStore() = default;

    // Reads plugin config (e.g. store location / connection string).
    virtual bool init(const std::string& config) = 0;

    // Client side: persists one file's covered point set into the wave
    // staging. Points must be globally unique (see CoverageUtil::MakePoint).
    // Returns false when the write failed so the client can mark the file
    // as not ok instead of silently losing coverage.
    virtual bool saveFileCoverage(const std::string& run_id, int wave,
                                  const std::string& file,
                                  const std::vector<std::uint64_t>& points) = 0;

    // Client side: persists one file's OpenCppCoverage .cov binary (optional
    // artifact kept for audit and alternative total-coverage computation).
    // Returns false when the write failed.
    virtual bool saveFileCov(const std::string& run_id, int wave, const std::string& file,
                             const std::string& cov_data, std::int64_t size_bytes) = 0;

    // Scheduler side: loads all files' coverage point sets of a wave.
    // Returns false when the staging could not be read completely.
    virtual bool loadWave(const std::string& run_id, int wave,
                          std::unordered_map<std::string, std::set<std::uint64_t>>*
                              file_points) = 0;

    // Client side: loads one file's already-stored point set. Returns true
    // when the file has staged coverage (used for idempotent execution);
    // false when nothing was stored for this file.
    virtual bool loadFileCoverage(const std::string& run_id, int wave,
                                  const std::string& file,
                                  std::set<std::uint64_t>* points) = 0;

    // Removes the wave staging (point sets and .cov binaries) after
    // settlement. Returns false when the cleanup failed.
    virtual bool clearWave(const std::string& run_id, int wave) = 0;

    // Appends files to the run's selected set.
    virtual bool appendSelected(const std::string& run_id,
                                const std::vector<std::string>& files) = 0;

    // Scheduler side: persists the cumulative point set after each wave
    // settlement so the next wave starts from the same coverage.
    virtual bool saveCumulative(const std::string& run_id, int wave,
                                const std::vector<std::uint64_t>& points) = 0;

    // Scheduler side: upserts the run summary (total/selected/cumulative).
    virtual bool saveRunSummary(const std::string& run_id, int total_files,
                                int selected_files, std::int64_t cumulative_points) = 0;

    // Scheduler side: persists/loads the run checkpoint blob (plugin-defined
    // serialized state) used to resume after STOP or crash.
    virtual bool saveCheckpoint(const std::string& run_id, const std::string& blob) = 0;
    virtual bool loadCheckpoint(const std::string& run_id, std::string* blob) = 0;

    // Scheduler side: loads the cumulative point set saved after a settled
    // wave. Returns false when the expected data cannot be read; the output
    // is left empty when the wave does not exist (wave < 0).
    virtual bool loadCumulative(const std::string& run_id, int wave,
                                std::vector<std::uint64_t>* points) = 0;

    // Cov merge support (scheduler side). Materializes the wave's .cov
    // binaries under dir so OpenCppCoverage can merge them; the returned
    // paths are local files owned by the store until dir is removed.
    // Returns false on I/O errors; paths may be empty when no covs exist.
    virtual bool exportWaveCovs(const std::string& run_id, int wave,
                                const std::string& dir,
                                std::vector<std::string>& paths) = 0;

    // Materializes the run's latest cumulative merged .cov under dir.
    // Returns false on I/O errors; path is left empty when no cumulative
    // cov exists yet (first wave).
    virtual bool exportCumulativeCov(const std::string& run_id,
                                     const std::string& dir,
                                     std::string& path) = 0;

    // Scheduler side: stores the wave's merged cumulative .cov (the starting
    // coverage for the next wave). Reads the file at cov_path.
    virtual bool saveCumulativeCov(const std::string& run_id, int wave,
                                   const std::string& cov_path) = 0;

    // Scheduler side: upserts one wave's cumulative total coverage row
    // (covered/total lines and percent), used by the visualization module.
    virtual bool saveWaveSummary(const std::string& run_id, int wave,
                                 std::int64_t covered_lines,
                                 std::int64_t total_lines, double percent) = 0;
};

// Creates the default (file-based) store implementation.
// Selects the backend from config: "store_type" == "postgres" uses libpq,
// anything else uses local files.
std::unique_ptr<CoverageStore> CreateCoverageStore(const std::string& config);

}  // namespace coverage_plugin
