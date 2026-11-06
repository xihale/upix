#pragma once

#include <expected>
#include <string>

namespace upix {

// Project alias for std::expected. Kept as a thin header so existing call
// sites read `upix::expected<T, E>`; the type is exactly std::expected.
template <typename T, typename E>
using expected = std::expected<T, E>;

} // namespace upix
