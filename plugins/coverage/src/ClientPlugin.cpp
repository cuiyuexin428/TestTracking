#include "coverage/ClientPlugin.h"

#include "common/Logger.h"

#include "coverage.pb.h"
#include "coverage/CoverageUtil.h"
#include "tinyxml2.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace coverage_plugin {

namespace {

bool CopyTree(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    if (ec) {
        return false;
    }
    std::filesystem::copy(from, to,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    return !ec;
}

#ifdef _WIN32
std::string NormalizePath(std::string path) {
    for (char& c : path) {
        if (c == '/') {
            c = '\\';
        }
    }
    return path;
}
#else
std::string NormalizePath(const std::string& path) {
    return path;
}
#endif

std::vector<std::uint64_t> SyntheticPoints(const std::string& file) {
    const std::uint64_t seed = Fnv1a64(file);
    const std::uint64_t count = 10 + (seed % 40);
    std::vector<std::uint64_t> points;
    points.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        points.push_back(MakePoint(
            file, static_cast<std::uint32_t>((seed + i * 7919ULL) % 1000000ULL)));
    }
    return points;
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
    program_version_ = cfg.value("program_version", std::string());
    dataset_version_ = cfg.value("dataset_version", std::string());
    deployed_ = cfg.value("deployed", false);
    deploy_state_path_ = cfg.value("deploy_state", std::string());
    deploy_max_attempts_ = cfg.value("deploy_max_attempts", 3);
    deploy_retry_delay_ms_ = cfg.value("deploy_retry_delay_ms", 5000);
    for (const nlohmann::json& dll : cfg.value("occt_dlls", nlohmann::json::array())) {
        occt_dlls_.push_back(dll.get<std::string>());
    }

    if (deploy_state_path_.empty() && !program_dir_.empty()) {
        deploy_state_path_ = program_dir_ + "/deploy.json";
    }
    LoadDeployState();

    if (!export_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(export_dir_, ec);
    }

    store_ = CreateCoverageStore(config);
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
    info.set_deployed(deployed_);
    return info.SerializeToString(payload);
}

common::RegisterOutcome ClientPlugin::handleValidationReply(const std::string& reply_payload) {
    coverage::CoverageValidationReply reply;
    if (!reply.ParseFromString(reply_payload)) {
        LOG_ERROR("coverage plugin: unparseable validation reply");
        return common::RegisterOutcome::kFailed;
    }
    if (!reply.deploy_required()) {
        return common::RegisterOutcome::kReady;
    }

    int attempt = 1;
    bool ok = false;
    while (attempt <= deploy_max_attempts_) {
        ok = DeployFromShared(reply.shared_dir());
        if (ok) {
            break;
        }
        if (attempt < deploy_max_attempts_) {
            LOG_WARN("coverage plugin: deploy attempt " << attempt << " failed, retrying in "
                                                        << deploy_retry_delay_ms_ << " ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(deploy_retry_delay_ms_));
        }
        ++attempt;
    }
    if (!ok) {
        LOG_ERROR("coverage plugin: deployment failed after " << deploy_max_attempts_
                                                             << " attempts");
        return common::RegisterOutcome::kFailed;
    }

    program_version_ = reply.target_program_version();
    dataset_version_ = reply.target_dataset_version();
    deployed_ = true;
    ResolveProgramPaths();
    if (!SaveDeployState()) {
        LOG_ERROR("coverage plugin: failed to persist deployment state");
        return common::RegisterOutcome::kFailed;
    }
    LOG_INFO("coverage plugin: deploy to program=" << reply.target_program_version()
                                                   << " dataset=" << reply.target_dataset_version());
    return common::RegisterOutcome::kRetry;
}

bool ClientPlugin::LoadDeployState() {
    if (deploy_state_path_.empty()) {
        return false;
    }
    std::ifstream in(deploy_state_path_);
    if (!in.is_open()) {
        return false;
    }
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.value("deployed", false)) {
        return false;
    }
    program_version_ = json.value("program_version", std::string());
    dataset_version_ = json.value("dataset_version", std::string());
    deployed_ = true;
    LOG_INFO("coverage plugin: deployment state loaded from " << deploy_state_path_);
    return true;
}

bool ClientPlugin::SaveDeployState() {
    if (deploy_state_path_.empty()) {
        LOG_WARN("coverage plugin: deploy_state not configured, deployment state not persisted");
        return true;
    }
    nlohmann::json json;
    json["deployed"] = true;
    json["program_version"] = program_version_;
    json["dataset_version"] = dataset_version_;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(deploy_state_path_).parent_path(),
                                        ec);
    if (ec) {
        LOG_ERROR("coverage plugin: cannot create deploy state dir: " << ec.message());
        return false;
    }
    const std::string tmp_path = deploy_state_path_ + ".tmp";
    {
        std::ofstream out(tmp_path);
        if (!out.is_open()) {
            LOG_ERROR("coverage plugin: cannot open deploy state temp file");
            return false;
        }
        out << json.dump();
        out.flush();
        if (!out.good()) {
            LOG_ERROR("coverage plugin: write deploy state temp file failed");
            return false;
        }
    }
    std::filesystem::rename(tmp_path, deploy_state_path_, ec);
    if (ec) {
        LOG_ERROR("coverage plugin: rename deploy state failed: " << ec.message());
        return false;
    }
    LOG_INFO("coverage plugin: deployment state saved to " << deploy_state_path_);
    return true;
}

