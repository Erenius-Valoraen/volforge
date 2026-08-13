#include "harness.hpp"

#include "volforge/event_loop.hpp"
#include "volforge/synthetic.hpp"

#include <cmath>
#include <memory>

using namespace volforge;

namespace {

StrategyTask straddle_with_stop(Ctx& ctx, double stop, double target) {
    co_await ctx.at("09:20");
    const auto legs = ctx.chain().straddle();
    if (legs.empty()) co_return;
    const auto pos = ctx.sell(legs, 1, "atm straddle");
    if (!pos) co_return;
    co_await (ctx.pnl_pct_at_most(*pos, -stop) | ctx.pnl_pct_at_least(*pos, target)
              | ctx.at("15:15"));
    ctx.close(*pos);
    co_await (ctx.position_closed(*pos) | ctx.at("15:29"));
}

struct Run {
    InstrumentRegistry registry;
    SyntheticSession   synth;
    RunResult          result;

    explicit Run(double stop = 0.30, double target = 0.50, std::int64_t delay = 0) {
        synth = make_synthetic_session(registry);
        RunConfig cfg;
        cfg.execution_delay_nanos = delay;
        result = run_session(*synth.data, synth.underlying, synth.spot,
                             [&](Ctx& c) { return straddle_with_stop(c, stop, target); }, cfg);
    }
};

// A two-instrument session with hand-chosen quotes, for asserting exact prices.
struct Handmade {
    InstrumentRegistry registry;
    UnderlyingId       underlying{};
    InstrumentId       spot = InstrumentId::Invalid;
    InstrumentId       call = InstrumentId::Invalid;
    std::shared_ptr<MemorySessionData> data;

    Handmade() {
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
        c.strike     = Price::from_double(25000);
        c.right      = Right::Call;
        c.lot_size   = 75;
        call = registry.add(c);

        data = std::make_shared<MemorySessionData>(Date{20250701}, registry);
    }

