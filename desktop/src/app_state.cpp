#include "app_state.hpp"
#include "utils/platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace upix::gui {

namespace fs = std::filesystem;

namespace {

bool isSupportedImage(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".avif";
}

std::string formatBytes(uint64_t bytes) {
    char buf[64];
    double mb = static_cast<double>(bytes) / 1048576.0;
    if (mb >= 10.0) std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    else std::snprintf(buf, sizeof(buf), "%.2f MB", mb);
    return buf;
}

std::vector<ModelFamily> buildGroups(const std::vector<ModelItem>& list, bool remote,
                                     std::unordered_set<std::string>& seen) {
    std::vector<std::string> names;
    std::unordered_map<std::string, ModelItem> byName;
    for (const auto& m : list) {
        if (seen.count(m.name)) continue;
        seen.insert(m.name);
        names.push_back(m.name);
        byName[m.name] = m;
    }
    std::vector<ModelFamily> out;
    std::vector<ModelItem> orphans;
    for (const auto& g : upix::groups(names)) {
        std::vector<ModelItem> items;
        for (const auto& it : g.items) {
            auto found = byName.find(it.name);
            if (found == byName.end()) continue;
            ModelItem mi = found->second;
            mi.short_name = it.short_name;
            mi.remote = remote;
            items.push_back(std::move(mi));
        }
        if (items.empty()) continue;
        if (g.label.empty()) {
            orphans.insert(orphans.end(), items.begin(), items.end());
        } else {
            out.push_back(ModelFamily{g.label, std::move(items)});
        }
    }
    if (!orphans.empty()) {
        out.push_back(ModelFamily{"独立模型", std::move(orphans)});
    }
    return out;
}

} // namespace

struct AppState::HostWorker {
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<int> task_pct{-1};
    std::mutex pending_mtx;
    std::optional<upix::PendingApproval> pending;
    upix::Coordinator coordinator;

    ~HostWorker() {
        stopping.store(true);
        coordinator.stop();
        if (thread.joinable()) thread.join();
    }
};

AppState::AppState() {
#ifdef UPIX_VERSION
    snap_.version = UPIX_VERSION;
#endif
    snap_.models_dir = upix::default_models_dir();
    loadPrefs();

    const char* fake = std::getenv("UPIX_FAKE");
    if (fake && fake[0] != '\0') {
        applyFixture("empty");
        return;
    }

    refreshModels();
    refreshDaemonStatus();

    // Populate initial host fingerprint & local IPs even while idle
    try {
        auto id = upix::IdentityKey::load_or_create(upix::default_identity_path());
        auto card = upix::build_connection_card(static_cast<uint16_t>(snap_.host.port), id.public_key());
        snap_.host.fingerprint = card.fingerprint;
        snap_.host.ips = card.addresses;
    } catch (...) {}
}

AppState::~AppState() {
    job_cancel_.store(true);
    if (job_thread_.joinable()) job_thread_.join();
    delete host_worker_;
}

Snapshot AppState::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_;
}

void AppState::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(fn));
    dirty_.store(true);
}

bool AppState::pollEvents() {
    std::vector<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local.swap(queue_);
    }
    for (auto& fn : local) {
        fn();
    }
    return dirty_.exchange(false);
}

void AppState::rebuildModelGroups() {
    std::unordered_set<std::string> seen;
    snap_.installed_groups = buildGroups(snap_.installed, false, seen);
    snap_.remote_groups = buildGroups(snap_.remote, true, seen);
    if (snap_.model.empty() && !snap_.installed.empty()) {
        snap_.model = snap_.installed.front().name;
    }
}

