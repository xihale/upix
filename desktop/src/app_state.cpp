#include "app_state.hpp"
#include "utils/platform.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace upix::gui {

namespace fs = std::filesystem;

namespace {

struct PrefsData {
    std::string model = "AnimeSharp-x4";
    std::string format = "";
    int quality = 90;
    int chroma = 420;
    int avif_speed = 6;
    bool webp_lossless = false;
    int png_level = 6;
    std::string custom_out_dir = "";
    bool overwrite_original = false;
    bool backup_original = true;
    bool recursive = false;
    bool gpu = true;
    int tile_size = 0;
    bool force = false;
    bool dark_mode = true;
    bool reduce_motion = false;
    std::string model_source = "";
    int host_port = 9527;
    int daemon_idle_secs = 300;
};

std::string formatHostLog(const char* tag, const std::string& msg) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char ts[16];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(ts) + "|" + tag + "|" + msg + "|";
}

bool isSupportedImage(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".avif";
}

bool shouldSkipUpixDir(const fs::path& p) {
    std::string name = p.filename().string();
    return name.rfind("upix_", 0) == 0;
}

std::vector<std::string> gatherSupportedImagesInDir(const fs::path& root, bool recursive) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(
             root, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::error_code stEc;
        if (it->is_directory(stEc)) {
            if (!recursive || shouldSkipUpixDir(it->path())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!stEc && it->is_regular_file(stEc) && !stEc && isSupportedImage(it->path())) {
            out.push_back(it->path().string());
        }
    }
    return out;
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

std::string uniqueOutputPath(const std::string& candidate, std::unordered_set<std::string>& reserved) {
    if (!reserved.count(candidate)) {
        reserved.insert(candidate);
        return candidate;
    }
    fs::path p(candidate);
    const fs::path dir = p.parent_path();
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();
    for (int n = 2; n < 10000; ++n) {
        std::string alt = (dir / (stem + "_" + std::to_string(n) + ext)).string();
        if (!reserved.count(alt)) {
            reserved.insert(alt);
            return alt;
        }
    }
    reserved.insert(candidate);
    return candidate;
}

std::vector<TrustedDeviceItem> trustedItemsFromInfos(
    const std::vector<upix::TrustedDeviceInfo>& infos, bool online) {
    std::vector<TrustedDeviceItem> out;
    out.reserve(infos.size());
    int idx = 1;
    for (const auto& info : infos) {
        std::string name = info.label.empty()
            ? ("已授权设备 #" + std::to_string(idx))
            : info.label;
        ++idx;
        out.push_back(TrustedDeviceItem{std::move(name), info.fingerprint, online});
    }
    return out;
}

std::vector<TrustedDeviceItem> loadTrustedDevicesFromDisk() {
    auto ts = upix::TrustStore::load(upix::default_trust_path());
    return trustedItemsFromInfos(ts.trusted_devices(), false);
}

} // namespace

struct AppState::HostWorker {
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> stopping{false};
    std::atomic<int> task_pct{-1};
    std::mutex pending_mtx;
    std::deque<upix::PendingApproval> pending_queue;
    upix::Coordinator coordinator;

    ~HostWorker() {
        stopping.store(true, std::memory_order_release);
        alive->store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(pending_mtx);
            for (const auto& p : pending_queue) {
                coordinator.resolve_approval(p.id, false);
            }
            pending_queue.clear();
        }
        coordinator.stop();
        if (thread.joinable()) thread.join();
    }
};

AppState::AppState() {
    initRuntime();
}

AppState::AppState(std::string custom_prefs_path)
    : prefs_path_(std::move(custom_prefs_path)) {
    initRuntime();
}

void AppState::initRuntime() {
#ifdef UPIX_VERSION
    snap_.version = UPIX_VERSION;
#endif
    snap_.models_dir = upix::default_models_dir();
    snap_.model_source = std::string(upix::model_source_id(upix::default_model_source()));
    loadPrefs();

    const char* fake = std::getenv("UPIX_FAKE");
    if (fake && fake[0] != '\0') {
        applyFixture("empty");
        return;
    }

    refreshModels();
    refreshDaemonStatus();

    try {
        auto id = upix::IdentityKey::load_or_create(upix::default_identity_path());
        auto card = upix::build_connection_card(static_cast<uint16_t>(snap_.host.port), id.public_key());
        snap_.host.fingerprint = card.fingerprint;
        snap_.host.ips = card.addresses;
        snap_.host.trusted_devices = loadTrustedDevicesFromDisk();
    } catch (...) {}
}

AppState::~AppState() {
    alive_->store(false, std::memory_order_release);
    job_cancel_.store(true, std::memory_order_release);
    if (job_thread_.joinable()) job_thread_.join();
    host_worker_.reset();
    std::vector<TrackedWorker> remaining;
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        remaining.swap(workers_);
    }
    remaining.clear(); // jthreads join deterministically
}

std::string AppState::resolvePrefsPath() const {
    if (!prefs_path_.empty()) return prefs_path_;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return (fs::path(xdg) / "upix" / "gui-prefs.json").string();
    }
    if (const char* home = upix::user_home(); home && *home) {
        return (fs::path(home) / ".config" / "upix" / "gui-prefs.json").string();
    }
    return "gui-prefs.json";
}

void AppState::setPrefsPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    prefs_path_ = path;
}