bool ClientPlugin::DeployFromShared(const std::string& shared) {
    if (!shared.empty() && (!program_dir_.empty() || !stp_root_.empty())) {
        bool ok = true;
        if (!program_dir_.empty()) {
            ok = CopyTree(shared + "/program", program_dir_) && ok;
        }
        if (!stp_root_.empty()) {
            ok = CopyTree(shared + "/dataset", stp_root_) && ok;
        }
        if (!ok) {
            LOG_ERROR("coverage plugin: deployment copy failed from " << shared);
            return false;
        }
        if (!program_dir_.empty() && !VerifyManifest(program_dir_, true)) {
            LOG_ERROR("coverage plugin: program deployment verification failed");
            return false;
        }
        if (!stp_root_.empty() && !VerifyManifest(stp_root_, false)) {
            LOG_ERROR("coverage plugin: dataset deployment verification failed");
            return false;
        }
        LOG_INFO("coverage plugin: deployed from " << shared << " program=" << program_dir_
                                                   << " dataset=" << stp_root_);
    } else {
        LOG_WARN("coverage plugin: shared_dir/deploy dirs not configured, simulating deployment");
    }
    return true;
}

bool ClientPlugin::VerifyManifest(const std::string& dir, bool hash_files) {
    const std::string manifest_path = dir + "/manifest.txt";
    std::ifstream in(manifest_path);
    if (!in.is_open()) {
        LOG_ERROR("coverage plugin: manifest missing: " << manifest_path);
        return false;
    }
    std::string line;
    size_t checked = 0;
    while (std::getline(in, line)) {
        // Strip a UTF-8 BOM; editors and PowerShell scripts commonly add one
        // to text manifests.
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (line.empty()) {
            continue;
        }
        const size_t first_comma = line.find(',');
        if (first_comma == std::string::npos) {
            LOG_ERROR("coverage plugin: bad manifest line: " << line);
            return false;
        }
        const std::string name = line.substr(0, first_comma);
        const size_t second_comma = line.find(',', first_comma + 1);
        const std::string size_str =
            line.substr(first_comma + 1,
                        second_comma == std::string::npos
                            ? std::string::npos
                            : second_comma - first_comma - 1);
        const std::uint64_t expected_size =
            std::strtoull(size_str.c_str(), nullptr, 10);
        const std::string expected_hash =
            second_comma == std::string::npos ? std::string() : line.substr(second_comma + 1);

        const std::string path = dir + "/" + name;
        std::error_code ec;
        const std::uintmax_t actual_size = std::filesystem::file_size(path, ec);
        if (ec || actual_size != expected_size) {
            LOG_ERROR("coverage plugin: manifest size mismatch " << name << " expected="
                                                                 << expected_size
                                                                 << " actual="
                                                                 << (ec ? 0 : actual_size));
            return false;
        }
        if (hash_files && !expected_hash.empty()) {
            const std::string actual_hash = Sha256Hex(path);
            if (actual_hash.empty() || actual_hash != expected_hash) {
                LOG_ERROR("coverage plugin: manifest hash mismatch " << name);
                return false;
            }
        }
        ++checked;
    }
    if (checked == 0) {
        LOG_ERROR("coverage plugin: manifest is empty: " << manifest_path);
        return false;
    }
    LOG_INFO("coverage plugin: manifest verified " << manifest_path << " files=" << checked);
    return true;
}

