#include "identity.hpp"
#include "utils/crypto.hpp"  // sha256_hex
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sodium.h>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

namespace upix {

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

// Create/truncate `file` with mode 0600 *in the open call*, so the contents
// never exist even briefly with the process umask's permissions — a plain
// ofstream creates the file 0644 first and only gets chmod'ed afterwards,
// leaving a window where another local user can read the Ed25519 secret key.
auto write_private_file(const std::filesystem::path& file, const void* data,
                        size_t size) -> bool {
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return false;
    }
    const char* ptr = static_cast<const char*>(data);
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, ptr + written, size - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        written += static_cast<size_t>(n);
    }
    ::close(fd);
    return true;
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

    // 新建。父目录(如 ~/.upix/)可能不存在,先创建,权限 0700。
    if (file.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        // create_directories 在目录已存在时返回 false 但不报错;只有 ec 才是真错误。
        if (ec) {
            throw std::runtime_error("Failed to create identity directory " +
                                     file.parent_path().string() + ": " + ec.message());
        }
    }

    auto kp = sodium::ed25519_keygen();
    if (!write_private_file(file, kp.private_key.data(), 64)) {
        throw std::runtime_error("Failed to create identity file: " +
                                 file.string());
    }
    ensure_0600(file);
    return IdentityKey(kp);
}

auto IdentityKey::sign(const uint8_t* msg, size_t len) const
    -> std::array<uint8_t, 64> {
    return sodium::ed25519_sign(kp_.private_key, msg, len);
}

IdentityKey::~IdentityKey() {
    sodium_memzero(&kp_, sizeof(kp_));
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
    if (file_.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
        // 目录已存在时 ec 清空;真错误忽略(best-effort,与历史行为一致)。
    }
    std::string body;
    for (const auto& pk : entries_) {
        body += base64_encode(pk.data(), ED_PUBKEY_LEN);
        body += '\n';
    }
    // Same 0600-at-creation rule as the identity key: never expose the file
    // (or a rewritten one) with the umask's permissions first.
    if (!write_private_file(file_, body.data(), body.size())) {
        return;  // best-effort, as before: callers only log
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
// Standard (padded, no line-wrap) base64 via libsodium. Byte-for-byte
// compatible with the former hand-rolled codec, so persisted TrustStore
// entries and exchanged peer-pubkey strings keep decoding identically.
auto base64_encode(const uint8_t* data, size_t len) -> std::string {
    const size_t enc_len = sodium_base64_encoded_len(len, sodium_base64_VARIANT_ORIGINAL);
    std::string out(enc_len, '\0');
    sodium_bin2base64(out.data(), enc_len, data, len, sodium_base64_VARIANT_ORIGINAL);
    if (!out.empty() && out.back() == '\0') out.pop_back();  // drop embedded NUL
    return out;
}

auto base64_decode(std::string_view s) -> std::optional<std::vector<uint8_t>> {
    std::vector<uint8_t> out(s.size() * 3 / 4 + 1);
    size_t out_len = 0;
    if (sodium_base642bin(out.data(), out.size(),
                          reinterpret_cast<const char*>(s.data()), s.size(),
                          nullptr, &out_len, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return std::nullopt;  // 非法字符
    }
    out.resize(out_len);
    return out;
}

} // namespace upix
