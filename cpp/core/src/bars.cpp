#include "volforge/bars.hpp"

#include <algorithm>
#include <stdexcept>

namespace volforge {

BarSeries BarSeries::build(const QuoteColumns& cols, std::int64_t interval_nanos,
                           Timestamp anchor, BarPrice source, Timestamp valid_until) {
    if (interval_nanos <= 0) throw std::invalid_argument("BarSeries::build: interval must be > 0");

    BarSeries out;
    out.interval_ = interval_nanos;
    out.cols_     = cols;
    out.anchor_   = anchor;
    out.source_   = source;
    out.valid_until_ = valid_until;
    if (cols.empty()) return out;

    auto price_of = [&](std::size_t i) {
        if (source == BarPrice::Mid) {
            const Quote q = cols.at(i);
            return q.two_sided() ? q.mid() : q.last;
        }
        return cols.last[i];
    };

    // Bucket index relative to the session anchor, so boundaries land on the
    // session grid rather than on absolute epoch arithmetic.
    auto bucket_of = [&](Timestamp t) {
        const std::int64_t delta = t.nanos - anchor.nanos;
        // Floor division, correct for observations before the anchor.
        return delta >= 0 ? delta / interval_nanos
                          : -(((-delta) + interval_nanos - 1) / interval_nanos);
    };

    std::int64_t current = 0;
    bool         open_bar = false;

    for (std::size_t i = 0; i < cols.size(); ++i) {
        const Price px = price_of(i);
        if (px.minor <= 0) continue;   // no usable print yet

        const std::int64_t b = bucket_of(cols.ts[i]);
        if (!open_bar || b != current) {
            Bar bar;
            bar.open_time  = Timestamp{anchor.nanos + b * interval_nanos};
            bar.close_time = Timestamp{bar.open_time.nanos + interval_nanos};
            bar.open = bar.high = bar.low = bar.close = px;
            bar.volume = cols.last_qty[i];
            bar.observations = 1;
            out.bars_.push_back(bar);
            current  = b;
            open_bar = true;
            continue;
        }

        Bar& bar = out.bars_.back();
        bar.high = Price::from_minor(std::max(bar.high.minor, px.minor));
        bar.low  = Price::from_minor(std::min(bar.low.minor, px.minor));
        bar.close = px;
        bar.volume += cols.last_qty[i];
        ++bar.observations;
    }

    return out;
}

std::size_t BarSeries::known_count(Timestamp now) const {
    if (valid_until_.nanos != 0 && now > valid_until_) {
        throw std::logic_error(
            "bar series belongs to an earlier session; re-acquire it for the current one");
    }
    // Bars are ordered by close_time, so this is the count of bars that have
    // finished. A bar closing exactly at `now` counts — it is complete.
    const auto it = std::upper_bound(
        bars_.begin(), bars_.end(), now,
        [](Timestamp t, const Bar& b) { return t < b.close_time; });
    return static_cast<std::size_t>(std::distance(bars_.begin(), it));
}

const Bar* BarSeries::completed(Timestamp now, int back) const {
    if (back < 0) throw std::invalid_argument("BarSeries::completed: negative offset reads forward");
    const std::size_t k = known_count(now);
    if (k == 0) return nullptr;
    const auto steps = static_cast<std::size_t>(back);
    if (steps >= k) return nullptr;
    return &bars_[k - 1 - steps];
}

std::optional<Bar> BarSeries::forming(Timestamp now) const {
    const std::size_t k = known_count(now);
    if (k >= bars_.size()) return std::nullopt;

    const Bar& stored = bars_[k];
    if (now < stored.open_time) return std::nullopt;   // has not started

    // Re-aggregate from observations at or before `now`. Returning `stored`
    // would hand back a bar built from the whole bucket — including
    // observations still in the future — which is precisely the leak this
    // accessor is supposed to make visible rather than hide.
    auto price_of = [&](std::size_t i) {
        if (source_ == BarPrice::Mid) {
            const Quote q = cols_.at(i);
            return q.two_sided() ? q.mid() : q.last;
        }
        return cols_.last[i];
    };

    const std::size_t upto = cols_.index_at_or_before(now);
    if (upto == QuoteColumns::npos) return std::nullopt;

    Bar bar;
    bar.open_time  = stored.open_time;
    bar.close_time = stored.close_time;
    bool started = false;

    for (std::size_t i = 0; i <= upto; ++i) {
        if (cols_.ts[i] < stored.open_time) continue;
        const Price px = price_of(i);
        if (px.minor <= 0) continue;

        if (!started) {
            bar.open = bar.high = bar.low = bar.close = px;
            started = true;
        } else {
            bar.high = Price::from_minor(std::max(bar.high.minor, px.minor));
            bar.low  = Price::from_minor(std::min(bar.low.minor, px.minor));
            bar.close = px;
        }
        bar.volume += cols_.last_qty[i];
        ++bar.observations;
    }

    if (!started) return std::nullopt;
    return bar;
}

}  // namespace volforge
