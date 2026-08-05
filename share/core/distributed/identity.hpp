#pragma once

#include "utils/crypto_sodium.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace crossscale {

// 持久 Ed25519 身份。加载或新建并落盘。
class IdentityKey {
public:
    // 路径不存在则生成新密钥对并写文件(权限 0600)。存在则加载。
    // 失败抛 std::runtime_error。
    static auto load_or_create(const std::filesystem::path& file) -> IdentityKey;

    auto public_key() const -> std::array<uint8_t, 32> { return kp_.public_key; }
    auto sign(const uint8_t* msg, size_t len) const -> std::array<uint8_t, 64>;

    // 文件格式:64 字节 libsodium Ed25519 secret key = seed(32) || pk(32)。
    // (crypto_sign_SECRETKEYBYTES == 64; 公钥可从 sk 后 32 字节直接读出。)
    // 加载后 zero 内存由析构负责。
    ~IdentityKey();

private:
    explicit IdentityKey(sodium::Ed25519Keypair kp) : kp_(kp) {}
    sodium::Ed25519Keypair kp_;
};

// 信任白名单:每行一个 base64 的 32 字节公钥。
class TrustStore {
public:
    static auto load(const std::filesystem::path& file) -> TrustStore;
    auto contains(const std::array<uint8_t, 32>& pubkey) const -> bool;
    void add(const std::array<uint8_t, 32>& pubkey);  // 加到内存
    void save() const;  // 落盘(若 file 非空)
    auto size() const -> size_t { return entries_.size(); }

private:
    TrustStore(std::filesystem::path file,
               std::vector<std::array<uint8_t, 32>> entries)
        : file_(std::move(file)), entries_(std::move(entries)) {}
    std::filesystem::path file_;
    std::vector<std::array<uint8_t, 32>> entries_;
};

// sha256(pubkey) 前 8 字节 hex 大写,如 "ABCD1234EF567890"
auto fingerprint(const std::array<uint8_t, 32>& pubkey) -> std::string;

// base64 编解码(32 字节公钥 <-> 字符串,供存储/QR/TXT 用)
auto base64_encode(const uint8_t* data, size_t len) -> std::string;
auto base64_decode(std::string_view s) -> std::optional<std::vector<uint8_t>>;

} // namespace crossscale
