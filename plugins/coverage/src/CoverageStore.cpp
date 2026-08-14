#include "coverage/CoverageStore.h"

#include "coverage/CoverageUtil.h"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <utility>

namespace coverage_plugin {

namespace {

std::string WaveDir(const std::string& store_dir, const std::string& run_id, int wave) {
    return (std::filesystem::path(store_dir) / run_id / ("wave-" + std::to_string(wave)))
        .string();
}

std::string PointFileName(const std::string& file) {
    return std::to_string(Fnv1a64(file)) + ".json";
}

class FileCoverageStore : public CoverageStore {
public:
    bool init(const std::string& config) override {
        const nlohmann::json cfg = nlohmann::json::parse(config);
        store_dir_ = cfg.value("store_dir", std::string());
        return !store_dir_.empty();
    }

    void saveFileCoverage(const std::string& run_id, int wave, const std::string& file,
                          const std::vector<std::uint64_t>& points) override {
        nlohmann::json json;
        json["file"] = file;
        for (std::uint64_t point : points) {
            json["points"].push_back(point);
        }

        const std::filesystem::path dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out((dir / PointFileName(file)).string());
        if (out.is_open()) {
            out << json.dump();
        }
    }

    std::unordered_map<std::string, std::set<std::uint64_t>> loadWave(
        const std::string& run_id, int wave) override {
        std::unordered_map<std::string, std::set<std::uint64_t>> file_points;
        const std::filesystem::path dir(WaveDir(store_dir_, run_id, wave));
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            std::ifstream in(entry.path().string());
            if (!in.is_open()) {
                continue;
            }
            const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
            if (json.is_discarded() || !json.contains("file") || !json.contains("points")) {
                continue;
            }
            std::set<std::uint64_t>& points = file_points[json["file"].get<std::string>()];
            for (const nlohmann::json& point : json["points"]) {
                points.insert(point.get<std::uint64_t>());
            }
        }
        return file_points;
    }

    void clearWave(const std::string& run_id, int wave) override {
        std::error_code ec;
        std::filesystem::remove_all(WaveDir(store_dir_, run_id, wave), ec);
    }

    void appendSelected(const std::string& run_id,
                        const std::vector<std::string>& files) override {
        const std::filesystem::path dir(store_dir_);
        std::error_code ec;
        std::filesystem::create_directories(dir / run_id, ec);
        std::ofstream out((dir / run_id / "selected.txt").string(), std::ios::app);
        for (const std::string& file : files) {
            out << file << "\n";
        }
    }

private:
    std::string store_dir_;
};

}  // namespace

std::unique_ptr<CoverageStore> CreateCoverageStore() {
    return std::make_unique<FileCoverageStore>();
}

}  // namespace coverage_plugin
