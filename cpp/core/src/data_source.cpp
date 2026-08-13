#include "volforge/data_source.hpp"

#include <algorithm>
#include <stdexcept>

namespace volforge {

// ---------------------------------------------------------------------------
// ReplayClock
// ---------------------------------------------------------------------------

void ReplayClock::advance_to(Timestamp t) {
    if (t < now_) {
        // Time running backwards would silently invalidate every look-ahead
        // guarantee built on top of the clock, so it is an error rather than a
        // clamp.
        throw std::logic_error("ReplayClock::advance_to moved backwards");
    }
    now_ = t;
}

// ---------------------------------------------------------------------------
// EventCursor
// ---------------------------------------------------------------------------

namespace {

// Ordering for the merge heap. std::*_heap builds a max-heap, so "greater"
// yields the min-heap we want.
//
// The instrument tiebreak is not cosmetic: the feed is 1-second resolution
// across ~900 instruments, so simultaneous events are the common case rather
// than an edge case. Without a total order, replay order would depend on heap
// internals and results would not reproduce.
struct RunGreater {
    bool operator()(const auto& a, const auto& b) const {
        if (a.ts != b.ts) return b.ts < a.ts;
        return index_of(b.instrument) < index_of(a.instrument);
    }
};

}  // namespace

EventCursor::EventCursor(const SessionData& data)
    : data_(&data), order_(data.event_order()) {
    if (!order_.empty()) return;   // nothing to merge, just walk the index

    const auto instruments = data.instruments();
    heap_.reserve(instruments.size());
    for (const InstrumentId id : instruments) {
        const QuoteColumns cols = data.quotes(id);
        if (cols.empty()) continue;
        heap_.push_back(Run{id, 0, static_cast<std::uint32_t>(cols.size()), cols.ts[0]});
    }
    std::make_heap(heap_.begin(), heap_.end(), RunGreater{});
}

bool EventCursor::next(Event& out) {
    if (!order_.empty()) {
        if (order_pos_ >= order_.size()) return false;
        out  = order_[order_pos_++];
        now_ = out.ts;
        ++emitted_;
        return true;
    }

    if (heap_.empty()) return false;

    std::pop_heap(heap_.begin(), heap_.end(), RunGreater{});
    Run& run = heap_.back();

    out = Event{run.ts, run.instrument, run.row};
    now_ = run.ts;
    ++emitted_;

    if (++run.row < run.end) {
        run.ts = data_->quotes(run.instrument).ts[run.row];
        std::push_heap(heap_.begin(), heap_.end(), RunGreater{});
    } else {
        heap_.pop_back();
    }
    return true;
}

// ---------------------------------------------------------------------------
// MarketView
// ---------------------------------------------------------------------------

MarketView::MarketView(const SessionData& data, const Clock& clock)
    : data_(&data), clock_(&clock) {}

std::optional<Quote> MarketView::quote(InstrumentId id, int back) const {
    if (back < 0) {
        // close[-1] is a request for the future. There is no correct value to
        // return, so this is loud rather than lenient.
        throw std::invalid_argument("MarketView::quote: negative offset reads the future");
    }

    const QuoteColumns cols = data_->quotes(id);
    if (cols.empty()) return std::nullopt;

    const std::size_t idx = cols.index_at_or_before(clock_->now());
    if (idx == QuoteColumns::npos) return std::nullopt;      // has not printed yet

    const auto steps = static_cast<std::size_t>(back);
    if (steps > idx) return std::nullopt;                    // history too short

    return cols.at(idx - steps);
}

bool MarketView::has_quote(InstrumentId id) const {
    // Routed through the session so a file-backed source can answer from its
    // directory instead of decoding the instrument.
    return data_->printed_by(id, clock_->now());
}

}  // namespace volforge
