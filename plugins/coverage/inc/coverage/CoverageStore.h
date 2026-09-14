#pragma once
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>
namespace coverage_plugin {
struct FileCoverageResult {
    std::string run_id,file,attempt_id,detail,cov;
    std::vector<std::uint64_t> points;
    bool successful=false;
};
// An execution's points, optional binary report and outcome form one atomic
// publication. First successful publication wins; failed attempts may retry.
class CoverageResultStore {
public:
    virtual ~CoverageResultStore()=default;
    virtual bool init(const std::string& config)=0;
    virtual bool publish(const FileCoverageResult& result)=0;
    virtual bool loadFileCoverage(const std::string& run,const std::string& file,
                                  std::set<std::uint64_t>* points)=0;
    virtual bool hasFileCov(const std::string& run,const std::string& file)=0;
};
std::unique_ptr<CoverageResultStore> CreateCoverageResultStore(const std::string& config);
}
