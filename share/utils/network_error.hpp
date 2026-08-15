#pragma once

#include <string>
#include <cstdint>

namespace crossscale {

struct NetworkError {
    int code;
    std::string message;
};

} // namespace crossscale
