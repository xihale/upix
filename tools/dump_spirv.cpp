// Host-only: compile frozen-profile SPIR-V via ncnn::compile_spirv_module
// (glslang-linked ncnn with NCNN_SKIP_DEVICE_DEFINES) and write the pack
// consumed at runtime. Not a user-facing CLI command.
//
//   xmake build dump-spirv
//   xmake run dump-spirv -- share/core/spirv_pack.bin

#include "core/gpu_precision.hpp"
#include "core/vulkan_shaders.hpp"

#include <gpu.h>
#include <option.h>

#include <libdeflate.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr char kMagic[4] = {'C', 'S', 'p', 'v'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kKindLayer = 0;
constexpr uint32_t kKindCustom = 1;
constexpr int kMaxShaderIndex = 240;

struct Entry {
    uint32_t kind;
    uint32_t id;
    uint32_t opt_key;
    uint32_t offset;
    uint32_t size;
};

struct BlobKey {
    uint64_t h0;
    uint64_t h1;
    bool operator==(const BlobKey& o) const { return h0 == o.h0 && h1 == o.h1; }
};

struct BlobKeyHash {
    size_t operator()(const BlobKey& k) const {
        return std::hash<uint64_t>{}(k.h0 ^ (k.h1 * 0x9e3779b97f4a7c15ULL));
    }
};

BlobKey hash_spv(const std::vector<uint32_t>& spv) {
    uint64_t h0 = 0xcbf29ce484222325ULL;
    uint64_t h1 = 0x100000001b3ULL;
    for (uint32_t w : spv) {
        h0 ^= w;
        h0 *= 0x100000001b3ULL;
        h1 ^= w * 0x9e3779b1u;
        h1 *= 0x100000001b3ULL;
    }
    h0 ^= static_cast<uint64_t>(spv.size());
    return BlobKey{h0, h1};
}

struct NamedOpt {
    const char* name;
    ncnn::Option opt;
};

