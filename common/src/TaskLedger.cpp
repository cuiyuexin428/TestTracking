#include "common/TaskLedger.h"
#include <json.hpp>

namespace common {
namespace {
std::string Hex(const std::string& bytes) {
    static const char digits[]="0123456789abcdef";
    std::string result; result.reserve(bytes.size()*2);
    for (unsigned char ch : bytes) { result += digits[ch>>4]; result += digits[ch&15]; }
    return result;
}
bool Unhex(const std::string& text, std::string* bytes) {
    if (text.size()%2) return false;
    auto digit=[](char c) { return c>='0'&&c<='9' ? c-'0' : c>='a'&&c<='f' ? c-'a'+10 : -1; };
    bytes->clear(); bytes->reserve(text.size()/2);
    for (std::size_t i=0;i<text.size();i+=2) {
        int high=digit(text[i]), low=digit(text[i+1]);
        if (high<0||low<0) return false;
        *bytes += static_cast<char>((high<<4)|low);
    }
    return true;
}
}
bool TaskLedger::init(const std::string& config, const std::string& plugin,
                      const std::string& checkpoint_table) {
    const auto cfg = nlohmann::json::parse(config);
    expected_program_version_ = cfg.value("expected_program_version",std::string());
    expected_dataset_version_ = cfg.value("expected_dataset_version",std::string());
    return store_.init(config, plugin, checkpoint_table);
}
void TaskLedger::clear() {
    run_.clear(); tasks_.clear(); pending_.clear(); priority_.clear(); queued_.clear();
    in_flight_.clear(); done_.clear(); finished_=false;
    identity_ = {};
}
bool TaskLedger::loadTasks(const std::vector<Task>& tasks,const std::set<std::string>& completed) {
    for (const auto& task : tasks) {
        if (task.task_id().empty() || !tasks_.emplace(task.task_id(),task).second) return false;
        if (completed.count(task.task_id())) done_.insert(task.task_id());
        else { pending_.push_back(task.task_id()); queued_.insert(task.task_id()); }
    }
    for (const auto& id : completed) if (!tasks_.count(id)) return false;
    return true;
}
bool TaskLedger::plan(const std::string& run,const std::vector<Task>& tasks,
                      const std::vector<std::string>& files,const std::string& fingerprint,
                      const std::string& dataset_version) {
    clear(); run_=run;
    identity_ = {expected_program_version_,
                 dataset_version.empty() ? expected_dataset_version_ : dataset_version, fingerprint};
    if (!loadTasks(tasks,{})) { clear(); return false; }
    nlohmann::json state={{"version",4},{"run",run},{"files",files},{"fingerprint",fingerprint},
                           {"tasks",nlohmann::json::array()}};
    for (const auto& task : tasks) state["tasks"].push_back(Hex(task.SerializeAsString()));
    if (!store_.savePlan(run,state.dump(),files,fingerprint,tasks.size(),identity_.dataset_version)) {
        clear(); return false;
    }
    if (tasks.empty()) finished_=store_.finish(run_);
    return true;
}
bool TaskLedger::loadLegacy(const std::string& run,std::string* bytes) { return store_.loadPlan(run,bytes); }
bool TaskLedger::restore(const std::string& run, const std::string& manifest_fingerprint) {
    clear();
    std::string bytes;
    if (!store_.loadPlan(run,&bytes)) return false;
    auto state=nlohmann::json::parse(bytes,nullptr,false);
    if (state.is_discarded() || !state.is_object() || state.value("version",0)!=4 ||
        state.value("run",std::string())!=run || !state.contains("tasks") || !state["tasks"].is_array()) return false;
    if (!manifest_fingerprint.empty()) {
        if (!store_.loadIdentity(run,&identity_))
            throw RecoveryBlocked("cannot read saved deployment identity for run " + run);
        if (identity_.program_version != expected_program_version_ ||
            (!expected_dataset_version_.empty() && identity_.dataset_version != expected_dataset_version_) ||
            identity_.manifest_fingerprint != manifest_fingerprint)
            throw RecoveryBlocked("deployment version or manifest differs from saved run " + run +
                                  "; restore its original program, dataset and manifest");
    }
    std::vector<Task> tasks;
    for (const auto& encoded : state["tasks"]) {
        std::string data; Task task;
        if (!encoded.is_string() || !Unhex(encoded.get<std::string>(),&data) || !task.ParseFromString(data)) return false;
        tasks.push_back(task);
    }
    std::set<std::string> completed;
    if (!store_.loadCompletions(run,&completed)) return false;
    run_=run;
    if (!loadTasks(tasks,completed)) { clear(); return false; }
    if (done_.size()==tasks_.size()) finished_=store_.finish(run_);
    return true;
}
bool TaskLedger::request(Task* task) {
    auto& queue=priority_.empty()?pending_:priority_;
    if (!task || queue.empty() || finished_) return false;
    auto id=queue.front(); queue.pop_front(); queued_.erase(id);
    in_flight_.insert(id); *task=tasks_.at(id); return true;
}
void TaskLedger::requeue(const std::vector<std::string>& ids) {
    for (const auto& id:ids) {
        in_flight_.erase(id);
        if (tasks_.count(id) && !done_.count(id) && queued_.insert(id).second) priority_.push_back(id);
    }
}
const Task* TaskLedger::task(const std::string& id) const {
    auto it=tasks_.find(id); return it==tasks_.end()?nullptr:&it->second;
}
bool TaskLedger::commit(const TaskResult& result) {
    if ((!result.run_id().empty() && result.run_id()!=run_) || !tasks_.count(result.task_id())) return false;
    if (!done_.count(result.task_id())) {
        if (!in_flight_.count(result.task_id()) ||
            !store_.recordCompletion(run_,result.task_id())) return false;
        done_.insert(result.task_id()); in_flight_.erase(result.task_id());
    }
    if (done_.size()==tasks_.size()) {
        if (!store_.finish(run_)) return false;
        finished_=true;
    }
    return true;
}
bool TaskLedger::save(const std::string& run) {
    if (run!=run_ || run_.empty()) return false;
    if (done_.size()==tasks_.size() && !finished_) { finished_=store_.finish(run_); return finished_; }
    // The plan and accepted completion records are already durable.
    return true;
}
bool TaskLedger::finished() {
    if (!run_.empty() && done_.size()==tasks_.size() && !finished_) finished_=store_.finish(run_);
    return finished_;
}
}
