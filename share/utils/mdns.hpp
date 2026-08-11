#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
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

    auto start(const std::string& service_name, uint16_t port,
               const std::array<uint8_t, 32>& identity_pubkey)
        -> expected<void, MDNSError>;
    void stop();
    bool is_running() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