void AppState::loadPrefs() {
    const fs::path p = resolvePrefsPath();
    std::ifstream in(p);
    if (!in.is_open()) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    PrefsData data;
    if (glz::read_json(data, ss.str()) != glz::error_code::none) {
        return;
    }
    if (!data.model.empty()) snap_.model = data.model;
    snap_.overwrite_original = data.overwrite_original;
    snap_.format = snap_.overwrite_original ? std::string{} : data.format;
    snap_.quality = std::clamp(data.quality, 1, 100);
    snap_.chroma = (data.chroma == 444) ? 444 : 420;
    snap_.avif_speed = std::clamp(data.avif_speed, 0, 10);
    snap_.webp_lossless = data.webp_lossless;
    snap_.png_level = std::clamp(data.png_level, 0, 9);
    snap_.custom_out_dir = data.custom_out_dir;
    snap_.backup_original = data.backup_original;
    snap_.recursive = data.recursive;
    snap_.gpu = data.gpu;
    snap_.tile_size = data.tile_size;
    snap_.force = data.force;
    snap_.dark_mode = data.dark_mode;
    snap_.reduce_motion = data.reduce_motion;
    if (auto parsed = upix::parse_model_source(data.model_source)) {
        snap_.model_source = std::string(upix::model_source_id(*parsed));
    }
    if (data.host_port > 0 && data.host_port <= 65535) {
        snap_.host.port = data.host_port;
    }
    snap_.daemon.idle_secs = std::max(0, data.daemon_idle_secs);
}

void AppState::savePrefs() {
    PrefsData data;
    data.model = snap_.model;
    data.format = snap_.overwrite_original ? std::string{} : snap_.format;
    data.quality = snap_.quality;
    data.chroma = snap_.chroma;
    data.avif_speed = snap_.avif_speed;
    data.webp_lossless = snap_.webp_lossless;
    data.png_level = snap_.png_level;
    data.custom_out_dir = snap_.custom_out_dir;
    data.overwrite_original = snap_.overwrite_original;
    data.backup_original = snap_.backup_original;
    data.recursive = snap_.recursive;
    data.gpu = snap_.gpu;
    data.tile_size = snap_.tile_size;
    data.force = snap_.force;
    data.dark_mode = snap_.dark_mode;
    data.reduce_motion = snap_.reduce_motion;
    data.model_source = snap_.model_source;
    data.host_port = snap_.host.port;
    data.daemon_idle_secs = snap_.daemon.idle_secs;

    std::string buf;
    if (glz::write_json(data, buf)) return;
    const fs::path p = resolvePrefsPath();
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = fs::path(p.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return;
        out << buf << '\n';
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
}

Snapshot AppState::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_;
}

void AppState::post(std::function<void()> fn) {
    if (!alive_->load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(fn));
    dirty_.store(true);
}