void AppState::applyFixture(const std::string& state) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_ = Snapshot{};
    snap_.models_dir = "/home/xihale/.local/share/upix/models";
    snap_.model = "AnimeSharp-x4";
    snap_.installed = {
        {"Real-ESRGANv3-anime-x2", "anime-x2", 2, "1.8 MB", "动漫", false, false},
        {"Real-ESRGANv3-anime-x4", "anime-x4", 4, "31.4 MB", "动漫", true, false},
        {"Real-ESRGANv3-general-x4", "general-x4", 4, "32.0 MB", "综合", false, false},
        {"AnimeSharp-x4", "AnimeSharp-x4", 4, "31.2 MB", "动漫", true, false},
        {"Real-CUGAN-denoise-x2", "Real-CUGAN-denoise-x2", 2, "2.4 MB", "动漫", false, false},
        {"Remacri-x4", "Remacri-x4", 4, "31.1 MB", "照片", false, false},
    };
    snap_.remote = {
        {"Real-CUGAN-pro-conservative-x2", "conservative-x2", 2, "2.4 MB", "动漫", false, true},
        {"Real-CUGAN-pro-denoise-x3", "denoise-x3", 3, "4.1 MB", "动漫", false, true},
        {"BSRGAN-x4", "BSRGAN-x4", 4, "32.0 MB", "修复", false, true},
        {"DeJPG-RealPLKSR-x1", "DeJPG-RealPLKSR-x1", 1, "13.0 MB", "修复", false, true},
        {"UltraSharp-x4", "UltraSharp-x4", 4, "31.2 MB", "照片", true, true},
    };
    rebuildModelGroups();
    snap_.daemon = {true, 4321, "热引擎已就绪"};
    snap_.host.fingerprint = "EC61F2B66D4C25C4";
    snap_.host.ips = {"192.168.0.126", "10.210.210.2"};
    snap_.host.port = 9527;
    snap_.host.trusted_devices = {
        {"Pixel 9 Pro", "EC61 · F2B6 · 6D4C · 25C4", true},
        {"Xiaomi 14 Ultra", "89A4 · C012 · 77D1 · 11F0", false},
    };

    if (state == "running") {
        snap_.format = "avif";
        snap_.quality = 88;
        snap_.custom_out_dir = "~/Pictures/Exports";
        snap_.inputs = {
            {"shot_01.png", "shot_01.png", false, 1, 640, 427, "4× · 0.8s", 2},
            {"shot_02.png", "shot_02.png", false, 1, 640, 427, "4× · 0.9s", 2},
            {"shot_03.png", "shot_03.png", false, 1, 640, 427, "75% · 18/24", 1},
            {"shot_04.png", "shot_04.png", false, 1, 640, 427, "640×427", 0},
            {"shot_05.png", "shot_05.png", false, 1, 640, 427, "640×427", 0},
        };
        snap_.job.phase = JobPhase::Running;
        snap_.job.index = 3;
        snap_.job.total = 5;
        snap_.job.percent = 75;
        snap_.job.tiles_done = 18;
        snap_.job.tiles_total = 24;
        snap_.job.eta_sec = 0;
        snap_.job.current_file = "shot_03.png";
    } else if (state == "done") {
        snap_.inputs = {
            {"scene.png", "scene.png", false, 1, 640, 427, "4× · 0.84s", 2},
        };
        snap_.job.phase = JobPhase::Done;
        snap_.job.percent = 100;
        snap_.job.elapsed_sec = 0.84;
        snap_.job.in_w = 640;
        snap_.job.in_h = 427;
        snap_.job.out_w = 2560;
        snap_.job.out_h = 1708;
        snap_.job.out_size_str = "14.2 MB";
        snap_.job.outputs = {"scene_out.png"};
    } else if (state == "error") {
        snap_.inputs = {
            {"shot_01.png", "shot_01.png", false, 1, 640, 427, "4× · 0.8s", 2},
            {"shot_02.png", "shot_02.png", false, 1, 640, 427, "4× · 0.9s", 2},
            {"shot_03.png", "shot_03.png", false, 1, 640, 427, "显存不足", 3},
            {"shot_04.png", "shot_04.png", false, 1, 640, 427, "640×427", 0},
            {"shot_05.png", "shot_05.png", false, 1, 640, 427, "640×427", 0},
        };
        snap_.job.phase = JobPhase::Error;
        snap_.job.index = 3;
        snap_.job.total = 5;
        snap_.job.message = "显存分配不足";
        snap_.job.diag_detail = "VK_ERROR_OUT_OF_DEVICE_MEMORY · tile 512px";
    } else if (state == "models") {
        snap_.installed = {
            {"Real-ESRGAN-anime-x2-v3", "anime-x2", 2, "1.8 MB", "动漫", false, false},
            {"Real-ESRGAN-anime-x4-v3", "anime-x4", 4, "31.4 MB", "动漫", true, false},
            {"Real-ESRGAN-general-x4-v3", "general-x4", 4, "32.0 MB", "综合", false, false},
            {"AnimeSharp-x4", "AnimeSharp-x4", 4, "31.2 MB", "动漫", true, false},
            {"Real-CUGAN-denoise-x2", "Real-CUGAN-denoise-x2", 2, "2.4 MB", "动漫", false, false},
            {"Remacri-x4", "Remacri-x4", 4, "31.1 MB", "照片", false, false},
        };
        snap_.installed_groups = {
            {"Real-ESRGAN v3", {snap_.installed[0], snap_.installed[1], snap_.installed[2]}},
            {"独立模型", {snap_.installed[3], snap_.installed[4], snap_.installed[5]}},
        };
        snap_.remote = {
            {"Real-CUGAN-pro-conservative-x2", "conservative-x2", 2, "2.4 MB", "动漫", false, true},
            {"Real-CUGAN-pro-denoise-x3", "denoise-x3", 3, "4.1 MB", "动漫", false, true},
            {"BSRGAN-x4", "BSRGAN-x4", 4, "32.0 MB", "修复", false, true},
            {"DeJPG-RealPLKSR-x1", "DeJPG-RealPLKSR-x1", 1, "13.0 MB", "修复", false, true},
            {"UltraSharp-x4", "UltraSharp-x4", 4, "31.2 MB", "照片", true, true},
        };
        snap_.remote_groups = {
            {"Real-CUGAN pro", {snap_.remote[0], snap_.remote[1]}},
            {"独立模型", {snap_.remote[2], snap_.remote[3], snap_.remote[4]}},
        };
        snap_.downloading = "BSRGAN-x4";
        snap_.download_pct = 60;
    } else if (state == "picker") {
        snap_.installed = {
            {"AnimeSharp-x4", "AnimeSharp-x4", 4, "8 MB", "动漫", true, false},
            {"Real-ESRGAN-x4plus", "Real-ESRGAN-x4plus", 4, "64 MB", "综合", false, false},
            {"4x-UltraSharp", "4x-UltraSharp", 4, "67 MB", "照片", false, false},
            {"Real-CUGAN", "Real-CUGAN", 2, "2 MB", "动漫", false, false},
            {"waifu2x", "waifu2x", 2, "1 MB", "动漫", false, false},
        };
        snap_.installed_groups = {
            {"独立模型", snap_.installed},
        };
    } else if (state == "host" || state == "pairing") {
        snap_.host.running = true;
        snap_.host.qr_payload = "{\"v\":2,\"ip\":\"192.168.0.126\",\"port\":9527,\"fp\":\"EC61F2B66D4C25C4\",\"pk\":\"dGVzdCBwdWJsaWMga2V5IDMyIGJ5dGVzISE=\"}";
        snap_.host.task_percent = 78;
        snap_.host.active_device = "Pixel 9 Pro";
        snap_.host.logs = {
            "12:28:04|HOST|192.168.0.126:9527 · 监听已就绪|",
            "12:28:11|CODEC|协商能力: Vulkan FP16 · AVIF 双向编解码已启用|信任库: 2 台设备",
            "12:28:19|TASK|Pixel 9 Pro 协同推理 · AnimeSharp-x4 (4×)|12 / 16 块",
        };
        if (state == "pairing") {
            snap_.host.pending_device = "Pixel 9 Pro";
            snap_.host.pending_fp = "EC61F2B66D4C25C4";
        }
    } else if (state == "host-idle") {
        snap_.host.running = false;
        snap_.host.trusted_devices.clear();
        snap_.host.logs.clear();
    }
    dirty_.store(true);
}

