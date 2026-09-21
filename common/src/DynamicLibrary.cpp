#include "common/DynamicLibrary.h"
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace common {

std::unique_ptr<DynamicLibrary> DynamicLibrary::load(const std::string& path) {
#ifdef _WIN32
    // Uploaded plugin bundles may carry private DLL dependencies beside the
    // plugin; resolve their directory while retaining executable-directory fallback.
    const auto input_path = std::filesystem::u8path(path);
    const auto full_path = std::filesystem::absolute(input_path);
    std::error_code error;
    HMODULE handle = input_path.has_parent_path() || std::filesystem::exists(full_path, error)
        ? LoadLibraryExW(full_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)
        : LoadLibraryW(input_path.c_str());
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (handle == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<DynamicLibrary>(new DynamicLibrary(handle));
}

std::string DynamicLibrary::lastError() {
#ifdef _WIN32
    const DWORD err = GetLastError();
    char buf[512] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                   buf, sizeof(buf), nullptr);
    return std::string(buf);
#else
    const char* message = dlerror();
    return message != nullptr ? message : "";
#endif
}

DynamicLibrary::DynamicLibrary(void* handle) : handle_(handle) {
}

DynamicLibrary::~DynamicLibrary() {
    if (handle_ != nullptr) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }
}

void* DynamicLibrary::findSymbol(const std::string& name) const {
    if (handle_ == nullptr) {
        return nullptr;
    }
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name.c_str()));
#else
    return dlsym(handle_, name.c_str());
#endif
}

}  // namespace common