void AppState::reapWorkers() {
    std::vector<TrackedWorker> finished;
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        auto it = workers_.begin();
        while (it != workers_.end()) {
            if (it->done->load(std::memory_order_acquire)) {
                finished.push_back(std::move(*it));
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    finished.clear();
}

void AppState::spawnWorker(std::function<void(std::shared_ptr<std::atomic<bool>> alive)> fn) {
    reapWorkers();
    if (!alive_->load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(workers_mtx_);
    TrackedWorker w;
    auto doneFlag = w.done;
    auto aliveFlag = alive_;
    w.thread = std::jthread([aliveFlag, doneFlag, f = std::move(fn)](std::stop_token) {
        try {
            if (aliveFlag->load(std::memory_order_acquire)) {
                f(aliveFlag);
            }
        } catch (...) {}
        doneFlag->store(true, std::memory_order_release);
    });
    workers_.push_back(std::move(w));
}

bool AppState::pollEvents() {
    reapWorkers();
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
#ifdef UPIX_VERSION
    snap_.version = UPIX_VERSION;
#endif
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
    snap_.daemon = {true, false, 4321, 300, "热引擎已就绪"};
    snap_.host.fingerprint = "EC61F2B66D4C25C4";
    snap_.host.ips = {"192.168.0.126", "10.210.210.2"};
    snap_.host.port = 9527;
    snap_.host.trusted_devices = {
        {"Pixel 9 Pro", "EC61 · F2B6 · 6D4C · 25C4", true},
        {"Xiaomi 14 Ultra", "89A4 · C012 · 77D1 · 11F0", false},
    };

    if (state == "default" || state == "picked") {
        snap_.inputs = {
            {1, "scene.png", "scene.png", "", "", false, 1, 640, 427, "待处理", 0},
            {2, "portrait_02.jpg", "portrait_02.jpg", "", "", false, 1, 1280, 720, "待处理", 0},
            {3, "wallpaper_03.webp", "wallpaper_03.webp", "", "", false, 1, 1920, 1080, "待处理", 0},
        };
    } else if (state == "running") {
        snap_.format = "avif";
        snap_.quality = 88;
        snap_.custom_out_dir = "~/Pictures/Exports";
        snap_.inputs = {
            {1, "shot_01.png", "shot_01.png", "", "shot_01_out.avif", false, 1, 640, 427, "4× · 0.8s", 2},
            {2, "shot_02.png", "shot_02.png", "", "shot_02_out.avif", false, 1, 640, 427, "4× · 0.9s", 2},
            {3, "shot_03.png", "shot_03.png", "", "", false, 1, 640, 427, "75% · 18/24", 1},
            {4, "shot_04.png", "shot_04.png", "", "", false, 1, 640, 427, "640×427", 0},
            {5, "shot_05.png", "shot_05.png", "", "", false, 1, 640, 427, "640×427", 0},
        };
        snap_.active_input = 2;
        snap_.job.phase = JobPhase::Running;
        snap_.job.index = 3;
        snap_.job.total = 5;
        snap_.job.percent = 75;
        snap_.job.tiles_done = 18;
        snap_.job.tiles_total = 24;
        snap_.job.eta_sec = 2;
        snap_.job.current_file = "shot_03.png";
    } else if (state == "done") {
        snap_.inputs = {
            {1, "__fixture_before__", "scene.png", "", "__fixture_after__", false, 1, 640, 427, "4× · 0.84s", 2},
        };
        snap_.job.phase = JobPhase::Done;
        snap_.job.index = 1;
        snap_.job.total = 1;
        snap_.job.percent = 100;
        snap_.job.elapsed_sec = 0.84;
        snap_.job.in_w = 640;
        snap_.job.in_h = 427;
        snap_.job.out_w = 2560;
        snap_.job.out_h = 1708;
        snap_.job.out_size_str = "14.2 MB";
        snap_.job.outputs = {"__fixture_after__"};
    } else if (state == "error") {
        snap_.inputs = {
            {1, "shot_01.png", "shot_01.png", "", "shot_01_out.png", false, 1, 640, 427, "4× · 0.8s", 2},
            {2, "shot_02.png", "shot_02.png", "", "shot_02_out.png", false, 1, 640, 427, "4× · 0.9s", 2},
            {3, "shot_03.png", "shot_03.png", "", "", false, 1, 640, 427, "显存不足", 3},
            {4, "shot_04.png", "shot_04.png", "", "", false, 1, 640, 427, "640×427", 0},
            {5, "shot_05.png", "shot_05.png", "", "", false, 1, 640, 427, "640×427", 0},
        };
        snap_.active_input = 2;
        snap_.job.phase = JobPhase::Error;
        snap_.job.index = 3;
        snap_.job.total = 5;
        snap_.job.message = "显存分配不足";
        snap_.job.diag_detail = "VK_ERROR_OUT_OF_DEVICE_MEMORY · tile 512px";
    } else if (state == "error-missing-model" || state == "error_missing_model") {
        snap_.model = "BSRGAN-x4";
        snap_.inputs = {
            {1, "shot_01.png", "shot_01.png", "", "", false, 1, 640, 427, "缺少模型", 3},
        };
        snap_.job.phase = JobPhase::Error;
        snap_.job.index = 1;
        snap_.job.total = 1;
        snap_.job.missing_model = "BSRGAN-x4";
        snap_.job.message = "未找到模型 BSRGAN-x4";
        snap_.job.diag_detail = "请先在模型库下载或切换已安装模型";
    } else if (state == "models") {
        snap_.downloading = "BSRGAN-x4";
        snap_.download_pct = 60;
        snap_.models_status = "正在下载 BSRGAN-x4 · 60%";
    } else if (state == "picker") {
        snap_.inputs = {
            {1, "scene.png", "scene.png", "", "", false, 1, 640, 427, "待处理", 0},
        };
    } else if (state == "host" || state == "pairing") {
        snap_.host.running = true;
        snap_.host.qr_payload =
            "{\"v\":2,\"ip\":\"192.168.0.126\",\"port\":9527,\"fp\":\"EC61F2B66D4C25C4\","
            "\"pk\":\"dGVzdCBwdWJsaWMga2V5IDMyIGJ5dGVzISE=\"}";
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
    } else if (state == "host-idle" || state == "host_idle") {
        snap_.host.running = false;
        snap_.host.trusted_devices.clear();
        snap_.host.logs.clear();
    } else if (state == "settings") {
        // Base populated state is ideal for settings
    }
    dirty_.store(true);
}

void AppState::addPaths(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) {
        snap_.notice = "任务进行中，请先等待完成或取消";
        snap_.notice_seq++;
        dirty_.store(true);
        return;
    }

    int skipped = 0;
    const bool rec = snap_.recursive;
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
        if (clean.empty()) clean = p.lexically_normal().string();
        bool dup = false;
        for (const auto& it : snap_.inputs) {
            if (it.path == clean) { dup = true; break; }
        }
        if (dup) continue;

        InputItem item;
        item.id = next_input_id_++;
        item.path = clean;
        item.name = p.filename().string();
        if (item.name.empty()) item.name = clean;
        item.is_dir = is_dir;
        if (is_dir) {
            item.batch_root = clean;
            const int cnt = static_cast<int>(gatherSupportedImagesInDir(p, rec).size());
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
    snap_.job = JobState{};
    if (!snap_.inputs.empty()) {
        snap_.active_input = std::clamp(snap_.active_input, 0, static_cast<int>(snap_.inputs.size()) - 1);
    } else {
        snap_.active_input = 0;
    }
    if (skipped > 0) {
        snap_.notice = "已忽略 " + std::to_string(skipped) + " 个不支持的文件";
        snap_.notice_seq++;
    }
    dirty_.store(true);
}

void AppState::addFolder(const std::string& path, bool recursive) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) return;
        snap_.recursive = recursive;
    }
    addPaths({path});
}

bool AppState::addFromClipboardText(const std::string& text) {
    if (text.empty()) return false;
    std::vector<std::string> candidates;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        if (start > 0) line = line.substr(start);
        if (line.size() >= 2 && ((line.front() == '"' && line.back() == '"') ||
                                  (line.front() == '\'' && line.back() == '\''))) {
            line = line.substr(1, line.size() - 2);
        }
        if (line.rfind("file://", 0) == 0) {
            line = line.substr(7);
        }
        if (line.empty()) continue;
        std::error_code ec;
        if (fs::exists(line, ec)) candidates.push_back(line);
    }
    if (candidates.empty()) return false;
    const size_t before = snapshot().inputs.size();
    addPaths(candidates);
    return snapshot().inputs.size() > before;
}

bool AppState::addFromClipboardRgba(int width, int height, const std::uint8_t* rgba, std::size_t bytes) {
    if (width <= 0 || height <= 0 || !rgba) return false;
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (bytes < expected) return false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) return false;
    }

    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "upix-clipboard";
    fs::create_directories(dir, ec);
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    fs::path out = dir / ("clip-" + std::to_string(ts) + ".png");

    upix::Image img(width, height, 4);
    std::copy_n(rgba, expected, img.data());
    if (!img.save(out.string())) return false;
    addPaths({out.string()});
    return true;
}