    void tick(int sec, double spot_px, double bid, double ask) {
        const auto ts = timestamp_of(Date{20250701}, sec, kISTOffsetSeconds);

        Quote sq;
        sq.ts = ts;
        sq.last = sq.bid = sq.ask = Price::from_double(spot_px);
        sq.last_qty = 1;
        data->append(spot, sq);

        Quote cq;
        cq.ts  = ts;
        cq.bid = Price::from_double(bid);
        cq.ask = Price::from_double(ask);
        cq.last = Price::from_double((bid + ask) / 2);
        cq.bid_qty = cq.ask_qty = 75;
        data->append(call, cq);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// The execution-timing guarantee
// ---------------------------------------------------------------------------

TEST(orders_never_fill_on_the_observation_that_triggered_them) {
    Run r;
    CHECK(r.result.trades > 0);
    for (const TradeRecord& t : r.result.trade_log) {
        // The whole point: a strategy that learns a price at T cannot have
        // traded on it at T.
        CHECK(t.fill_ts > t.signal_ts);
    }
}

TEST(execution_delay_only_ever_adds_to_the_structural_minimum) {
    Run immediate(0.30, 0.50, 0);
    Run delayed(0.30, 0.50, 30LL * 1'000'000'000);   // 30 seconds

    CHECK(immediate.result.trades > 0);
    CHECK(delayed.result.trades > 0);

    auto gap = [](const TradeRecord& t) { return t.fill_ts.nanos - t.signal_ts.nanos; };

    for (const TradeRecord& t : immediate.result.trade_log) CHECK(gap(t) > 0);
    for (const TradeRecord& t : delayed.result.trade_log) {
        CHECK(gap(t) >= 30LL * 1'000'000'000);
    }
}

TEST(entry_fill_price_comes_from_after_the_decision) {
    Handmade h;
    // The option is cheap at the moment of the decision and jumps immediately
    // after. Filling at the decision's price would be the flattering bug.
    h.tick(9 * 3600 + 15 * 60, 25000, 100.0, 100.5);
    h.tick(9 * 3600 + 20 * 60, 25000, 100.0, 100.5);   // decision lands here
    h.tick(9 * 3600 + 20 * 60 + 1, 25000, 180.0, 180.5);
    h.tick(9 * 3600 + 20 * 60 + 2, 25000, 181.0, 181.5);
    h.data->build_event_order();

    const auto result = run_session(
        *h.data, h.underlying, h.spot,
        [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(25000), Right::Call);
            if (!call) co_return;
            ctx.sell({*call}, 1, "short call");
            co_await ctx.at("15:29");
        });

    CHECK_EQ(result.trade_log.size(), std::size_t{1});
    const TradeRecord& t = result.trade_log.front();

    // Sold, so filled at the bid — and at the bid *after* the decision second.
    CHECK(std::abs(t.price.to_double() - 180.0) < 1e-9);
    CHECK(t.fill_ts > t.signal_ts);
}

// ---------------------------------------------------------------------------
// Marking and stops
// ---------------------------------------------------------------------------

TEST(short_positions_mark_at_the_ask_they_would_be_bought_back_at) {
    Handmade h;
    h.tick(9 * 3600 + 15 * 60, 25000, 100.0, 100.5);
    h.tick(9 * 3600 + 20 * 60, 25000, 100.0, 100.5);
    h.tick(9 * 3600 + 20 * 60 + 1, 25000, 100.0, 101.0);   // fill at bid 100.00
    // Spread widens with an unchanged mid. Marking at mid would report no loss;
    // marking at the ask, which is where a buy-back happens, reports one.
    h.tick(9 * 3600 + 20 * 60 + 2, 25000, 90.0, 110.0);
    h.data->build_event_order();

    double observed_pct = 0.0;
    const auto result = run_session(
        *h.data, h.underlying, h.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(25000), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.sell({*call}, 1, "short call");
            if (!pos) co_return;
            // Mid is unchanged at 100, so a mid-marked position shows no loss
            // and this never fires. Ask-marked, it is down 10%.
            co_await ctx.pnl_pct_at_most(*pos, -0.05);
            observed_pct = ctx.portfolio().at(*pos).pnl_pct(ctx.market());
        });

    CHECK_EQ(result.trade_log.size(), std::size_t{1});
    CHECK(std::abs(result.trade_log.front().price.to_double() - 100.0) < 1e-9);

    // Entry 100.00, marked at ask 110.00 -> a 10% loss on premium collected.
    CHECK(observed_pct < -0.09 && observed_pct > -0.11);
}

TEST(stop_fires_near_the_configured_level) {
    Run r(0.30, 5.0);   // target unreachable, so the stop or the clock decides
    if (r.result.trade_log.size() < 4) { CHECK(r.result.trades > 0); return; }

    double credit = 0.0, debit = 0.0;
    for (const TradeRecord& t : r.result.trade_log) {
        const double value = t.price.to_double() * t.qty;
        (t.side == Side::Sell ? credit : debit) += value;
    }
    CHECK(credit > 0.0);

    const double loss_pct = (credit - debit) / credit;
    // Exits at the next observation after the breach, so it overshoots slightly
    // rather than landing exactly on the level.
    CHECK(loss_pct < -0.25);
    CHECK(loss_pct > -0.45);
}

TEST(pnl_is_zero_until_every_leg_has_filled) {
    Handmade h;
    h.tick(9 * 3600 + 15 * 60, 25000, 100.0, 100.5);
    h.tick(9 * 3600 + 20 * 60, 25000, 100.0, 100.5);
    h.data->build_event_order();   // nothing after the decision, so nothing fills

    const auto result = run_session(
        *h.data, h.underlying, h.spot,
        [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(25000), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.sell({*call}, 1, "short call");
            if (!pos) co_return;
            // A stop on an unfilled position must not fire; treating unfilled as
            // flat would trigger it instantly.
            co_await (ctx.pnl_pct_at_most(*pos, -0.01) | ctx.at("15:29"));
        });

    CHECK_EQ(result.trades, std::size_t{0});
    CHECK_EQ(result.unfilled_orders, std::size_t{1});
    CHECK(result.realized.minor == 0);
}

// ---------------------------------------------------------------------------
// Loop behaviour
// ---------------------------------------------------------------------------

TEST(strategy_code_runs_a_handful_of_times_per_session) {
    Run r;
    CHECK(r.result.steps > 20'000);
    // The entire design rests on this ratio: conditions are checked thousands of
    // times, strategy code is entered a handful.
    CHECK(r.result.resumes < 10);
    CHECK(r.result.condition_evals > r.result.resumes * 100);
}

TEST(a_spinning_strategy_is_caught_rather_than_hanging) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    bool threw = false;
    try {
        run_session(*synth.data, synth.underlying, synth.spot, [](Ctx&) -> StrategyTask {
            while (true) co_await when([](const EvalCtx&) { return true; });
        });
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(a_strategy_that_never_trades_still_completes) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    const auto result = run_session(*synth.data, synth.underlying, synth.spot,
                                    [](Ctx& ctx) -> StrategyTask { co_await ctx.at("15:29"); });

    CHECK(result.strategy_finished);
    CHECK_EQ(result.trades, std::size_t{0});
    CHECK(result.realized.minor == 0);
}

TEST(runs_are_reproducible) {
    Run a, b;
    CHECK_EQ(a.result.trades, b.result.trades);
    CHECK(a.result.realized == b.result.realized);
    CHECK_EQ(a.result.resumes, b.result.resumes);
    for (std::size_t i = 0; i < a.result.trade_log.size(); ++i) {
        CHECK(a.result.trade_log[i].fill_ts == b.result.trade_log[i].fill_ts);
        CHECK(a.result.trade_log[i].price == b.result.trade_log[i].price);
    }
}
