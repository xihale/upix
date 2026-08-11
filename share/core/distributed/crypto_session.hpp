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
    ~EncryptedSession();

    auto active() const -> bool { return active_; }

    // 激活:传入 X25519 派生的会话密钥。激活后才能 seal/open。
    void activate(const SessionKey& key);

    // seal: 输入明文 header + payload,输出完整 wire bytes。
    // wire = [MessageHeader(15B,type 不变,payload_length 改为 nonce+ct 长度)]
    //        [nonce(24B)][ciphertext(payload_len + 16 MAC)]
    auto seal(const MessageHeader& header, const void* plaintext_payload) const
        -> std::vector<uint8_t>;

    // open: 输入完整 wire,返回 {明文 header, 明文 payload}。MAC 失败返回 nullopt。
    auto open(const uint8_t* wire, size_t wire_len) const
        -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>>;

    // ── 数据流(大 payload)明文旁路 ──────────────────────────────────────────
    // 控制帧(握手/DEVICE_PROFILE/CONNECT_*/HEARTBEAT 等)始终走 seal/open,
    // 不受此开关影响。仅图像 tile / 结果数据通过 seal_stream/open_stream,
    // 以便在吞吐优先时降级为明文传输。默认明文(stream_plaintext_=true)。
    void set_stream_plaintext(bool plaintext) { stream_plaintext_ = plaintext; }
    bool stream_plaintext() const { return stream_plaintext_; }

    // seal_stream: 明文模式时按原样序列化(无 nonce/ct),加密模式时等价 seal()。
    auto seal_stream(const MessageHeader& header, const void* plaintext_payload) const
        -> std::vector<uint8_t>;
    // open_stream: 与 seal_stream 对应。明文模式直接解析 header+payload,
    // 加密模式等价 open()。两种模式由调用方按同一 session 状态配对。
    auto open_stream(const uint8_t* wire, size_t wire_len) const
        -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>>;

private:
    bool active_ = false;
    bool stream_plaintext_ = true;  // 默认数据流明文(吞吐优先)
    SessionKey key_{};
};

} // namespace crossscale
