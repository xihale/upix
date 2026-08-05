#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace crossscale {
namespace sodium {

// 进程级初始化,幂等。失败抛 std::runtime_error。
void init();

// X25519 (Curve25519 ECDH)
struct X25519Keypair {
    std::array<uint8_t, 32> public_key;
    std::array<uint8_t, 32> private_key;
};
auto x25519_keygen() -> X25519Keypair;
auto x25519_shared(const std::array<uint8_t, 32>& my_private,
                   const std::array<uint8_t, 32>& peer_public)
    -> std::array<uint8_t, 32>;

// Ed25519 (签名身份)
struct Ed25519Keypair {
    std::array<uint8_t, 32> public_key;
    std::array<uint8_t, 64> private_key;  // libsodium seed+pub 拼接
};
auto ed25519_keygen() -> Ed25519Keypair;
auto ed25519_sign(const std::array<uint8_t, 64>& private_key,
                  const uint8_t* msg, size_t len) -> std::array<uint8_t, 64>;
auto ed25519_verify(const std::array<uint8_t, 32>& public_key,
                    const std::array<uint8_t, 64>& sig,
                    const uint8_t* msg, size_t len) -> bool;

// crypto_secretbox (XSalsa20-Poly1305)
// seal: 产出 nonce(24) + ciphertext(plaintext_len + 16 MAC)
auto secretbox_seal(const uint8_t key[32],
                    const uint8_t* plaintext, size_t len)
    -> std::vector<uint8_t>;
// open: 输入 nonce+ciphertext,返回 plaintext;MAC 失败返回 nullopt
auto secretbox_open(const uint8_t key[32],
                    const uint8_t* nonce_ct, size_t nonce_ct_len)
    -> std::optional<std::vector<uint8_t>>;

} // namespace sodium
} // namespace crossscale
