#pragma once
#include "common/RunStore.h"
#include "common/TaskPlugin.h"
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace common {
// Single-owner task state. The immutable plan is written once; each committed
// task adds one small durable journal entry. Dispatch/requeue never rewrite N tasks.
class TaskLedger {
public:
    bool init(const std::string& config, const std::string& plugin);
    bool plan(const std::string& run, const std::vector<Task>& tasks,
              const std::vector<std::string>& files, const std::string& fingerprint);
    bool restore(const std::string& run);
    bool loadLegacy(const std::string& run, std::string* bytes);
    bool request(Task* task);
    void requeue(const std::vector<std::string>& ids);
    bool commit(const TaskResult& result);
    bool save(const std::string& run);
    bool finished();
    bool contains(const std::string& id) const { return tasks_.count(id) != 0; }
    const Task* task(const std::string& id) const;
    void clear();
private:
    bool loadTasks(const std::vector<Task>& tasks, const std::set<std::string>& completed);
    RunStore store_;
    std::string run_;
    std::unordered_map<std::string,Task> tasks_;
    std::deque<std::string> pending_, priority_;
    std::unordered_set<std::string> queued_, in_flight_, done_;
    bool finished_ = false;
};
}