void ClientPlugin::ResolveProgramPaths() {
    if (program_dir_.empty()) {
        return;
    }
    auto resolve = [this](const std::string& raw) {
        if (raw.empty()) {
            return raw;
        }
        if (raw.size() >= 2 && raw[1] == ':') {
            return raw;  // drive absolute
        }
        if (raw.rfind("\\\\", 0) == 0 || raw[0] == '/' || raw[0] == '\\') {
            return raw;  // UNC or rooted
        }
        return program_dir_ + "/" + raw;
    };
    target_exe_ = resolve(target_exe_);
    opencppcoverage_ = resolve(opencppcoverage_);
    for (std::string& dll : occt_dlls_) {
        dll = resolve(dll);
    }
}

bool ClientPlugin::RunOneFile(const std::string& run_id, int wave, const std::string& file,
                              std::vector<std::uint64_t>& points) {
    const std::string stp_path = stp_root_ + "/" + file;
    const std::string xml_path =
        NormalizePath(export_dir_ + "/" + std::to_string(Fnv1a64(file)) + ".xml");
    const std::string cov_path =
        NormalizePath(export_dir_ + "/" + std::to_string(Fnv1a64(file)) + ".cov");

    std::string command = "\"" + opencppcoverage_ + "\" --quiet";
    for (const std::string& module : occt_dlls_) {
        command += " --modules=" + NormalizePath(module);
    }
    command += " --export_type=cobertura:" + xml_path;
    if (store_cov_) {
        command += " --export_type=binary:" + cov_path;
    }
    command += " -- \"" + NormalizePath(target_exe_) + "\" \"" + NormalizePath(stp_path) + "\"";

    const int exit_code = RunCommand(command);
    if (exit_code != 0) {
        LOG_WARN("coverage plugin: opencppcoverage exit=" << exit_code << " for " << file);
        return false;
    }
    if (!ParseCobertura(xml_path, points)) {
        LOG_WARN("coverage plugin: failed to parse " << xml_path);
        return false;
    }
    if (store_cov_) {
        std::ifstream cov_in(cov_path, std::ios::binary | std::ios::ate);
        if (cov_in.is_open()) {
            const std::streamsize size = cov_in.tellg();
            std::string data(static_cast<size_t>(size), '\0');
            cov_in.seekg(0);
            cov_in.read(&data[0], size);
            if (!store_->saveFileCov(run_id, wave, file, data,
                                     static_cast<std::int64_t>(size))) {
                LOG_WARN("coverage plugin: failed to store cov for " << file);
                return false;
            }
            std::error_code ec;
            std::filesystem::remove(cov_path, ec);
        } else {
            LOG_WARN("coverage plugin: failed to read cov " << cov_path);
            return false;
        }
    }
    if (!keep_reports_) {
        std::error_code ec;
        std::filesystem::remove(xml_path, ec);
    }
    return true;
}

bool ClientPlugin::execute(const common::Task& task, common::TaskResult& result) {
    coverage::CoverageTask cov_task;
    if (!cov_task.ParseFromString(task.payload())) {
        result.set_exit_code(-1);
        return false;
    }

    coverage::CoverageResult coverage_result;
    for (const std::string& file : cov_task.files()) {
        coverage::FileCoverage entry;
        entry.set_file(file);

        std::vector<std::uint64_t> points;
        bool ok = false;
        if (dry_run_) {
            points = SyntheticPoints(file);
            ok = true;
        } else {
            ok = RunOneFile(cov_task.run_id(), cov_task.wave_id(), file, points);
        }
        // The same (path, line) can appear once per module; dedupe so counts
        // and the point sets are exact.
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        if (ok) {
            ok = store_->saveFileCoverage(cov_task.run_id(), cov_task.wave_id(), file, points);
            if (!ok) {
                LOG_WARN("coverage plugin: failed to store points for " << file);
            }
        }

        entry.set_ok(ok);
        entry.set_covered_lines(static_cast<std::int32_t>(points.size()));
        LOG_INFO("coverage plugin: file " << file << (ok ? " ok" : " failed")
                                          << " covered_lines=" << points.size());
        *coverage_result.add_files() = entry;
    }

    if (!coverage_result.SerializeToString(result.mutable_result())) {
        LOG_ERROR("coverage plugin: failed to serialize task result " << task.task_id());
        result.set_exit_code(-1);
        return false;
    }
    result.set_exit_code(0);
    return true;
}

}  // namespace coverage_plugin
