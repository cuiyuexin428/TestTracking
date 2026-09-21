// Instrumentable native workload for packaged acceptance tests only.
// This does not implement STEP conversion or validate business geometry.
#include <cstddef>
#include <cstdint>

extern "C" __declspec(dllexport) std::uint64_t acceptance_compute(
    const unsigned char* bytes, std::size_t size) {
    std::uint64_t result = 14695981039346656037ULL;
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] >= '0' && bytes[index] <= '9') {
            result ^= bytes[index] - '0';
        } else if (bytes[index] == '\n') {
            result ^= 257;
        } else {
            result ^= bytes[index];
        }
        result *= 1099511628211ULL;
    }
    return result;
}
