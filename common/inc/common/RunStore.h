#pragma once
#include "common/Manifest.h"
#include "common/PgUtil.h"
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace common {
struct RunIdentity {
    std::string program_version, dataset_version, manifest_fingerprint;
};
// Durable execution manifest is separate from the disposable recovery journal.
class RunStore {
public:
    // The plugin owns the checkpoint table name. RunStore persists the common
    // plan there and keeps completion records in the shared journal tables.
    bool init(const std::string& config, const std::string& plugin,
              const std::string& checkpoint_table);
    bool savePlan(const std::string& run, const std::string& blob,
                  const std::vector<ManifestEntry>& files, const std::string& fingerprint,
                  std::size_t planned_tasks, const std::string& dataset_version = {});
    // Reads return false for confirmed missing/invalid data. Storage access
    // failures throw RecoveryBlocked so recovery cannot discard a valid run.
    bool loadPlan(const std::string& run, std::string* blob);
    bool loadIdentity(const std::string& run, RunIdentity* identity);
    bool recordCompletion(const std::string& run, const std::string& task,
                          std::optional<int> exit_code = std::nullopt);
    bool loadCompletions(const std::string& run, std::set<std::string>* tasks);
    bool finish(const std::string& run);
private:
    std::string plugin_, directory_, checkpoint_table_, program_version_, dataset_version_;
    bool postgres_ = false;
#ifdef _WIN32
    PgConnection connection_;
#endif
};
}
