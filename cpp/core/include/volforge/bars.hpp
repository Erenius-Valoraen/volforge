// Resampled bars.
//
// A bar carries three times and they are not interchangeable:
//
//   open_time   the label. Bars are open-stamped, so the 09:20 one-minute bar
//               covers [09:20:00, 09:21:00).
//   close_time  open_time + interval. The 09:20 bar closes at 09:21:00.
//   known_at    when the bar could first have been observed == close_time.
//
// Every accessor gates on close_time, never on the label. A one-hour bar
// labelled 09:15 is not readable at 09:30, 09:45 or 10:00 — it becomes readable
// at 10:15. Indexing by label is how engines leak an hour of future into every
// decision. See docs/execution-semantics.md.

#pragma once

#include "volforge/quotes.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace volforge {

// Which quote field drives OHLC.
//
// Both answers are defensible and they differ materially on illiquid strikes:
// 82.8% of rows in the sample day carry no trade, so last-price bars on a quiet
// strike go stale while the quote moves underneath.
enum class BarPrice : std::uint8_t {
    Last,   // last traded price
    Mid,    // bid/ask midpoint — continuous, but not tradeable
};

struct Bar {
    Timestamp open_time{};    // the label
    Timestamp close_time{};   // == known_at
    Price     open, high, low, close;
    Qty       volume = 0;
    std::size_t observations = 0;

    [[nodiscard]] Timestamp known_at() const { return close_time; }
};

class BarSeries {
public:
    BarSeries() = default;

    // Resamples one instrument. `anchor` is the session open, so boundaries
    // follow the trading session rather than wall-clock arithmetic: with a 09:15
    // open, five-minute bars run 09:15-09:20, 09:20-09:25 and so on.
    static BarSeries build(const QuoteColumns& cols, std::int64_t interval_nanos,
                           Timestamp anchor, BarPrice source);

    [[nodiscard]] const std::vector<Bar>& all() const { return bars_; }
    [[nodiscard]] std::int64_t interval_nanos() const { return interval_; }

    // How many bars have closed at or before `now`. This is the causality gate:
    // a bar whose close_time is in the future does not exist yet.
    [[nodiscard]] std::size_t known_count(Timestamp now) const;

    // The most recently completed bar at `now`, stepped `back` bars earlier.
    // Never returns a bar that is still forming.
    [[nodiscard]] const Bar* completed(Timestamp now, int back = 0) const;

    // The bar currently building, aggregated **as of now** rather than as it
    // will eventually finish.
    //
    // Returning the stored bar here would be a silent leak: the stored bar was
    // aggregated from every observation in its bucket, including ones that have
    // not happened yet, so its close is the future close. This recomputes from
    // observations at or before `now`, so `close` is the running close.
    //
    // Reading it is still a decision rather than a default — an unfinished bar's
    // close is not a value anyone could have acted on at the next bar boundary.
    [[nodiscard]] std::optional<Bar> forming(Timestamp now) const;

private:
    std::vector<Bar> bars_;
    QuoteColumns     cols_;      // retained so `forming` can aggregate as-of
    Timestamp        anchor_{};
    BarPrice         source_ = BarPrice::Last;
    std::int64_t     interval_ = 0;
};

}  // namespace volforge
