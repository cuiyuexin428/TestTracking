#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace coverage_plugin {

// Client-side store: persists per-file raw coverage (point sets and optional
// .cov binaries) and reads back a file's already-stored point set (idempotent
// execution). All settlement / minimal-set selection / cov merge happens in a
// standalone post-processing tool that reads the same tables, so raw per-file
// results are retained (never consumed by the scheduler).
class CoverageResultStore {
public:
    virtual ~CoverageResultStore() = default;

    // Reads plugin config (e.g. store location / connection string).
    virtual bool init(const std::string& config) = 0;

    // Persists one file's covered point set. Points must be globally unique
    // (see CoverageUtil::MakePoint). Returns false when the write failed so
    // the client can mark the file as not ok instead of silently losing
    // coverage.
    virtual bool saveFileCoverage(const std::string& run_id, const std::string& file,
                                  const std::vector<std::uint64_t>& points) = 0;

    // Persists one file's OpenCppCoverage .cov binary (optional artifact kept
    // for merging at post-processing time). Returns false when the write failed.
    virtual bool saveFileCov(const std::string& run_id, const std::string& file,
                             const std::string& cov_data, std::int64_t size_bytes) = 0;

    // Loads one file's already-stored point set. Returns true when the file has
    // staged coverage; false when nothing was stored for this file.
    virtual bool loadFileCoverage(const std::string& run_id, const std::string& file,
                                  std::set<std::uint64_t>* points) = 0;
};

// Server-side store: persists/loads the run checkpoint blob (plugin-defined
// serialized state) used to resume after STOP or crash. Separated from the
// client-side result store so each consumer only depends on its own methods.
class CoverageCheckpointStore {
public:
    virtual ~CoverageCheckpointStore() = default;

    virtual bool init(const std::string& config) = 0;

    virtual bool saveCheckpoint(const std::string& run_id, const std::string& blob) = 0;
    virtual bool loadCheckpoint(const std::string& run_id, std::string* blob) = 0;
};

// Selects the backend from config: "store_type" == "postgres" uses libpq on
// Windows, anything else uses local files. Both factories return the same
// underlying backend, exposed only through the role each consumer needs.
std::unique_ptr<CoverageResultStore> CreateCoverageResultStore(const std::string& config);
std::unique_ptr<CoverageCheckpointStore> CreateCoverageCheckpointStore(const std::string& config);

}  // namespace coverage_plugin
