#include "utils/crypto_sodium.hpp"
#include "core/distributed/identity.hpp"
#include "utils/crypto.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
using namespace crossscale;  // NOLINT
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

    // 5. IdentityKey load_or_create 幂等
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "cs_test_identity";
    fs::remove(tmp);
    {
        auto id1 = IdentityKey::load_or_create(tmp);
        auto id2 = IdentityKey::load_or_create(tmp);
        CHECK(id1.public_key() == id2.public_key());  // 同一文件 -> 同一公钥
    }
    // 验签往返
    {
        auto id = IdentityKey::load_or_create(tmp);
        const char m[] = "id test";
        auto sig = id.sign((const uint8_t*)m, sizeof(m)-1);
        CHECK(ed25519_verify(id.public_key(), sig,
                             (const uint8_t*)m, sizeof(m)-1));
    }
    fs::remove(tmp);
    std::printf("OK identity load_or_create + sign\n");

    // 6. fingerprint 稳定 + 格式 + base64 往返
    std::array<uint8_t,32> pk{}; pk[0] = 0xAB; pk[1] = 0xCD;
    auto fp = fingerprint(pk);
    CHECK(fp.size() == 16);
    // fingerprint 是 sha256 前 8 字节 hex 大写;sha256_hex 返回小写,需转大写比较
    std::string lower = crypto::sha256_hex(
        std::string((const char*)pk.data(), 32)).substr(0, 16);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    CHECK(fp == lower);
    auto b64 = base64_encode(pk.data(), 32);
    auto dec = base64_decode(b64);
    CHECK(dec.has_value());
    CHECK(dec->size() == 32);
    CHECK(std::memcmp(dec->data(), pk.data(), 32) == 0);
    std::printf("OK fingerprint + base64 roundtrip\n");

    // 7. TrustStore add/contains/save/load
    fs::path ts_path = fs::temp_directory_path() / "cs_test_trust";
    fs::remove(ts_path);
    {
        auto ts = TrustStore::load(ts_path);
        CHECK(!ts.contains(pk));
        ts.add(pk);
        CHECK(ts.contains(pk));
        ts.save();
    }
    {
        auto ts2 = TrustStore::load(ts_path);
        CHECK(ts2.contains(pk));
        CHECK(ts2.size() == 1);
    }
    fs::remove(ts_path);
    std::printf("OK truststore add/contains/save/load\n");

    std::printf("ALL CRYPTO TESTS PASSED\n");
    return 0;
}
