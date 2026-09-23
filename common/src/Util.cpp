#include "common/Util.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>
#include <json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace common {
std::string NormalizeRunParameters(const std::string& parameters) {
    if (parameters.find_first_not_of(" \t\r\n") == std::string::npos) return {};
    const auto value = nlohmann::json::parse(parameters, nullptr, false);
    if (value.is_object()) return value.empty() ? std::string() : value.dump();
    return parameters;
}

namespace {
constexpr std::array<std::uint32_t, 64> kSha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t RotateRight(std::uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

std::string Sha256Bytes(const unsigned char* bytes, std::size_t size) {
    std::vector<unsigned char> message(bytes, bytes + size);
    const std::uint64_t bit_size = static_cast<std::uint64_t>(size) * 8U;
    message.push_back(0x80U);
    while (message.size() % 64U != 56U) message.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(static_cast<unsigned char>((bit_size >> shift) & 0xffU));

    std::array<std::uint32_t, 8> hash = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                         0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                         0x1f83d9abU, 0x5be0cd19U};
    for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t at = offset + i * 4U;
            words[i] = (static_cast<std::uint32_t>(message[at]) << 24U) |
                       (static_cast<std::uint32_t>(message[at + 1]) << 16U) |
                       (static_cast<std::uint32_t>(message[at + 2]) << 8U) |
                       static_cast<std::uint32_t>(message[at + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto s0 = RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^
                            (words[i - 15] >> 3U);
            const auto s1 = RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^
                            (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = hash[0], b = hash[1], c = hash[2], d = hash[3];
        auto e = hash[4], f = hash[5], g = hash[6], h = hash[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto sigma1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temp1 = h + sigma1 + choose + kSha256Constants[i] + words[i];
            const auto sigma0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = sigma0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
        hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : hash) output << std::setw(8) << value;
    return output.str();
}
}  // namespace

std::uint64_t Fnv1a64(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint32_t Fnv1a32(const std::string& text) {
    std::uint32_t hash = 2166136261U;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619U;
    }
    return hash;
}

std::string Sha256Hex(const std::string& data) {
    return Sha256Bytes(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

bool FileSha256Hex(const std::string& path, std::string* out) {
    if (out == nullptr) return false;
    std::ifstream input;
    try { input.open(std::filesystem::u8path(path), std::ios::binary); }
    catch (const std::system_error&) {}
#ifdef _WIN32
    if (!input.is_open()) {
        input.clear();
        input.open(path, std::ios::binary);
    }
#endif
    if (!input) return false;
    std::ostringstream data;
    data << input.rdbuf();
    if (input.bad()) return false;
    *out = Sha256Hex(data.str());
    return true;
}

std::string NormalizePath(const std::string& path) {
#ifdef _WIN32
    std::string out = path;
    for (char& c : out) {
        if (c == '/') {
            c = '\\';
        }
    }
    return out;
#else
    return path;
#endif
}

bool ReadTextFile(const std::string& path, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

void ReplaceAll(std::string& text, const std::string& token,
                const std::string& value) {
    if (token.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
}

bool IsValidIdentifier(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

int RunCommand(const std::string& command) {
#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string mutable_command = command;
    if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
#else
    return std::system(command.c_str());
#endif
}

}  // namespace common
