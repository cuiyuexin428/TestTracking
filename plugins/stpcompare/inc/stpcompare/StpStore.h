#pragma once

#include <memory>
#include <string>

namespace stpcompare {

// Client-side store: persists one file's two txt outputs into the configured
// data table (or under the run dir for the file backend). Returns false when
// the write failed so the client can mark the file failed.
class StpResultStore {
public:
    virtual ~StpResultStore() = default;

    // Reads plugin config (store location / connection string / table name).
    virtual bool init(const std::string& config) = 0;

    // Client side: insert one file's info outputs.
    virtual bool saveFileResult(const std::string& run_id, const std::string& file,
                                const std::string& info_txt,
                                const std::string& info_new_txt) = 0;
};

// Server-side store: persists/loads the run checkpoint blob (plugin-defined
// serialized state) used to resume after STOP or crash. The two halves are
// separate interfaces so neither side depends on the other's methods.
class StpCheckpointStore {
public:
    virtual ~StpCheckpointStore() = default;

    virtual bool init(const std::string& config) = 0;

    // Scheduler side: persist/load the run checkpoint blob.
    virtual bool saveCheckpoint(const std::string& run_id, const std::string& blob) = 0;
    virtual bool loadCheckpoint(const std::string& run_id, std::string* blob) = 0;
};

// Builds the backend selected by config "store_type" ("postgres" uses libpq on
// Windows, anything else uses local files). Both return the same underlying
// backend but expose only the role each consumer needs.
std::unique_ptr<StpResultStore> CreateStpResultStore(const std::string& config);
std::unique_ptr<StpCheckpointStore> CreateStpCheckpointStore(const std::string& config);

}  // namespace stpcompare