void AppState::clearInputs() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) return;
    snap_.inputs.clear();
    snap_.active_input = 0;
    snap_.job = JobState{};
    dirty_.store(true);
}

void AppState::removeInput(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) return;
    snap_.inputs.erase(
        std::remove_if(snap_.inputs.begin(), snap_.inputs.end(),
                       [&](const InputItem& i) { return i.path == path; }),
        snap_.inputs.end());
    if (snap_.inputs.empty()) {
        snap_.active_input = 0;
        snap_.job = JobState{};
    } else {
        snap_.active_input = std::clamp(snap_.active_input, 0, static_cast<int>(snap_.inputs.size()) - 1);
    }
    dirty_.store(true);
}

void AppState::setActiveInput(int idx) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.inputs.empty()) {
        snap_.active_input = 0;
    } else {
        snap_.active_input = std::clamp(idx, 0, static_cast<int>(snap_.inputs.size()) - 1);
    }
    dirty_.store(true);
}

void AppState::setModel(const std::string& m) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) return;
    snap_.model = m;
    savePrefs();
    dirty_.store(true);
}

void AppState::setFormat(const std::string& f) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) return;
    if (snap_.overwrite_original) {
        snap_.format.clear();
    } else {
        snap_.format = f;
    }
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
    snap_.chroma = (c == 444) ? 444 : 420;
    savePrefs();
    dirty_.store(true);
}

void AppState::setAvifSpeed(int s) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.avif_speed = std::clamp(s, 0, 10);
    savePrefs();
    dirty_.store(true);
}

void AppState::setTileSize(int ts) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.tile_size = ts;
    savePrefs();
    dirty_.store(true);
}

void AppState::setGpu(bool g) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.gpu = g;
    savePrefs();
    dirty_.store(true);
}

void AppState::setForce(bool f) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.force = f;
    savePrefs();
    dirty_.store(true);
}

void AppState::setOverwriteOriginal(bool ov) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) return;
    snap_.overwrite_original = ov;
    if (ov) {
        snap_.format.clear();
    }
    savePrefs();
    dirty_.store(true);
}

void AppState::setBackupOriginal(bool b) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.backup_original = b;
    savePrefs();
    dirty_.store(true);
}

void AppState::setRecursive(bool r) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.recursive = r;
    savePrefs();
    dirty_.store(true);
}

void AppState::setCustomOutDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.custom_out_dir = dir;
    savePrefs();
    dirty_.store(true);
}

void AppState::setDarkMode(bool dark, bool persist) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.dark_mode = dark;
    if (persist) savePrefs();
    dirty_.store(true);
}

void AppState::setReduceMotion(bool reduce) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.reduce_motion = reduce;
    savePrefs();
    dirty_.store(true);
}

void AppState::setNotice(const std::string& msg) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.notice = msg;
    snap_.notice_seq++;
    dirty_.store(true);
}

void AppState::clearNotice() {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.notice.clear();
    dirty_.store(true);
}

void AppState::clearModelsStatus() {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.models_status.clear();
    dirty_.store(true);
}

