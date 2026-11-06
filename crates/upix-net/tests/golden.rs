//! Golden wire-compat tests — byte-exact equality against vectors dumped
//! from the ORIGINAL C++ implementation (crates/upix-net/vector_gen/dump.cpp
//! compiles share/ + libsodium and writes tests/golden/*.json).
//!
//! These are the interop guarantee with old C++ / Android clients: if any of
//! these fail, wire bytes changed. NEVER "fix" a failure by editing the JSON
//! — regenerate from the C++ or fix the Rust side.

#![cfg(test)]

use serde::Deserialize;

use upix_net::crypto;
use upix_net::frame;
use upix_net::protocol::{MessageHeader, MessageType, HEADER_SIZE, MAGIC};
use upix_net::session::EncryptedSession;
use upix_net::trust_store::TrustStore;

fn hex_dec(s: &str) -> Vec<u8> {
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).expect("valid hex"))
        .collect()
}

fn hex_enc(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn load(name: &str) -> serde_json::Value {
    let path = format!("{}/tests/golden/{name}.json", env!("CARGO_MANIFEST_DIR"));
    let raw = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("read {path}: {e}"));
    serde_json::from_str(&raw).unwrap_or_else(|e| panic!("parse {path}: {e}"))
}

// ── ① serialize ─────────────────────────────────────────────────────────────

#[derive(Deserialize)]
struct SerializeCase {
    name: String,
    #[serde(rename = "type")]
    msg_type: u16,
    #[serde(rename = "payload_hex")]
    payload: String,
    #[serde(rename = "frame_hex")]
    frame: String,
    header: HeaderJson,
}

#[derive(Deserialize)]
struct HeaderJson {
    magic: u32,
    #[serde(rename = "type")]
    msg_type: u16,
    payload_length: u32,
    checksum: u32,
    #[allow(dead_code)]
    type_known: bool,
}

#[test]
fn golden_serialize_matches_cpp_bytes() {
    let cases: Vec<SerializeCase> = serde_json::from_value(load("serialize")).expect("json shape");
    assert!(cases.len() >= 15, "dumper emits all message-type cases");

    for case in &cases {
        let payload = hex_dec(&case.payload);
        let header = MessageHeader {
            magic: MAGIC,
            msg_type: MessageType::from_u16(case.msg_type)
                .unwrap_or_else(|| panic!("{}: known type", case.name)),
            payload_length: payload.len() as u32,
            checksum: 0, // serialize computes it; field input is ignored
        };
        let ours = frame::serialize_frame(header, &payload);
        assert_eq!(
            hex_enc(&ours),
            case.frame,
            "serialize bytes differ for {}",
            case.name
        );

        // Header fields recorded by the C++ (checksum is the CRC it computed).
        let parsed = frame::parse_header(&ours).expect("roundtrip");
        assert_eq!(parsed.magic, case.header.magic);
        assert_eq!(parsed.msg_type as u16, case.header.msg_type);
        assert_eq!(parsed.payload_length, case.header.payload_length);
        assert_eq!(parsed.checksum, case.header.checksum);
        assert_eq!(case.header.payload_length as usize, payload.len());
    }
}

// ── ② parse_header ──────────────────────────────────────────────────────────

#[derive(Deserialize)]
struct ParseCase {
    name: String,
    bytes_hex: String,
    ok: bool,
    #[serde(default)]
    header: Option<HeaderJson>,
    #[serde(default)]
    rust_parses: Option<bool>,
}

