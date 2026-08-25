#include "crypto_session.hpp"
#include "utils/crypto_sodium.hpp"
#include <sodium.h>
#include <cstring>
#include <stdexcept>

namespace crossscale {

void EncryptedSession::activate(const SessionKey& key) {
    key_ = key;
    active_ = true;
}

EncryptedSession::~EncryptedSession() {
    sodium_memzero(&key_, sizeof(key_));
}

namespace {
inline void put_u32(uint8_t* p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
inline void put_u16(uint8_t* p, uint16_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
}

auto EncryptedSession::seal(const MessageHeader& header,
                            const void* plaintext_payload) const -> std::vector<uint8_t> {
    if (!active_) throw std::runtime_error("EncryptedSession::seal before activate");
    const uint32_t pt_len = header.payload_length;
    const size_t nonce_len = 24;  // crypto_box_NONCEBYTES == crypto_secretbox_NONCEBYTES
    const size_t ct_len = pt_len + 16;  // crypto_secretbox_MACBYTES

    // 修改后的 header:payload_length 改为 nonce+ct 长度,checksum 置 0
    // (明文 CRC 会泄露信息,且 Poly1305 MAC 已提供强完整性校验)
    MessageHeader out_hdr = header;
    out_hdr.payload_length = static_cast<uint32_t>(nonce_len + ct_len);
    out_hdr.checksum = 0;

    // 手工 pack 14B header(小端,与 parse_header 读取顺序一致)
    std::vector<uint8_t> wire(Protocol::HEADER_SIZE);
    put_u32(wire.data()+0, out_hdr.magic);
    put_u16(wire.data()+4, static_cast<uint16_t>(out_hdr.type));
    put_u32(wire.data()+6, out_hdr.payload_length);
    put_u32(wire.data()+10, out_hdr.checksum);

    // 产 nonce+ct,追加
    auto nonce_ct = sodium::secretbox_seal(
        key_.data(), static_cast<const uint8_t*>(plaintext_payload), pt_len);
    wire.insert(wire.end(), nonce_ct.begin(), nonce_ct.end());
    return wire;
}

auto EncryptedSession::open(const uint8_t* wire, size_t wire_len) const
    -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>> {
    if (!active_) return std::nullopt;
    if (wire_len < Protocol::HEADER_SIZE) return std::nullopt;

    auto hdr = Protocol::parse_header({wire, Protocol::HEADER_SIZE});
    if (!hdr) return std::nullopt;

    const size_t nonce_ct_len = wire_len - Protocol::HEADER_SIZE;
    if (hdr->payload_length != nonce_ct_len) return std::nullopt;

    auto opened = sodium::secretbox_open(
        key_.data(), wire + Protocol::HEADER_SIZE, nonce_ct_len);
    if (!opened) return std::nullopt;

    // 还原原始 header(payload_length 是明文长度)
    MessageHeader real_hdr = *hdr;
    real_hdr.payload_length = static_cast<uint32_t>(opened->size());
    return std::make_pair(real_hdr, std::move(*opened));
}

} // namespace crossscale
