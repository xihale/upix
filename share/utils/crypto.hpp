#pragma once

#include <string>
#include <cstdint>

namespace crossscale {

// Cryptographic utilities used across the system.
namespace crypto {

// SHA-256 hash, returned as lowercase hex.
auto sha256_hex(const std::string& str) -> std::string;

// RFC 4122 v4 UUID.
auto generate_uuid() -> std::string;

} // namespace crypto

} // namespace crossscale