void AppState::addPaths(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lk(mtx_);
    int skipped = 0;
    for (const auto& raw : paths) {
        fs::path p(raw);
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            ++skipped;
            continue;
        }
        bool is_dir = fs::is_directory(p, ec);
        if (!is_dir && !isSupportedImage(p)) {
            ++skipped;
            continue;
        }
        std::string clean = fs::weakly_canonical(p, ec).string();
        bool dup = false;
        for (const auto& it : snap_.inputs) {
            if (it.path == clean) { dup = true; break; }
        }
        if (dup) continue;

        InputItem item;
        item.path = clean;
        item.name = p.filename().string();
        item.is_dir = is_dir;
        if (is_dir) {
            int cnt = 0;
            for (auto& de : fs::recursive_directory_iterator(p, ec)) {
                if (de.is_regular_file() && isSupportedImage(de.path())) ++cnt;
            }
            if (cnt == 0) {
                snap_.notice = "文件夹里没有受支持的图片";
                snap_.notice_seq++;
                continue;
            }
            item.count = cnt;
            item.status_text = std::to_string(cnt) + " 张图片";
        } else {
            item.status_text = "待处理";
        }
        snap_.inputs.push_back(std::move(item));
    }
    if (snap_.job.phase != JobPhase::Running) {
        snap_.job = JobState{};
    }
    if (skipped > 0) {
        snap_.notice = "已忽略 " + std::to_string(skipped) + " 个不支持的文件";
        snap_.notice_seq++;
    }
    dirty_.store(true);
}

