#include "coverage/CoverageUtil.h"

#include <filesystem>

namespace coverage_plugin {

std::string PackPoints(const std::vector<std::uint64_t>& points) {
    std::string out;
    out.reserve(points.size() * sizeof(std::uint64_t));
    for (std::uint64_t p : points) {
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<char>((p >> (8 * i)) & 0xFF));
        }
    }
    return out;
}

std::vector<std::uint64_t> UnpackPoints(const std::string& data) {
    std::vector<std::uint64_t> out;
    out.reserve(data.size() / sizeof(std::uint64_t));
    for (size_t i = 0; i + 8 <= data.size(); i += 8) {
        std::uint64_t p = 0;
        for (int j = 0; j < 8; ++j) {
            p |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i + j])) << (8 * j);
        }
        out.push_back(p);
    }
    return out;
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