void AppState::startJob() {
    Snapshot runSnap;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (snap_.job.phase == JobPhase::Running || snap_.job.phase == JobPhase::Cancelling) return;
        if (snap_.inputs.empty() || snap_.model.empty()) return;

        // Check whether chosen model actually exists before spawning inference.
        const std::string modelsDir = upix::default_models_dir();
        const bool paramExists = fs::exists(fs::path(modelsDir) / (snap_.model + ".param"));
        const bool binExists = fs::exists(fs::path(modelsDir) / (snap_.model + ".bin"));
        if (!paramExists || !binExists) {
            snap_.job = JobState{};
            snap_.job.phase = JobPhase::Error;
            snap_.job.missing_model = snap_.model;
            snap_.job.message = "未找到模型 " + snap_.model;
            snap_.job.diag_detail = "请先下载该模型或切换其他已安装模型";
            for (auto& inp : snap_.inputs) {
                inp.status_kind = 3;
                inp.status_text = "缺少模型";
            }
            dirty_.store(true);
            return;
        }

        snap_.job = JobState{};
        snap_.job.phase = JobPhase::Running;
        snap_.job.total = static_cast<int>(snap_.inputs.size());
        snap_.job.index = 1;
        snap_.job.current_file = snap_.inputs.front().name;
        for (auto& inp : snap_.inputs) {
            inp.status_kind = 0;
            inp.status_text = "待处理";
            inp.output_path.clear();
        }
        snap_.inputs.front().status_kind = 1;
        snap_.inputs.front().status_text = "推理中";
        runSnap = snap_;
        dirty_.store(true);
    }

    job_cancel_.store(false);
    if (job_thread_.joinable()) job_thread_.join();

    auto aliveFlag = alive_;
    job_thread_ = std::jthread([this, aliveFlag, runSnap](std::stop_token) {
        if (!aliveFlag->load(std::memory_order_acquire)) return;
        const auto t0 = std::chrono::steady_clock::now();

        upix::BatchRequest req;
        req.models_dir = upix::default_models_dir();
        req.model_name = runSnap.model;
        req.use_gpu = runSnap.gpu;
        req.force = runSnap.force;
        req.overwrite_original = runSnap.overwrite_original;
        req.backup_original = runSnap.backup_original;
        req.recommend_tile_size = (runSnap.tile_size <= 0);
        req.config.use_gpu = runSnap.gpu;
        req.config.auto_tile_size = (runSnap.tile_size <= 0);
        req.config.tile_size = (runSnap.tile_size > 0) ? runSnap.tile_size : 256;
        req.config.tile_overlap = upix::kDefaultTileOverlap;
        req.config.output_quality = runSnap.quality;
        if (!runSnap.overwrite_original && !runSnap.format.empty()) {
            req.config.output_format = upix::normalize_output_format(runSnap.format);
        }
        req.config.encoder.jpeg_subsampling = runSnap.chroma;
        req.config.encoder.avif_subsampling = runSnap.chroma;
        req.config.encoder.avif_speed = runSnap.avif_speed;
        req.config.encoder.webp_lossless = runSnap.webp_lossless;
        req.config.encoder.png_level = runSnap.png_level;

        std::unordered_set<std::string> reservedOutputs;
        std::unordered_map<std::string, size_t> pathToInputIdx;
        std::unordered_map<std::string, size_t> outPathToInputIdx;
        std::unordered_map<std::string, size_t> outStemToInputIdx;

        auto registerTask = [&](std::string inPath, std::string outPath, size_t idx) {
            pathToInputIdx[inPath] = idx;
            outPathToInputIdx[outPath] = idx;
            fs::path op(outPath);
            outStemToInputIdx[(op.parent_path() / op.stem()).string()] = idx;
            req.tasks.emplace_back(std::move(inPath), std::move(outPath));
        };

        for (size_t idx = 0; idx < runSnap.inputs.size(); ++idx) {
            const auto& inp = runSnap.inputs[idx];
            if (inp.is_dir) {
                if (runSnap.overwrite_original) {
                    auto files = gatherSupportedImagesInDir(inp.path, runSnap.recursive);
                    for (auto& f : files) {
                        std::string outCopy = f;
                        registerTask(std::move(f), std::move(outCopy), idx);
                    }
                } else {
                    std::string err;
                    std::string outDir = runSnap.custom_out_dir.empty()
                        ? upix::derive_default_output(inp.path, req.model_name, true)
                        : runSnap.custom_out_dir;
                    auto tasks = upix::gather_directory_tasks(inp.path, outDir, err, runSnap.recursive);
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
                    for (auto& pair : *tasks) {
                        pair.second = uniqueOutputPath(pair.second, reservedOutputs);
                        registerTask(std::move(pair.first), std::move(pair.second), idx);
                    }
                }
            } else {
                std::string outPath = upix::compute_batch_output_path(
                    upix::BatchInput{inp.path, ""}, req.model_name, req.config,
                    runSnap.custom_out_dir, runSnap.overwrite_original).string();
                if (!runSnap.overwrite_original) {
                    outPath = uniqueOutputPath(outPath, reservedOutputs);
                }
                registerTask(inp.path, std::move(outPath), idx);
            }
        }

        const int totalTasks = static_cast<int>(req.tasks.size());
        post([this, totalTasks]() {
            snap_.job.total = std::max(1, totalTasks);
        });

        std::mutex cbMtx;
        int completedTasks = 0;
        size_t lastActiveIdx = 0;
        std::unordered_map<size_t, std::chrono::steady_clock::time_point> startByInput;

        upix::BatchCallbacks cb;
        cb.on_current = [this, &cbMtx, &pathToInputIdx, &completedTasks, &lastActiveIdx, &startByInput](const std::string& file) {
            size_t idxCopy = 0;
            int curNum = 1;
            {
                std::lock_guard<std::mutex> lk(cbMtx);
                auto it = pathToInputIdx.find(file);
                if (it != pathToInputIdx.end()) lastActiveIdx = it->second;
                idxCopy = lastActiveIdx;
                startByInput.try_emplace(idxCopy, std::chrono::steady_clock::now());
                curNum = completedTasks + 1;
            }
            post([this, file, curNum, idxCopy]() {
                snap_.job.index = curNum;
                snap_.job.current_file = fs::path(file).filename().string();
                snap_.job.percent = 0;
                if (idxCopy < snap_.inputs.size()) {
                    snap_.inputs[idxCopy].status_kind = 1;
                    snap_.inputs[idxCopy].status_text = "推理中";
                }
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
        cb.on_saved = [this, &cbMtx, &completedTasks, &lastActiveIdx, &pathToInputIdx, &outPathToInputIdx, &outStemToInputIdx, &startByInput](const std::string& finalOut) {
            size_t idxCopy = 0;
            double itemSec = 0.0;
            {
                std::lock_guard<std::mutex> lk(cbMtx);
                ++completedTasks;
                if (auto it = pathToInputIdx.find(finalOut); it != pathToInputIdx.end()) {
                    idxCopy = it->second;
                } else if (auto oit = outPathToInputIdx.find(finalOut); oit != outPathToInputIdx.end()) {
                    idxCopy = oit->second;
                } else {
                    fs::path fp(finalOut);
                    std::string stemKey = (fp.parent_path() / fp.stem()).string();
                    if (auto sit = outStemToInputIdx.find(stemKey); sit != outStemToInputIdx.end()) {
                        idxCopy = sit->second;
                    } else {
                        idxCopy = lastActiveIdx;
                    }
                }
                auto now = std::chrono::steady_clock::now();
                if (auto stIt = startByInput.find(idxCopy); stIt != startByInput.end()) {
                    itemSec = std::chrono::duration<double>(now - stIt->second).count();
                }
            }
            std::error_code ec;
            auto fsz = fs::file_size(finalOut, ec);
            std::string szStr = ec ? "" : formatBytes(fsz);
            post([this, finalOut, szStr, idxCopy, itemSec]() {
                snap_.job.outputs.push_back(finalOut);
                if (!szStr.empty()) snap_.job.out_size_str = szStr;
                if (idxCopy < snap_.inputs.size()) {
                    snap_.inputs[idxCopy].output_path = finalOut;
                    if (!snap_.inputs[idxCopy].is_dir) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "完成 · %.1fs", itemSec);
                        snap_.inputs[idxCopy].status_kind = 2;
                        snap_.inputs[idxCopy].status_text = buf;
                    } else {
                        snap_.inputs[idxCopy].status_kind = 2;
                        snap_.inputs[idxCopy].status_text = "批处理完成";
                    }
                }
            });
        };
        cb.on_skipped = [this, &cbMtx, &completedTasks, &pathToInputIdx, &lastActiveIdx](const std::string& file) {
            size_t idxCopy = 0;
            {
                std::lock_guard<std::mutex> lk(cbMtx);
                ++completedTasks;
                auto it = pathToInputIdx.find(file);
                idxCopy = (it != pathToInputIdx.end()) ? it->second : lastActiveIdx;
            }
            post([this, idxCopy]() {
                if (idxCopy < snap_.inputs.size() && !snap_.inputs[idxCopy].is_dir) {
                    snap_.inputs[idxCopy].status_kind = 2;
                    snap_.inputs[idxCopy].status_text = "已跳过";
                }
            });
        };
        cb.on_error = [this, &cbMtx, &pathToInputIdx, &lastActiveIdx](const std::string& file, const std::string& reason) {
            size_t idxCopy = 0;
            {
                std::lock_guard<std::mutex> lk(cbMtx);
                auto it = pathToInputIdx.find(file);
                idxCopy = (it != pathToInputIdx.end()) ? it->second : lastActiveIdx;
            }
            post([this, idxCopy, reason]() {
                if (idxCopy < snap_.inputs.size()) {
                    snap_.inputs[idxCopy].status_kind = 3;
                    snap_.inputs[idxCopy].status_text = "失败";
                }
                if (!reason.empty()) snap_.job.diag_detail = reason;
            });
        };

        auto stats = upix::run_batch(req, cb, job_cancel_);
        if (job_cancel_.load(std::memory_order_acquire)) {
            post([this]() {
                snap_.job.phase = JobPhase::Cancelled;
                snap_.job.message = "已取消";
                for (auto& inp : snap_.inputs) {
                    if (inp.status_kind == 1 || inp.status_kind == 0) {
                        inp.status_kind = 0;
                        inp.status_text = "已取消";
                    }
                }
            });
            return;
        }
        if (!stats || (stats->succeeded + stats->skipped < stats->total)) {
            size_t failIdx = 0;
            {
                std::lock_guard<std::mutex> lk(cbMtx);
                failIdx = lastActiveIdx;
            }
            post([this, failIdx]() {
                snap_.job.phase = JobPhase::Error;
                snap_.job.message = "推理执行失败";
                if (snap_.job.diag_detail.empty()) {
                    snap_.job.diag_detail = "请检查模型文件或计算设备状态";
                }
                for (auto& inp : snap_.inputs) {
                    if (inp.status_kind == 1) {
                        inp.status_kind = 3;
                        inp.status_text = "失败";
                    }
                }
                if (failIdx < snap_.inputs.size() && snap_.inputs[failIdx].status_kind == 0) {
                    snap_.inputs[failIdx].status_kind = 3;
                    snap_.inputs[failIdx].status_text = "失败";
                }
            });
            return;
        }

        double totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        post([this, totalSec]() {
            snap_.job.phase = JobPhase::Done;
            snap_.job.percent = 100;
            snap_.job.elapsed_sec = totalSec;
            snap_.job.message = "完成";
            for (auto& inp : snap_.inputs) {
                if (inp.status_kind == 1 || inp.status_kind == 0) {
                    inp.status_kind = 2;
                    if (inp.status_text == "推理中" || inp.status_text == "待处理") {
                        inp.status_text = "完成";
                    }
                }
            }
        });
    });
}

