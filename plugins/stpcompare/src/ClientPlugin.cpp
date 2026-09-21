#include "stpcompare/ClientPlugin.h"

#include "common/Logger.h"
#include "common/Util.h"
#include "common/AtomicFile.h"
#include "common/ProcessRunner.h"
#include <atomic>

#include "stpcompare.pb.h"
#include <json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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

std::string CompositeProgramVersion(const std::string& baseline,const std::string& candidate) {
    return "baseline="+baseline+";candidate="+candidate;
}

bool ContainsPlaceholder(const std::vector<std::string>& arguments,const std::string& placeholder) {
    for(const auto& argument:arguments)
        if(argument.find(placeholder)!=std::string::npos) return true;
    return false;
}

void AddRawOutput(const std::string& role,const std::string& stage,
                  const std::string& version,const std::string& filename,
                  std::string content,FileResult* output) {
    RawOutput raw;
    raw.program_role=role;
    raw.export_stage=stage;
    raw.program_version=version;
    raw.original_filename=filename;
    raw.content_size=content.size();
    raw.content_sha256=common::Sha256Hex(content);
    raw.content=std::move(content);
    output->outputs.push_back(std::move(raw));
}

}  // namespace

std::string ClientPlugin::pluginType() const {
    return "stpcompare";
}

bool ClientPlugin::init(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
    dry_run_ = cfg.value("dry_run", true);
    stp_root_ = cfg.value("stp_root", std::string());
    if(!ReadProgram(cfg,"baseline",&baseline_) || !ReadProgram(cfg,"candidate",&candidate_))
        return false;
    work_base_ = cfg.value("work_dir", std::string("stp_compare_work"));
    if (work_base_.empty()) {
        work_base_ = "stp_compare_work";
    }
    keep_work_ = cfg.value("keep_work", false);
    dry_run_delay_ms_ = cfg.value("dry_run_delay_ms", 0);
    process_timeout_ms_=cfg.value("process_timeout_ms",300000);
    if(process_timeout_ms_<=0) return false;
    const std::string dataset_version_file =
        cfg.value("dataset_version_file", std::string());

    dataset_version_ =
        dataset_version_file.empty() ? ReadVersionFile(stp_root_ + "/version.txt")
                                     : ReadVersionFile(dataset_version_file);

    if (!dry_run_) {
        if (stp_root_.empty()) {
            LOG_ERROR("stpcompare plugin: stp_root is required when dry_run=false");
            return false;
        }
        for(const auto* program:{&baseline_,&candidate_}) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(program->executable,ec) || ec) {
                LOG_ERROR("stpcompare plugin: "<<program->role<<" executable not found: "
                          <<program->executable);
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

bool ClientPlugin::ReadProgram(const nlohmann::json& config,const std::string& role,
                               Program* program) {
    if(!program || !config.contains("programs") || !config["programs"].is_object() ||
       !config["programs"].contains(role) || !config["programs"][role].is_object()) {
        LOG_ERROR("stpcompare plugin: programs."<<role<<" is required");
        return false;
    }
    const auto& item=config["programs"][role];
    program->role=role;
    program->executable=item.value("executable",std::string());
    program->arguments=item.value("arguments",std::vector<std::string>{
        "--input","{stp}","--output-dir","{outdir}"});
    if(program->executable.empty() || program->arguments.empty() ||
       !ContainsPlaceholder(program->arguments,"{stp}") ||
       !ContainsPlaceholder(program->arguments,"{outdir}")) {
        LOG_ERROR("stpcompare plugin: programs."<<role
                  <<" needs executable and argument placeholders {stp}, {outdir}");
        return false;
    }
    const auto version_file=item.value("version_file",std::string());
    program->version=item.value("version",std::string());
    if(program->version.empty()) {
        const auto path=version_file.empty()
            ? std::filesystem::path(program->executable).parent_path()/"version.txt"
            : std::filesystem::path(version_file);
        program->version=ReadVersionFile(path.string());
    }
    return true;
}

bool ClientPlugin::collectRegistrationInfo(std::string* payload) {
    stpcompare::StpCompareClientInfo info;
    info.set_baseline_program_version(baseline_.version);
    info.set_candidate_program_version(candidate_.version);
    info.set_program_version(CompositeProgramVersion(baseline_.version,candidate_.version));
    info.set_dataset_version(dataset_version_);
    return info.SerializeToString(payload);
}

common::RegisterOutcome ClientPlugin::handleValidationReply(const std::string&) {
    return common::RegisterOutcome::kReady;
}

std::string ClientPlugin::heartbeatInfo() const {
    return "baseline="+baseline_.version+",candidate="+candidate_.version+
        ",dataset="+dataset_version_;
}

bool ClientPlugin::RunProgram(const Program& program,const std::filesystem::path& task_dir,
                              const std::string& stp_path,const std::string& file,
                              const std::string& run_id,FileResult* output,
                              const common::CancellationToken& cancellation) {
    const auto outdir=task_dir/program.role;
    std::error_code ec;
    if(!std::filesystem::create_directory(outdir,ec) || ec) {
        output->detail="cannot create "+program.role+" output directory";
        return false;
    }
    common::ProcessOptions options;
    options.executable=std::filesystem::absolute(program.executable).string();
    options.arguments=program.arguments;
    options.working_directory=outdir.string();
    options.timeout=std::chrono::milliseconds(process_timeout_ms_);
    for(auto& argument:options.arguments)
        argument=ExpandArgumentTemplate(argument,stp_path,outdir.string(),file,run_id);
    const auto process=common::ProcessRunner::run(options,cancellation);
    if(!process.started || process.exit_code!=0 || process.cancelled || process.timed_out) {
        output->detail=process.cancelled?"cancelled":process.timed_out?
            program.role+" process timed out":program.role+" converter failed: "+
            std::to_string(process.exit_code)+" "+process.error;
        return false;
    }
    std::string first,second;
    if(!common::ReadCompleteFile(outdir/"info.txt",&first) ||
       !common::ReadCompleteFile(outdir/"info_new.txt",&second)) {
        output->detail=program.role+" did not produce info.txt and info_new.txt";
        return false;
    }
    AddRawOutput(program.role,"first",program.version,"info.txt",std::move(first),output);
    AddRawOutput(program.role,"second",program.version,"info_new.txt",std::move(second),output);
    return true;
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
    if(!RunProgram(baseline_,outdir,stp_path,file,run_id,output,cancellation) ||
       !RunProgram(candidate_,outdir,stp_path,file,run_id,output,cancellation)) {
        output->outputs.clear();
        return false;
    }
    return output->outputs.size()==4;
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
        const bool cached=store_->hasSuccessfulResult(payload.run_id(),file);
        if(cached) output.successful=true;
        else if(dry_run_) {
            if(cancellation.waitFor(std::chrono::milliseconds(dry_run_delay_ms_))) {
                result.set_exit_code(-2); return false;
            }
            output.successful=true;
            for(const auto* program:{&baseline_,&candidate_}) {
                AddRawOutput(program->role,"first",program->version,"info.txt",
                             "dry-run role="+program->role+" file="+file+" first\n",&output);
                AddRawOutput(program->role,"second",program->version,"info_new.txt",
                             "dry-run role="+program->role+" file="+file+" second\n",&output);
            }
        } else output.successful=RunOneFile(payload.run_id(),file,&output,cancellation);
        if(cancellation.isCancellationRequested()) { result.set_exit_code(-2); return false; }
        if(!cached && !store_->publish(output)) {
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
