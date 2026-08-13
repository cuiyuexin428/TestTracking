#include "common/DynamicLibrary.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace common {

std::unique_ptr<DynamicLibrary> DynamicLibrary::load(const std::string& path) {
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path.c_str());
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