void AppState::clearInputs() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.job.phase == JobPhase::Running) return;
    snap_.inputs.clear();
    snap_.job = JobState{};
    dirty_.store(true);
}

void AppState::removeInput(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.inputs.erase(
        std::remove_if(snap_.inputs.begin(), snap_.inputs.end(),
                       [&](const InputItem& i) { return i.path == path; }),
        snap_.inputs.end());
    dirty_.store(true);
}

void AppState::setModel(const std::string& m) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.model = m;
    savePrefs();
    dirty_.store(true);
}

void AppState::setFormat(const std::string& f) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.format = f;
    savePrefs();
    dirty_.store(true);
}

void AppState::setQuality(int q) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.quality = std::clamp(q, 1, 100);
    savePrefs();
    dirty_.store(true);
}

void AppState::setChroma(int c) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.chroma = c;
    dirty_.store(true);
}

void AppState::setAvifSpeed(int s) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.avif_speed = s;
    dirty_.store(true);
}

void AppState::setTileSize(int ts) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.tile_size = ts;
    dirty_.store(true);
}

void AppState::setGpu(bool g) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.gpu = g;
    dirty_.store(true);
}

void AppState::setForce(bool f) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.force = f;
    savePrefs();
    dirty_.store(true);
}

void AppState::setCustomOutDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.custom_out_dir = dir;
    dirty_.store(true);
}

void AppState::setNotice(const std::string& msg) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.notice = msg;
    snap_.notice_seq++;
    dirty_.store(true);
}

