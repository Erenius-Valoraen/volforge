#include "harness.hpp"

#include "volforge/lookahead.hpp"
#include "volforge/synthetic.hpp"

#include <cmath>

using namespace volforge;

namespace {

constexpr int kOpen = 9 * 3600 + 15 * 60;

Timestamp t_at(int sec) { return timestamp_of(Date{20250701}, sec, kISTOffsetSeconds); }

int second_of_day(Timestamp ts) {
    const std::int64_t local = ts.seconds() + kISTOffsetSeconds;
    return static_cast<int>(((local % 86400) + 86400) % 86400);
}

// Spot plus two option legs, driven tick by tick.
struct Book {
    InstrumentRegistry registry;
    UnderlyingId       underlying{};
    InstrumentId       spot = InstrumentId::Invalid;
    InstrumentId       call = InstrumentId::Invalid;
    InstrumentId       put  = InstrumentId::Invalid;
    std::shared_ptr<MemorySessionData> data;

    Book() {
        underlying = registry.intern_underlying("NIFTY");

        InstrumentSpec s;
        s.underlying = underlying;
        s.kind       = InstrumentKind::Spot;
        s.lot_size   = 1;
        spot = registry.add(s);

        auto opt = [&](Right r) {
            InstrumentSpec c;
            c.underlying = underlying;
            c.kind       = InstrumentKind::Option;
            c.expiry     = Date{20250703};
            c.strike     = Price::from_double(100);
            c.right      = r;
            c.lot_size   = 75;
            return registry.add(c);
        };
        call = opt(Right::Call);
        put  = opt(Right::Put);

        data = std::make_shared<MemorySessionData>(Date{20250701}, registry);
    }

    void tick(int sec, double spot_px, double call_bid, double call_ask,
              double put_bid = 20.0, double put_ask = 20.5) {
        const auto ts = t_at(sec);

        Quote sq;
        sq.ts = ts;
        sq.last = sq.bid = sq.ask = Price::from_double(spot_px);
        sq.last_qty = 1;
        data->append(spot, sq);

        auto leg = [&](InstrumentId id, double bid, double ask) {
            Quote q;
            q.ts   = ts;
            q.bid  = Price::from_double(bid);
            q.ask  = Price::from_double(ask);
            q.last = Price::from_double((bid + ask) / 2);
            q.bid_qty = q.ask_qty = 75;
            data->append(id, q);
        };
        leg(call, call_bid, call_ask);
        leg(put, put_bid, put_ask);
    }
};

// Flat at 100.00/100.50 until `spike_sec`, then the call ask jumps far enough to
// breach a 30% stop on a short at 100.00 (which needs an ask of 130).
Book spiking_book(int spike_sec) {
    Book b;
    for (int s = kOpen; s < spike_sec; s += 5) b.tick(s, 25000, 100.0, 100.5);
    for (int s = spike_sec; s <= spike_sec + 30; s += 1) b.tick(s, 25000, 139.0, 140.0);
    for (int s = spike_sec + 60; s <= 15 * 3600 + 30 * 60; s += 60) {
        b.tick(s, 25000, 139.0, 140.0);
    }
    b.data->build_event_order();
    return b;
}

}  // namespace

// ---------------------------------------------------------------------------
// A stop is armed continuously, not only while the strategy watches it
// ---------------------------------------------------------------------------

TEST(an_attached_stop_fires_while_the_strategy_waits_on_something_else) {
    const int spike = 10 * 3600 + 33 * 60 + 17;   // 10:33:17, deliberately not a boundary
    Book b = spiking_book(spike);

    const auto result = run_session(
        *b.data, b.underlying, b.spot, [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.sell({*call}, 1, "short call");
            if (!pos) co_return;

            pos->stop_loss(0.30);

            // The strategy now waits on something entirely unrelated, hours
            // away. The stop must not care.
            co_await ctx.at("15:15");
        });

    CHECK_EQ(result.rules_fired, std::size_t{1});
    CHECK_EQ(result.trade_log.size(), std::size_t{2});

    const TradeRecord& exit = result.trade_log.back();
    CHECK(exit.from_rule);
    CHECK(exit.side == Side::Buy);

    // Fired at the spike, not at 15:15 where the strategy was looking.
    CHECK(second_of_day(exit.signal_ts) == spike);
    CHECK(second_of_day(exit.fill_ts) < 11 * 3600);
}

TEST(a_stop_fires_at_base_resolution_not_at_a_bar_boundary) {
    const int spike = 10 * 3600 + 33 * 60 + 17;
    Book b = spiking_book(spike);

    const auto result = run_session(
        *b.data, b.underlying, b.spot, [](Ctx& ctx) -> StrategyTask {
            // The strategy reasons in hours. Its fills must not.
            const auto hourly = ctx.spot_bars(3600);
            co_await ctx.at("09:20");

            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.sell({*call}, 1, "short call");
            if (!pos) co_return;

            pos->stop_loss(0.30);

            // Await an hourly-bar event, so the only thing the strategy itself
            // could wake on is a 3600-second boundary.
            co_await when([hourly](const EvalCtx& c) {
                return hourly->known_count(c.now) >= 6;
            });
        });

    CHECK_EQ(result.rules_fired, std::size_t{1});
    const TradeRecord& exit = result.trade_log.back();

    const int sig = second_of_day(exit.signal_ts);
    CHECK(sig == spike);

    // Emphatically not rounded to the hour the strategy thinks in.
    CHECK(sig % 3600 != 0);
    CHECK(sig % 60 != 0);
}

