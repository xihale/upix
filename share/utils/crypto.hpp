#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace crossscale {

// Simple cryptographic utilities
namespace crypto {

// SHA-256 hash
auto sha256(const void* data, size_t size) -> std::vector<uint8_t>;
auto sha256(const std::string& str) -> std::vector<uint8_t>;
auto sha256_hex(const std::string& str) -> std::string;

// MD5 hash (for non-security purposes like quick file identification)
auto md5(const void* data, size_t size) -> std::vector<uint8_t>;
auto md5_hex(const std::string& str) -> std::string;

// Generate random bytes
auto random_bytes(size_t size) -> std::vector<uint8_t>;

// Generate random hex string
auto random_hex(size_t size) -> std::string;

// Base64 encode/decode
auto base64_encode(const void* data, size_t size) -> std::string;
auto base64_encode(const std::vector<uint8_t>& data) -> std::string;
auto base64_decode(const std::string& encoded) -> std::vector<uint8_t>;

// XOR encryption (simple, for auth key obfuscation)
auto xor_encrypt(const std::string& data, const std::string& key) -> std::string;
auto xor_decrypt(const std::string& data, const std::string& key) -> std::string;

// Simple hash for quick comparison
uint64_t hash64(const void* data, size_t size);
uint32_t hash32(const void* data, size_t size);

// UUID generation
auto generate_uuid() -> std::string;

// File hash (for model verification)
auto file_hash(const std::string& path) -> std::string;

} // namespace crypto

} // namespace crossscale