void AppState::cancelJob() {
    job_cancel_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(mtx_);
    if (snap_.job.phase == JobPhase::Running) {
        snap_.job.phase = JobPhase::Cancelling;
        snap_.job.message = "正在取消…";
        dirty_.store(true);
    }
}

void AppState::setModelSource(const std::string& src) {
    auto parsed = upix::parse_model_source(src);
    if (!parsed) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const std::string normalized(upix::model_source_id(*parsed));
        if (snap_.model_source == normalized) return;
        snap_.model_source = normalized;
        savePrefs();
        dirty_.store(true);
    }
    refreshModels();
}

void AppState::refreshModels() {
    upix::ModelSource src = upix::default_model_source();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (auto parsed = upix::parse_model_source(snap_.model_source)) {
            src = *parsed;
        }
    }
    spawnWorker([this, src](std::shared_ptr<std::atomic<bool>>) {
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

        auto cat = upix::fetch_remote_models_or_catalog("", src);
        std::string err = cat.fallback_error.value_or("");
        std::vector<ModelItem> rem;
        rem.reserve(cat.models.size());
        for (const auto& m : cat.models) {
            auto meta = upix::classify_model_scene(m.name);
            rem.push_back(ModelItem{
                m.name, m.name, m.scale,
                m.size_bytes > 0 ? formatBytes(m.size_bytes) : std::string("目录"),
                std::string(meta.scene), meta.recommended, true
            });
        }
        post([this, rem = std::move(rem), err = std::move(err)]() mutable {
            snap_.remote_error = std::move(err);
            snap_.remote = std::move(rem);
            rebuildModelGroups();
        });
    });
}

void AppState::importModels(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.models_status = "正在导入模型…";
        dirty_.store(true);
    }
    spawnWorker([this, paths](std::shared_ptr<std::atomic<bool>>) {
        auto res = upix::import_model_files(paths, upix::default_models_dir());
        post([this, res]() {
            if (res.ok) {
                snap_.models_status = "模型导入完成";
                snap_.notice = "模型导入完成";
                snap_.notice_seq++;
                refreshModels();
            } else {
                snap_.models_status = res.error.empty() ? "导入失败" : res.error;
                snap_.notice = snap_.models_status;
                snap_.notice_seq++;
            }
        });
    });
}