TEST(a_rule_exit_obeys_the_same_fill_timing_as_a_discretionary_one) {
    const int spike = 10 * 3600 + 33 * 60 + 17;
    Book b = spiking_book(spike);

    const auto result = run_session(
        *b.data, b.underlying, b.spot, [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.sell({*call}, 1, "short call");
            if (!pos) co_return;
            pos->stop_loss(0.30);
            co_await ctx.at("15:15");
        });

    for (const TradeRecord& t : result.trade_log) {
        // A stop is an order. It cannot fill on the observation that triggered
        // it any more than a discretionary exit can.
        CHECK(t.fill_ts > t.signal_ts);
    }

    const TradeRecord& exit = result.trade_log.back();
    // Filled by crossing the spread, so bought back at the ask.
    CHECK(std::abs(exit.price.to_double() - 140.0) < 1e-9);
}

// ---------------------------------------------------------------------------
// Level of application
// ---------------------------------------------------------------------------

TEST(a_leg_stop_closes_only_that_leg) {
    const int spike = 10 * 3600 + 33 * 60 + 17;
    Book b = spiking_book(spike);

    std::size_t open_legs = 0, closed_legs = 0;
    run_session(*b.data, b.underlying, b.spot, [&](Ctx& ctx) -> StrategyTask {
        co_await ctx.at("09:20");
        const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
        const auto put  = ctx.chain().option(Price::from_double(100), Right::Put);
        if (!call || !put) co_return;

        const auto pos = ctx.sell({*call, *put}, 1, "strangle");
        if (!pos) co_return;

        // Only the call leg is protected. The put is left alone.
        pos->leg(0).stop_loss(0.30);

        co_await ctx.at("15:15");

        for (const Leg& l : ctx.portfolio().at(pos->id()).legs()) {
            if (l.state == LegState::Closed) ++closed_legs; else ++open_legs;
        }
    });

    CHECK_EQ(closed_legs, std::size_t{1});
    CHECK_EQ(open_legs, std::size_t{1});
}

TEST(a_position_stop_uses_the_combined_view) {
    const int spike = 10 * 3600 + 33 * 60 + 17;
    Book b = spiking_book(spike);

    // Call marked at 140 against a 100 entry is a 40-point loss; the put is
    // unchanged at 20.5 against 20.0. Combined premium is 120, so the combined
    // loss is about 34% — over a 30% position stop but under a 45% one.
    auto run_with = [&](double pct) {
        return run_session(*b.data, b.underlying, b.spot, [pct](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            const auto put  = ctx.chain().option(Price::from_double(100), Right::Put);
            if (!call || !put) co_return;
            const auto pos = ctx.sell({*call, *put}, 1, "strangle");
            if (!pos) co_return;
            pos->stop_loss(pct);
            co_await ctx.at("15:15");
        });
    };

    CHECK_EQ(run_with(0.30).rules_fired, std::size_t{1});
    CHECK_EQ(run_with(0.45).rules_fired, std::size_t{0});
}

TEST(exit_at_fires_on_the_clock_regardless_of_pnl) {
    Book b;
    for (int s = kOpen; s <= 15 * 3600 + 30 * 60; s += 30) b.tick(s, 25000, 100.0, 100.5);
    b.data->build_event_order();

    const auto result = run_session(
        *b.data, b.underlying, b.spot, [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.sell({*call}, 1, "short call");
            if (!pos) co_return;
            pos->exit_at("14:00");
            co_await ctx.at("15:29");
        });

    CHECK_EQ(result.rules_fired, std::size_t{1});
    const TradeRecord& exit = result.trade_log.back();
    CHECK(exit.from_rule);
    CHECK(second_of_day(exit.signal_ts) >= 14 * 3600);
    CHECK(second_of_day(exit.signal_ts) < 14 * 3600 + 60);
}

// ---------------------------------------------------------------------------
// Guards
// ---------------------------------------------------------------------------

TEST(a_stop_on_an_unfilled_position_does_not_fire) {
    Book b;
    b.tick(kOpen, 25000, 100.0, 100.5);
    b.tick(9 * 3600 + 20 * 60, 25000, 100.0, 100.5);
    b.data->build_event_order();   // nothing after the decision, so nothing fills

    const auto result = run_session(
        *b.data, b.underlying, b.spot, [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.sell({*call}, 1, "short call");
            if (!pos) co_return;
            // Treating "not yet filled" as flat would fire this immediately.
            pos->stop_loss(0.01);
            co_await ctx.at("15:29");
        });

    CHECK_EQ(result.rules_fired, std::size_t{0});
    CHECK_EQ(result.trades, std::size_t{0});
}

TEST(a_stop_fires_once_and_does_not_rearm) {
    const int spike = 10 * 3600 + 33 * 60 + 17;
    Book b = spiking_book(spike);

    const auto result = run_session(
        *b.data, b.underlying, b.spot, [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.sell({*call}, 1, "short call");
            if (!pos) co_return;
            pos->stop_loss(0.30);
            co_await ctx.at("15:29");
        });

    CHECK_EQ(result.rules_fired, std::size_t{1});
    CHECK_EQ(result.trade_log.size(), std::size_t{2});   // one entry, one exit
    CHECK_EQ(result.open_positions, std::size_t{0});
}

TEST(attached_rules_survive_the_lookahead_detector) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    const StrategyFn fn = [](Ctx& ctx) -> StrategyTask {
        co_await ctx.at("09:20");
        const auto legs = ctx.chain().straddle();
        if (legs.empty()) co_return;
        const auto pos = ctx.sell(legs, 1, "straddle");
        if (!pos) co_return;
        pos->stop_loss(0.30);
        pos->take_profit(0.50);
        pos->exit_at("15:15");
        co_await ctx.at("15:29");
    };

    const auto report = check_lookahead(*synth.data, synth.underlying, synth.spot, fn, {}, 50);
    CHECK(report.clean);
    CHECK(report.trades_compared > 0);
}
