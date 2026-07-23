// Local serve daemon: holds a warm ncnn engine across many client invocations
// so that `crossscale process` single-image calls skip the ~490 ms Vulkan-init
// + model-load fixed cost (see docs/PERF_REPORT_REAL_IMAGES.md §4).
//
// Architecture: the daemon owns one Engine (initialized + model-loaded once).
// It listens on a Unix domain socket. Each accepted connection: read one
// ServeRequest + image bytes, run TileProcessor::process_image_u8
// (identical path to the single-file CLI branch, so output is byte-identical),
// send ServeResponse + result bytes. Engine access is serialized by a mutex
// because Vulkan does not support concurrent submits from multiple threads
// (engine.hpp:96). Model is lazily reloaded when a request names a different
// model than the one currently loaded.
//
// Available on Linux/macOS only (AF_UNIX). On other platforms this header
// still parses but ServeDaemon is not instantiable — the CLI gates on that.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace crossscale {

class Engine;
class ModelManager;
class UnixServer;
class UnixSocket;
class TileProcessor;

enum class ServeDevice { CPU = 0, GPU = 1 };

class ServeDaemon {
public:
    ServeDaemon();
    ~ServeDaemon();

    ServeDaemon(const ServeDaemon&) = delete;
    ServeDaemon& operator=(const ServeDaemon&) = delete;

    // Initialize the engine (init + load_model) and start listening on
    // socket_path. Returns 0 on success, non-zero on error (engine init or
    // model load failure, or socket bind failure). After this returns 0 the
    // daemon is ready to accept connections; call run() to enter the accept
    // loop. models_dir locates .param/.bin files; model_name selects the
    // initial model (empty = first available).
    auto start(const std::string& socket_path,
               const std::string& models_dir,
               const std::string& model_name,
               ServeDevice device) -> int;

    // Blocking accept loop. Spawns one thread per connection. Returns when
    // stop() is called (from a signal handler or another thread) or on fatal
    // server error.
    auto run() -> void;

    // Request shutdown from any thread. Wakes the accept loop within ~500 ms.
    auto stop() -> void;

private:
    // Handle one client connection on its own thread. Takes ownership of the
    // socket; the fd is closed exactly once when sock is destroyed on return.
    auto handle_client(std::unique_ptr<UnixSocket> sock) -> void;

    // Ensure engine has `model_name` loaded; reload if it differs. Serialized
    // by engine_mutex_. Returns the param/bin resolution error if lookup fails.
    auto ensure_model_loaded(const std::string& model_name) -> int;

    std::unique_ptr<Engine> engine_;
    std::unique_ptr<ModelManager> models_;
    std::unique_ptr<UnixServer> server_;
    std::unique_ptr<TileProcessor> processor_;
    std::mutex engine_mutex_;     // serializes engine access (Vulkan constraint)
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::string socket_path_;
    std::string loaded_model_name_;
    ServeDevice device_ = ServeDevice::GPU;
};

// Default socket path: $XDG_CACHE_HOME/crossscale/serve.sock or
// $HOME/.cache/crossscale/serve.sock. Empty if neither env nor HOME is set.
auto default_serve_socket_path() -> std::string;

} // namespace crossscale
