#include "coverage/ClientPlugin.h"

#include "common/Logger.h"

#include "coverage.pb.h"
#include "coverage/CoverageUtil.h"
#include "tinyxml2.h"

#include <json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace coverage_plugin {

namespace {

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

int RunCommand(const std::string& command) {
#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string mutable_command = command;
    if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
#else
    return std::system(command.c_str());
#endif
}

std::string CanonicalPath(const std::string& filename,
                          const std::vector<std::string>& sources) {
    if (filename.size() >= 2 && filename[1] == ':') {
        return filename;  // already absolute
    }
    for (const std::string& root : sources) {
        const std::string candidate = root + "\\" + filename;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    // Deterministic fallback: identity stays stable even when the build
    // machine's source tree is not present locally.
    return sources.empty() ? filename : (sources[0] + "\\" + filename);
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

std::string ClientPlugin::taskType() const {
    return "coverage";
}

bool ClientPlugin::init(const std::string& config) {
    const nlohmann::json cfg = nlohmann::json::parse(config);
    dry_run_ = cfg.value("dry_run", true);
    stp_root_ = cfg.value("stp_root", std::string());
    opencppcoverage_ = cfg.value("opencppcoverage", std::string("OpenCppCoverage.exe"));
    target_exe_ = cfg.value("target_exe", std::string());
    export_dir_ = cfg.value("export_dir", std::string());
    keep_reports_ = cfg.value("keep_reports", false);
    for (const nlohmann::json& dll : cfg.value("occt_dlls", nlohmann::json::array())) {
        occt_dlls_.push_back(dll.get<std::string>());
    }

    if (!export_dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(export_dir_, ec);
    }

    store_ = CreateCoverageStore();
    if (!store_->init(config)) {
        LOG_ERROR("coverage plugin: coverage store init failed");
        return false;
    }
    return true;
}

bool ClientPlugin::RunOneFile(const std::string& run_id, int wave, const std::string& file,
                              std::vector<std::uint64_t>& points) {
    (void)run_id;
    (void)wave;
    const std::string stp_path = stp_root_ + "/" + file;
    const std::string xml_path =
        NormalizePath(export_dir_ + "/" + std::to_string(Fnv1a64(file)) + ".xml");

    std::string command = "\"" + opencppcoverage_ + "\" --quiet";
    for (const std::string& module : occt_dlls_) {
        command += " --modules=" + NormalizePath(module);
    }
    command += " --export_type=cobertura:" + xml_path;
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
    if (!keep_reports_) {
        std::error_code ec;
        std::filesystem::remove(xml_path, ec);
    }
    return true;
}

bool ClientPlugin::execute(const common::TaskSpec& task, common::TaskResult& result) {
    coverage::CoverageTask coverage_task;
    if (!coverage_task.ParseFromString(task.payload())) {
        result.set_exit_code(-1);
        return false;
    }

    coverage::CoverageResult coverage_result;
    for (const std::string& file : coverage_task.files()) {
        coverage::FileCoverage* entry = coverage_result.add_files();
        entry->set_file(file);

        std::vector<std::uint64_t> points;
        bool ok = false;
        if (dry_run_) {
            points = SyntheticPoints(file);
            ok = true;
        } else {
            ok = RunOneFile(coverage_task.run_id(), coverage_task.wave_id(), file, points);
        }
        // The same (path, line) can appear once per module; dedupe so counts
        // and the point sets are exact.
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        if (ok) {
            store_->saveFileCoverage(coverage_task.run_id(), coverage_task.wave_id(), file,
                                     points);
        }

        entry->set_ok(ok);
        entry->set_covered_lines(static_cast<std::int32_t>(points.size()));
        LOG_INFO("coverage plugin: file " << file << (ok ? " ok" : " failed")
                                          << " covered_lines=" << points.size());
    }
    coverage_result.SerializeToString(result.mutable_result());
    result.set_exit_code(0);
    return true;
}

}  // namespace coverage_plugin
