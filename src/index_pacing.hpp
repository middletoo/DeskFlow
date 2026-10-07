#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace desk::search_detail {
// Budget sustained background CPU against the whole machine, with a quarter
// of one core ceiling. Large indivisible OS/SQLite calls can exceed a slice.
inline unsigned backgroundDelayMs(std::uint64_t cpuTicks, std::uint64_t elapsedTicks,
                                  unsigned logicalProcessors) {
    const double fraction = std::min(0.25, 0.005 * std::max(1u, logicalProcessors));
    const double missing = cpuTicks / fraction - elapsedTicks;
    if (missing <= 0) return 0;
    return static_cast<unsigned>(std::min(3000.0, std::ceil(missing / 10000.0)));
}
}
