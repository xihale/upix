#include "crypto.hpp"
#include <format>
#include <random>
#include <cstring>

namespace crossscale {
namespace crypto {

// Portable right rotation
static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static std::random_device& get_rng() {
    static std::random_device rd;
    return rd;
}

// Raw 32-byte SHA-256. Kept internal; only sha256_hex is exposed.
static auto sha256(const void* data, size_t size) -> std::vector<uint8_t> {
    std::vector<uint8_t> hash(32);
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    size_t padded_size = ((size + 9) / 64 + 1) * 64;
    std::vector<uint8_t> padded(padded_size, 0);
    std::memcpy(padded.data(), ptr, size);
    padded[size] = 0x80;

    uint64_t bit_len = size * 8;
    for (int i = 0; i < 8; ++i) {
        padded[padded_size - 1 - i] = (bit_len >> (i * 8)) & 0xFF;
    }

    for (size_t chunk = 0; chunk < padded_size; chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (padded[chunk + i*4] << 24) | (padded[chunk + i*4 + 1] << 16) |
                   (padded[chunk + i*4 + 2] << 8) | padded[chunk + i*4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    for (int i = 0; i < 8; ++i) {
        hash[i*4] = (h[i] >> 24) & 0xFF;
        hash[i*4 + 1] = (h[i] >> 16) & 0xFF;
        hash[i*4 + 2] = (h[i] >> 8) & 0xFF;
        hash[i*4 + 3] = h[i] & 0xFF;
    }

    return hash;
}

auto sha256_hex(const std::string& str) -> std::string {
    auto hash = sha256(str.data(), str.size());
    std::string out;
    out.reserve(hash.size() * 2);
    for (uint8_t b : hash) {
        out += std::format("{:02x}", b);
    }
    return out;
}

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
} // namespace crossscale