void AppState::startJob() {
    Snapshot runSnap;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (snap_.job.phase == JobPhase::Running) return;
        if (snap_.inputs.empty() || snap_.model.empty()) return;
        snap_.job = JobState{};
        snap_.job.phase = JobPhase::Running;
        snap_.job.total = static_cast<int>(snap_.inputs.size());
        snap_.job.index = 1;
        snap_.job.current_file = snap_.inputs.front().name;
        for (auto& inp : snap_.inputs) {
            inp.status_kind = 0;
            inp.status_text = "待处理";
        }
        snap_.inputs.front().status_kind = 1;
        snap_.inputs.front().status_text = "推理中";
        runSnap = snap_;
        dirty_.store(true);
    }

    job_cancel_.store(false);
    if (job_thread_.joinable()) job_thread_.join();

    job_thread_ = std::thread([this, runSnap]() {
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t idx = 0; idx < runSnap.inputs.size(); ++idx) {
            if (job_cancel_.load()) break;
            const auto& inp = runSnap.inputs[idx];

            post([this, idx, name = inp.name]() {
                snap_.job.index = static_cast<int>(idx + 1);
                snap_.job.current_file = name;
                snap_.job.percent = 0;
                if (idx < snap_.inputs.size()) {
                    snap_.inputs[idx].status_kind = 1;
                    snap_.inputs[idx].status_text = "推理中";
                }
            });

            upix::BatchRequest req;
            req.models_dir = upix::default_models_dir();
            req.model_name = runSnap.model;
            req.use_gpu = runSnap.gpu;
            req.force = runSnap.force;
            req.config.use_gpu = runSnap.gpu;
            req.config.auto_tile_size = (runSnap.tile_size <= 0);
            req.config.tile_size = (runSnap.tile_size > 0) ? runSnap.tile_size : 256;
            req.config.tile_overlap = upix::kDefaultTileOverlap;
            req.config.output_quality = runSnap.quality;
            if (!runSnap.format.empty()) {
                req.config.output_format = upix::normalize_output_format(runSnap.format);
            }
            req.config.encoder.jpeg_subsampling = runSnap.chroma;
            req.config.encoder.avif_subsampling = runSnap.chroma;
            req.config.encoder.avif_speed = runSnap.avif_speed;
            req.config.encoder.webp_lossless = runSnap.webp_lossless;
            req.config.encoder.png_level = runSnap.png_level;

            std::string err;
            if (inp.is_dir) {
                std::string outDir = runSnap.custom_out_dir.empty()
                    ? upix::derive_default_output(inp.path, req.model_name, true)
                    : runSnap.custom_out_dir;
                auto tasks = upix::gather_directory_tasks(inp.path, outDir, err);
                if (!tasks) {
                    post([this, idx, err]() {
                        snap_.job.phase = JobPhase::Error;
                        snap_.job.message = "目录读取失败";
                        snap_.job.diag_detail = err;
                        if (idx < snap_.inputs.size()) {
                            snap_.inputs[idx].status_kind = 3;
                            snap_.inputs[idx].status_text = "异常";
                        }
                    });
                    return;
                }
                req.tasks = std::move(*tasks);
            } else {
                std::string outPath = upix::derive_default_output(inp.path, req.model_name, false);
                if (!runSnap.custom_out_dir.empty()) {
                    fs::path fn = fs::path(outPath).filename();
                    outPath = (fs::path(runSnap.custom_out_dir) / fn).string();
                }
                req.tasks.emplace_back(inp.path, outPath);
            }

            const auto itemStart = std::chrono::steady_clock::now();
            upix::BatchCallbacks cb;
            cb.on_current = [this](const std::string& file) {
                post([this, file]() {
                    snap_.job.current_file = fs::path(file).filename().string();
                });
            };
            cb.on_progress = [this](const upix::ProcessingProgress& prog, int eta) {
                if (prog.total_tiles <= 0) return;
                int pct = std::min(100, static_cast<int>(prog.completed_tiles * 100 / prog.total_tiles));
                post([this, pct, done = static_cast<int>(prog.completed_tiles),
                      tot = static_cast<int>(prog.total_tiles), eta]() {
                    snap_.job.percent = pct;
                    snap_.job.tiles_done = done;
                    snap_.job.tiles_total = tot;
                    snap_.job.eta_sec = std::max(0, eta);
                });
            };
            cb.on_saved = [this](const std::string& out) {
                std::error_code ec;
                auto fsz = fs::file_size(out, ec);
                std::string szStr = ec ? "" : formatBytes(fsz);
                post([this, out, szStr]() {
                    snap_.job.outputs.push_back(out);
                    if (!szStr.empty()) snap_.job.out_size_str = szStr;
                });
            };

            auto stats = upix::run_batch(req, cb, job_cancel_);
            if (job_cancel_.load()) {
                post([this]() {
                    snap_.job.phase = JobPhase::Cancelled;
                    snap_.job.message = "已取消";
                });
                return;
            }
            if (!stats || (stats->succeeded + stats->skipped < stats->total)) {
                post([this, idx]() {
                    snap_.job.phase = JobPhase::Error;
                    snap_.job.message = "推理执行失败";
                    snap_.job.diag_detail = "请检查模型文件或计算设备状态";
                    if (idx < snap_.inputs.size()) {
                        snap_.inputs[idx].status_kind = 3;
                        snap_.inputs[idx].status_text = "失败";
                    }
                });
                return;
            }
            double itemSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - itemStart).count();
            post([this, idx, itemSec]() {
                if (idx < snap_.inputs.size()) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "完成 · %.1fs", itemSec);
                    snap_.inputs[idx].status_kind = 2;
                    snap_.inputs[idx].status_text = buf;
                }
            });
        }

        double totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        post([this, totalSec]() {
            snap_.job.phase = JobPhase::Done;
            snap_.job.percent = 100;
            snap_.job.elapsed_sec = totalSec;
            snap_.job.message = "完成";
        });
    });
}

