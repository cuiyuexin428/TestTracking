#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace common {
struct ManifestEntry { std::string path; std::uint64_t size = 0; };
bool ReadManifest(const std::string& path, std::vector<ManifestEntry>* entries,
                  std::string* fingerprint = nullptr);
}
