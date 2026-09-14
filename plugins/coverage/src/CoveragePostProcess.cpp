#include "coverage/CoveragePostProcess.h"

#include "coverage/CoverageUtil.h"
#include "tinyxml2.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace coverage_plugin {

std::vector<std::string> SelectGreedyMinSet(
    const std::unordered_map<std::string, std::set<std::uint64_t>>& file_points,
    std::set<std::uint64_t>* cumulative) {
    std::vector<std::string> selected;
    std::unordered_set<std::string> taken;
    while (true) {
        std::string best;
        size_t best_new = 0;
        for (const auto& item : file_points) {
            if (taken.count(item.first)) {
                continue;
            }
            size_t new_count = 0;
            for (std::uint64_t point : item.second) {
                if (!cumulative->count(point)) {
                    ++new_count;
                }
            }
            if (new_count > best_new ||
                (new_count > 0 && new_count == best_new && item.first < best)) {
                best_new = new_count;
                best = item.first;
            }
        }
        if (best.empty() || best_new == 0) {
            break;
        }
        selected.push_back(best);
        taken.insert(best);
        cumulative->insert(file_points.at(best).begin(), file_points.at(best).end());
    }
    return selected;
}

bool MergeCoverageFiles(const std::string& opencppcoverage,
                        const std::vector<std::string>& cov_files,
                        const std::string& out_cov,
                        const std::string& out_xml) {
    if (cov_files.empty()) {
        return false;
    }
    const auto quote = [](const std::string& value) {
        std::string result = "\"";
        for (const char ch : value) {
            if (ch == '\"') {
                result += "\\\"";
            } else {
                result += ch;
            }
        }
        result += '\"';
        return result;
    };
    const auto build_command = [&](const std::vector<std::string>& inputs,
                                   const std::string& output_cov,
                                   const std::string* output_xml) {
        std::string command = quote(opencppcoverage) + " --quiet";
        for (const std::string& input : inputs) {
            command += " --input_coverage=" + quote(input);
        }
        command += " --export_type=binary:" + quote(output_cov);
        if (output_xml != nullptr) {
            command += " --export_type=cobertura:" + quote(*output_xml);
        }
        return command;
    };

    std::vector<std::string> inputs = cov_files;
    std::sort(inputs.begin(), inputs.end());
    int round = 0;
    while (inputs.size() > 2) {
        std::vector<std::string> next_round;
        next_round.reserve((inputs.size() + 1) / 2);
        for (size_t i = 0; i < inputs.size(); i += 2) {
            if (i + 1 == inputs.size()) {
                next_round.push_back(inputs[i]);
                continue;
            }
            const std::string merged_path =
                (std::filesystem::path(out_cov).parent_path() /
                 ("merge-r" + std::to_string(round) + "-p" + std::to_string(i / 2) + ".cov"))
                    .string();
            const std::vector<std::string> pair = {inputs[i], inputs[i + 1]};
            const int exit_code = common::RunCommand(build_command(pair, merged_path, nullptr));
            if (exit_code != 0) {
                return false;
            }
            std::error_code exists_ec;
            if (!std::filesystem::is_regular_file(merged_path, exists_ec) || exists_ec) {
                return false;
            }
            next_round.push_back(merged_path);
        }
        inputs.swap(next_round);
        ++round;
    }
    const int exit_code = common::RunCommand(build_command(inputs, out_cov, &out_xml));
    return exit_code == 0;
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

}  // namespace coverage_plugin