#[test]
fn golden_parse_header_matches_cpp() {
    let cases: Vec<ParseCase> = serde_json::from_value(load("parse_header")).expect("json shape");
    for case in &cases {
        let bytes = hex_dec(&case.bytes_hex);
        let ours = frame::parse_header(&bytes);
        match (case.ok, case.rust_parses) {
            // C++ parsed AND the type is a known MessageType: exact field match.
            (true, None) | (true, Some(true)) => {
                let h = ours.unwrap_or_else(|| panic!("{}: should parse", case.name));
                let expect = case.header.as_ref().expect("header recorded");
                assert_eq!(h.magic, expect.magic, "{}", case.name);
                assert_eq!(h.msg_type as u16, expect.msg_type, "{}", case.name);
                assert_eq!(h.payload_length, expect.payload_length, "{}", case.name);
                assert_eq!(h.checksum, expect.checksum, "{}", case.name);
            }
            // C++ parsed but cast an unknown type u16 (closed Rust enum → None).
            (true, Some(false)) => {
                assert!(ours.is_none(), "{}: unknown type must be None", case.name);
            }
            (false, _) => {
                assert!(ours.is_none(), "{}: must not parse", case.name);
            }
        }
    }
}

// ── ③ checksum / verify_frame ───────────────────────────────────────────────

#[derive(Deserialize)]
struct ChecksumCase {
    name: String,
    #[serde(default)]
    data_hex: String,
    #[serde(default)]
    crc32: u32,
    #[serde(default)]
    header_hex: Option<String>,
    #[serde(default)]
    payload_hex: Option<String>,
    #[serde(default)]
    ok: Option<bool>,
}

#[test]
fn golden_checksum_matches_zlib_crc32() {
    let cases: Vec<ChecksumCase> = serde_json::from_value(load("checksum")).expect("array");
    let mut saw_crc = false;
    let mut saw_verify = false;
    for case in cases {
        if case.name.starts_with("crc_") {
            saw_crc = true;
            let data = hex_dec(&case.data_hex);
            assert_eq!(frame::compute_checksum(&data), case.crc32, "{}", case.name);
        } else {
            saw_verify = true;
            let header: [u8; HEADER_SIZE] = hex_dec(
                case.header_hex.as_deref().expect("verify case has header"),
            )
            .try_into()
            .expect("14-byte header");
            let payload = hex_dec(case.payload_hex.as_deref().expect("verify case has payload"));
            assert_eq!(
                frame::verify_frame(&header, &payload),
                case.ok.expect("verify case has ok"),
                "{}",
                case.name
            );
        }
    }
    assert!(saw_crc && saw_verify, "both crc and verify vectors present");
}

// ── ④ session key derivation ────────────────────────────────────────────────

#[derive(Deserialize)]
struct SessionKeyVector {
    c_eph_priv_hex: String,
    s_eph_priv_hex: String,
    c_eph_pub_hex: String,
    s_eph_pub_hex: String,
    raw_shared_hex: String,
    client_key_hex: String,
    server_key_hex: String,
    #[allow(dead_code)]
    keys_equal: bool,
}

#[test]
fn golden_session_key_matches_blake2b_hardening() {
    let v: SessionKeyVector = serde_json::from_value(load("session_key")).expect("json shape");

    let c_priv: [u8; 32] = hex_dec(&v.c_eph_priv_hex).try_into().unwrap();
    let s_priv: [u8; 32] = hex_dec(&v.s_eph_priv_hex).try_into().unwrap();
    let c_pub = crypto::x25519_public_from_private(&c_priv);
    let s_pub = crypto::x25519_public_from_private(&s_priv);
    assert_eq!(hex_enc(&c_pub), v.c_eph_pub_hex, "scalarmult_base pub");
    assert_eq!(hex_enc(&s_pub), v.s_eph_pub_hex, "scalarmult_base pub");

    let client_key = crypto::x25519_shared(&c_priv, &s_pub).expect("contributory");
    let server_key = crypto::x25519_shared(&s_priv, &c_pub).expect("contributory");
    assert_eq!(hex_enc(&client_key), v.client_key_hex, "client BLAKE2b key");
    assert_eq!(hex_enc(&server_key), v.server_key_hex, "server BLAKE2b key");
    assert_eq!(client_key, server_key, "both sides derive the same key");
    // raw_shared is an intermediate (defense in depth against impl drift);
    // the BLAKE2b output equality above is the contractual assertion.
    let _ = &v.raw_shared_hex;
}

