#include "spirv_pack.hpp"

#if NCNN_VULKAN

#include "utils/logger.hpp"
#include <libdeflate.h>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace crossscale::spirv_pack {
namespace {

constexpr char kMagic[4] = {'C', 'S', 'p', 'v'};
constexpr char kZMagic[4] = {'C', 'S', 'p', 'z'};
constexpr uint32_t kVersion = 1;

struct Header {
    char magic[4];
    uint32_t version;
    uint32_t entry_count;
    uint32_t blob_off;
};

struct Entry {
    uint32_t kind;
    uint32_t id;
    uint32_t opt_key;
    uint32_t offset;
    uint32_t size;
};

struct Key {
    uint32_t kind;
    uint32_t id;
    uint32_t opt_key;
    bool operator==(const Key& o) const {
        return kind == o.kind && id == o.id && opt_key == o.opt_key;
    }
};

struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t v = (uint64_t)k.kind << 48 | (uint64_t)k.id << 16 | k.opt_key;
        return std::hash<uint64_t>{}(v);
    }
};

extern "C" {
extern const unsigned char crossscale_spirv_pack_blob[];
extern const unsigned char crossscale_spirv_pack_blob_end[];
}

std::vector<unsigned char> g_raw;
std::unordered_map<Key, std::span<const uint32_t>, KeyHash> g_index;
bool g_loaded = false;
std::mutex g_lock;

bool inflate_if_needed(const unsigned char* data, size_t size) {
    if (size >= 16 && std::memcmp(data, kZMagic, 4) == 0) {
        uint32_t raw_size = 0;
        uint32_t comp_size = 0;
        std::memcpy(&raw_size, data + 4, 4);
        std::memcpy(&comp_size, data + 8, 4);
        if (comp_size + 16 != size || raw_size == 0) {
            return false;
        }
        g_raw.resize(raw_size);
        libdeflate_decompressor* d = libdeflate_alloc_decompressor();
        if (d == nullptr) {
            return false;
        }
        size_t actual = 0;
        const auto st = libdeflate_zlib_decompress(d, data + 16, comp_size, g_raw.data(),
                                                    g_raw.size(), &actual);
        libdeflate_free_decompressor(d);
        return st == LIBDEFLATE_SUCCESS && actual == raw_size;
    }
    g_raw.assign(data, data + size);
    return true;
}

bool parse_pack(const unsigned char* data, size_t size) {
    g_index.clear();
    if (size < sizeof(Header)) {
        return false;
    }
    Header hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (std::memcmp(hdr.magic, kMagic, 4) != 0 || hdr.version != kVersion) {
        return false;
    }
    const size_t table_bytes = static_cast<size_t>(hdr.entry_count) * sizeof(Entry);
    if (hdr.blob_off < sizeof(Header) + table_bytes || hdr.blob_off > size) {
        return false;
    }
    const auto* entries = reinterpret_cast<const Entry*>(data + sizeof(Header));
    const unsigned char* blob = data + hdr.blob_off;
    for (uint32_t i = 0; i < hdr.entry_count; ++i) {
        const Entry& e = entries[i];
        if (e.size % 4 != 0) {
            return false;
        }
        const size_t end = static_cast<size_t>(e.offset) + e.size;
        if (hdr.blob_off + end > size) {
            return false;
        }
        auto words = std::span<const uint32_t>(
            reinterpret_cast<const uint32_t*>(blob + e.offset), e.size / 4);
        g_index.insert_or_assign(Key{e.kind, e.id, e.opt_key}, words);
    }
    return true;
}

} // namespace

bool load_embedded() {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_loaded) {
        return !g_index.empty();
    }
    g_loaded = true;
    const auto* blob = crossscale_spirv_pack_blob;
    const size_t blob_size =
        static_cast<size_t>(crossscale_spirv_pack_blob_end - crossscale_spirv_pack_blob);
    if (!inflate_if_needed(blob, blob_size) ||
        !parse_pack(g_raw.data(), g_raw.size())) {
        LOG_WARNING("Engine", "SPIR-V pack is empty or corrupt");
        return false;
    }
    LOG_INFO("Engine", "Loaded SPIR-V pack with {} shaders", g_index.size());
    return true;
}

std::span<const uint32_t> find(uint32_t kind, uint32_t id, uint32_t opt_key) {
    load_embedded();
    auto it = g_index.find(Key{kind, id, opt_key});
    if (it == g_index.end()) {
        return {};
    }
    return it->second;
}

bool copy(uint32_t kind, uint32_t id, uint32_t opt_key, std::vector<uint32_t>& out) {
    auto spv = find(kind, id, opt_key);
    if (spv.empty()) {
        return false;
    }
    out.assign(spv.begin(), spv.end());
    return true;
}

namespace {

// Misses are engine bugs (dump/run key skew) and used to fail silently with
// only ncnn's downstream "binding_count not match" + SIGSEGV as symptoms —
// log the key so the culprit is identifiable in the field.
} // namespace

bool load_or_compile_custom(detail::CustomShader id, const char* src, const ncnn::Option& opt,
                            std::vector<uint32_t>& out) {
    const uint32_t key = option_key(opt);
    if (copy_custom(id, key, out)) {
        return true;
    }
    LOG_WARNING("Engine", "SPIR-V pack miss: custom={} key=0x{:x}",
                static_cast<uint32_t>(id), key);
    if (ncnn::compile_spirv_module(src, opt, out) != 0) {
        return false;
    }
    return true;
}

int lookup_layer(int shader_type_index, const ncnn::Option& opt, std::vector<uint32_t>& spirv) {
    const uint32_t key = option_key(opt);
    if (copy(kKindLayer, static_cast<uint32_t>(shader_type_index), key, spirv)) {
        return 0;
    }
    LOG_WARNING("Engine", "SPIR-V pack miss: layer={} key=0x{:x}",
                shader_type_index, key);
    return -1;
}

int lookup_source(const char* /*comp_data*/, int /*comp_data_size*/, const ncnn::Option& /*opt*/,
                  std::vector<uint32_t>& /*spirv*/) {
    // Custom shaders go through load_or_compile_custom, not this path.
    return -1;
}

void register_ncnn_lookup() {
    load_embedded();
    ncnn::set_compile_spirv_module_lookup(lookup_layer, lookup_source);
}

} // namespace crossscale::spirv_pack

#endif // NCNN_VULKAN
