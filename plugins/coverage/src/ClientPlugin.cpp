#include "coverage/ClientPlugin.h"

#include "common/Logger.h"
#include "common/Util.h"
#include "common/ProcessRunner.h"
#include "common/AtomicFile.h"

#include "coverage.pb.h"
#include "coverage/CoverageUtil.h"
#include "tinyxml2.h"

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace coverage_plugin {

namespace {

std::vector<std::uint64_t> SyntheticPoints(const std::string& file) {
    const std::uint64_t seed = common::Fnv1a64(file);
    const std::uint64_t count = 10 + (seed % 40);
    std::vector<std::uint64_t> points;
    points.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        points.push_back(MakePoint(
            file, static_cast<std::uint32_t>((seed + i * 7919ULL) % 1000000ULL)));
    }
    return points;
}

std::string ReadVersionFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return std::string();
    }
    std::string version;
    std::getline(in, version);
    if (!version.empty() && version.back() == '\r') {
        version.pop_back();
    }
    return version;
}

bool ParseCobertura(const std::string& xml_path, std::vector<std::uint64_t>& points) {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(xml_path.c_str()) != tinyxml2::XML_SUCCESS) {
        return false;
    }

    const tinyxml2::XMLElement* coverage = doc.FirstChildElement("coverage");
    if (coverage == nullptr) {
        return false;
    }

    std::vector<std::string> sources;
    if (const tinyxml2::XMLElement* srcs = coverage->FirstChildElement("sources")) {
        for (const tinyxml2::XMLElement* s = srcs->FirstChildElement("source"); s != nullptr;
             s = s->NextSiblingElement("source")) {
            sources.push_back(s->GetText() != nullptr ? s->GetText() : "");
        }
    }

    const tinyxml2::XMLElement* packages = coverage->FirstChildElement("packages");
    if (packages == nullptr) {
        return false;
    }
    for (const tinyxml2::XMLElement* pkg = packages->FirstChildElement("package"); pkg != nullptr;
         pkg = pkg->NextSiblingElement("package")) {
        const tinyxml2::XMLElement* classes = pkg->FirstChildElement("classes");
        if (classes == nullptr) {
            continue;
        }
        for (const tinyxml2::XMLElement* cls = classes->FirstChildElement("class");
             cls != nullptr; cls = cls->NextSiblingElement("class")) {
            const char* filename = cls->Attribute("filename");
            if (filename == nullptr) {
                continue;
            }
            const std::string path = CanonicalPath(filename, sources);
            const tinyxml2::XMLElement* lines = cls->FirstChildElement("lines");
            if (lines == nullptr) {
                continue;
            }
            for (const tinyxml2::XMLElement* line = lines->FirstChildElement("line");
                 line != nullptr; line = line->NextSiblingElement("line")) {
                const int hits = line->IntAttribute("hits", 0);
                if (hits <= 0) {
                    continue;
                }
                const int number = line->IntAttribute("number", 0);
                if (number <= 0) {
                    continue;
                }
                points.push_back(MakePoint(path, static_cast<std::uint32_t>(number)));
            }
        }
    }
    return true;
}

}  // namespace

std::string ClientPlugin::pluginType() const {
    return "coverage";
}

bool ClientPlugin::init(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
    dry_run_ = cfg.value("dry_run", true);
    stp_root_ = cfg.value("stp_root", std::string());
    program_dir_ = cfg.value("program_dir", std::string());
    opencppcoverage_ = cfg.value("opencppcoverage", std::string("OpenCppCoverage.exe"));
    target_exe_ = cfg.value("target_exe", std::string());
    export_dir_ = cfg.value("export_dir", std::string());
    keep_reports_ = cfg.value("keep_reports", false);
    store_cov_ = cfg.value("store_cov", false);
    dry_run_delay_ms_ = cfg.value("dry_run_delay_ms", 0);
    process_timeout_ms_ = cfg.value("process_timeout_ms", 300000);
    if (process_timeout_ms_ <= 0) return false;
    engine_dlls_.clear();
    for (const nlohmann::json& dll : cfg.value("engine_dlls", nlohmann::json::array())) {
        engine_dlls_.push_back(dll.get<std::string>());
    }

    // Managed releases inject a version derived from the verified program
    // artifact SHA. Existing manually deployed programs keep version.txt.
    program_version_ = cfg.value("program_version", std::string());
    if (program_version_.empty())
        program_version_ = ReadVersionFile(program_dir_ + "/version.txt");
    dataset_version_ = ReadVersionFile(stp_root_ + "/version.txt");
    if (!dry_run_) {
        if (target_exe_.empty() || stp_root_.empty()) {
            LOG_ERROR("coverage plugin: target_exe and stp_root are required when dry_run=false");
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::exists(target_exe_, ec) || ec) {
            LOG_ERROR("coverage plugin: target_exe not found: " << target_exe_);
            return false;
        }
        if (!std::filesystem::exists(opencppcoverage_, ec) || ec) {
            LOG_ERROR("coverage plugin: opencppcoverage not found: " << opencppcoverage_);
            return false;
        }
    }
    if (dry_run_delay_ms_ < 0) {
        dry_run_delay_ms_ = 0;
    }

    if (!dry_run_ && export_dir_.empty()) {
        export_dir_ = "reports";
    }
    if (!export_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(export_dir_, ec);
        if (ec) {
            LOG_ERROR("coverage plugin: cannot create export_dir " << export_dir_
                                                                  << ": " << ec.message());
            return false;
        }
    }

    store_ = CreateCoverageResultStore(config);
    if (!store_->init(config)) {
        LOG_ERROR("coverage plugin: coverage store init failed");
        return false;
    }
    return true;
}

