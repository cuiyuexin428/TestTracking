#pragma once

#include <json.hpp>

#include <string>

namespace common {

// Loads a JSON configuration file and provides typed access with defaults.
// Keys use dot notation for nested objects, e.g. "server.port". The config
// is read-only after load(); not thread-safe by design.
class Config {
public:
    static Config& instance();

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // Throws std::runtime_error when the file is missing or not valid JSON.
    void load(const std::string& path);

    bool loaded() const;

    // Returns default_value when the key is missing or the type mismatches.
    template <typename T>
    T get(const std::string& key, const T& default_value) const {
        const nlohmann::json* node = FindNode(key);
        if (node == nullptr) {
            return default_value;
        }
        try {
            return node->get<T>();
        } catch (const nlohmann::json::exception&) {
            return default_value;
        }
    }

    // Returns false when the key is missing or the type mismatches.
    template <typename T>
    bool get(const std::string& key, T& out) const {
        const nlohmann::json* node = FindNode(key);
        if (node == nullptr) {
            return false;
        }
        try {
            out = node->get<T>();
            return true;
        } catch (const nlohmann::json::exception&) {
            return false;
        }
    }

    // Returns the JSON node for a dotted key, or nullptr when missing.
    const nlohmann::json* find(const std::string& key) const;

private:
    Config() = default;

    const nlohmann::json* FindNode(const std::string& key) const;

    nlohmann::json json_;
    bool loaded_ = false;
};

}  // namespace common
