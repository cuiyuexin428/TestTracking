#include "common/Manifest.h"
#include "common/Util.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <json.hpp>
namespace common {
namespace {
bool ParseSha256(std::string value, std::string* result) {
    if (value.empty()) {
        result->clear();
        return true;
    }
    if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        })) return false;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    *result = std::move(value);
    return true;
}
}
bool ReadManifest(const std::string& path, std::vector<ManifestEntry>* entries,
                  std::string* fingerprint) {
    if (!entries) return false;
    entries->clear();
    std::ifstream input;
    try { input.open(std::filesystem::u8path(path), std::ios::binary); }
    catch (const std::system_error&) {}
#ifdef _WIN32
    if (!input.is_open()) {
        input.clear();
        input.open(path, std::ios::binary);
    }
#endif
    if (!input) return false;
    std::string data((std::istreambuf_iterator<char>(input)), {});
    if (fingerprint) *fingerprint = std::to_string(data.size()) + ":" + std::to_string(Fnv1a64(data));
    std::unordered_set<std::string> seen;
    std::size_t offset = 0;
    while (offset <= data.size()) {
        auto end = data.find('\n', offset);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(offset, end - offset);
        if (offset == 0 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) line.erase(0,3);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        std::size_t first = 0; while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
        line.erase(0, first);
        if (!line.empty()) {
            std::string file, content_sha256; std::uint64_t size = 0; bool has_size = false;
            if (line.front() == '{') {
                auto json = nlohmann::json::parse(line, nullptr, false);
                if (!json.is_object() || !json.contains("path") || !json["path"].is_string() ||
                    (json.contains("size") && !json["size"].is_number_unsigned()) ||
                    (json.contains("size_bytes") && !json["size_bytes"].is_number_unsigned()) ||
                    (json.contains("size") && json.contains("size_bytes")) ||
                    (json.contains("sha256") && !json["sha256"].is_string()) ||
                    (json.contains("content_sha256") && !json["content_sha256"].is_string()) ||
                    (json.contains("sha256") && json.contains("content_sha256"))) return false;
                file = json["path"].get<std::string>();
                if (json.contains("size_bytes")) {
                    size = json["size_bytes"].get<std::uint64_t>(); has_size = true;
                } else if (json.contains("size")) {
                    size = json["size"].get<std::uint64_t>(); has_size = true;
                }
                const auto hash = json.contains("content_sha256")
                    ? json["content_sha256"].get<std::string>()
                    : json.value("sha256", std::string());
                if (!ParseSha256(hash, &content_sha256)) return false;
            } else {
                const auto comma = line.find_last_of(',');
                if (comma == std::string::npos) file = line;
                else {
                    file = line.substr(0, comma);
                    const auto number=line.substr(comma+1);
                    if(number.empty() || number.find_first_not_of("0123456789")!=std::string::npos) return false;
                    try { size = std::stoull(number); has_size = true; }
                    catch (...) { return false; }
                }
            }
            if (file.empty() || !seen.insert(file).second) return false;
            entries->push_back({file,size,has_size,content_sha256});
        }
        if (end == data.size()) break;
        offset = end + 1;
    }
    return !entries->empty();
}
}