void AppState::downloadModel(const std::string& name) {
    upix::ModelSource src = upix::default_model_source();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!snap_.downloading.empty()) return;
        if (auto parsed = upix::parse_model_source(snap_.model_source)) {
            src = *parsed;
        }
        snap_.downloading = name;
        snap_.download_pct = 0;
        snap_.models_status = "正在下载 " + name + "…";
        dirty_.store(true);
    }
    spawnWorker([this, name, src](std::shared_ptr<std::atomic<bool>>) {
        std::string err;
        auto remote = upix::fetch_remote_models("", src, err);
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
                snap_.models_status = "未在远端清单中找到: " + name;
                snap_.notice = snap_.models_status;
                snap_.notice_seq++;
            });
            return;
        }
        auto res = upix::download_model(*target, upix::default_models_dir(),
            [this, name](const upix::DownloadProgress& p) {
                if (p.total <= 0) return;
                int pct = static_cast<int>(p.downloaded * 100 / p.total);
                post([this, name, pct]() {
                    snap_.download_pct = pct;
                    snap_.models_status = "正在下载 " + name + " · " + std::to_string(pct) + "%";
                });
            },
            src);
        post([this, name, res]() {
            snap_.downloading.clear();
            snap_.download_pct = -1;
            if (res.ok) {
                snap_.models_status = "已安装 " + name;
                snap_.notice = "已安装 " + name;
                snap_.notice_seq++;
                refreshModels();
            } else {
                snap_.models_status = "下载失败: " + name;
                snap_.notice = "下载失败: " + name;
                snap_.notice_seq++;
            }
        });
    });
}

void AppState::removeModel(const std::string& name) {
    std::string dir = upix::default_models_dir();
    std::error_code ec;
    fs::remove(fs::path(dir) / (name + ".param"), ec);
    fs::remove(fs::path(dir) / (name + ".bin"), ec);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.models_status = "已移除模型: " + name;
        snap_.notice = snap_.models_status;
        snap_.notice_seq++;
        dirty_.store(true);
    }
    refreshModels();
}

void AppState::refreshDaemonStatus() {
    std::string sock = upix::default_serve_socket_path();
    auto st = upix::serve_daemon_status(sock);
    const bool ready = st.running && (!st.socket_missing || upix::daemon_endpoint_present(sock));
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.daemon.running = ready;
    snap_.daemon.loading = false;
    snap_.daemon.pid = static_cast<int>(st.pid);
    snap_.daemon.label = ready ? "热引擎已就绪" : "热引擎未启动";
    dirty_.store(true);
}

void AppState::setDaemonIdleSecs(int secs) {
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.daemon.idle_secs = std::clamp(secs, 0, 86400);
    savePrefs();
    dirty_.store(true);
}

void AppState::toggleDaemon() {
    const auto snap = snapshot();
    const bool running = snap.daemon.running;
    const int idleSecs = snap.daemon.idle_secs;
    const std::string modelName = snap.model;
    const bool forceCpu = !snap.gpu;
    std::string sock = upix::default_serve_socket_path();
    if (running) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snap_.daemon.loading = true;
            snap_.daemon.label = "正在停止热引擎…";
            dirty_.store(true);
        }
        spawnWorker([this, sock](std::shared_ptr<std::atomic<bool>>) {
            upix::serve_daemon_stop(sock);
            post([this]() { refreshDaemonStatus(); });
        });
    } else {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snap_.daemon.loading = true;
            snap_.daemon.label = "正在启动热引擎…";
            dirty_.store(true);
        }
        spawnWorker([this, sock, idleSecs, modelName, forceCpu](std::shared_ptr<std::atomic<bool>>) {
            std::string err;
            bool ok = upix::ensure_serve_daemon_running(
                upix::default_models_dir(), idleSecs, err, sock, modelName, forceCpu);
            post([this, ok, err = std::move(err)]() {
                refreshDaemonStatus();
                if (!ok && !err.empty()) {
                    snap_.notice = "热引擎启动失败: " + err;
                    snap_.notice_seq++;
                }
            });
        });
    }
}

