#include "harness.hpp"

#include "volforge/lookahead.hpp"
#include "volforge/synthetic.hpp"

#include <cmath>
#include <vector>

using namespace volforge;

namespace {

constexpr int kOpen = 9 * 3600 + 15 * 60;
constexpr std::int64_t kSec = 1'000'000'000;

Timestamp t_at(int sec) { return timestamp_of(Date{20250701}, sec, kISTOffsetSeconds); }

// A session with a hand-driven spot path, so band levels and crossing seconds
// are known exactly rather than inferred.
struct Scripted {
    InstrumentRegistry registry;
    UnderlyingId       underlying{};
    InstrumentId       spot = InstrumentId::Invalid;
    InstrumentId       call = InstrumentId::Invalid;
    std::shared_ptr<MemorySessionData> data;

    Scripted() {
        underlying = registry.intern_underlying("NIFTY");

        InstrumentSpec s;
        s.underlying = underlying;
        s.kind       = InstrumentKind::Spot;
        s.lot_size   = 1;
        spot = registry.add(s);

        InstrumentSpec c;
        c.underlying = underlying;
        c.kind       = InstrumentKind::Option;
        c.expiry     = Date{20250703};
        c.strike     = Price::from_double(100);
        c.right      = Right::Call;
        c.lot_size   = 75;
        call = registry.add(c);

        data = std::make_shared<MemorySessionData>(Date{20250701}, registry);
    }

    void tick(int sec, double px) {
        Quote q;
        q.ts = t_at(sec);
        q.last = q.bid = q.ask = Price::from_double(px);
        q.last_qty = 1;
        data->append(spot, q);

        Quote o;
        o.ts  = t_at(sec);
        o.bid = Price::from_double(9.5);
        o.ask = Price::from_double(10.5);
        o.last = Price::from_double(10.0);
        o.bid_qty = o.ask_qty = 75;
        data->append(call, o);
    }

