// Market observations.
//
// The vendor feed is a snapshot stream rather than a trade stream: most records
// carry no trade at all (82.8% of the sample day), reporting only that the book
// moved. Both facts matter to a fill model, so the type keeps them distinct
// instead of collapsing everything into "a tick".

#pragma once

#include "volforge/types.hpp"

#include <cstddef>
#include <span>

namespace volforge {

// One observation of one instrument.
struct Quote {
    Timestamp ts;

    Price last;      // last traded price; stale when last_qty has been 0 for a while
    Price bid;
    Price ask;

    Qty bid_qty  = 0;
    Qty ask_qty  = 0;
    Qty last_qty = 0;   // 0 means "quote update, no trade in this interval"

    std::int64_t open_interest = 0;

    [[nodiscard]] constexpr bool traded() const { return last_qty > 0; }
    [[nodiscard]] constexpr bool two_sided() const { return bid.minor > 0 && ask.minor > 0; }
    [[nodiscard]] constexpr Price spread() const { return ask - bid; }

    // Midpoint, rounded toward zero. Continuous, but not a price anyone can
    // trade at — see the fill model before using it for anything but analytics.
    [[nodiscard]] constexpr Price mid() const {
        return Price::from_minor((bid.minor + ask.minor) / 2);
    }
};

// A whole session of one instrument, column-oriented.
//
// Columns rather than an array of Quote because the engine's hot paths are
// vectorized passes over one or two fields — indicator precomputation, stop
// evaluation — and pulling eight fields through cache to read one is the
// difference the storage benchmarks measured.
//
// All spans have equal length and are sorted ascending by ts.
struct QuoteColumns {
    std::span<const Timestamp>    ts;
    std::span<const Price>        last;
    std::span<const Price>        bid;
    std::span<const Price>        ask;
    std::span<const Qty>          bid_qty;
    std::span<const Qty>          ask_qty;
    std::span<const Qty>          last_qty;
    std::span<const std::int64_t> open_interest;

    [[nodiscard]] std::size_t size() const { return ts.size(); }
    [[nodiscard]] bool empty() const { return ts.empty(); }

    [[nodiscard]] Quote at(std::size_t i) const {
        return Quote{ts[i], last[i], bid[i], ask[i],
                     bid_qty[i], ask_qty[i], last_qty[i], open_interest[i]};
    }

    // Index of the last observation at or before `t`, or npos if none exists.
    //
    // This is the only sanctioned way to locate a point in time, and it never
    // looks forward: an instrument that has not printed since 09:20 reports its
    // 09:20 quote at 09:47, rather than borrowing the next one.
    [[nodiscard]] std::size_t index_at_or_before(Timestamp t) const;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
};

}  // namespace volforge
