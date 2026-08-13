#include "harness.hpp"

#include "volforge/data_source.hpp"
#include "volforge/memory_source.hpp"
#include "volforge/synthetic.hpp"

#include <cstddef>
#include <limits>

using namespace volforge;

namespace {

// Hides a session's precomputed order so the k-way merge path is exercised.
// The two must agree exactly, or a sweep would replay a day differently
// depending on whether the store happened to cache an index.
class NoOrderSession final : public SessionData {
public:
    explicit NoOrderSession(const SessionData& inner) : inner_(&inner) {}

    [[nodiscard]] Date date() const override { return inner_->date(); }
    [[nodiscard]] const InstrumentRegistry& registry() const override { return inner_->registry(); }
    [[nodiscard]] std::span<const InstrumentId> instruments() const override {
        return inner_->instruments();
    }
    [[nodiscard]] QuoteColumns quotes(InstrumentId id) const override { return inner_->quotes(id); }
    [[nodiscard]] std::size_t total_observations() const override {
        return inner_->total_observations();
    }
    // event_order() intentionally left at the base implementation (empty).

private:
    const SessionData* inner_;
};

struct Fixture {
    InstrumentRegistry registry;
    SyntheticSession   session;

    Fixture() { session = make_synthetic_session(registry); }
};

}  // namespace

// ---------------------------------------------------------------------------
// Instrument registry
// ---------------------------------------------------------------------------

TEST(registry_deduplicates_identical_instruments) {
    InstrumentRegistry reg;
    const auto nifty = reg.intern_underlying("NIFTY");

    InstrumentSpec s;
    s.underlying = nifty;
    s.kind       = InstrumentKind::Option;
    s.expiry     = Date{20250703};
    s.strike     = Price::from_double(25000);
    s.right      = Right::Call;

    const auto a = reg.add(s);
    const auto b = reg.add(s);
    CHECK(a == b);
    CHECK_EQ(reg.size(), std::size_t{1});

    s.right = Right::Put;
    CHECK(reg.add(s) != a);
    CHECK_EQ(reg.size(), std::size_t{2});
}

TEST(registry_finds_options_by_identity_not_name) {
    InstrumentRegistry reg;
    const auto nifty = reg.intern_underlying("NIFTY");

    InstrumentSpec s;
    s.underlying = nifty;
    s.kind       = InstrumentKind::Option;
    s.expiry     = Date{20250703};
    s.strike     = Price::from_double(25000);
    s.right      = Right::Call;
    const auto id = reg.add(s);

    const auto found = reg.find_option(nifty, Date{20250703}, Price::from_double(25000), Right::Call);
    CHECK(found.has_value());
    CHECK(*found == id);

    CHECK(!reg.find_option(nifty, Date{20250703}, Price::from_double(25050), Right::Call).has_value());
    CHECK(!reg.find_option(nifty, Date{20250710}, Price::from_double(25000), Right::Call).has_value());
}

TEST(registry_reports_expiries_ascending_and_unique) {
    InstrumentRegistry reg;
    const auto nifty = reg.intern_underlying("NIFTY");

    for (const std::int32_t e : {20250710, 20250703, 20250703, 20250717}) {
        InstrumentSpec s;
        s.underlying = nifty;
        s.kind       = InstrumentKind::Option;
        s.expiry     = Date{e};
        s.strike     = Price::from_double(25000);
        s.right      = Right::Call;
        reg.add(s);
    }

    const auto ex = reg.expiries(nifty);
    CHECK_EQ(ex.size(), std::size_t{3});
    CHECK(ex[0] == Date{20250703});
    CHECK(ex[1] == Date{20250710});
    CHECK(ex[2] == Date{20250717});
}

// ---------------------------------------------------------------------------
// Point-in-time lookup
// ---------------------------------------------------------------------------

