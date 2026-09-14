#include "common/AtomicFile.h"
#include "common/Logger.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace common {
bool WriteAtomicFile(const std::filesystem::path& target, const std::string& bytes) {
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return false;
    static std::atomic<unsigned long long> sequence{0};
    auto temporary = target;
    temporary += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(sequence.fetch_add(1));
    bool ok = false;
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const auto length = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1 << 20));
        if (!WriteFile(file, bytes.data() + offset, length, &written, nullptr) || written != length) break;
        offset += written;
    }
    ok = offset == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), target.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const int file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = write(file, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) break;
        offset += static_cast<std::size_t>(count);
    }
    ok = offset == bytes.size() && fsync(file) == 0;
    close(file);
    if (ok) {
        std::filesystem::rename(temporary, target, error);
        ok = !error;
    }
#endif
    if (!ok) {
        std::filesystem::remove(temporary, error);
        LOG_ERROR("atomic file: publish failed: " << target);
    }
    return ok;
}

bool ReadCompleteFile(const std::filesystem::path& path, std::string* bytes,
                      std::size_t maximum_size) {
    if (!bytes) return false;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size < 0 || static_cast<unsigned long long>(size) > maximum_size) return false;
    std::string result(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (size > 0 && !input.read(result.data(), size)) return false;
    *bytes = std::move(result);
    return true;
}

FilePublicationLock::FilePublicationLock(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
#ifdef _WIN32
    for (int retry = 0; retry < 100; ++retry) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) { handle_ = file; acquired_ = true; return; }
        if (GetLastError() != ERROR_SHARING_VIOLATION) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    descriptor_ = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (descriptor_ >= 0) acquired_ = flock(descriptor_, LOCK_EX) == 0;
#endif
}
FilePublicationLock::~FilePublicationLock() {
#ifdef _WIN32
    if (handle_) CloseHandle(handle_);
#else
    if (descriptor_ >= 0) close(descriptor_);
#endif
}
}
