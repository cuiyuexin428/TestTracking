#include "stpcompare/ClientPlugin.h"

#include "common/Logger.h"
#include "common/Util.h"
#include "common/AtomicFile.h"
#include "common/ProcessRunner.h"
#include <atomic>

#include "stpcompare.pb.h"
#include "stpcompare/StpUtil.h"

#include <json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace stpcompare {

namespace {

std::string ExpandArgumentTemplate(const std::string& input,
                                   const std::string& stp_path,
                                   const std::string& outdir,
                                   const std::string& file,
                                   const std::string& run_id) {
    struct Placeholder { const char* token; const std::string* value; };
    const Placeholder placeholders[] = {
        {"{stp}", &stp_path},
        {"{outdir}", &outdir},
        {"{workdir}", &outdir},
        {"{file}", &file},
        {"{run_id}", &run_id},
    };
    std::string output;
    output.reserve(input.size());
    for (std::size_t pos = 0; pos < input.size();) {
        const Placeholder* match = nullptr;
        for (const auto& placeholder : placeholders) {
            const std::string token(placeholder.token);
            if (input.compare(pos, token.size(), token) == 0) {
                match = &placeholder;
                break;
            }
        }
        if (match != nullptr) {
            output += *match->value;
            pos += std::char_traits<char>::length(match->token);
        } else {
            output += input[pos++];
        }
    }
    return output;
}

std::string ReadVersionFile(const std::string& path) {
    if (path.empty()) {
        return std::string();
    }
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

}  // namespace

std::string ClientPlugin::pluginType() const {
    return "stpcompare";
}

bool ClientPlugin::init(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
    dry_run_ = cfg.value("dry_run", true);
    stp_root_ = cfg.value("stp_root", std::string());
    program_exe_ = cfg.value("program_exe", std::string());
    program_cmd_ = cfg.value("program_cmd", std::string());
    program_args_=cfg.value("program_args",std::vector<std::string>{"{stp}"});
    // Legacy templates are tokenized before substitution, so filenames can
    // never create additional arguments or shell syntax.
    if(!program_cmd_.empty()) {
        std::vector<std::string> parsed;
        if(!ParseProgramArguments(program_cmd_,&parsed) || parsed.empty()) return false;
        program_exe_=parsed.front();
        program_args_.assign(parsed.begin()+1,parsed.end());
    }
    info_file_ = cfg.value("info_file", std::string("info.txt"));
    info_new_file_ = cfg.value("info_new_file", std::string("info_new.txt"));
    work_base_ = cfg.value("work_dir", std::string("stp_compare_work"));
    if (work_base_.empty()) {
        work_base_ = "stp_compare_work";
    }
    keep_work_ = cfg.value("keep_work", false);
    dry_run_delay_ms_ = cfg.value("dry_run_delay_ms", 0);
    process_timeout_ms_=cfg.value("process_timeout_ms",300000);
    if(process_timeout_ms_<=0) return false;
    const std::string program_dir = cfg.value("program_dir", std::string());
    const std::string dataset_version_file =
        cfg.value("dataset_version_file", std::string());

    program_version_ = ReadVersionFile(program_dir + "/version.txt");
    dataset_version_ =
        dataset_version_file.empty() ? ReadVersionFile(stp_root_ + "/version.txt")
                                     : ReadVersionFile(dataset_version_file);

    if (!dry_run_) {
        if (stp_root_.empty() || (program_exe_.empty() && program_cmd_.empty())) {
            LOG_ERROR(
                "stp_compare plugin: stp_root and program_exe/program_cmd are required "
                "when dry_run=false");
            return false;
        }
        if (!program_cmd_.empty() || !program_exe_.empty()) {
            std::error_code ec;
            if (!program_exe_.empty() &&
                (!std::filesystem::exists(program_exe_, ec) || ec)) {
                LOG_ERROR("stp_compare plugin: program_exe not found: " << program_exe_);
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(work_base_, ec);
    }
    if (dry_run_delay_ms_ < 0) {
        dry_run_delay_ms_ = 0;
    }

    store_ = CreateStpResultStore(config);
    if (!store_->init(config)) {
        LOG_ERROR("stp_compare plugin: store init failed");
        return false;
    }
    return true;
}

bool ClientPlugin::collectRegistrationInfo(std::string* payload) {
    stpcompare::StpCompareClientInfo info;
    info.set_program_version(program_version_);
    info.set_dataset_version(dataset_version_);
    return info.SerializeToString(payload);
}

common::RegisterOutcome ClientPlugin::handleValidationReply(const std::string&) {
    return common::RegisterOutcome::kReady;
}

std::string ClientPlugin::heartbeatInfo() const {
    return "program=" + program_version_ + ",dataset=" + dataset_version_;
}

bool ClientPlugin::RunOneFile(const std::string& run_id, const std::string& file,
                              FileResult* output, const common::CancellationToken& cancellation) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto outdir=std::filesystem::absolute(work_base_)/
        (std::to_string(stamp)+"-"+std::to_string(sequence.fetch_add(1)));
    std::error_code ec;
    if(!std::filesystem::create_directory(outdir,ec) || ec) {
        output->detail="cannot create work directory"; return false;
    }
    struct Cleanup {
        std::filesystem::path path; bool keep;
        ~Cleanup() { if(!keep) { std::error_code ignored; std::filesystem::remove_all(path,ignored); } }
    } cleanup{outdir,keep_work_};
    const auto stp_path=std::filesystem::absolute(std::filesystem::path(stp_root_)/file).string();
    common::ProcessOptions options;
    options.executable=std::filesystem::absolute(program_exe_).string();
    options.arguments=program_args_;
    options.working_directory=outdir.string();
    options.timeout=std::chrono::milliseconds(process_timeout_ms_);
    for (auto& argument : options.arguments)
        argument = ExpandArgumentTemplate(argument, stp_path, outdir.string(), file, run_id);
    const auto process=common::ProcessRunner::run(options,cancellation);
    if(!process.started || process.exit_code!=0 || process.cancelled || process.timed_out) {
        output->detail=process.cancelled?"cancelled":process.timed_out?"process timed out":
            "converter failed: "+std::to_string(process.exit_code)+" "+process.error;
        return false;
    }
    if(!common::ReadCompleteFile(outdir/info_file_,&output->info_txt) ||
       !common::ReadCompleteFile(outdir/info_new_file_,&output->info_new_txt)) {
        output->detail="missing converter output"; return false;
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
    StpCompareTask payload;
    if(!payload.ParseFromString(task.payload()) ||
       (!task.run_id().empty() && task.run_id()!=payload.run_id())) {
        result.set_exit_code(-1); return false;
    }
    StpCompareResult summary; bool all_ok=true,all_persisted=true;
    for(const auto& file:payload.files()) {
        if(cancellation.isCancellationRequested()) { result.set_exit_code(-2); return false; }
        FileResult output;
        output.run_id=payload.run_id(); output.file=file; output.attempt_id=task.attempt_id();
        if(dry_run_) {
            if(cancellation.waitFor(std::chrono::milliseconds(dry_run_delay_ms_))) {
                result.set_exit_code(-2); return false;
            }
            output.successful=true;
            output.info_txt="dry-run file="+file+"\n";
            output.info_new_txt="dry-run file="+file+" (converted)\n";
        } else output.successful=RunOneFile(payload.run_id(),file,&output,cancellation);
        if(cancellation.isCancellationRequested()) { result.set_exit_code(-2); return false; }
        if(!store_->publish(output)) {
            output.successful=false; all_persisted=false;
            output.detail="failed to publish raw execution result";
        }
        auto* entry=summary.add_files(); entry->set_file(file);
        entry->set_ok(output.successful); entry->set_detail(output.detail);
        all_ok=all_ok && output.successful;
        LOG_INFO("stpcompare: file "<<file<<(output.successful?" ok":" failed")<<" "<<output.detail);
    }
    if(!summary.SerializeToString(result.mutable_result())) { result.set_exit_code(-1); return false; }
    result.set_exit_code(all_persisted?(all_ok?0:1):-1);
    return all_persisted;
}
}  // namespace stpcompare
