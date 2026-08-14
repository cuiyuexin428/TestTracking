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

    // Scheduler side: loads all files' coverage point sets of a wave.
    virtual std::unordered_map<std::string, std::set<std::uint64_t>> loadWave(
        const std::string& run_id, int wave) = 0;

    // Removes the wave staging after settlement.
    virtual void clearWave(const std::string& run_id, int wave) = 0;

    // Appends files to the run's selected set.
    virtual void appendSelected(const std::string& run_id,
                                const std::vector<std::string>& files) = 0;
};

// Creates the default (file-based) store implementation.
std::unique_ptr<CoverageStore> CreateCoverageStore();

}  // namespace coverage_plugin
