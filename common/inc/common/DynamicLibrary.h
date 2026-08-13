#pragma once

#include <memory>
#include <string>

namespace common {

// RAII wrapper around platform dynamic library loading
// (LoadLibrary/GetProcAddress on Windows, dlopen/dlsym on POSIX).
class DynamicLibrary {
public:
    // Loads a library; returns nullptr on failure.
    static std::unique_ptr<DynamicLibrary> load(const std::string& path);

    // Human-readable message for the last load/symbol lookup failure.
    static std::string lastError();

    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    // Looks up an exported symbol; returns nullptr when missing.
    void* findSymbol(const std::string& name) const;

    // Typed convenience: auto create = lib->get<CreatePluginFn>("CreatePlugin");
    template <typename T>
    T get(const std::string& name) const {
        return reinterpret_cast<T>(findSymbol(name));
    }

private:
    explicit DynamicLibrary(void* handle);

    void* handle_ = nullptr;
};

}  // namespace common
