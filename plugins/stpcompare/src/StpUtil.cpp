#include "stpcompare/StpUtil.h"

#include "common/Util.h"

namespace stpcompare {

std::string BuildProgramCommand(const std::string& program_cmd,
                                const std::string& exe,
                                const std::string& stp_path,
                                const std::string& outdir,
                                const std::string& file,
                                const std::string& run_id) {
    std::string cmd = program_cmd;
    if (cmd.empty()) {
        cmd = "\"" + exe + "\" {stp}";
    }
    common::ReplaceAll(cmd, "{stp}", stp_path);
    common::ReplaceAll(cmd, "{outdir}", outdir);
    common::ReplaceAll(cmd, "{workdir}", outdir);
    common::ReplaceAll(cmd, "{file}", file);
    common::ReplaceAll(cmd, "{run_id}", run_id);
    return cmd;
}

}  // namespace stpcompare
