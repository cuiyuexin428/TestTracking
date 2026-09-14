#pragma once

#include <cstdint>
#include <string>

namespace common {

// FNV-1a hashes. Used for deterministic file-name suffixes and point keys.
std::uint64_t Fnv1a64(const std::string& text);
std::uint32_t Fnv1a32(const std::string& text);

// Normalizes a path for command-line use: '/' -> '\\' on Windows, unchanged
// elsewhere. Used to build converter / coverage command lines.
std::string NormalizePath(const std::string& path);

// Reads a whole text file into out. Returns false when unreadable.
bool ReadTextFile(const std::string& path, std::string* out);

// Replaces every occurrence of token with value (empty token is a no-op).
void ReplaceAll(std::string& text, const std::string& token,
                const std::string& value);

// True when name contains only [A-Za-z0-9_], so a configurable DB identifier
// cannot inject SQL.
bool IsValidIdentifier(const std::string& name);

// Runs a command line and returns its process exit code. On Windows the
// process is created without a console window; -1 means it could not start.
int RunCommand(const std::string& command);

}  // namespace common