void AppState::cancelJob() {
    job_cancel_.store(true);
}

void AppState::refreshModels() {
    std::thread([this]() {
        const std::string dir = upix::default_models_dir();
        std::vector<ModelItem> inst;
        {
            upix::ModelManager mgr(dir);
            auto models = mgr.scan_models();
            if (models) {
                for (const auto& m : *models) {
                    auto meta = upix::classify_model_scene(m.name);
                    inst.push_back(ModelItem{
                        m.name, m.name, m.scale_factor,
                        formatBytes(m.total_size),
                        std::string(meta.scene), meta.recommended, false
                    });
                }
            }
        }
        post([this, dir, inst = std::move(inst)]() mutable {
            snap_.models_dir = dir;
            snap_.installed = std::move(inst);
            rebuildModelGroups();
        });

        std::string err;
        std::vector<ModelItem> rem;
        if (auto r = upix::fetch_remote_models("", err)) {
            for (const auto& m : *r) {
                auto meta = upix::classify_model_scene(m.name);
                rem.push_back(ModelItem{
                    m.name, m.name, m.scale,
                    formatBytes(m.size_bytes),
                    std::string(meta.scene), meta.recommended, true
                });
            }
        }
        post([this, rem = std::move(rem), err]() mutable {
            snap_.remote_error = err;
            snap_.remote = std::move(rem);
            rebuildModelGroups();
        });
    }).detach();
}

void AppState::importModels(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    std::thread([this, paths]() {
        auto res = upix::import_model_files(paths, upix::default_models_dir());
        post([this, res]() {
            if (res.ok) {
                snap_.notice = "模型导入完成";
                snap_.notice_seq++;
                refreshModels();
            } else {
                snap_.notice = res.error.empty() ? "导入失败" : res.error;
                snap_.notice_seq++;
            }
        });
    }).detach();
}

void AppState::downloadModel(const std::string& name) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!snap_.downloading.empty()) return;
        snap_.downloading = name;
        snap_.download_pct = 0;
        dirty_.store(true);
    }
    std::thread([this, name]() {
        std::string err;
        auto remote = upix::fetch_remote_models("", err);
        const upix::RemoteModel* target = nullptr;
        if (remote) {
            for (const auto& m : *remote) {
                if (m.name == name) { target = &m; break; }
            }
        }
        if (!target) {
            post([this, name]() {
                snap_.downloading.clear();
                snap_.download_pct = -1;
                snap_.notice = "未在远端清单中找到: " + name;
                snap_.notice_seq++;
            });
            return;
        }
        auto res = upix::download_model(*target, upix::default_models_dir(),
            [this](const upix::DownloadProgress& p) {
                if (p.total <= 0) return;
                int pct = static_cast<int>(p.downloaded * 100 / p.total);
                post([this, pct]() { snap_.download_pct = pct; });
            });
        post([this, name, res]() {
            snap_.downloading.clear();
            snap_.download_pct = -1;
            if (res.ok) {
                snap_.notice = "已安装 " + name;
                snap_.notice_seq++;
                refreshModels();
            } else {
                snap_.notice = "下载失败: " + name;
                snap_.notice_seq++;
            }
        });
    }).detach();
}

void AppState::removeModel(const std::string& name) {
    std::string dir = upix::default_models_dir();
    std::error_code ec;
    fs::remove(fs::path(dir) / (name + ".param"), ec);
    fs::remove(fs::path(dir) / (name + ".bin"), ec);
    setNotice("已移除模型: " + name);
    refreshModels();
}