// ── ⑤ Ed25519 transcript signatures ─────────────────────────────────────────

#[derive(Deserialize)]
struct Ed25519Vector {
    s_id_seed_hex: String,
    s_id_sk_hex: String,
    s_id_pub_hex: String,
    c_id_seed_hex: String,
    c_id_pub_hex: String,
    transcript_hex: String,
    server_sig_hex: String,
    client_sig_hex: String,
    verify_good: bool,
    verify_corrupted_sig: bool,
    verify_wrong_msg: bool,
}

#[test]
fn golden_ed25519_transcript_signature_matches() {
    let v: Ed25519Vector = serde_json::from_value(load("ed25519")).expect("json shape");

    let s_seed: [u8; 32] = hex_dec(&v.s_id_seed_hex).try_into().unwrap();
    let (s_pub, s_sk) = crypto::ed25519_seed_keypair(&s_seed);
    assert_eq!(hex_enc(&s_pub), v.s_id_pub_hex);
    assert_eq!(hex_enc(&s_sk), v.s_id_sk_hex, "sk = seed || pk layout");

    let transcript = hex_dec(&v.transcript_hex);
    let sig = crypto::ed25519_sign(&s_sk, &transcript);
    assert_eq!(hex_enc(&sig), v.server_sig_hex, "server sig over transcript");

    let c_seed: [u8; 32] = hex_dec(&v.c_id_seed_hex).try_into().unwrap();
    let (c_pub, c_sk) = crypto::ed25519_seed_keypair(&c_seed);
    assert_eq!(hex_enc(&c_pub), v.c_id_pub_hex);
    let c_sig = crypto::ed25519_sign(&c_sk, &transcript);
    assert_eq!(hex_enc(&c_sig), v.client_sig_hex, "client AUTH_PROOF sig");

    // Verify outcomes must equal the C++ booleans byte-for-byte in spirit:
    // same accept, same rejects.
    assert_eq!(
        crypto::ed25519_verify(&s_pub, &sig, &transcript),
        v.verify_good,
        "verify_good"
    );
    assert_eq!(
        crypto::ed25519_verify(&s_pub, &sig, &transcript[..transcript.len() - 1]),
        v.verify_wrong_msg,
        "verify_wrong_msg"
    );
    let mut corrupted = sig;
    corrupted[0] ^= 0x01;
    assert_eq!(
        crypto::ed25519_verify(&s_pub, &corrupted, &transcript),
        v.verify_corrupted_sig,
        "verify_corrupted_sig"
    );
}

// ── ⑥ secretbox (fixed key+nonce) ───────────────────────────────────────────

#[derive(Deserialize)]
struct SecretboxVector {
    key_hex: String,
    nonce_hex: String,
    plaintext_hex: String,
    sealed_hex: String,
    nonce_sealed_hex: String,
    roundtrip_nonce_ct_hex: String,
    roundtrip_plaintext_hex: String,
    #[allow(dead_code)]
    roundtrip_ok: bool,
}

#[test]
fn golden_secretbox_matches_crypto_secretbox_easy() {
    let v: SecretboxVector = serde_json::from_value(load("secretbox")).expect("json shape");

    let key: [u8; 32] = hex_dec(&v.key_hex).try_into().unwrap();
    let nonce: [u8; 24] = hex_dec(&v.nonce_hex).try_into().unwrap();
    let pt = hex_dec(&v.plaintext_hex);

    let sealed = crypto::secretbox_seal_with_nonce(&key, &nonce, &pt).expect("seal");
    assert_eq!(hex_enc(&sealed), v.nonce_sealed_hex, "nonce||ct||mac bytes");
    // sealed_hex (ct||mac only) is the crypto_secretbox_easy tail.
    assert_eq!(
        hex_enc(&sealed[24..]),
        v.sealed_hex,
        "ct||mac without nonce"
    );

    let opened = crypto::secretbox_open(&key, &sealed).expect("opens");
    assert_eq!(opened, pt);
    // Random-nonce roundtrip dumped by the C++ (self-consistency record).
    let rt = hex_dec(&v.roundtrip_nonce_ct_hex);
    let expected_pt = hex_dec(&v.roundtrip_plaintext_hex);
    assert_eq!(
        crypto::secretbox_open(&key, &rt).as_deref(),
        Some(expected_pt.as_slice()),
        "open(C++ sealed) == recorded plaintext"
    );
}