void AppState::hostStart() {
    if (host_worker_) return;
    int port = snapshot().host.port;
    host_worker_ = std::make_unique<HostWorker>();
    HostWorker* w = host_worker_.get();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.host.running = true;
        snap_.host.logs.push_back(formatHostLog("HOST", "正在启动服务 :" + std::to_string(port)));
        dirty_.store(true);
    }

    auto aliveFlag = alive_;
    auto hostAlive = w->alive;
    w->thread = std::jthread([this, aliveFlag, hostAlive, w, port](std::stop_token) {
        if (!aliveFlag->load(std::memory_order_acquire) ||
            w->stopping.load(std::memory_order_acquire)) return;
        try {
            auto id = upix::IdentityKey::load_or_create(upix::default_identity_path());
            w->coordinator.set_models_directory(upix::default_models_dir());
            w->coordinator.set_trust_store_path(upix::default_trust_path());
            w->coordinator.on_task_progress() = [w](float prog) {
                w->task_pct.store(prog < 0 ? -1 : static_cast<int>(prog * 100));
            };
            if (w->stopping.load(std::memory_order_acquire) ||
                !aliveFlag->load(std::memory_order_acquire)) return;
            auto started = w->coordinator.start(static_cast<uint16_t>(port), id);
            if (!started) {
                post([this, hostAlive, msg = started.error().message]() {
                    if (!hostAlive->load(std::memory_order_acquire)) return;
                    snap_.host.running = false;
                    snap_.host.qr_payload.clear();
                    snap_.notice = "服务启动失败: " + msg;
                    snap_.notice_seq++;
                    snap_.host.logs.push_back(formatHostLog("ERR", msg));
                    host_worker_.reset();
                });
                return;
            }
            auto card = upix::build_connection_card(static_cast<uint16_t>(port), id.public_key());
            auto trusted = trustedItemsFromInfos(w->coordinator.trusted_devices(), false);
            post([this, hostAlive, port, card, trusted = std::move(trusted)]() mutable {
                if (!hostAlive->load(std::memory_order_acquire)) return;
                snap_.host.running = true;
                snap_.host.fingerprint = card.fingerprint;
                snap_.host.ips = card.addresses;
                if (snap_.host.selected_ip_idx >= static_cast<int>(snap_.host.ips.size())) {
                    snap_.host.selected_ip_idx = 0;
                }
                snap_.host.qr_payload = card.qr_payload;
                snap_.host.trusted_devices = std::move(trusted);
                snap_.host.logs.push_back(formatHostLog("HOST", "监听已就绪 :" + std::to_string(port) + " · 二维码可用"));
            });

            int lastPct = -1;
            while (!w->stopping.load(std::memory_order_acquire) &&
                   aliveFlag->load(std::memory_order_acquire) &&
                   w->coordinator.is_running()) {
                if (auto pend = w->coordinator.pop_pending_approval()) {
                    bool showNow = false;
                    {
                        std::lock_guard<std::mutex> lk(w->pending_mtx);
                        showNow = w->pending_queue.empty();
                        w->pending_queue.push_back(*pend);
                    }
                    post([this, hostAlive, dev = pend->device_name, fp = pend->fingerprint, showNow]() {
                        if (!hostAlive->load(std::memory_order_acquire)) return;
                        snap_.host.logs.push_back(formatHostLog("PAIR", "收到配对请求 · " + dev));
                        if (showNow || snap_.host.pending_device.empty()) {
                            snap_.host.pending_device = dev;
                            snap_.host.pending_fp = fp;
                        }
                    });
                }
                int pct = w->task_pct.load(std::memory_order_acquire);
                if (pct != lastPct) {
                    lastPct = pct;
                    post([this, hostAlive, pct]() {
                        if (!hostAlive->load(std::memory_order_acquire)) return;
                        snap_.host.task_percent = pct;
                    });
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            post([this, hostAlive]() {
                if (!hostAlive->load(std::memory_order_acquire)) return;
                snap_.host.running = false;
                snap_.host.qr_payload.clear();
            });
        } catch (const std::exception& e) {
            post([this, hostAlive, msg = std::string(e.what())]() {
                if (!hostAlive->load(std::memory_order_acquire)) return;
                snap_.host.running = false;
                snap_.host.qr_payload.clear();
                snap_.notice = "服务异常: " + msg;
                snap_.notice_seq++;
                snap_.host.logs.push_back(formatHostLog("ERR", msg));
                host_worker_.reset();
            });
        } catch (...) {
            post([this, hostAlive]() {
                if (!hostAlive->load(std::memory_order_acquire)) return;
                snap_.host.running = false;
                snap_.host.qr_payload.clear();
                snap_.notice = "服务启动异常";
                snap_.notice_seq++;
                host_worker_.reset();
            });
        }
    });
}

void AppState::hostStop() {
    if (!host_worker_) return;
    host_worker_->stopping.store(true, std::memory_order_release);
    host_worker_.reset();
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.host.running = false;
    snap_.host.qr_payload.clear();
    snap_.host.pending_device.clear();
    snap_.host.pending_fp.clear();
    snap_.host.logs.push_back(formatHostLog("HOST", "服务已停止"));
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
    std::string nextDev;
    std::string nextFp;
    std::string resolvedDev;
    std::string resolvedFp;
    if (host_worker_) {
        std::lock_guard<std::mutex> lk(host_worker_->pending_mtx);
        if (!host_worker_->pending_queue.empty()) {
            const auto head = host_worker_->pending_queue.front();
            resolvedDev = head.device_name;
            resolvedFp = head.fingerprint;
            host_worker_->coordinator.resolve_approval(head.id, allow);
            host_worker_->pending_queue.pop_front();
            if (!host_worker_->pending_queue.empty()) {
                nextDev = host_worker_->pending_queue.front().device_name;
                nextFp = host_worker_->pending_queue.front().fingerprint;
            }
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!resolvedDev.empty()) {
        snap_.host.logs.push_back(
            formatHostLog("PAIR", std::string(allow ? "已允许配对 · " : "已拒绝配对 · ") + resolvedDev));
        if (allow) {
            bool exists = false;
            for (const auto& td : snap_.host.trusted_devices) {
                if (td.fingerprint == resolvedFp) { exists = true; break; }
            }
            if (!exists) {
                snap_.host.trusted_devices.push_back(TrustedDeviceItem{resolvedDev, resolvedFp, true});
            }
        }
    }
    snap_.host.pending_device = nextDev;
    snap_.host.pending_fp = nextFp;
    dirty_.store(true);
}

void AppState::removeTrustedDevice(const std::string& fingerprint) {
    if (host_worker_) {
        host_worker_->coordinator.remove_trusted_device(fingerprint);
    } else {
        auto ts = upix::TrustStore::load(upix::default_trust_path());
        if (ts.remove_by_fingerprint(fingerprint)) {
            std::error_code ec;
            ts.save(ec);
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.host.trusted_devices.erase(
        std::remove_if(snap_.host.trusted_devices.begin(), snap_.host.trusted_devices.end(),
                       [&](const TrustedDeviceItem& td) { return td.fingerprint == fingerprint; }),
        snap_.host.trusted_devices.end());
    dirty_.store(true);
}

void AppState::clearTrustedDevices() {
    if (host_worker_) {
        host_worker_->coordinator.clear_trusted_devices();
    } else {
        auto ts = upix::TrustStore::load(upix::default_trust_path());
        ts.clear();
        std::error_code ec;
        ts.save(ec);
    }
    std::lock_guard<std::mutex> lk(mtx_);
    snap_.host.trusted_devices.clear();
    dirty_.store(true);
}

} // namespace upix::gui
