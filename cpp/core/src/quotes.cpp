#include "volforge/quotes.hpp"

#include <algorithm>

namespace volforge {

std::size_t QuoteColumns::index_at_or_before(Timestamp t) const {
    // upper_bound finds the first observation strictly after t; the one before
    // it is the most recent that had already happened. When t precedes the whole
    // series the instrument has not printed yet, which is a real state — a
    // strike can sit untouched for the first hour of a session — and is reported
    // as npos rather than clamped to the first row.
    const auto it = std::upper_bound(ts.begin(), ts.end(), t);
    if (it == ts.begin()) return npos;
    return static_cast<std::size_t>(std::distance(ts.begin(), it)) - 1;
}

}  // namespace volforge
