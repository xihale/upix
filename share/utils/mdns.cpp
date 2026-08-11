#include "mdns.hpp"
#include "logger.hpp"
#include "core/distributed/identity.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#ifdef __linux__
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace crossscale {

namespace {

#ifdef __linux__

constexpr auto AVAHI_DAEMON_SOCKET_PATH = "/run/avahi-daemon/socket";
constexpr auto AVAHI_DAEMON_SOCKET_PATH_ALT = "/var/run/avahi-daemon/socket";

auto is_avahi_socket_available() -> bool {
    std::error_code first_error;
    if (std::filesystem::exists(AVAHI_DAEMON_SOCKET_PATH, first_error) && !first_error) {
        return true;
    }

    std::error_code second_error;
    if (std::filesystem::exists(AVAHI_DAEMON_SOCKET_PATH_ALT, second_error) && !second_error) {
        return true;
    }

    return false;
}

auto summarize_publish_failure(int child_status) -> std::string {
    if (WIFEXITED(child_status)) {
        const int exit_code = WEXITSTATUS(child_status);
        if (exit_code == 127) {
            return "avahi-publish-service is not installed or not in PATH";
        }

        if (exit_code == 1) {
            if (!is_avahi_socket_available()) {
                return "avahi-publish-service failed because avahi-daemon is not running";
            }
            return "avahi-publish-service exited with status 1";
        }

        return "avahi-publish-service exited with status " + std::to_string(exit_code);
    }

    if (WIFSIGNALED(child_status)) {
        return "avahi-publish-service terminated by signal " + std::to_string(WTERMSIG(child_status));
    }

    return "avahi-publish-service exited before publish became active";
}

#endif

}

class MDNSBroadcaster::Impl {
public:
    bool running = false;
    std::string service_name;
    uint16_t port = 0;
    std::array<uint8_t, 32> identity_pubkey{};
#ifdef __linux__
    pid_t process_id = -1;
#endif
};

MDNSBroadcaster::MDNSBroadcaster() : impl_(std::make_unique<Impl>()) {}

MDNSBroadcaster::~MDNSBroadcaster() {
    stop();
}

auto MDNSBroadcaster::start(const std::string& service_name, uint16_t port,
                            const std::array<uint8_t, 32>& identity_pubkey)
    -> expected<void, MDNSError> {

    if (service_name.empty() || port == 0) {
        return std::unexpected(MDNSError{1, "Invalid mDNS publish parameters"});
    }

    if (impl_->running) {
        if (impl_->service_name == service_name &&
            impl_->port == port &&
            impl_->identity_pubkey == identity_pubkey) {
            return {};
        }

        stop();
    }

    LOG_INFO("mDNS", "Starting mDNS broadcaster for service: " + service_name);

    impl_->service_name = service_name;
    impl_->port = port;
    impl_->identity_pubkey = identity_pubkey;

#ifdef __linux__
    if (!is_avahi_socket_available()) {
        return std::unexpected(MDNSError{5, "mDNS publish requires avahi-daemon, but daemon socket was not found"});
    }

    const std::string port_string = std::to_string(port);
    const std::string version_txt = "version=2";
    const std::string role_txt = "role=coordinator";
    const std::string fp_txt = "id_fingerprint=" + fingerprint(identity_pubkey);
    const std::string pk_txt = "id_pubkey=" + base64_encode(identity_pubkey.data(), 32);

    pid_t child_pid = fork();
    if (child_pid < 0) {
        return std::unexpected(MDNSError{2, "Failed to create mDNS publisher process"});
    }

    if (child_pid == 0) {
        execlp(
            "avahi-publish-service",
            "avahi-publish-service",
            "-s",
            service_name.c_str(),
            "_crossscale._tcp",
            port_string.c_str(),
            version_txt.c_str(),
            role_txt.c_str(),
            fp_txt.c_str(),
            pk_txt.c_str(),
            static_cast<char*>(nullptr)
        );
        _exit(127);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int child_status = 0;
    pid_t child_state = waitpid(child_pid, &child_status, WNOHANG);
    if (child_state < 0) {
        return std::unexpected(MDNSError{6, "Failed to inspect mDNS publisher process: " + std::string(std::strerror(errno))});
    }

    if (child_state == child_pid) {
        return std::unexpected(MDNSError{3, summarize_publish_failure(child_status)});
    }

    impl_->process_id = child_pid;
    impl_->running = true;
    LOG_INFO("mDNS", "Published _crossscale._tcp on port " + std::to_string(port));
#else
    LOG_WARNING("mDNS", "mDNS broadcasting not implemented on this platform");
    return std::unexpected(MDNSError{4, "mDNS broadcaster not implemented on this platform"});
#endif

    return {};
}

void MDNSBroadcaster::stop() {
    if (impl_->running) {
        LOG_INFO("mDNS", "Stopping mDNS broadcaster");

#ifdef __linux__
        if (impl_->process_id > 0) {
            kill(impl_->process_id, SIGTERM);
            waitpid(impl_->process_id, nullptr, 0);
            impl_->process_id = -1;
        }
#endif

        impl_->running = false;
    }
}

bool MDNSBroadcaster::is_running() const {
#ifdef __linux__
    if (!impl_->running || impl_->process_id <= 0) {
        return false;
    }
    return kill(impl_->process_id, 0) == 0;
#else
    return impl_->running;
#endif
}

}
