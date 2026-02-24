#include "mdns.hpp"
#include "logger.hpp"
#include <chrono>
#include <thread>

#ifdef __linux__
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace crossscale {

class MDNSBroadcaster::Impl {
public:
    bool running = false;
    std::string service_name;
    uint16_t port = 0;
#ifdef __linux__
    pid_t process_id = -1;
#endif
};

MDNSBroadcaster::MDNSBroadcaster() : impl_(std::make_unique<Impl>()) {}

MDNSBroadcaster::~MDNSBroadcaster() {
    stop();
}

auto MDNSBroadcaster::start(const std::string& service_name, uint16_t port,
                            const std::string& auth_key)
    -> expected<void, MDNSError> {

    (void)auth_key;

    if (service_name.empty() || port == 0) {
        return MDNSError{1, "Invalid mDNS publish parameters"};
    }

    if (impl_->running) {
        return {};
    }

    LOG_INFO("mDNS", "Starting mDNS broadcaster for service: " + service_name);

    impl_->service_name = service_name;
    impl_->port = port;

#ifdef __linux__
    const std::string port_string = std::to_string(port);
    pid_t child_pid = fork();
    if (child_pid < 0) {
        return MDNSError{2, "Failed to create mDNS publisher process"};
    }

    if (child_pid == 0) {
        execlp(
            "avahi-publish-service",
            "avahi-publish-service",
            service_name.c_str(),
            "_crossscale._tcp",
            port_string.c_str(),
            static_cast<char*>(nullptr)
        );
        _exit(127);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int child_status = 0;
    pid_t child_state = waitpid(child_pid, &child_status, WNOHANG);
    if (child_state == child_pid) {
        return MDNSError{3, "avahi-publish-service exited before publish became active"};
    }

    impl_->process_id = child_pid;
    impl_->running = true;
    LOG_INFO("mDNS", "Published _crossscale._tcp on port " + std::to_string(port));
#else
    LOG_WARNING("mDNS", "mDNS broadcasting not implemented on this platform");
    return MDNSError{4, "mDNS broadcaster not implemented on this platform"};
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

class MDNSDiscovery::Impl {
public:
    bool running = false;
    std::vector<DiscoveredService> services;
    std::function<void(const DiscoveredService&)> callback;
};

MDNSDiscovery::MDNSDiscovery() : impl_(std::make_unique<Impl>()) {}

MDNSDiscovery::~MDNSDiscovery() {
    stop();
}

auto MDNSDiscovery::start(std::function<void(const DiscoveredService&)> callback)
    -> expected<void, MDNSError> {

    LOG_INFO("mDNS", "Starting mDNS discovery");

    impl_->callback = callback;
    impl_->running = true;

    LOG_WARNING("mDNS", "mDNS discovery not fully implemented on this platform");

    return {};
}

void MDNSDiscovery::stop() {
    if (impl_->running) {
        LOG_INFO("mDNS", "Stopping mDNS discovery");
        impl_->running = false;
    }
}

bool MDNSDiscovery::is_running() const {
    return impl_->running;
}

auto MDNSDiscovery::get_discovered_services() const -> std::vector<DiscoveredService> {
    return impl_->services;
}

}