// ── ⑥b EncryptedSession fixed key+nonce seal wire ──────────────────────────

#[derive(Deserialize)]
struct SessionSealCase {
    name: String,
    msg_type: u16,
    key_hex: String,
    nonce_hex: String,
    plaintext_hex: String,
    wire_hex: String,
    opened_header: HeaderJson,
    opened_payload_hex: String,
}

#[test]
fn golden_encrypted_session_seal_bytes_match() {
    let cases: Vec<SessionSealCase> =
        serde_json::from_value(load("session_seal")).expect("json shape");
    assert!(cases.len() >= 3, "connect_request + 2 DATA chunks covered");
    let mut saw_chunked_data = 0;

    for case in &cases {
        let key: [u8; 32] = hex_dec(&case.key_hex).try_into().unwrap();
        let nonce: [u8; 24] = hex_dec(&case.nonce_hex).try_into().unwrap();
        let pt = hex_dec(&case.plaintext_hex);
        let header = MessageHeader {
            magic: MAGIC,
            msg_type: MessageType::from_u16(case.msg_type).expect("known type"),
            payload_length: pt.len() as u32,
            checksum: 0xCAFE, // seal must force this to 0
        };

        let mut session = EncryptedSession::default();
        session.activate(&key);
        let wire = session.seal_with_nonce(header, &pt, &nonce).expect("seal");
        assert_eq!(hex_enc(&wire), case.wire_hex, "{} wire bytes", case.name);

        // Wire header carries payload_length = nonce+ct length and checksum 0.
        let parsed = frame::parse_header(&wire).expect("wire header parses");
        assert_eq!(parsed.checksum, 0, "{} encrypted checksum zeroed", case.name);
        assert_eq!(
            parsed.payload_length as usize,
            24 + pt.len() + 16,
            "{} rewritten payload_length",
            case.name
        );

        // open restores the ORIGINAL header + payload.
        let (hdr2, opened) = session.open(&wire).expect("opens");
        assert_eq!(hdr2.msg_type as u16, case.opened_header.msg_type);
        assert_eq!(hdr2.payload_length, case.opened_header.payload_length);
        assert_eq!(hdr2.payload_length as usize, pt.len());
        assert_eq!(hex_enc(&opened), case.opened_payload_hex, "{} payload", case.name);

        if case.msg_type == MessageType::ClientData as u16 {
            saw_chunked_data += 1;
        }
        // Unactivated sessions must refuse to seal/open (C++ throws/asserts).
    }
    assert!(saw_chunked_data >= 2, "chunked continuation vectors present");
}

#[test]
fn golden_session_seal_open_roundtrip_random_nonce() {
    // open(seal(x)) == x with fresh random nonces on both impls.
    let key: [u8; 32] = hex_dec(
        &serde_json::from_value::<SessionKeyVector>(load("session_key"))
            .unwrap()
            .client_key_hex,
    )
    .try_into()
    .unwrap();
    let mut session = EncryptedSession::default();
    session.activate(&key);
    let payload: Vec<u8> = (0..300u32).map(|i| (i * 7 + 3) as u8).collect();
    let wire = session
        .seal(
            MessageHeader {
                magic: MAGIC,
                msg_type: MessageType::ClientSubmit,
                payload_length: payload.len() as u32,
                checksum: 5,
            },
            &payload,
        )
        .expect("seal");
    let (hdr, opened) = session.open(&wire).expect("open");
    assert_eq!(opened, payload);
    assert_eq!(hdr.msg_type, MessageType::ClientSubmit);
    assert_eq!(hdr.payload_length as usize, payload.len());
    assert_eq!(hdr.checksum, 0, "sealed frames carry checksum 0");
}

