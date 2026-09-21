#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace stpcompare {
struct RawOutput {
    std::string program_role;
    std::string export_stage;
    std::string program_version;
    std::string original_filename;
    std::string content;
    std::string content_sha256;
    std::uint64_t content_size=0;
};
struct FileResult {
    std::string run_id,file,attempt_id,detail;
    std::vector<RawOutput> outputs;
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
