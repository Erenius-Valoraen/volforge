// An in-memory DataSource.
//
// This is not only test scaffolding. Every vendor adapter decodes into these
// structures, so the storage layer's job — whenever its format is settled — is
// to populate a MemorySessionData (or expose the same spans over mapped memory).
// Keeping the interface honest against a trivial implementation first is what
// prevents the vendor format from leaking upward into the engine.

#pragma once

#include "volforge/data_source.hpp"

#include <map>
#include <memory>
#include <vector>

namespace volforge {

class MemorySessionData final : public SessionData {
public:
    MemorySessionData(Date date, const InstrumentRegistry& registry);

    // Appends one observation. Must be non-decreasing in time per instrument;
    // out-of-order appends throw, because silently sorting would hide a broken
    // adapter until it produced a subtly wrong backtest.
    void append(InstrumentId id, const Quote& q);

    // Builds the merged time order once, so repeated runs over the same session
    // — the sweep case — do not each pay for a k-way merge.
    void build_event_order();

    [[nodiscard]] Date date() const override { return date_; }
    [[nodiscard]] const InstrumentRegistry& registry() const override { return *registry_; }
    [[nodiscard]] std::span<const InstrumentId> instruments() const override { return instruments_; }
    [[nodiscard]] QuoteColumns quotes(InstrumentId) const override;
    [[nodiscard]] std::size_t total_observations() const override { return total_; }
    [[nodiscard]] std::span<const Event> event_order() const override { return order_; }
    [[nodiscard]] std::span<const Timestamp> timeline() const override { return timeline_; }

private:
    struct Series {
        std::vector<Timestamp>    ts;
        std::vector<Price>        last, bid, ask;
        std::vector<Qty>          bid_qty, ask_qty, last_qty;
        std::vector<std::int64_t> open_interest;
    };

    Series& series_for(InstrumentId id);

    Date                                 date_;
    const InstrumentRegistry*            registry_;
    std::map<std::int32_t, Series>       series_;      // ordered, so ids stay deterministic
    std::vector<InstrumentId>            instruments_;
    std::vector<Event>                   order_;
    std::vector<Timestamp>               timeline_;
    std::size_t                          total_ = 0;
};

class MemoryDataSource final : public DataSource {
public:
    explicit MemoryDataSource(InstrumentRegistry registry);

    // Takes ownership of a prepared session.
    void add_session(std::shared_ptr<MemorySessionData> session);

    [[nodiscard]] const InstrumentRegistry& registry() const override { return registry_; }
    [[nodiscard]] std::vector<Date> sessions() const override;
    [[nodiscard]] std::shared_ptr<SessionData> load(Date) override;

    [[nodiscard]] InstrumentRegistry& mutable_registry() { return registry_; }

private:
    InstrumentRegistry                                          registry_;
    std::map<std::int32_t, std::shared_ptr<MemorySessionData>>  sessions_;
};

}  // namespace volforge