bool ClientPlugin::collectRegistrationInfo(std::string* payload) {
    coverage::CoverageClientInfo info;
    info.set_program_version(program_version_);
    info.set_dataset_version(dataset_version_);
    return info.SerializeToString(payload);
}

common::RegisterOutcome ClientPlugin::handleValidationReply(const std::string&) {
    // Registration is version-gated by the server; the client has nothing to
    // do after a successful validation.
    return common::RegisterOutcome::kReady;
}

std::string ClientPlugin::heartbeatInfo() const {
    return "program=" + program_version_ + ",dataset=" + dataset_version_;
}

bool ClientPlugin::RunOneFile(const std::string& run_id, const std::string& file,
                              FileCoverageResult* output,
                              const common::CancellationToken& cancellation) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto report_dir=std::filesystem::absolute(export_dir_)/
        (std::to_string(stamp)+"-"+std::to_string(sequence.fetch_add(1)));
    std::error_code ec;
    if (!std::filesystem::create_directory(report_dir,ec) || ec) {
        output->detail="cannot create execution directory"; return false;
    }
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
    } cleanup{report_dir};
    const auto xml_path=report_dir/"coverage.xml";
    const auto cov_path=report_dir/"coverage.cov";
    common::ProcessOptions options;
    options.executable=std::filesystem::absolute(opencppcoverage_).string();
    options.arguments={"--quiet"};
    options.working_directory=report_dir.string();
    options.timeout=std::chrono::milliseconds(process_timeout_ms_);
    for(const auto& module:engine_dlls_)
        options.arguments.push_back("--modules="+common::NormalizePath(std::filesystem::absolute(module).string()));
    options.arguments.push_back("--export_type=cobertura:"+common::NormalizePath(xml_path.string()));
    if(store_cov_) options.arguments.push_back("--export_type=binary:"+common::NormalizePath(cov_path.string()));
    options.arguments.push_back("--");
    options.arguments.push_back(common::NormalizePath(std::filesystem::absolute(target_exe_).string()));
    options.arguments.push_back(common::NormalizePath(std::filesystem::absolute(std::filesystem::path(stp_root_)/file).string()));
    const auto process=common::ProcessRunner::run(options,cancellation);
    if(!process.started || process.exit_code!=0 || process.cancelled || process.timed_out) {
        output->detail=process.cancelled?"cancelled":process.timed_out?"process timed out":
            "coverage process failed: "+std::to_string(process.exit_code)+" "+process.error;
        return false;
    }
    if(!ParseCobertura(xml_path.string(),output->points)) {
        output->detail="missing or invalid Cobertura report"; return false;
    }
    if(store_cov_ && (!common::ReadCompleteFile(cov_path,&output->cov) || output->cov.empty())) {
        output->detail="missing or empty coverage binary"; return false;
    }
    if(keep_reports_) {
        std::string xml;
        const auto published=std::filesystem::path(export_dir_)/
            (std::to_string(common::Fnv1a64(file))+".xml");
        if(!common::ReadCompleteFile(xml_path,&xml) || !common::WriteAtomicFile(published,xml)) {
            output->detail="cannot publish validation report"; return false;
        }
    }
    return true;
}

bool ClientPlugin::execute(const common::Task& task, common::TaskResult& result) {
    return execute(task,result,common::CancellationToken{});
}

bool ClientPlugin::execute(const common::Task& task, common::TaskResult& result,
                           const common::CancellationToken& cancellation) {
    if (cancellation.isCancellationRequested()) {
        result.set_exit_code(-2);
        return false;
    }
    coverage::CoverageTask payload;
    if(!payload.ParseFromString(task.payload()) ||
       (!task.run_id().empty() && task.run_id()!=payload.run_id())) {
        result.set_exit_code(-1); return false;
    }
    coverage::CoverageResult summary;
    bool all_ok=true, all_persisted=true;
    for(const auto& file:payload.files()) {
        if(cancellation.isCancellationRequested()) { result.set_exit_code(-2); return false; }
        FileCoverageResult output;
        output.run_id=payload.run_id(); output.file=file; output.attempt_id=task.attempt_id();
        std::set<std::uint64_t> existing;
        const bool cached=store_->loadFileCoverage(payload.run_id(),file,&existing) &&
            (!store_cov_ || dry_run_ || store_->hasFileCov(payload.run_id(),file));
        if(cached) {
            output.points.assign(existing.begin(),existing.end()); output.successful=true;
        } else if(dry_run_) {
            if(cancellation.waitFor(std::chrono::milliseconds(dry_run_delay_ms_))) {
                result.set_exit_code(-2); return false;
            }
            output.points=SyntheticPoints(file); output.successful=true;
        } else {
            output.successful=RunOneFile(payload.run_id(),file,&output,cancellation);
        }
        if(cancellation.isCancellationRequested()) { result.set_exit_code(-2); return false; }
        std::sort(output.points.begin(),output.points.end());
        output.points.erase(std::unique(output.points.begin(),output.points.end()),output.points.end());
        if(!cached && !store_->publish(output)) {
            output.successful=false; all_persisted=false;
            output.detail="failed to publish raw execution result";
        }
        auto* entry=summary.add_files(); entry->set_file(file);
        entry->set_ok(output.successful);
        entry->set_covered_lines(static_cast<std::int32_t>(output.points.size()));
        all_ok=all_ok && output.successful;
        LOG_INFO("coverage: file "<<file<<(output.successful?" ok":" failed")<<" "<<output.detail);
    }
    if(!summary.SerializeToString(result.mutable_result())) { result.set_exit_code(-1); return false; }
    // A durable business failure is a completed execution; a failed database
    // publication must be retried by the framework, never reported committed.
    result.set_exit_code(all_persisted?(all_ok?0:1):-1);
    return all_persisted;
}
}  // namespace coverage_plugin