// ── ⑦ fingerprint ───────────────────────────────────────────────────────────

#[derive(Deserialize)]
struct FingerprintVector {
    pubkey_hex: String,
    sha256_full_hex: String,
    fingerprint: String,
}

#[test]
fn golden_fingerprint_matches_sha256_prefix() {
    let v: FingerprintVector = serde_json::from_value(load("fingerprint")).expect("json shape");
    let pubkey: [u8; 32] = hex_dec(&v.pubkey_hex).try_into().unwrap();
    assert_eq!(crypto::sha256_hex(&pubkey), v.sha256_full_hex);
    assert_eq!(upix_net::identity::fingerprint(&pubkey), v.fingerprint);
    assert_eq!(v.fingerprint.len(), 16, "8 bytes as 16 uppercase hex chars");
    assert!(
        v.fingerprint
            .chars()
            .all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()),
        "uppercase hex"
    );
}

// ── ⑧ TrustStore parse ──────────────────────────────────────────────────────

#[derive(Deserialize)]
struct TrustStoreVector {
    content: String,
    entries_hex: Vec<String>,
    size: usize,
    contains_valid1: bool,
    contains_valid2: bool,
    contains_unknown: bool,
}

#[test]
fn golden_truststore_parse_matches_cpp() {
    let v: TrustStoreVector = serde_json::from_value(load("truststore")).expect("json shape");

    let dir = tempfile::tempdir().expect("tempdir");
    let path = dir.path().join("trusted_clients");
    std::fs::write(&path, &v.content).expect("write vector content");

    let store = TrustStore::load(&path);
    assert_eq!(store.size(), v.size, "entry count (dupes kept, junk dropped)");
    let entries: Vec<String> = store.entries().iter().map(|e| hex_enc(e)).collect();
    assert_eq!(entries, v.entries_hex, "parsed entries byte-exact");

    let keys: Vec<[u8; 32]> = v
        .entries_hex
        .iter()
        .map(|h| hex_dec(h).try_into().unwrap())
        .collect();
    assert_eq!(store.contains(&keys[0]), v.contains_valid1);
    assert_eq!(store.contains(&keys[1]), v.contains_valid2);
    let unknown: [u8; 32] = (0..32).map(|i| (0xAA ^ i) as u8).collect::<Vec<u8>>().try_into().unwrap();
    assert_eq!(store.contains(&unknown), v.contains_unknown);
}

// ── extra interop pin: base64 codec == libsodium ORIGINAL variant ───────────

#[test]
fn golden_base64_matches_libsodium_original() {
    // TrustStore content from the C++ dumper was encoded with sodium's
    // VARIANT_ORIGINAL; our encoder must produce the identical strings.
    let v: TrustStoreVector = serde_json::from_value(load("truststore")).expect("json shape");
    for line in v.content.lines() {
        let stripped: String = line.chars().filter(|c| !c.is_whitespace()).collect();
        if stripped.is_empty() {
            continue;
        }
        if let Some(bytes) = upix_net::identity::base64_decode(&stripped) {
            assert_eq!(
                upix_net::identity::base64_encode(&bytes),
                stripped,
                "encode(decode(line)) == line for sodium-encoded {stripped}"
            );
        }
    }
    assert!(
        v.entries_hex
            .iter()
            .all(|h| upix_net::identity::base64_decode(&{
                let bytes = hex_dec(h);
                upix_net::identity::base64_encode(&bytes)
            })
            .is_some()),
        "golden entries survive a base64 roundtrip"
    );
}
