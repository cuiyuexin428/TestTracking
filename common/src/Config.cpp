#include "common/Config.h"

#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace common {

Config& Config::instance() {
    static Config config;
    return config;
}

void Config::load(const std::string& path) {
    std::ifstream in;
    try {
        in.open(std::filesystem::u8path(path));
    } catch (const std::filesystem::filesystem_error&) {
        // Older entry points still receive Windows ANSI argv. New callers
        // (including the process Agent) consistently pass UTF-8 paths.
    }
#ifdef _WIN32
    if (!in.is_open()) {
        in.clear();
        in.open(path);
    }
#endif
    if (!in.is_open()) {
        throw std::runtime_error("Config: cannot open file: " + path);
    }
    try {
        json_ = nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("Config: invalid JSON in " + path + ": " + e.what());
    }
    loaded_ = true;
}

bool Config::loaded() const {
    return loaded_;
}

const nlohmann::json* Config::find(const std::string& key) const {
    return FindNode(key);
}

const nlohmann::json* Config::FindNode(const std::string& key) const {
    const nlohmann::json* node = &json_;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = key.find('.', start);
        const std::string part =
            key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!node->is_object()) {
            return nullptr;
        }
        const auto it = node->find(part);
        if (it == node->end()) {
            return nullptr;
        }
        node = &(*it);
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return node;
}

}  // namespace common
