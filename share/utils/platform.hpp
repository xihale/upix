#pragma once

#include <cstdlib>

namespace upix {

// User home for on-disk state (identity, trust store, models, caches).
// HOME covers Linux/macOS; USERPROFILE covers Windows, where HOME is
// typically unset for GUI-launched processes. Null when neither is set —
// callers keep their own degenerate fallback.
inline auto user_home() -> const char* {
    if (const char* home = std::getenv("HOME"); home && *home) return home;
    return std::getenv("USERPROFILE");
}

} // namespace upix
