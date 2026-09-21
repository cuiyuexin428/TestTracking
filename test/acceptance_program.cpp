// Executable fixture for real process / OpenCppCoverage / four-output DB tests.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

extern "C" __declspec(dllimport) std::uint64_t acceptance_compute(
    const unsigned char*, std::size_t);

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path input, output;
    if (argc == 2) input = argv[1];
    else if (argc == 5 && std::string(argv[1]) == "--input" &&
             std::string(argv[3]) == "--output-dir") {
        input = argv[2]; output = argv[4];
    } else return 2;
    std::ifstream source(input, std::ios::binary);
    if (!source) return 3;
    const std::string bytes((std::istreambuf_iterator<char>(source)), {});
    const auto checksum = acceptance_compute(
        reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!output.empty()) {
        fs::create_directories(output);
        for (const auto* name : {"info.txt", "info_new.txt"}) {
            std::ofstream info(output / name, std::ios::binary);
            info << "acceptance-fixture\nrole=" << output.filename().string()
                 << "\nfile=" << input.filename().string() << "\noutput=" << name
                 << "\nsize=" << bytes.size() << "\nchecksum=" << checksum << '\n';
            if (!info) return 4;
        }
    }
    return 0;
}