void AppState::refreshDaemonStatus() {
    std::string sock = upix::default_serve_socket_path();
    auto st = upix::serve_daemon_status(sock);
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.daemon.running = st.running;
    snap_.daemon.pid = static_cast<int>(st.pid);
    snap_.daemon.label = st.running ? "热引擎已就绪" : "热引擎未启动";
    dirty_.store(true);
}

void AppState::toggleDaemon() {
    bool running = snapshot().daemon.running;
    std::string sock = upix::default_serve_socket_path();
    if (running) {
        std::thread([this, sock]() {
            upix::serve_daemon_stop(sock);
            post([this]() { refreshDaemonStatus(); });
        }).detach();
    } else {
        setNotice("正在后台唤醒常驻热引擎…");
    }
}

void AppState::hostStart() {
    if (host_worker_) return;
    int port = snapshot().host.port;
    host_worker_ = new HostWorker();
    HostWorker* w = host_worker_;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.host.running = true;
        snap_.host.logs.push_back("[HOST] Starting Coordinator on :" + std::to_string(port));
        dirty_.store(true);
    }

    w->thread = std::thread([this, w, port]() {
        auto id = upix::IdentityKey::load_or_create(upix::default_identity_path());
        w->coordinator.set_models_directory(upix::default_models_dir());
        w->coordinator.set_trust_store_path(upix::default_trust_path());
        w->coordinator.on_task_progress() = [w](float prog) {
            w->task_pct.store(prog < 0 ? -1 : static_cast<int>(prog * 100));
        };
        auto started = w->coordinator.start(static_cast<uint16_t>(port), id);
        if (!started) {
            post([this, msg = started.error().message]() {
                snap_.host.running = false;
                snap_.notice = "服务启动失败: " + msg;
                snap_.notice_seq++;
            });
            return;
        }
        auto card = upix::build_connection_card(static_cast<uint16_t>(port), id.public_key());
        post([this, card]() {
            snap_.host.running = true;
            snap_.host.fingerprint = card.fingerprint;
            snap_.host.ips = card.addresses;
            snap_.host.qr_payload = card.qr_payload;
            snap_.host.logs.push_back("[HOST] Coordinator listening · QR ready");
        });

        int lastPct = -1;
        while (!w->stopping.load() && w->coordinator.is_running()) {
            if (auto pend = w->coordinator.pop_pending_approval()) {
                {
                    std::lock_guard<std::mutex> lk(w->pending_mtx);
                    w->pending = pend;
                }
                post([this, dev = pend->device_name, fp = pend->fingerprint]() {
                    snap_.host.pending_device = dev;
                    snap_.host.pending_fp = fp;
                    snap_.host.logs.push_back("[PAIR] Pairing request from " + dev);
                });
            }
            int pct = w->task_pct.load();
            if (pct != lastPct) {
                lastPct = pct;
                post([this, pct]() { snap_.host.task_percent = pct; });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        post([this]() {
            snap_.host.running = false;
            snap_.host.qr_payload.clear();
        });
    });
}

void AppState::hostStop() {
    if (!host_worker_) return;
    delete host_worker_;
    host_worker_ = nullptr;
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.host.running = false;
    snap_.host.qr_payload.clear();
    snap_.host.logs.push_back("[HOST] Service stopped");
    dirty_.store(true);
}

void AppState::setHostPort(int port) {
    if (port <= 0 || port > 65535) return;
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.host.port = port;
    savePrefs();
    dirty_.store(true);
}

void AppState::selectHostIp(int idx) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (idx < 0 || idx >= static_cast<int>(snap_.host.ips.size())) return;
    snap_.host.selected_ip_idx = idx;
    dirty_.store(true);
}

void AppState::hostDecide(bool allow) {
    if (host_worker_) {
        std::lock_guard<std::mutex> lk(host_worker_->pending_mtx);
        if (host_worker_->pending) {
            host_worker_->coordinator.resolve_approval(host_worker_->pending->id, allow);
            host_worker_->pending.reset();
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.host.pending_device.clear();
    snap_.host.pending_fp.clear();
    dirty_.store(true);
}

void AppState::loadPrefs() {}
void AppState::savePrefs() {}

} // namespace upix::gui
