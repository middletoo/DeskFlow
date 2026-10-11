#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace desk::search_detail {
// Initial construction may use one core. Daily maintenance remains capped at
// 0.5% of the machine and a quarter core. OS/SQLite calls can exceed a slice.
inline unsigned backgroundDelayMs(std::uint64_t cpuTicks, std::uint64_t elapsedTicks,
                                  unsigned logicalProcessors, bool initialMft = false) {
    const double fraction = initialMft ? std::min(1.0, 0.08 * std::max(1u, logicalProcessors))
                                      : std::min(0.25, 0.005 * std::max(1u, logicalProcessors));
    const double missing = cpuTicks / fraction - elapsedTicks;
    if (missing <= 0) return 0;
    return static_cast<unsigned>(std::min(3000.0, std::ceil(missing / 10000.0)));
}
}
