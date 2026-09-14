#include "common/Manifest.h"
#include "common/Util.h"
#include <fstream>
#include <unordered_set>
#include <json.hpp>
namespace common {
bool ReadManifest(const std::string& path, std::vector<ManifestEntry>* entries,
                  std::string* fingerprint) {
    if (!entries) return false;
    entries->clear();
    std::ifstream input(path, std::ios::binary);
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
            std::string file; std::uint64_t size = 0;
            if (line.front() == '{') {
                auto json = nlohmann::json::parse(line, nullptr, false);
                if (!json.is_object() || !json.contains("path") || !json["path"].is_string() ||
                    (json.contains("size") && !json["size"].is_number_unsigned())) return false;
                file = json["path"].get<std::string>(); size = json.value("size", 0ULL);
            } else {
                const auto comma = line.find_last_of(',');
                if (comma == std::string::npos) file = line;
                else {
                    file = line.substr(0, comma);
                    const auto number=line.substr(comma+1);
                    if(number.empty() || number.find_first_not_of("0123456789")!=std::string::npos) return false;
                    try { size = std::stoull(number); }
                    catch (...) { return false; }
                }
            }
            if (file.empty() || !seen.insert(file).second) return false;
            entries->push_back({file,size});
        }
        if (end == data.size()) break;
        offset = end + 1;
    }
    return !entries->empty();
}
}
