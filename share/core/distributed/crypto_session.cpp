#include "crypto_session.hpp"
#include "utils/crypto_sodium.hpp"
#include <cassert>
#include <cstring>

namespace crossscale {

void EncryptedSession::activate(const SessionKey& key) {
    key_ = key;
    active_ = true;
}

namespace {
inline void put_u32(uint8_t* p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
inline void put_u16(uint8_t* p, uint16_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
}

auto EncryptedSession::seal(const MessageHeader& header,
                            const void* plaintext_payload) -> std::vector<uint8_t> {
    assert(active_ && "seal before activate");
    const uint32_t pt_len = header.payload_length;
    const size_t nonce_len = 24;  // crypto_box_NONCEBYTES == crypto_secretbox_NONCEBYTES
    const size_t ct_len = pt_len + 16;  // crypto_secretbox_MACBYTES

    // 修改后的 header:payload_length 改为 nonce+ct 长度,checksum 保留明文 CRC(诊断用)
    MessageHeader out_hdr = header;
    out_hdr.payload_length = static_cast<uint32_t>(nonce_len + ct_len);
    out_hdr.checksum = Protocol::compute_checksum(plaintext_payload, pt_len);

    // 手工 pack 15B header(小端,与 parse_header 读取顺序一致)
    std::vector<uint8_t> wire(Protocol::HEADER_SIZE);
    put_u32(wire.data()+0, out_hdr.magic);
    wire[4] = out_hdr.version;
    put_u16(wire.data()+5, static_cast<uint16_t>(out_hdr.type));
    put_u32(wire.data()+7, out_hdr.payload_length);
    put_u32(wire.data()+11, out_hdr.checksum);

    // 产 nonce+ct,追加
    auto nonce_ct = sodium::secretbox_seal(
        key_.data(), static_cast<const uint8_t*>(plaintext_payload), pt_len);
    wire.insert(wire.end(), nonce_ct.begin(), nonce_ct.end());
    return wire;
}

auto EncryptedSession::open(const uint8_t* wire, size_t wire_len)
    -> std::optional<std::pair<MessageHeader, std::vector<uint8_t>>> {
    assert(active_ && "open before activate");
    if (wire_len < Protocol::HEADER_SIZE) return std::nullopt;

    auto hdr = Protocol::parse_header(wire, Protocol::HEADER_SIZE);
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