TEST(index_at_or_before_never_looks_forward) {
    const Timestamp ts[] = {Timestamp::from_seconds(100), Timestamp::from_seconds(200),
                            Timestamp::from_seconds(300)};
    const Price prices[] = {Price::from_minor(1), Price::from_minor(2), Price::from_minor(3)};
    const Qty  qtys[]    = {0, 0, 0};
    const std::int64_t oi[] = {0, 0, 0};

    QuoteColumns c{ts, prices, prices, prices, qtys, qtys, qtys, oi};

    // Before the first observation the instrument simply has no price yet.
    CHECK(c.index_at_or_before(Timestamp::from_seconds(99)) == QuoteColumns::npos);

    CHECK_EQ(c.index_at_or_before(Timestamp::from_seconds(100)), std::size_t{0});

    // Between observations, the older one still stands — the next has not
    // happened yet and borrowing it would be look-ahead.
    CHECK_EQ(c.index_at_or_before(Timestamp::from_seconds(299)), std::size_t{1});

    CHECK_EQ(c.index_at_or_before(Timestamp::from_seconds(300)), std::size_t{2});
    CHECK_EQ(c.index_at_or_before(Timestamp::from_seconds(10'000)), std::size_t{2});
}

// ---------------------------------------------------------------------------
// MarketView: the structural look-ahead guarantee
// ---------------------------------------------------------------------------

TEST(market_view_rejects_negative_offsets) {
    Fixture f;
    ReplayClock clock;
    clock.advance_to(timestamp_of(Date{20250701}, 10 * 3600, kISTOffsetSeconds));
    MarketView view(*f.session.data, clock);

    CHECK_THROWS(view.quote(f.session.spot, -1));
}

TEST(market_view_reports_nothing_before_an_instrument_prints) {
    Fixture f;
    ReplayClock clock;

    // One second into the session: the spot has printed, distant wings have not.
    clock.advance_to(timestamp_of(Date{20250701}, 9 * 3600 + 15 * 60, kISTOffsetSeconds));
    MarketView view(*f.session.data, clock);

    CHECK(view.has_quote(f.session.spot));

    int silent = 0;
    for (const InstrumentId id : f.session.data->instruments()) {
        if (!view.has_quote(id)) ++silent;
    }
    CHECK(silent > 0);   // wings start late, by construction
}

TEST(market_view_walks_history_backwards) {
    Fixture f;
    ReplayClock clock;
    clock.advance_to(timestamp_of(Date{20250701}, 12 * 3600, kISTOffsetSeconds));
    MarketView view(*f.session.data, clock);

    const auto now  = view.quote(f.session.spot, 0);
    const auto prev = view.quote(f.session.spot, 1);
    CHECK(now.has_value());
    CHECK(prev.has_value());
    CHECK(prev->ts < now->ts);
    CHECK(now->ts <= clock.now());
}

TEST(market_view_holds_a_stale_quote_rather_than_borrowing_the_next) {
    Fixture f;

    // Find an instrument with a gap, then read inside that gap. The correct
    // answer is the older observation, not the one that has not happened yet.
    for (const InstrumentId id : f.session.data->instruments()) {
        const auto cols = f.session.data->quotes(id);
        if (cols.size() < 3) continue;

        for (std::size_t i = 1; i + 1 < cols.size(); ++i) {
            const auto gap = cols.ts[i + 1].nanos - cols.ts[i].nanos;
            if (gap < 3 * 1'000'000'000LL) continue;

            ReplayClock clock;
            clock.advance_to(Timestamp{cols.ts[i].nanos + gap / 2});
            MarketView view(*f.session.data, clock);

            const auto q = view.quote(id);
            CHECK(q.has_value());
            CHECK(q->ts == cols.ts[i]);            // the stale one
            CHECK(q->ts != cols.ts[i + 1]);        // never the future one
            return;
        }
    }
    CHECK(false);   // the generator is supposed to produce gaps
}

TEST(replay_clock_refuses_to_move_backwards) {
    ReplayClock clock;
    clock.advance_to(Timestamp::from_seconds(1000));
    clock.advance_to(Timestamp::from_seconds(1000));   // idempotent is fine
    CHECK_THROWS(clock.advance_to(Timestamp::from_seconds(999)));
}

// ---------------------------------------------------------------------------
// Event iteration
// ---------------------------------------------------------------------------

TEST(cursor_emits_every_observation_in_time_order) {
    Fixture f;
    EventCursor cursor(*f.session.data);

    Event ev;
    std::size_t count = 0;
    Timestamp   last{std::numeric_limits<std::int64_t>::min()};

    while (cursor.next(ev)) {
        CHECK(ev.ts >= last);
        last = ev.ts;
        ++count;
    }
    CHECK_EQ(count, f.session.data->total_observations());
}

TEST(cursor_events_point_at_the_right_row) {
    Fixture f;
    EventCursor cursor(*f.session.data);

    Event ev;
    int checked = 0;
    while (cursor.next(ev) && checked < 5000) {
        const auto cols = f.session.data->quotes(ev.instrument);
        CHECK(ev.row < cols.size());
        CHECK(cols.ts[ev.row] == ev.ts);
        ++checked;
    }
    CHECK(checked > 0);
}

TEST(merged_and_precomputed_orders_agree_exactly) {
    Fixture f;
    NoOrderSession merged_view(*f.session.data);

    EventCursor precomputed(*f.session.data);
    EventCursor merged(merged_view);

    CHECK(!f.session.data->event_order().empty());   // precondition for the comparison

    Event a, b;
    std::size_t n = 0;
    while (true) {
        const bool ha = precomputed.next(a);
        const bool hb = merged.next(b);
        CHECK_EQ(ha, hb);
        if (!ha) break;
        CHECK(a.ts == b.ts);
        CHECK(a.instrument == b.instrument);
        CHECK(a.row == b.row);
        ++n;
    }
    CHECK_EQ(n, f.session.data->total_observations());
}

TEST(replay_is_deterministic_across_runs) {
    InstrumentRegistry r1, r2;
    const auto s1 = make_synthetic_session(r1);
    const auto s2 = make_synthetic_session(r2);

    CHECK_EQ(s1.data->total_observations(), s2.data->total_observations());

    EventCursor c1(*s1.data), c2(*s2.data);
    Event a, b;
    while (c1.next(a)) {
        CHECK(c2.next(b));
        CHECK(a.ts == b.ts);
        CHECK(a.instrument == b.instrument);
        CHECK(a.row == b.row);
    }
    CHECK(!c2.next(b));
}

// ---------------------------------------------------------------------------
// The generator reproduces the awkward parts of the real feed
// ---------------------------------------------------------------------------

TEST(synthetic_data_mirrors_the_measured_feed) {
    Fixture f;

    std::size_t option_rows = 0, no_trade = 0, wide = 0;
    for (const InstrumentId id : f.session.data->instruments()) {
        if (!f.session.data->registry().spec(id).is_option()) continue;
        const auto cols = f.session.data->quotes(id);
        for (std::size_t i = 0; i < cols.size(); ++i) {
            ++option_rows;
            if (cols.last_qty[i] == 0) ++no_trade;
            const auto q = cols.at(i);
            if (q.spread().to_double() > 0.02 * q.mid().to_double()) ++wide;
        }
    }

    CHECK(option_rows > 100'000);

    // The sample day is 82.8% quote-only; anywhere near that exercises the same
    // code paths a fill model will meet.
    const double quiet = static_cast<double>(no_trade) / static_cast<double>(option_rows);
    CHECK(quiet > 0.70 && quiet < 0.95);

    // Some strikes must be wide enough that filling at mid would be fiction.
    CHECK(wide > 0);
}

TEST(session_data_round_trips_through_the_source_interface) {
    InstrumentRegistry registry;
    auto synth = make_synthetic_session(registry);

    MemoryDataSource source(registry);
    source.add_session(synth.data);

    const auto dates = source.sessions();
    CHECK_EQ(dates.size(), std::size_t{1});
    CHECK(dates[0] == Date{20250701});

    const auto loaded = source.load(Date{20250701});
    CHECK(loaded != nullptr);
    CHECK_EQ(loaded->total_observations(), synth.data->total_observations());

    CHECK(source.load(Date{20250702}) == nullptr);
}

TEST(memory_session_rejects_out_of_order_appends) {
    InstrumentRegistry reg;
    const auto nifty = reg.intern_underlying("NIFTY");
    InstrumentSpec s;
    s.underlying = nifty;
    s.kind       = InstrumentKind::Spot;
    const auto id = reg.add(s);

    MemorySessionData session(Date{20250701}, reg);

    Quote q;
    q.ts = Timestamp::from_seconds(200);
    session.append(id, q);

    q.ts = Timestamp::from_seconds(199);
    CHECK_THROWS(session.append(id, q));
}
