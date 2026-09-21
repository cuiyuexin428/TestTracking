#pragma once

#ifdef _WIN32
#include <atomic>
#include <filesystem>
#include <memory>

namespace client {
// Owns Windows worker processes and applies staged deployments only after drain.
int RunProcessAgent(const std::filesystem::path& executable,
                    const std::filesystem::path& config,
                    const std::shared_ptr<std::atomic<bool>>& stop);
}
#endif
