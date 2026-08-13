// The data abstraction boundary.
//
// Everything above this interface — the event loop, indicators, fills, the
// strategy API — is written against these types and knows nothing about vendor
// formats, file layouts, or whether the data is a replay or a live feed.
// Switching to live trading swaps the implementations behind DataSource and
// Clock; it does not touch a strategy. See docs/design.md section 8.
//
// A note on virtual dispatch: the boundary sits at *session* granularity. A
// backtest calls load() once per day and quotes() once per instrument, then
// works on raw spans. Nothing here is called per event, so polymorphism costs
// nothing in the hot loop while buying a clean seam for live data.

#pragma once

#include "volforge/instrument.hpp"
#include "volforge/quotes.hpp"
#include "volforge/types.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace volforge {

// One observation, located. `row` indexes the instrument's QuoteColumns.
struct Event {
    Timestamp     ts;
    InstrumentId  instrument = InstrumentId::Invalid;
    std::uint32_t row        = 0;
};

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

// Current simulation time. Injected rather than global so that replay and live
// differ only in which implementation is supplied.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual Timestamp now() const = 0;
};

// A clock advanced explicitly by the event loop.
class ReplayClock final : public Clock {
public:
    [[nodiscard]] Timestamp now() const override { return now_; }

    // Monotonic by construction: time in a replay never moves backwards.
    void advance_to(Timestamp t);

private:
    Timestamp now_{};
};

// ---------------------------------------------------------------------------
// Session data
// ---------------------------------------------------------------------------

// All observations for one trading session.
//
// This is the *engine-internal* view and deliberately exposes whole columns,
// because indicator precomputation needs the full series up front. Strategy code
// never sees this type — it sees MarketView, which cannot look forward.
class SessionData {
public:
    virtual ~SessionData() = default;

    [[nodiscard]] virtual Date date() const = 0;
    [[nodiscard]] virtual const InstrumentRegistry& registry() const = 0;

    // Instruments carrying at least one observation this session.
    [[nodiscard]] virtual std::span<const InstrumentId> instruments() const = 0;

    // Columns for one instrument, ascending by time. Empty if the instrument did
    // not trade or quote this session.
    [[nodiscard]] virtual QuoteColumns quotes(InstrumentId) const = 0;

    [[nodiscard]] virtual std::size_t total_observations() const = 0;

    // A precomputed time-ordered index over every instrument, if the source has
    // one. Storage sorted by (instrument, time) compresses 3.3x better than the
    // reverse, so a store is expected to keep instrument-major order on disk and
    // may cache the merged order alongside it. Returning empty is fine and means
    // EventCursor will merge on the fly.
    [[nodiscard]] virtual std::span<const Event> event_order() const { return {}; }

    // Distinct observation times, ascending, or empty if the source does not
    // keep them.
    //
    // Stepping through a session needs only these. A source that supplies them
    // lets a replay advance through the day without decoding a single quote,
    // which is what keeps memory proportional to what a strategy touches rather
    // than to the size of the data.
    [[nodiscard]] virtual std::span<const Timestamp> timeline() const { return {}; }

    // Whether the instrument had printed by `t`.
    //
    // The default answers by looking, which costs whatever quotes() costs. A
    // file-backed source overrides it with a directory lookup, because chain
    // scans ask this of every strike and must not drag the whole day into
    // memory to do it.
    [[nodiscard]] virtual bool printed_by(InstrumentId id, Timestamp t) const {
        const QuoteColumns cols = quotes(id);
        return !cols.empty() && cols.index_at_or_before(t) != QuoteColumns::npos;
    }
};

// ---------------------------------------------------------------------------
// Data source
// ---------------------------------------------------------------------------

class DataSource {
public:
    virtual ~DataSource() = default;

    [[nodiscard]] virtual const InstrumentRegistry& registry() const = 0;
    [[nodiscard]] virtual std::vector<Date> sessions() const = 0;

    // Loads a session, or returns null if the source has no data for that date.
    //
    // Shared rather than unique ownership because the sweep pattern — hold one
    // day resident, run every configuration against it — wants the source free
    // to hand out the same decoded session repeatedly. That access pattern is
    // what makes the storage format nearly irrelevant to sweep cost.
    [[nodiscard]] virtual std::shared_ptr<SessionData> load(Date) = 0;
};

// ---------------------------------------------------------------------------
// Event iteration
// ---------------------------------------------------------------------------

// Walks a session in time order.
//
// Per-instrument runs are already sorted, so this is a k-way merge when the
// source supplies no precomputed order. Ties — and the vendor feed has many,
// since resolution is one second across ~900 instruments — are broken by
// instrument id so that a replay is deterministic and therefore reproducible.
class EventCursor {
public:
    explicit EventCursor(const SessionData& data);

    // Yields the next event, or false at end of session.
    bool next(Event& out);

    [[nodiscard]] Timestamp now() const { return now_; }
    [[nodiscard]] std::size_t emitted() const { return emitted_; }

private:
    struct Run {
        InstrumentId  instrument = InstrumentId::Invalid;
        std::uint32_t row        = 0;
        std::uint32_t end        = 0;
        Timestamp     ts{};
    };

    const SessionData*     data_;
    std::span<const Event> order_;      // non-empty when precomputed
    std::size_t            order_pos_ = 0;
    std::vector<Run>       heap_;       // min-heap by (ts, instrument)
    Timestamp              now_{};
    std::size_t            emitted_ = 0;
};

// ---------------------------------------------------------------------------
// Strategy-facing view
// ---------------------------------------------------------------------------

// What a strategy is allowed to see.
//
// This type is the structural half of the look-ahead guarantee. It holds a
// SessionData and a Clock, and every accessor resolves against the clock's
// current time. There is no method that returns a future value and no way to ask
// for one: `back` counts observations backwards, and a negative argument is a
// programming error rather than a request.
class MarketView {
public:
    MarketView(const SessionData& data, const Clock& clock);

    [[nodiscard]] Timestamp now() const { return clock_->now(); }

    // The instrument's observation `back` steps before the present, where 0 is
    // the most recent observation at or before now. Returns nullopt when the
    // instrument has not printed yet, or when history is too short.
    //
    // Note that "one step back" means one *observation*, not one second: an
    // illiquid strike may not update for a minute at a time. Series with a
    // regular grid are built on top of this by the resampling layer.
    [[nodiscard]] std::optional<Quote> quote(InstrumentId id, int back = 0) const;

    // True when the instrument has printed at least once at or before now.
    [[nodiscard]] bool has_quote(InstrumentId id) const;

    [[nodiscard]] const InstrumentRegistry& registry() const { return data_->registry(); }

private:
    const SessionData* data_;
    const Clock*       clock_;
};

}  // namespace volforge
