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
    virtual bool publish(const FileResult& result)=0;
};
std::unique_ptr<StpResultStore> CreateStpResultStore(const std::string& config);
}
