#include "volforge/condition.hpp"

#include <charconv>
#include <stdexcept>

namespace volforge {

Cond operator|(Cond a, Cond b) {
    return Cond([a, b](const EvalCtx& c) { return a.eval(c) || b.eval(c); });
}

Cond operator&(Cond a, Cond b) {
    return Cond([a, b](const EvalCtx& c) { return a.eval(c) && b.eval(c); });
}

Cond operator!(Cond a) {
    return Cond([a](const EvalCtx& c) { return !a.eval(c); });
}

Cond at_or_after(Timestamp t) {
    // "At or after", never "equals": the feed is irregular and the loop may not
    // step exactly onto t, so an equality test would silently never fire.
    return Cond([t](const EvalCtx& c) { return c.now >= t; });
}

int parse_time_of_day(std::string_view s) {
    auto field = [&](std::size_t offset) {
        int v = 0;
        const auto* first = s.data() + offset;
        const auto res = std::from_chars(first, first + 2, v);
        if (res.ec != std::errc{}) throw std::invalid_argument("bad time of day");
        return v;
    };

    if (s.size() != 5 && s.size() != 8) {
        throw std::invalid_argument("time of day must be HH:MM or HH:MM:SS");
    }
    const int h = field(0);
    const int m = field(3);
    const int sec = s.size() == 8 ? field(6) : 0;

    if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) {
        throw std::invalid_argument("time of day out of range");
    }
    return h * 3600 + m * 60 + sec;
}

}  // namespace volforge
