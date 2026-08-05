#pragma once

#include "core/distributed/protocol.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace crossscale {

// 包装/解包 secretbox 加密帧。握手前调 seal/open 会断言失败。
// 激活后,所有明文 Protocol::serialize 输出经 seal() 变成 wire bytes,
// 反之 wire 经 open() 还原成明文 header + payload。
class EncryptedSession {
public:
    using SessionKey = std::array<uint8_t, 32>;

    EncryptedSession() = default;

    auto active() const -> bool { return active_; }

    // 激活:传入 X25519 派生的会话密钥。激活后才能 seal/open。
    void activate(const SessionKey& key);

    // seal: 输入明文 header + payload,输出完整 wire bytes。
    // wire = [MessageHeader(15B,type 不变,payload_length 改为 nonce+ct 长度)]
    //        [nonce(24B)][ciphertext(payload_len + 16 MAC)]
    auto seal(const MessageHeader& header, const void* plaintext_payload)
        -> std::vector<uint8_t>;

    // open: 输入完整 wire,返回 {明文 header, 明文 payload}。MAC 失败返回 nullopt。
    auto open(const uint8_t* wire, size_t wire_len)
        -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>>;

private:
    bool active_ = false;
    SessionKey key_{};
};

} // namespace crossscale
