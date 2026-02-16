#pragma once

#include <string>
#include <functional>
#include <vector>
#include <cstdint>
#include <memory>
#include "utils/expected.hpp"

namespace crossscale {

struct MDNSError {
    int code;
    std::string message;
};

struct DiscoveredService {
    std::string name;
    std::string host;
    uint16_t port;
    std::string auth_key_hash;
};

class MDNSBroadcaster {
public:
    MDNSBroadcaster();
    ~MDNSBroadcaster();

    auto start(const std::string& service_name, uint16_t port, const std::string& auth_key)
        -> expected<void, MDNSError>;
    void stop();
    bool is_running() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class MDNSDiscovery {
public:
    MDNSDiscovery();
    ~MDNSDiscovery();

    auto start(std::function<void(const DiscoveredService&)> callback)
        -> expected<void, MDNSError>;
    void stop();
    bool is_running() const;

    auto get_discovered_services() const -> std::vector<DiscoveredService>;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
