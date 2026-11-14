#pragma once
#include "core/batch.hpp"
#include "core/config.hpp"
#include "core/distributed/connection_card.hpp"
#include "core/distributed/coordinator.hpp"
#include "core/distributed/identity.hpp"
#include "core/distributed/protocol.hpp"
#include "core/model_downloader.hpp"
#include "core/model_families.hpp"
#include "core/model_install.hpp"
#include "core/model_manager.hpp"
#include "core/model_metadata.hpp"
#include "core/serve/daemon.hpp"
#include "core/serve/daemon_control.hpp"
#include "utils/encoder_options.hpp"
#include "utils/image.hpp"
#include "utils/local_socket.hpp"
#include "qrcodegen.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace upix::gui {

struct InputItem {
    std::uint64_t id = 0;
    std::string path;
    std::string name;
    std::string batch_root;
    std::string output_path;
    bool is_dir = false;
    int count = 1;
    int width = 0;
    int height = 0;
    std::string status_text;
    int status_kind = 0; // 0=pending, 1=running, 2=done, 3=error
};

struct ModelItem {
    std::string name;
    std::string short_name;
    int scale = 4;
    std::string size;
    std::string scene;
    bool recommended = false;
    bool remote = false;
};

struct ModelFamily {
    std::string label;
    std::vector<ModelItem> items;
};

enum class JobPhase { Idle, Running, Cancelling, Done, Error, Cancelled };

struct JobState {
    JobPhase phase = JobPhase::Idle;
    std::string message;
    std::string diag_detail;
    std::string missing_model;
    int index = 0;
    int total = 0;
    int percent = 0;
    int tiles_done = 0;
    int tiles_total = 0;
    int eta_sec = 0;
    double elapsed_sec = 0.0;
    std::string current_file;
    std::vector<std::string> outputs;
    int in_w = 640, in_h = 427;
    int out_w = 2560, out_h = 1708;
    std::string out_size_str = "14.2 MB";
};

struct TrustedDeviceItem {
    std::string name;
    std::string fingerprint;
    bool online = false;
};

struct HostState {
    bool running = false;
    std::string qr_payload;
    std::string fingerprint;
    std::vector<std::string> ips;
    int selected_ip_idx = 0;
    int port = 9527;
    int task_percent = -1;
    std::string active_device;
    std::vector<std::string> logs;
    std::string pending_device;
    std::string pending_fp;
    std::vector<TrustedDeviceItem> trusted_devices;
};

struct DaemonState {
    bool running = false;
    bool loading = false;
    int pid = 0;
    int idle_secs = 300;
    std::string label = "热引擎未启动";
};

struct Snapshot {
    std::string version = "0.2.0";
    std::vector<InputItem> inputs;
    int active_input = 0;
    std::string model = "AnimeSharp-x4";
    std::string format = "";          // "" (跟随), "png", "jpg", "webp", "avif"
    int quality = 90;
    int chroma = 420;                 // 420 / 444
    int avif_speed = 6;               // 4 (精细), 6 (均衡), 8 (快速)
    bool webp_lossless = false;
    int png_level = 6;
    std::string custom_out_dir = "";  // "" = 原图同目录
    bool overwrite_original = false;
    bool backup_original = true;
    bool recursive = false;
    bool gpu = true;
    int tile_size = 0;                // 0 = 自动, 512, 256, 128
    bool force = false;
    bool dark_mode = true;
    bool reduce_motion = false;

    std::string models_dir;
    std::string model_source;         // "hf-mirror", "hf", "github"
    std::vector<ModelItem> installed;
    std::vector<ModelFamily> installed_groups;
    std::vector<ModelItem> remote;
    std::vector<ModelFamily> remote_groups;
    std::string remote_error;
    std::string models_status;
    std::string downloading;
    int download_pct = -1;

    JobState job;
    HostState host;
    DaemonState daemon;

    std::string notice;
    int notice_seq = 0;
};

class AppState {
public:
    AppState();
    explicit AppState(std::string custom_prefs_path);
    ~AppState();

    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;

    // Thread-safe snapshot copy for rendering on the main thread
    Snapshot snapshot() const;

    // Drain queued main-thread tasks; returns true if state changed
    bool pollEvents();

    // Fixture states for --shot / preview
    void applyFixture(const std::string& state);

    // Commands
    void addPaths(const std::vector<std::string>& paths);
    void addFolder(const std::string& path, bool recursive);
    bool addFromClipboardText(const std::string& text);
    bool addFromClipboardRgba(int width, int height, const std::uint8_t* rgba, std::size_t bytes);
    void clearInputs();
    void removeInput(const std::string& path);
    void setActiveInput(int idx);
    void startJob();
    void cancelJob();

    void setModel(const std::string& m);
    void setFormat(const std::string& f);
    void setQuality(int q);
    void setChroma(int c);
    void setAvifSpeed(int s);
    void setTileSize(int ts);
    void setGpu(bool g);
    void setForce(bool f);
    void setOverwriteOriginal(bool ov);
    void setBackupOriginal(bool b);
    void setRecursive(bool r);
    void setCustomOutDir(const std::string& dir);
    void setDarkMode(bool dark, bool persist = true);
    void setReduceMotion(bool reduce);

    void setModelSource(const std::string& src);
    void refreshModels();
    void importModels(const std::vector<std::string>& paths);
    void downloadModel(const std::string& name);
    void removeModel(const std::string& name);
    void clearModelsStatus();

    void toggleDaemon();
    void refreshDaemonStatus();
    void setDaemonIdleSecs(int secs);

    void hostStart();
    void hostStop();
    void setHostPort(int port);
    void selectHostIp(int idx);
    void hostDecide(bool allow);
    void removeTrustedDevice(const std::string& fingerprint);
    void clearTrustedDevices();

    void setNotice(const std::string& msg);
    void clearNotice();
    void setPrefsPath(const std::string& path);

private:
    struct TrackedWorker {
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
        std::jthread thread;
    };

    void initRuntime();
    void post(std::function<void()> fn);
    void spawnWorker(std::function<void(std::shared_ptr<std::atomic<bool>> alive)> fn);
    void reapWorkers();
    void rebuildModelGroups();
    void loadPrefs();
    void savePrefs();
    std::string resolvePrefsPath() const;

    mutable std::mutex mtx_;
    Snapshot snap_;
    std::vector<std::function<void()>> queue_;
    std::atomic<bool> dirty_{true};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::uint64_t next_input_id_ = 1;
    std::string prefs_path_;

    // Background job / worker / host coordination
    std::atomic<bool> job_cancel_{false};
    std::jthread job_thread_;

    std::mutex workers_mtx_;
    std::vector<TrackedWorker> workers_;

    struct HostWorker;
    std::unique_ptr<HostWorker> host_worker_;
};

} // namespace upix::gui
