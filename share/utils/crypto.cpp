#include "crypto.hpp"
#include <array>
#include <format>
#include <random>
#include <sodium.h>

namespace upix {
namespace crypto {

static std::random_device& get_rng() {
    static std::random_device rd;
    return rd;
}

// SHA-256 hash, returned as lowercase hex. Backed by libsodium's
// crypto_hash_sha256 (same algorithm as the former hand-rolled impl, so the
// fingerprint bytes are unchanged).
auto sha256_hex(const std::string& str) -> std::string {
    std::array<uint8_t, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(),
                       reinterpret_cast<const unsigned char*>(str.data()),
                       str.size());
    std::string out;
    out.reserve(hash.size() * 2);
    for (uint8_t b : hash) {
        out += std::format("{:02x}", b);
    }
    return out;
}

// RFC 4122 v4 UUID.
auto generate_uuid() -> std::string {
    std::uniform_int_distribution<uint8_t> dis(0, 255);
    auto& rng = get_rng();

    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) {
        bytes[i] = dis(rng);
    }
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    return std::format(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]
    );
}

} // namespace crypto
} // namespace upix
