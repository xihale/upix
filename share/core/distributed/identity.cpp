#include "identity.hpp"
#include "utils/crypto.hpp"  // sha256_hex
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sodium.h>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace upix {

namespace {
constexpr size_t ED_PUBKEY_LEN = 32;
std::atomic<uint64_t> g_tmp_seq{0};

auto current_pid_u64() -> uint64_t {
#ifdef _WIN32
    return static_cast<uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

auto sibling_tmp_path(const std::filesystem::path& target) -> std::filesystem::path {
    const uint64_t seq = g_tmp_seq.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path p = target;
    p += "." + std::to_string(current_pid_u64()) + "." + std::to_string(seq) + ".tmp";
    return p;
}

auto normalize_fp(std::string_view fp) -> std::string {
    std::string out;
    out.reserve(fp.size());
    for (unsigned char c : fp) {
        if (std::isspace(c) || c == '-' || c == ':') continue;
        out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

void ensure_0600(const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    using std::error_code;
    error_code ec;
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    // 权限设置失败不致命(如 Windows),忽略。
}
} // namespace

auto write_private_file_atomic(const std::filesystem::path& file,
                               const void* data, size_t size,
                               std::error_code& ec) -> bool {
    ec.clear();
    if (file.empty()) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    const std::filesystem::path tmp = sibling_tmp_path(file);
#ifdef _WIN32
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(::GetLastError()),
                             std::system_category());
        return false;
    }
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(size - written, 1024u * 1024u));
        DWORD wrote = 0;
        if (!::WriteFile(h, ptr + written, chunk, &wrote, nullptr) || wrote == 0) {
            ec = std::error_code(static_cast<int>(::GetLastError()),
                                 std::system_category());
            ::CloseHandle(h);
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        written += static_cast<size_t>(wrote);
    }
    if (!::FlushFileBuffers(h)) {
        ec = std::error_code(static_cast<int>(::GetLastError()),
                             std::system_category());
        ::CloseHandle(h);
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    ::CloseHandle(h);
    ensure_0600(tmp);
    if (!::MoveFileExW(tmp.c_str(), file.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(::GetLastError()),
                             std::system_category());
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
#else
    // Create with 0600 *at open time* so secrets never exist with wider mode,
    // then fsync + rename atomically so partial writes never truncate target.
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    const char* ptr = static_cast<const char*>(data);
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, ptr + written, size - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ec = std::error_code(errno, std::generic_category());
            ::close(fd);
            std::error_code rm_ec;
            std::filesystem::remove(tmp, rm_ec);
            return false;
        }
        written += static_cast<size_t>(n);
    }
    if (::fsync(fd) < 0 && errno != EINVAL && errno != ENOTSUP) {
        ec = std::error_code(errno, std::generic_category());
        ::close(fd);
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }
    if (::close(fd) < 0) {
        ec = std::error_code(errno, std::generic_category());
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }
    ensure_0600(tmp);
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }
    ensure_0600(file);
    return true;
#endif
}

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
    std::error_code write_ec;
    if (!write_private_file_atomic(file, kp.private_key.data(), 64, write_ec)) {
        throw std::runtime_error("Failed to create identity file: " +
                                 file.string() +
                                 (write_ec ? " (" + write_ec.message() + ")" : ""));
    }
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

void TrustStore::add(const std::array<uint8_t, 32>& pubkey,
                     std::string label,
                     std::string added_at) {
    if (!contains(pubkey)) entries_.push_back(pubkey);
    if (!label.empty() || !added_at.empty()) {
        const std::string fp = fingerprint(pubkey);
        auto& slot = meta_by_fp_[fp];
        if (!label.empty()) slot.first = std::move(label);
        if (!added_at.empty()) slot.second = std::move(added_at);
    }
}

auto TrustStore::remove_by_fingerprint(std::string_view fp) -> bool {
    const std::string needle = normalize_fp(fp);
    if (needle.empty()) return false;
    const size_t before = entries_.size();
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const std::array<uint8_t, 32>& pk) {
                           return fingerprint(pk) == needle;
                       }),
        entries_.end());
    meta_by_fp_.erase(needle);
    return entries_.size() != before;
}

void TrustStore::clear() {
    entries_.clear();
    meta_by_fp_.clear();
}

auto TrustStore::trusted_devices() const -> std::vector<TrustedDeviceInfo> {
    std::vector<TrustedDeviceInfo> out;
    out.reserve(entries_.size());
    for (const auto& pk : entries_) {
        TrustedDeviceInfo info;
        info.fingerprint = fingerprint(pk);
        if (const auto it = meta_by_fp_.find(info.fingerprint);
            it != meta_by_fp_.end()) {
            info.label = it->second.first;
            info.added_at = it->second.second;
        }
        out.push_back(std::move(info));
    }
    return out;
}

auto TrustStore::save(std::error_code& ec) const -> bool {
    ec.clear();
    if (file_.empty()) return true;
    std::string body;
    for (const auto& pk : entries_) {
        body += base64_encode(pk.data(), ED_PUBKEY_LEN);
        body += '\n';
    }
    return write_private_file_atomic(file_, body.data(), body.size(), ec);
}

void TrustStore::save() const {
    std::error_code ec;
    (void)save(ec);
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
