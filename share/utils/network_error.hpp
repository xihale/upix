#pragma once

#include <string>
#include <cstdint>

namespace upix {

struct NetworkError {
    int code;
    std::string message;
};

} // namespace upix
