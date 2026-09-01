#include "stpcompare/ClientPlugin.h"

#include "common/Logger.h"
#include "common/Util.h"

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
    info_file_ = cfg.value("info_file", std::string("info.txt"));
    info_new_file_ = cfg.value("info_new_file", std::string("info_new.txt"));
    work_base_ = cfg.value("work_dir", std::string("stp_compare_work"));
    if (work_base_.empty()) {
        work_base_ = "stp_compare_work";
    }
    keep_work_ = cfg.value("keep_work", false);
    dry_run_delay_ms_ = cfg.value("dry_run_delay_ms", 0);
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
                              std::string* info_txt, std::string* info_new_txt,
                              std::string* detail) {
    const std::filesystem::path outdir =
        std::filesystem::path(work_base_) / run_id / std::to_string(common::Fnv1a64(file));
    const auto cleanup = [&]() {
        if (!keep_work_) {
            std::error_code cleanup_ec;
            std::filesystem::remove_all(outdir, cleanup_ec);
        }
    };
    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);
    if (ec) {
        if (detail != nullptr) {
            *detail = "cannot create work dir: " + ec.message();
        }
        return false;
    }

    const std::string stp_path = common::NormalizePath(stp_root_ + "/" + file);
    const std::string cmd =
        BuildProgramCommand(program_cmd_, program_exe_, stp_path, outdir.string(),
                            file, run_id);
    const int exit_code = common::RunCommand(cmd);
    if (exit_code != 0) {
        if (detail != nullptr) {
            *detail = "program exit=" + std::to_string(exit_code);
        }
        LOG_WARN("stp_compare plugin: program exit=" << exit_code << " for " << file);
        cleanup();
        return false;
    }

    const std::string info_path = (outdir / info_file_).string();
    const std::string info_new_path = (outdir / info_new_file_).string();
    if (!common::ReadTextFile(info_path, info_txt)) {
        if (detail != nullptr) {
            *detail = "missing " + info_file_;
        }
        LOG_WARN("stp_compare plugin: missing " << info_path << " for " << file);
        cleanup();
        return false;
    }
    if (!common::ReadTextFile(info_new_path, info_new_txt)) {
        if (detail != nullptr) {
            *detail = "missing " + info_new_file_;
        }
        LOG_WARN("stp_compare plugin: missing " << info_new_path << " for " << file);
        cleanup();
        return false;
    }
    cleanup();
    return true;
}

bool ClientPlugin::execute(const common::Task& task, common::TaskResult& result) {
    stpcompare::StpCompareTask stp_task;
    if (!stp_task.ParseFromString(task.payload())) {
        result.set_exit_code(-1);
        return false;
    }

    stpcompare::StpCompareResult stp_result;
    for (const std::string& file : stp_task.files()) {
        stpcompare::FileOutcome outcome;
        outcome.set_file(file);

        std::string info_txt;
        std::string info_new_txt;
        std::string detail;
        bool ok = false;
        if (dry_run_) {
            ok = true;
            info_txt = "dry-run file=" + file + "\n";
            info_new_txt = "dry-run file=" + file + " (converted)\n";
            if (dry_run_delay_ms_ > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(dry_run_delay_ms_));
            }
        } else {
            ok = RunOneFile(stp_task.run_id(), file, &info_txt, &info_new_txt, &detail);
        }
        if (ok) {
            if (!store_->saveFileResult(stp_task.run_id(), file, info_txt, info_new_txt)) {
                ok = false;
                detail = "failed to save to store";
            }
        }

        outcome.set_ok(ok);
        outcome.set_detail(detail);
        LOG_INFO("stp_compare plugin: file " << file << (ok ? " ok" : " failed")
                                             << (detail.empty() ? "" : (" (" + detail + ")")));
        *stp_result.add_files() = outcome;
    }

    if (!stp_result.SerializeToString(result.mutable_result())) {
        LOG_ERROR("stp_compare plugin: failed to serialize task result " << task.task_id());
        result.set_exit_code(-1);
        return false;
    }
    result.set_exit_code(0);
    return true;
}

}  // namespace stpcompare
