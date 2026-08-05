#include "utils/crypto_sodium.hpp"
#include <sodium.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace crossscale::sodium;

// 断言宏:即使 NDEBUG 仍生效,保证检查执行并打印失败位置。
// 选用 abort() 而非异常,因这是独立测试 main,失败应直接终止。
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

int main() {
    init();

    // 1. X25519 双向派生一致
    auto a = x25519_keygen();
    auto b = x25519_keygen();
    auto sa = x25519_shared(a.private_key, b.public_key);
    auto sb = x25519_shared(b.private_key, a.public_key);
    CHECK(std::memcmp(sa.data(), sb.data(), 32) == 0);
    std::printf("OK x25519 shared secret matches\n");

    // 2. Ed25519 sign/verify
    auto id = ed25519_keygen();
    const char msg[] = "hello crossscale";
    auto sig = ed25519_sign(id.private_key,
                            (const uint8_t*)msg, sizeof(msg) - 1);
    CHECK(ed25519_verify(id.public_key, sig,
                         (const uint8_t*)msg, sizeof(msg) - 1));
    CHECK(!ed25519_verify(id.public_key, sig,
                          (const uint8_t*)"hello crossscalX", sizeof(msg) - 1));
    std::printf("OK ed25519 sign/verify + tamper reject\n");

    // 3. secretbox seal/open 往返
    uint8_t key[32] = {};
    randombytes_buf(key, 32);
    std::vector<uint8_t> pt(1000);
    randombytes_buf(pt.data(), pt.size());
    auto sealed = secretbox_seal(key, pt.data(), pt.size());
    auto opened = secretbox_open(key, sealed.data(), sealed.size());
    CHECK(opened.has_value());
    CHECK(opened->size() == pt.size());
    CHECK(std::memcmp(opened->data(), pt.data(), pt.size()) == 0);
    std::printf("OK secretbox roundtrip\n");

    // 4. secretbox 篡改应失败
    sealed[30] ^= 0xFF;
    auto tampered = secretbox_open(key, sealed.data(), sealed.size());
    CHECK(!tampered.has_value());
    std::printf("OK secretbox tamper reject\n");

    std::printf("ALL CRYPTO TESTS PASSED\n");
    return 0;
}
