// Placeholder test: verifies common compiles, links, and runs.
// Test framework TBD; plain asserts for now.
#include "common/version.h"
#include "common/Util.h"

#include <cstdio>
#include <cstring>

int main() {
    const char* v = common::version();
    if (v == nullptr || std::strlen(v) == 0) {
        std::fprintf(stderr, "FAIL: common::version() is empty\n");
        return 1;
    }
    if (common::Sha256Hex("abc") !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::fprintf(stderr, "FAIL: SHA-256 implementation returned an unexpected digest\n");
        return 1;
    }
    std::printf("common::version() = %s\n", v);
    return 0;
}
