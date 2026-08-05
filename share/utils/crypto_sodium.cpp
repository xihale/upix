#include "crypto_sodium.hpp"
#include <sodium.h>
#include <stdexcept>

namespace crossscale {
namespace sodium {

void init() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}

auto x25519_keygen() -> X25519Keypair {
    X25519Keypair kp;
    crypto_box_keypair(kp.public_key.data(), kp.private_key.data());
    return kp;
}

auto x25519_shared(const std::array<uint8_t, 32>& my_private,
                   const std::array<uint8_t, 32>& peer_public)
    -> std::array<uint8_t, 32> {
    std::array<uint8_t, 32> shared;
    if (crypto_scalarmult_curve25519(shared.data(), my_private.data(),
                                     peer_public.data()) != 0) {
        throw std::runtime_error("X25519 scalarmult failed");
    }
    return shared;
}

auto ed25519_keygen() -> Ed25519Keypair {
    Ed25519Keypair kp;
    crypto_sign_keypair(kp.public_key.data(), kp.private_key.data());
    return kp;
}

auto ed25519_sign(const std::array<uint8_t, 64>& private_key,
                  const uint8_t* msg, size_t len) -> std::array<uint8_t, 64> {
    std::array<uint8_t, 64> sig;
    crypto_sign_detached(sig.data(), nullptr, msg, len, private_key.data());
    return sig;
}

// 注:Ed25519 detached 签名在合法输入下恒成功,返回值无需检查。

auto ed25519_verify(const std::array<uint8_t, 32>& public_key,
                    const std::array<uint8_t, 64>& sig,
                    const uint8_t* msg, size_t len) -> bool {
    return crypto_sign_verify_detached(sig.data(), msg, len,
                                       public_key.data()) == 0;
}

auto secretbox_seal(const uint8_t key[32],
                    const uint8_t* plaintext, size_t len)
    -> std::vector<uint8_t> {
    std::vector<uint8_t> out(crypto_secretbox_NONCEBYTES + len + crypto_secretbox_MACBYTES);
    uint8_t* nonce = out.data();
    uint8_t* ct = out.data() + crypto_secretbox_NONCEBYTES;
    randombytes_buf(nonce, crypto_secretbox_NONCEBYTES);
    crypto_secretbox_easy(ct, plaintext, len, nonce, key);
    return out;
}

auto secretbox_open(const uint8_t key[32],
                    const uint8_t* nonce_ct, size_t nonce_ct_len)
    -> std::optional<std::vector<uint8_t>> {
    if (nonce_ct_len < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) {
        return std::nullopt;
    }
    const uint8_t* nonce = nonce_ct;
    const uint8_t* ct = nonce_ct + crypto_secretbox_NONCEBYTES;
    size_t ct_len = nonce_ct_len - crypto_secretbox_NONCEBYTES;
    std::vector<uint8_t> plaintext(ct_len - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(plaintext.data(), ct, ct_len, nonce, key) != 0) {
        return std::nullopt;
    }
    return plaintext;
}

} // namespace sodium
} // namespace crossscale
