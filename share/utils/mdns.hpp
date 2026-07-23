#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include "utils/expected.hpp"

namespace crossscale {

struct MDNSError {
    int code;
    std::string message;
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

}
