// Dumps what a .vfday actually contains, and flags anything that does not
// belong to the session it claims.

#include "volforge/vfday.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace volforge;

namespace {

std::string clock_time(Timestamp ts) {
    const std::int64_t local = ts.seconds() + kISTOffsetSeconds;
    const std::int64_t day = (local >= 0 ? local : local - 86399) / 86400;
    const std::int64_t sod = ((local % 86400) + 86400) % 86400;
    const Date d = date_from_days(day);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld", d.to_string().c_str(),
                  sod / 3600, (sod % 3600) / 60, sod % 60);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: inspect_day <file.vfday>\n"); return 2; }

    InstrumentRegistry registry;
    auto s = VfdaySession::open(argv[1], registry);

    std::printf("date          %s\n", s->date().to_string().c_str());
    std::printf("instruments   %zu\n", s->instruments().size());
    std::printf("rows          %zu\n", s->total_observations());

    const auto tl = s->timeline();
    std::printf("timeline      %zu entries\n", tl.size());
    if (!tl.empty()) {
        std::printf("  first       %s\n", clock_time(tl.front()).c_str());
        std::printf("  last        %s\n", clock_time(tl.back()).c_str());
    }

    // Anything outside the claimed session is what corrupts a replay.
    const Timestamp lo = timestamp_of(s->date(), 0, kISTOffsetSeconds);
    const Timestamp hi = timestamp_of(s->date(), 24 * 3600, kISTOffsetSeconds);

    std::size_t stray = 0;
    for (const Timestamp t : tl) {
        if (t < lo || t >= hi) {
            if (stray < 8) std::printf("  STRAY       %s\n", clock_time(t).c_str());
            ++stray;
        }
    }
    std::printf("stray times   %zu\n", stray);

    if (stray != 0) {
        std::printf("\ninstruments carrying them:\n");
        std::size_t shown = 0;
        for (const InstrumentId id : s->instruments()) {
            const auto c = s->quotes(id);
            if (c.empty()) continue;
            if (c.ts.front() >= lo && c.ts.back() < hi) continue;
            const InstrumentSpec& spec = registry.spec(id);
            std::printf("  %s %.0f %s   %s .. %s  (%zu rows)\n",
                        std::string(registry.underlying_name(spec.underlying)).c_str(),
                        spec.strike.to_double(), to_string(spec.right),
                        clock_time(c.ts.front()).c_str(), clock_time(c.ts.back()).c_str(),
                        c.size());
            if (++shown >= 10) break;
        }
    }
    return 0;
}