    // Twenty flat one-minute bars at 100, then a minute that pokes to 105 and
    // closes back at 99, then a minute that closes at 110.
    void script_poke_then_break() {
        for (int m = 0; m < 20; ++m) {
            for (int s = 0; s < 60; ++s) tick(kOpen + m * 60 + s, 100.0);
        }
        const int poke = kOpen + 20 * 60;
        for (int s = 0; s < 30; ++s) tick(poke + s, 100.0);
        for (int s = 30; s < 45; ++s) tick(poke + s, 105.0);   // crosses here
        for (int s = 45; s < 60; ++s) tick(poke + s, 99.0);    // closes back below

        const int brk = kOpen + 21 * 60;
        for (int s = 0; s < 60; ++s) tick(brk + s, 110.0);     // closes above

        for (int s = 0; s < 120; ++s) tick(kOpen + 22 * 60 + s, 110.0);
        data->build_event_order();
    }
};

// Records the moment a crossing condition first fires.
StrategyFn crossing_recorder(Confirm confirm, Timestamp* out, int period = 20, double k = 2.0) {
    return [confirm, out, period, k](Ctx& ctx) -> StrategyTask {
        const auto bars = ctx.spot_bars(60);
        const auto bb   = bollinger(bars, period, k);
        co_await (ctx.cross_above(bb.upper, confirm) | ctx.at("15:29"));
        *out = ctx.now();
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// Bar timing: label vs knowable
// ---------------------------------------------------------------------------

TEST(a_bar_is_not_readable_until_its_close_time) {
    Scripted s;
    for (int i = 0; i < 180; ++i) s.tick(kOpen + i, 100.0 + i * 0.1);
    s.data->build_event_order();

    const auto bars = BarSeries::build(s.data->quotes(s.spot), 60 * kSec, t_at(kOpen),
                                       BarPrice::Last);

    // The 09:15 bar covers [09:15:00, 09:16:00) and is labelled 09:15.
    CHECK(bars.all()[0].open_time == t_at(kOpen));
    CHECK(bars.all()[0].close_time == t_at(kOpen + 60));

    // Mid-bar it does not exist yet, however tempting its label is.
    CHECK_EQ(bars.known_count(t_at(kOpen + 30)), std::size_t{0});
    CHECK(bars.completed(t_at(kOpen + 30)) == nullptr);

    // It becomes readable exactly at close_time, not a second earlier.
    CHECK_EQ(bars.known_count(t_at(kOpen + 59)), std::size_t{0});
    CHECK_EQ(bars.known_count(t_at(kOpen + 60)), std::size_t{1});
}

TEST(a_five_minute_bar_leaks_nothing_at_its_label_time) {
    Scripted s;
    for (int i = 0; i < 900; ++i) s.tick(kOpen + i, 100.0 + i * 0.01);
    s.data->build_event_order();

    const auto bars = BarSeries::build(s.data->quotes(s.spot), 300 * kSec, t_at(kOpen),
                                       BarPrice::Last);

    // Boundaries follow the session anchor: 09:15-09:20, 09:20-09:25, ...
    CHECK(bars.all()[0].open_time == t_at(kOpen));
    CHECK(bars.all()[0].close_time == t_at(kOpen + 300));

    // At 09:20 the first bar has just closed; the second has not started
    // producing a readable value and will not until 09:25.
    CHECK_EQ(bars.known_count(t_at(kOpen + 300)), std::size_t{1});
    CHECK_EQ(bars.known_count(t_at(kOpen + 599)), std::size_t{1});
    CHECK_EQ(bars.known_count(t_at(kOpen + 600)), std::size_t{2});
}

TEST(reading_a_bar_backwards_past_the_start_returns_nothing_rather_than_wrapping) {
    Scripted s;
    for (int i = 0; i < 180; ++i) s.tick(kOpen + i, 100.0);
    s.data->build_event_order();

    const auto bars = BarSeries::build(s.data->quotes(s.spot), 60 * kSec, t_at(kOpen),
                                       BarPrice::Last);
    CHECK(bars.completed(t_at(kOpen + 60), 0) != nullptr);
    CHECK(bars.completed(t_at(kOpen + 60), 1) == nullptr);
    CHECK_THROWS(bars.completed(t_at(kOpen + 60), -1));
}

TEST(the_forming_bar_is_aggregated_as_of_now_not_as_it_will_finish) {
    Scripted s;
    for (int i = 0; i < 30; ++i) s.tick(kOpen + i, 100.0);
    for (int i = 30; i < 60; ++i) s.tick(kOpen + i, 200.0);   // second half of the bar
    for (int i = 60; i < 120; ++i) s.tick(kOpen + i, 100.0);
    s.data->build_event_order();

    const auto bars = BarSeries::build(s.data->quotes(s.spot), 60 * kSec, t_at(kOpen),
                                       BarPrice::Last);

    // Thirty seconds in, nothing above 100 has printed. A forming bar reported
    // as its finished self would show a close of 200 — the future.
    const auto mid = bars.forming(t_at(kOpen + 29));
    CHECK(mid.has_value());
    CHECK(std::abs(mid->close.to_double() - 100.0) < 1e-9);
    CHECK(std::abs(mid->high.to_double() - 100.0) < 1e-9);

    // The completed bar does close at 200, once it exists.
    const Bar* done = bars.completed(t_at(kOpen + 60));
    CHECK(done != nullptr);
    CHECK(std::abs(done->close.to_double() - 200.0) < 1e-9);
}

// ---------------------------------------------------------------------------
// Indicator knowability
// ---------------------------------------------------------------------------

TEST(an_indicator_is_unreadable_until_its_last_input_bar_closes) {
    Scripted s;
    for (int m = 0; m < 6; ++m) {
        for (int t = 0; t < 60; ++t) s.tick(kOpen + m * 60 + t, 100.0 + m);
    }
    s.data->build_event_order();

    auto bars = std::make_shared<const BarSeries>(
        BarSeries::build(s.data->quotes(s.spot), 60 * kSec, t_at(kOpen), BarPrice::Last));
    const auto avg = sma(bars, 3);

    // Warmup: two closed bars are not three, and a 3-period mean of 2 bars is a
    // different statistic wearing the same name.
    CHECK(!avg->at(t_at(kOpen + 60)).has_value());
    CHECK(!avg->at(t_at(kOpen + 120)).has_value());

    // Third bar closes at 09:18:00 and the value appears then, not before.
    CHECK(!avg->at(t_at(kOpen + 179)).has_value());
    const auto v = avg->at(t_at(kOpen + 180));
    CHECK(v.has_value());
    CHECK(std::abs(*v - 101.0) < 1e-9);   // mean of 100, 101, 102
}

TEST(an_indicator_value_holds_flat_across_the_forming_interval) {
    Scripted s;
    for (int m = 0; m < 5; ++m) {
        for (int t = 0; t < 60; ++t) s.tick(kOpen + m * 60 + t, 100.0 + m * 10);
    }
    s.data->build_event_order();

    auto bars = std::make_shared<const BarSeries>(
        BarSeries::build(s.data->quotes(s.spot), 60 * kSec, t_at(kOpen), BarPrice::Last));
    const auto avg = sma(bars, 2);

    // Through the whole of the 09:17 minute the value is the one fixed when the
    // 09:16 bar closed. A band that moved intrabar would be tracking a close
    // that has not happened.
    const auto at_start = avg->at(t_at(kOpen + 120));
    const auto at_mid   = avg->at(t_at(kOpen + 150));
    const auto at_end   = avg->at(t_at(kOpen + 179));
    CHECK(at_start.has_value());
    CHECK(*at_start == *at_mid);
    CHECK(*at_start == *at_end);

    // And steps only when the next bar closes.
    const auto after = avg->at(t_at(kOpen + 180));
    CHECK(after.has_value());
    CHECK(*after != *at_start);
}

TEST(indicator_rejects_a_negative_offset) {
    Scripted s;
    for (int i = 0; i < 300; ++i) s.tick(kOpen + i, 100.0);
    s.data->build_event_order();

    auto bars = std::make_shared<const BarSeries>(
        BarSeries::build(s.data->quotes(s.spot), 60 * kSec, t_at(kOpen), BarPrice::Last));
    const auto avg = sma(bars, 2);
    CHECK_THROWS(avg->at(t_at(kOpen + 180), -1));
}

// ---------------------------------------------------------------------------
// Confirmation policy — the scenario in full
// ---------------------------------------------------------------------------

TEST(instant_fires_at_the_exact_second_of_the_cross) {
    Scripted s;
    s.script_poke_then_break();

    Timestamp fired{};
    run_session(*s.data, s.underlying, s.spot, crossing_recorder(Confirm::Instant, &fired));

    // Twenty flat bars at 100 put the upper band at exactly 100. The first tick
    // above it is 09:35:30.
    CHECK(fired == t_at(kOpen + 20 * 60 + 30));
}

TEST(bar_close_ignores_a_poke_that_does_not_hold) {
    Scripted s;
    s.script_poke_then_break();

    Timestamp fired{};
    run_session(*s.data, s.underlying, s.spot, crossing_recorder(Confirm::BarClose, &fired));

    // The 09:35 bar reached 105 but closed at 99, so close confirmation ignores
    // it entirely. The next bar closes at 110, and that bar completes at
    // 09:37:00 — open_time plus the interval, since bars are open-stamped.
    CHECK(fired == t_at(kOpen + 22 * 60));

    // Emphatically not the second of the poke, nor the poke bar's close.
    CHECK(fired != t_at(kOpen + 20 * 60 + 30));
    CHECK(fired != t_at(kOpen + 21 * 60));
}

TEST(the_two_policies_disagree_on_purpose) {
    Scripted s;
    s.script_poke_then_break();

    Timestamp instant{}, confirmed{};
    run_session(*s.data, s.underlying, s.spot, crossing_recorder(Confirm::Instant, &instant));
    run_session(*s.data, s.underlying, s.spot, crossing_recorder(Confirm::BarClose, &confirmed));

    CHECK(instant < confirmed);
    // 90 seconds apart: one trades the touch, the other waits for the hold.
    CHECK_EQ(confirmed.nanos - instant.nanos, 90 * kSec);
}

TEST(a_bar_close_entry_fills_after_the_close_never_inside_the_bar) {
    Scripted s;
    s.script_poke_then_break();

    const auto result = run_session(
        *s.data, s.underlying, s.spot, [](Ctx& ctx) -> StrategyTask {
            const auto bars = ctx.spot_bars(60);
            const auto bb   = bollinger(bars, 20, 2.0);
            co_await (ctx.cross_above(bb.upper, Confirm::BarClose) | ctx.at("15:29"));
            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            if (!call) co_return;
            ctx.buy({*call}, 1, "breakout");
            co_await ctx.at("15:29");
        });

    CHECK_EQ(result.trade_log.size(), std::size_t{1});
    const TradeRecord& t = result.trade_log.front();

    const Timestamp bar_close = t_at(kOpen + 22 * 60);
    CHECK(t.signal_ts == bar_close);          // signals at the close, not the open
    CHECK(t.fill_ts > bar_close);             // and fills strictly after it
    CHECK(t.fill_ts != t_at(kOpen + 21 * 60));   // never the signalling bar's open
}

// ---------------------------------------------------------------------------
// Instrument universe
// ---------------------------------------------------------------------------

TEST(a_strike_is_not_selectable_before_it_quotes) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    // The generator lists far strikes but starts them late. At the open, the
    // chain must exclude them even though the registry knows they exist.
    std::size_t at_open = 0, at_noon = 0;
    run_session(*synth.data, synth.underlying, synth.spot, [&](Ctx& ctx) -> StrategyTask {
        co_await ctx.at("09:16");
        at_open = ctx.chain().strikes().size();
        co_await ctx.at("12:00");
        at_noon = ctx.chain().strikes().size();
    });

    CHECK(at_open > 0);
    CHECK(at_noon > at_open);   // the universe grows as strikes start trading

    // And the registry knew about all of them the whole time.
    std::size_t listed = 0;
    for (const auto& spec : registry.all()) {
        if (spec.is_option()) ++listed;
    }
    CHECK(listed > at_open);
}

// ---------------------------------------------------------------------------
// The detector itself
// ---------------------------------------------------------------------------

TEST(detector_reports_clean_for_an_honest_strategy) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    const StrategyFn honest = [](Ctx& ctx) -> StrategyTask {
        co_await ctx.at("09:20");
        const auto legs = ctx.chain().straddle();
        if (legs.empty()) co_return;
        const auto pos = ctx.sell(legs, 1, "straddle");
        if (!pos) co_return;
        co_await (pos->pnl_pct_at_most(-0.30) | ctx.at("15:15"));
        pos->close();
        co_await (pos->closed() | ctx.at("15:29"));
    };

    const auto report = check_lookahead(*synth.data, synth.underlying, synth.spot, honest, {}, 50);
    CHECK(report.clean);
    CHECK(report.cutoffs_tested > 20);
    CHECK(report.trades_compared > 0);
    CHECK(report.violations.empty());
}

TEST(detector_catches_a_strategy_that_indexes_bars_directly) {
    Scripted s;
    for (int m = 0; m < 40; ++m) {
        for (int t = 0; t < 60; ++t) s.tick(kOpen + m * 60 + t, 100.0 + (m % 7) * 3.0);
    }
    s.data->build_event_order();

    // The leak: reading the raw bar vector by index instead of through the
    // close_time gate, so the strategy sees a bar that has not closed. Nobody
    // wrote a negative offset; the bug is entirely in bypassing the gate.
    const StrategyFn cheating = [](Ctx& ctx) -> StrategyTask {
        const auto bars = ctx.spot_bars(60);
        co_await ctx.at("09:20");

        co_await when([bars](const EvalCtx& c) {
            const std::size_t known = bars->known_count(c.now);
            if (known + 1 >= bars->all().size()) return false;
            // One bar into the future.
            return bars->all()[known + 1].close.to_double() > 105.0;
        });

        const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
        if (!call) co_return;
        ctx.buy({*call}, 1, "cheating");
        co_await ctx.at("15:29");
    };

    const auto report = check_lookahead(*s.data, s.underlying, s.spot, cheating, {}, 60);

    // If the detector cannot catch a leak this blatant it is worthless, so this
    // test guards the detector rather than the engine.
    CHECK(!report.clean);
    CHECK(!report.violations.empty());
}

TEST(detector_catches_a_strategy_using_the_unfinished_bar) {
    Scripted s;
    for (int m = 0; m < 40; ++m) {
        for (int t = 0; t < 60; ++t) {
            // A spike in the back half of each minute, so the finished close
            // differs sharply from the running close.
            s.tick(kOpen + m * 60 + t, t < 40 ? 100.0 : 100.0 + (m % 5) * 4.0);
        }
    }
    s.data->build_event_order();

    // Acting on the forming bar is legal and explicit, but its close is not a
    // value anyone could have traded on at the next boundary. Truncation reveals
    // that the two runs disagree about what the running close was.
    const StrategyFn peeking = [](Ctx& ctx) -> StrategyTask {
        const auto bars = ctx.spot_bars(60);
        co_await ctx.at("09:20");
        co_await when([bars](const EvalCtx& c) {
            const std::size_t known = bars->known_count(c.now);
            if (known >= bars->all().size()) return false;
            return bars->all()[known].close.to_double() > 110.0;   // finished close
        });
        const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
        if (!call) co_return;
        ctx.buy({*call}, 1, "peeking");
        co_await ctx.at("15:29");
    };

    const auto report = check_lookahead(*s.data, s.underlying, s.spot, peeking, {}, 60);
    CHECK(!report.clean);
}

TEST(both_confirmation_policies_survive_the_detector) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    for (const Confirm confirm : {Confirm::Instant, Confirm::BarClose}) {
        const StrategyFn fn = [confirm](Ctx& ctx) -> StrategyTask {
            const auto bars = ctx.spot_bars(60);
            const auto bb   = bollinger(bars, 20, 2.0);
            co_await ctx.at("09:40");
            co_await (ctx.cross_above(bb.upper, confirm) | ctx.at("15:00"));
            const auto k = ctx.chain().atm_strike();
            if (!k) co_return;
            const auto call = ctx.chain().option(*k, Right::Call);
            if (!call) co_return;
            const auto pos = ctx.buy({*call}, 1, "breakout");
            if (!pos) co_return;
            co_await (ctx.after(600) | ctx.at("15:15"));
            pos->close();
            co_await (pos->closed() | ctx.at("15:29"));
        };

        const auto report = check_lookahead(*synth.data, synth.underlying, synth.spot, fn, {}, 50);
        CHECK(report.clean);
        CHECK(report.trades_compared > 0);
    }
}

TEST(truncated_sessions_preserve_prefixes_exactly) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    const Timestamp cutoff = t_at(12 * 3600);
    TruncatedSession clipped(*synth.data, cutoff);

    CHECK(clipped.total_observations() > 0);
    CHECK(clipped.total_observations() < synth.data->total_observations());

    for (const InstrumentId id : clipped.instruments()) {
        const auto full = synth.data->quotes(id);
        const auto cut  = clipped.quotes(id);
        CHECK(cut.size() <= full.size());
        if (!cut.empty()) {
            CHECK(cut.ts.back() <= cutoff);
            // Prefix, not a resample: every retained row is bit-identical.
            for (std::size_t i = 0; i < cut.size(); ++i) {
                CHECK(cut.ts[i] == full.ts[i]);
                CHECK(cut.bid[i] == full.bid[i]);
            }
        }
        if (cut.size() < full.size()) CHECK(full.ts[cut.size()] > cutoff);
    }
}
