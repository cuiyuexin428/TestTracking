#pragma once
#include <filesystem>
#include <string>

namespace common {
// Flush a complete sibling temporary file before atomically replacing target.
bool WriteAtomicFile(const std::filesystem::path& target, const std::string& bytes);
bool ReadCompleteFile(const std::filesystem::path& path, std::string* bytes,
                      std::size_t maximum_size = 512ULL * 1024 * 1024);
// Serializes publishers across processes. The lock file is permanent; OS locks
// are released on close/crash so a killed publisher cannot strand the lock.
class FilePublicationLock {
public:
    explicit FilePublicationLock(const std::filesystem::path& path);
    ~FilePublicationLock();
    FilePublicationLock(const FilePublicationLock&) = delete;
    FilePublicationLock& operator=(const FilePublicationLock&) = delete;
    bool acquired() const { return acquired_; }
private:
    void* handle_ = nullptr;
    int descriptor_ = -1;
    bool acquired_ = false;
};
}
