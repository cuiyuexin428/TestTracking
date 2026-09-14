#pragma once

#include "common/CancellationToken.h"

#include <chrono>
#include <string>
#include <vector>

namespace common {

struct ProcessOptions {
    std::string executable;
    std::vector<std::string> arguments;
    std::string working_directory;
    std::chrono::milliseconds timeout{0};  // zero means no deadline
};

struct ProcessResult {
    bool started = false;
    int exit_code = -1;
    bool cancelled = false;
    bool timed_out = false;
    std::string error;
};

// Synchronous adapter, intended for worker threads. Arguments are literal
// argv values, never shell fragments. Does not change the caller's cwd.
// Cancellation/timeout terminate the process and its descendants before return.
class ProcessRunner {
public:
    static ProcessResult run(const ProcessOptions& options,
                             const CancellationToken& cancellation = {});
};

}  // namespace common