std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& raw) {
    libdeflate_compressor* c = libdeflate_alloc_compressor(9);
    if (c == nullptr) {
        return {};
    }
    std::vector<uint8_t> out(libdeflate_zlib_compress_bound(c, raw.size()));
    const size_t n = libdeflate_zlib_compress(c, raw.data(), raw.size(), out.data(), out.size());
    libdeflate_free_compressor(c);
    out.resize(n);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    // `xmake run dump-spirv -- <path>` passes the separator itself as argv[1];
    // skip it so the documented invocation does not write a file named "--".
    std::string out_path = "share/core/spirv_pack.bin";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            continue;
        }
        out_path = argv[i];
        break;
    }

    if (ncnn::create_gpu_instance() != 0) {
        std::fprintf(stderr, "create_gpu_instance failed (glslang init lives here)\n");
        return 1;
    }

    std::vector<NamedOpt> opts;
    {
        ncnn::Option igpu;
        igpu.use_vulkan_compute = true;
        upix::apply_profile(igpu, upix::GpuPrecisionProfile::Integrated);
        opts.push_back({"integrated", igpu});

        ncnn::Option dgpu;
        dgpu.use_vulkan_compute = true;
        upix::apply_profile(dgpu, upix::GpuPrecisionProfile::Discrete);
        opts.push_back({"discrete", dgpu});

        opts.push_back({"pack_fp32", upix::packing_option(false, false)});
        opts.push_back({"pack_fp16", upix::packing_option(true, false)});
        opts.push_back({"pack_int8", upix::packing_option(false, true)});
        opts.push_back({"pack_fp16_int8", upix::packing_option(true, true)});
    }

    std::vector<Entry> entries;
    std::vector<uint8_t> blob;
    std::unordered_map<BlobKey, uint32_t, BlobKeyHash> dedup;
    int compiled = 0;
    int failed = 0;
    int reused = 0;

    auto add_spv = [&](uint32_t kind, uint32_t id, uint32_t key, std::vector<uint32_t>& spv) {
        const uint32_t bytes = static_cast<uint32_t>(spv.size() * 4);
        const BlobKey hk = hash_spv(spv);
        uint32_t offset = 0;
        if (auto it = dedup.find(hk); it != dedup.end()) {
            offset = it->second;
            ++reused;
        } else {
            offset = static_cast<uint32_t>(blob.size());
            blob.resize(blob.size() + bytes);
            std::memcpy(blob.data() + offset, spv.data(), bytes);
            dedup.emplace(hk, offset);
        }
        entries.push_back(Entry{kind, id, key, offset, bytes});
        ++compiled;
    };

    for (const auto& named : opts) {
        const uint32_t key = upix::option_key(named.opt);
        std::fprintf(stderr, "profile %s key=0x%x\n", named.name, key);

        std::vector<int> holes;
        for (int i = 0; i < kMaxShaderIndex; ++i) {
            std::vector<uint32_t> spv;
            if (ncnn::compile_spirv_module(i, named.opt, spv) != 0 || spv.empty()) {
                holes.push_back(i);
                continue;
            }
            add_spv(kKindLayer, static_cast<uint32_t>(i), key, spv);
        }
        // Some holes are expected (alternate specializations that are never
        // requested under a profile), but they must be visible: a profile
        // change that silently widens the hole set is exactly how a broken
        // pack gets frozen. Diff this line when the pack changes.
        if (!holes.empty()) {
            std::fprintf(stderr, "  %s holes (%zu):", named.name, holes.size());
            for (int h : holes) {
                std::fprintf(stderr, " %d", h);
            }
            std::fprintf(stderr, "\n");
            failed += static_cast<int>(holes.size());
        }

        for (const auto& shader : upix::detail::kCustomShaders) {
            std::vector<uint32_t> spv;
            if (ncnn::compile_spirv_module(shader.source, named.opt, spv) != 0 || spv.empty()) {
                std::fprintf(stderr, "  custom %s failed\n", shader.name);
                ++failed;
                continue;
            }
            add_spv(kKindCustom, static_cast<uint32_t>(shader.id), key, spv);
        }
    }

    ncnn::destroy_gpu_instance();

    const uint32_t blob_off =
        static_cast<uint32_t>(16 + entries.size() * sizeof(Entry));
    std::vector<uint8_t> file(blob_off + blob.size());
    std::memcpy(file.data(), kMagic, 4);
    uint32_t ver = kVersion;
    uint32_t count = static_cast<uint32_t>(entries.size());
    std::memcpy(file.data() + 4, &ver, 4);
    std::memcpy(file.data() + 8, &count, 4);
    std::memcpy(file.data() + 12, &blob_off, 4);
    if (!entries.empty()) {
        std::memcpy(file.data() + 16, entries.data(), entries.size() * sizeof(Entry));
    }
    if (!blob.empty()) {
        std::memcpy(file.data() + blob_off, blob.data(), blob.size());
    }

    const auto compressed = zlib_compress(file);
    if (compressed.empty()) {
        std::fprintf(stderr, "zlib compress failed\n");
        return 1;
    }
    std::vector<uint8_t> wrapped(16 + compressed.size());
    const char zmagic[4] = {'C', 'S', 'p', 'z'};
    const uint32_t raw_size = static_cast<uint32_t>(file.size());
    const uint32_t comp_size = static_cast<uint32_t>(compressed.size());
    std::memcpy(wrapped.data(), zmagic, 4);
    std::memcpy(wrapped.data() + 4, &raw_size, 4);
    std::memcpy(wrapped.data() + 8, &comp_size, 4);
    std::memset(wrapped.data() + 12, 0, 4);
    std::memcpy(wrapped.data() + 16, compressed.data(), compressed.size());

    {
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "failed to write %s\n", out_path.c_str());
            return 1;
        }
        out.write(reinterpret_cast<const char*>(wrapped.data()),
                  static_cast<std::streamsize>(wrapped.size()));
    }

    std::fprintf(stderr,
                 "wrote %s (%u raw -> %zu wrapped, %zu entries, %d compiled, %d reused, %d failed)\n",
                 out_path.c_str(), raw_size, wrapped.size(), entries.size(), compiled, reused,
                 failed);
    return compiled > 0 ? 0 : 1;
}
