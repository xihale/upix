#include "identity.hpp"
#include "utils/crypto.hpp"  // sha256_hex
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace crossscale {

namespace {
constexpr size_t ED_PUBKEY_LEN = 32;

void ensure_0600(const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    using std::error_code;
    error_code ec;
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    // 权限设置失败不致命(如 Windows),忽略。
}
} // namespace

auto IdentityKey::load_or_create(const std::filesystem::path& file)
    -> IdentityKey {
    if (std::filesystem::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Failed to open identity file: " +
                                     file.string());
        }
        sodium::Ed25519Keypair kp{};
        // libsodium Ed25519 secret key 格式: seed(32) || pk(32),共 64 字节。
        // (crypto_sign_ed25519_SECRETKEYBYTES == 64)
        in.read(reinterpret_cast<char*>(kp.private_key.data()), 64);
        if (in.gcount() != 64) {
            throw std::runtime_error("Identity file truncated: " + file.string());
        }
        // 公钥是 secret key 的后 32 字节
        std::memcpy(kp.public_key.data(), kp.private_key.data() + 32, 32);
        return IdentityKey(kp);
    }

    // 新建
    auto kp = sodium::ed25519_keygen();
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Failed to create identity file: " +
                                     file.string());
        }
        out.write(reinterpret_cast<const char*>(kp.private_key.data()), 64);
    }
    ensure_0600(file);
    return IdentityKey(kp);
}

auto IdentityKey::sign(const uint8_t* msg, size_t len) const
    -> std::array<uint8_t, 64> {
    return sodium::ed25519_sign(kp_.private_key, msg, len);
}

IdentityKey::~IdentityKey() {
    sodium::Ed25519Keypair zero{};
    kp_ = zero;  // best-effort zero
}

auto TrustStore::load(const std::filesystem::path& file) -> TrustStore {
    std::vector<std::array<uint8_t, 32>> entries;
    if (std::filesystem::exists(file)) {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            // 去空白
            line.erase(std::remove_if(line.begin(), line.end(),
                                       [](unsigned char c){ return std::isspace(c); }),
                       line.end());
            if (line.empty()) continue;
            auto bytes = base64_decode(line);
            if (bytes && bytes->size() == ED_PUBKEY_LEN) {
                std::array<uint8_t, 32> pk{};
                std::memcpy(pk.data(), bytes->data(), ED_PUBKEY_LEN);
                entries.push_back(pk);
            }
        }
    }
    return TrustStore(file, std::move(entries));
}

auto TrustStore::contains(const std::array<uint8_t, 32>& pubkey) const -> bool {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const std::array<uint8_t, 32>& e) {
                           return std::memcmp(e.data(), pubkey.data(), 32) == 0;
                       });
}

void TrustStore::add(const std::array<uint8_t, 32>& pubkey) {
    if (!contains(pubkey)) entries_.push_back(pubkey);
}

void TrustStore::save() const {
    if (file_.empty()) return;
    std::ofstream out(file_, std::ios::trunc);
    for (const auto& pk : entries_) {
        out << base64_encode(pk.data(), ED_PUBKEY_LEN) << "\n";
    }
    ensure_0600(file_);
}

auto fingerprint(const std::array<uint8_t, 32>& pubkey) -> std::string {
    auto full = crypto::sha256_hex(std::string(
        reinterpret_cast<const char*>(pubkey.data()), 32));
    // sha256_hex 返回 64 字符,取前 16 字符(8 字节)大写
    std::string fp = full.substr(0, 16);
    std::transform(fp.begin(), fp.end(), fp.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return fp;
}

// ── base64 ──────────────────────────────────────────────────────────────
namespace {
constexpr char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

auto base64_encode(const uint8_t* data, size_t len) -> std::string {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < len) n |= uint32_t(data[i+1]) << 8;
        if (i + 2 < len) n |= uint32_t(data[i+2]);
        out += B64[(n >> 18) & 63];
        out += B64[(n >> 12) & 63];
        out += (i + 1 < len) ? B64[(n >> 6) & 63] : '=';
        out += (i + 2 < len) ? B64[n & 63] : '=';
    }
    return out;
}

auto base64_decode(std::string_view s) -> std::optional<std::vector<uint8_t>> {
    static int8_t rev[256];
    static bool inited = false;
    if (!inited) {
        std::fill(std::begin(rev), std::end(rev), int8_t(-1));
        for (int i = 0; i < 64; ++i) rev[(unsigned char)B64[i]] = int8_t(i);
        inited = true;
    }
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=') break;
        int8_t v = rev[c];
        if (v < 0) return std::nullopt;  // 非法字符
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

} // namespace crossscale
