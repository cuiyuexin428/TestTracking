#pragma once
#include "common/PgUtil.h"
#include <set>
#include <string>
#include <vector>
namespace common {
// Durable execution manifest is separate from the disposable recovery journal.
class RunStore {
public:
    // The plugin owns the checkpoint table name. RunStore persists the common
    // plan there and keeps completion records in the shared journal tables.
    bool init(const std::string& config, const std::string& plugin,
              const std::string& checkpoint_table);
    bool savePlan(const std::string& run, const std::string& blob,
                  const std::vector<std::string>& files, const std::string& fingerprint);
    bool loadPlan(const std::string& run, std::string* blob);
    bool recordCompletion(const std::string& run, const std::string& task);
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
