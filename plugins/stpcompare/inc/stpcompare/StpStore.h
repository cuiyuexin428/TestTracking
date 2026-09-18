#pragma once
#include <memory>
#include <string>
namespace stpcompare {
struct FileResult {
    std::string run_id,file,attempt_id,detail,info_txt,info_new_txt;
    bool successful=false;
};
class StpResultStore {
public:
    virtual ~StpResultStore()=default;
    virtual bool init(const std::string& config)=0;
    virtual bool hasSuccessfulResult(const std::string& run, const std::string& file)=0;
    // On success, update the outcome to the durable winner if another attempt
    // already published success. Transport summaries must reflect that winner.
    virtual bool publish(FileResult& result)=0;
};
std::unique_ptr<StpResultStore> CreateStpResultStore(const std::string& config);
}
