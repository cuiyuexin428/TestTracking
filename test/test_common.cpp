// Placeholder test: verifies common compiles, links, and runs.
// Test framework TBD; plain asserts for now.
#include "common/version.h"

#include <cstdio>
#include <cstring>

int main() {
    const char* v = common::version();
    if (v == nullptr || std::strlen(v) == 0) {
        std::fprintf(stderr, "FAIL: common::version() is empty\n");
        return 1;
    }
    std::printf("common::version() = %s\n", v);
    return 0;
}
