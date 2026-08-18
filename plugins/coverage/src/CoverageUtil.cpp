#include "coverage/CoverageUtil.h"

#include "tinyxml2.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace coverage_plugin {

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

bool ParseCoverageTotals(const std::string& xml_path, std::int64_t* covered_lines,
                         std::int64_t* total_lines) {
    if (covered_lines == nullptr || total_lines == nullptr) {
        return false;
    }
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

    std::set<std::pair<std::string, int>> covered_keys;
    std::set<std::pair<std::string, int>> total_keys;
    for (const tinyxml2::XMLElement* pkg = packages->FirstChildElement("package");
         pkg != nullptr; pkg = pkg->NextSiblingElement("package")) {
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
                const int number = line->IntAttribute("number", 0);
                if (number <= 0) {
                    continue;
                }
                const auto key = std::make_pair(path, number);
                total_keys.insert(key);
                if (line->IntAttribute("hits", 0) > 0) {
                    covered_keys.insert(key);
                }
            }
        }
    }
    *covered_lines = static_cast<std::int64_t>(covered_keys.size());
    *total_lines = static_cast<std::int64_t>(total_keys.size());
    return *total_lines > 0;
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

}  // namespace coverage_plugin
