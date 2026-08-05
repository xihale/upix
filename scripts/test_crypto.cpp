#include "utils/crypto_sodium.hpp"
#include "core/distributed/identity.hpp"
#include "core/distributed/crypto_session.hpp"
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

    // 8. EncryptedSession seal/open 往返
    {
        EncryptedSession s;
        EncryptedSession::SessionKey k{};
        for (auto& b : k) b = 0x42;
        s.activate(k);

        MessageHeader hdr;
        hdr.type = MessageType::MSG_HEARTBEAT;
        std::vector<uint8_t> payload(64);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = (uint8_t)i;
        hdr.payload_length = (uint32_t)payload.size();

        auto wire = s.seal(hdr, payload.data());
        CHECK(wire.size() == 15 + 24 + 64 + 16);

        auto opened = s.open(wire.data(), wire.size());
        CHECK(opened.has_value());
        CHECK(opened->first.type == MessageType::MSG_HEARTBEAT);
        CHECK(opened->first.payload_length == 64);
        CHECK(opened->second.size() == 64);
        CHECK(std::memcmp(opened->second.data(), payload.data(), 64) == 0);

        // 篡改密文应失败(flip a ciphertext byte at offset 15+24+5 = 44)
        wire[44] ^= 0xFF;
        CHECK(!s.open(wire.data(), wire.size()));
        printf("OK encrypted session seal/open + tamper reject\n");
    }

    // 9. EncryptedSession empty payload round-trip (MSG_HEARTBEAT-style)
    {
        EncryptedSession s;
        EncryptedSession::SessionKey k{};
        for (auto& b : k) b = 0x42;
        s.activate(k);
        MessageHeader hdr;
        hdr.type = MessageType::MSG_HEARTBEAT;
        hdr.payload_length = 0;
        auto wire = s.seal(hdr, nullptr);
        CHECK(wire.size() == 15 + 24 + 0 + 16);  // 55
        auto opened = s.open(wire.data(), wire.size());
        CHECK(opened.has_value());
        CHECK(opened->first.payload_length == 0);
        CHECK(opened->second.empty());
        printf("OK encrypted session empty payload\n");
    }

    // 10. open rejects too-short / tampered wire
    {
        EncryptedSession s;
        EncryptedSession::SessionKey k{};
        s.activate(k);
        // too short (< 15)
        uint8_t tiny[10] = {};
        CHECK(!s.open(tiny, sizeof(tiny)));
        // header-only with bogus payload_length
        std::vector<uint8_t> bogus(15 + 24 + 16, 0);  // min-size but all-zero nonce+ct
        // parse_header needs magic; set it so we get past header parse to MAC fail
        // magic = 0x53534358 little-endian
        bogus[0]=0x58; bogus[1]=0x43; bogus[2]=0x53; bogus[3]=0x53;
        bogus[4]=2;  // version
        // type=0x0006 (HEARTBEAT) little-endian
        bogus[5]=0x06; bogus[6]=0x00;
        // payload_length = 40 (24+16) little-endian
        bogus[7]=40; bogus[8]=0; bogus[9]=0; bogus[10]=0;
        // checksum = 0
        // bytes 15.. end are all zero nonce+ct → MAC will fail
        CHECK(!s.open(bogus.data(), bogus.size()));
        printf("OK encrypted session rejects short/bogus wire\n");
    }

    std::printf("ALL CRYPTO TESTS PASSED\n");
    return 0;
}
