#pragma once

#include <string>

namespace stpcompare {

// Substitutes {stp} {outdir} {workdir} {file} {run_id} in program_cmd. Falls
// back to "<exe> {stp}" when program_cmd is empty.
std::string BuildProgramCommand(const std::string& program_cmd,
                                const std::string& exe,
                                const std::string& stp_path,
                                const std::string& outdir,
                                const std::string& file,
                                const std::string& run_id);

}  // namespace stpcompare
