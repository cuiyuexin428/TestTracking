#pragma once

// Wire-level compatibility contract for the client/scheduler handshake.
// Keep this value in the shared framework headers so both sides reject
// incompatible peers instead of silently dropping unsupported messages.
namespace common {
inline constexpr unsigned int kProtocolVersion = 1;
inline constexpr const char* kAttemptIdsCapability = "attempt_ids";
}  // namespace common
