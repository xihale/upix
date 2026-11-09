//! Connection card + phone-pairing QR payload.
//!
//! Byte layout is contractual — it must stay identical to the C++ CLI card
//! (`cli/src/common.cpp::build_connection_card`) that the Android
//! JoinInviteParser and the Qt `desktop/src/qrprovider.h` image provider
//! consume:
//!
//! ```text
//! {"v":2,"ip":"<first local address>","port":<port>,"fp":"<fingerprint>","pk":"<base64 pubkey>"}
//! ```
//!
//! v1 (no fp/pk) is rejected by Android; "name" was removed deliberately
//! (13 bytes of QR modules for a field the parser never reads).

use base64::Engine as _;

/// v2 card JSON the Android app understands. `addresses[0]` is the QR IP —
/// the same rule the C++ card applies (first address from getifaddrs, with a
/// `127.0.0.1` fallback when enumeration comes up empty).
pub fn pairing_qr_payload(ip: &str, port: u16, fingerprint: &str, pubkey: &[u8; 32]) -> String {
    let pk = base64::engine::general_purpose::STANDARD.encode(pubkey);
    format!("{{\"v\":2,\"ip\":\"{ip}\",\"port\":{port},\"fp\":\"{fingerprint}\",\"pk\":\"{pk}\"}}")
}

/// Full connection card for `host info` / the GUI Host page.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnectionCard {
    pub addresses: Vec<String>,
    pub port: u16,
    pub fingerprint: String,
    pub pubkey_b64: String,
    pub qr_payload: String,
}

/// Build the card from a local-address enumeration (CONTRACT §net pins
/// `tcp::local_ips()`; callers pass the upix-net result here so the fallback
/// below is the only address logic this crate owns).
pub fn build_connection_card(
    port: u16,
    identity_pubkey: &[u8; 32],
    fingerprint: &str,
    mut addresses: Vec<String>,
) -> ConnectionCard {
    if addresses.is_empty() {
        addresses.push("127.0.0.1".to_string());
    }
    let pubkey_b64 = base64::engine::general_purpose::STANDARD.encode(identity_pubkey);
    let qr_payload = pairing_qr_payload(&addresses[0], port, fingerprint, identity_pubkey);
    ConnectionCard {
        addresses,
        port,
        fingerprint: fingerprint.to_string(),
        pubkey_b64,
        qr_payload,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Byte-exactness against `build_connection_card` in legacy/cli/src/common.cpp:
    /// std::format(R"({{"v":2,"ip":"{}","port":{},"fp":"{}","pk":"{}"}})", ...)
    #[test]
    fn payload_matches_cpp_byte_for_byte() {
        let key = [0xABu8; 32];
        let got = pairing_qr_payload("192.168.1.10", 8900, "AABBCCDDEEFF0011", &key);
        // base64(0xAB*32) = q6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6u r padded —
        // 32 bytes → 44 chars with one '='.
        let pk = base64::engine::general_purpose::STANDARD.encode([0xABu8; 32]);
        assert_eq!(
            got,
            format!("{{\"v\":2,\"ip\":\"192.168.1.10\",\"port\":8900,\"fp\":\"AABBCCDDEEFF0011\",\"pk\":\"{pk}\"}}")
        );
        assert!(got.starts_with("{\"v\":2,\"ip\":\"192.168.1.10\",\"port\":8900,"));
        assert_eq!(got.matches('"').count(), 16);
    }

    #[test]
    fn card_falls_back_to_loopback() {
        let card = build_connection_card(1, &[0; 32], "FP", vec![]);
        assert_eq!(card.addresses, vec!["127.0.0.1"]);
        assert!(card.qr_payload.contains("\"ip\":\"127.0.0.1\""));
    }
}
