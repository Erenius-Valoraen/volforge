// Tests for the ways a backtest overstates returns.
//
// Every case here corresponds to a defect found by auditing the P&L path, and
// each one flattered results before it was fixed.

#include "harness.hpp"

#include "volforge/event_loop.hpp"
#include "volforge/synthetic.hpp"

#include <cmath>

using namespace volforge;

namespace {

constexpr int kOpen = 9 * 3600 + 15 * 60;
Timestamp t_at(int sec) { return timestamp_of(Date{20250701}, sec, kISTOffsetSeconds); }

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

        auto opt = [&](Right r, double strike) {
            InstrumentSpec c;
            c.underlying = underlying;
            c.kind       = InstrumentKind::Option;
            c.expiry     = Date{20250703};
            c.strike     = Price::from_double(strike);
            c.right      = r;
            c.lot_size   = 75;
            return registry.add(c);
        };
        call = opt(Right::Call, 100);
        put  = opt(Right::Put, 100);
        data = std::make_shared<MemorySessionData>(Date{20250701}, registry);
    }

    void spot_tick(int sec, double px) {
        Quote q;
        q.ts = t_at(sec);
        q.last = q.bid = q.ask = Price::from_double(px);
        q.last_qty = 1;
        data->append(spot, q);
    }

    void opt_tick(InstrumentId id, int sec, double bid, double ask, double last,
                  Qty size = 75) {
        Quote q;
        q.ts   = t_at(sec);
        q.bid  = Price::from_double(bid);
        q.ask  = Price::from_double(ask);
        q.last = Price::from_double(last);
        q.bid_qty = q.ask_qty = size;
        data->append(id, q);
    }
};

// Costs off, so P&L assertions are about marking rather than charges.
RunConfig no_costs() {
    RunConfig c;
    c.costs = std::make_shared<const NoCosts>();
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// Marking
// ---------------------------------------------------------------------------

TEST(a_long_option_going_worthless_books_the_full_loss) {
    Book b;
    for (int s = kOpen; s <= kOpen + 400; s += 10) {
        b.spot_tick(s, 100);
        b.opt_tick(b.call, s, 9.5, 10.0, 9.75);
    }
    // The option dies: no bid at all, and the last print collapses.
    for (int s = kOpen + 410; s <= kOpen + 600; s += 10) {
        b.spot_tick(s, 100);
        b.opt_tick(b.call, s, 0.0, 0.05, 0.05);
    }
    b.data->build_event_order();

    double observed_pct = 0.0;
    Money  observed_pnl{};

    const auto result = run_session(
        *b.data, b.underlying, b.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:15");
            const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
            if (!call) co_return;
            const auto pos = ctx.buy({*call}, 1, "long call");
            if (!pos) co_return;
            co_await ctx.after(500);
            observed_pct = ctx.portfolio().at(pos->id()).pnl_pct(ctx.market());
            observed_pnl = ctx.portfolio().at(pos->id()).pnl(ctx.market());
        },
        no_costs());

    CHECK_EQ(result.trades, std::size_t{1});

    // Bought 75 at 10.00 for 750. A zero bid means it cannot be sold at all, so
    // the loss is total. Reporting zero here — as an earlier version did —
    // deleted a 100% loss and stopped any percentage stop from ever firing.
    CHECK(std::abs(observed_pnl.to_double() + 750.0) < 1e-6);
    CHECK(std::abs(observed_pct + 1.0) < 1e-9);
}

TEST(a_short_with_no_offer_does_not_book_a_fictitious_profit) {
    Book b;
    for (int s = kOpen; s <= kOpen + 400; s += 10) {
        b.spot_tick(s, 100);
        b.opt_tick(b.call, s, 10.0, 10.5, 10.25);
    }
    // Offer disappears. There is nothing to buy back against.
    for (int s = kOpen + 410; s <= kOpen + 600; s += 10) {
        b.spot_tick(s, 100);
        b.opt_tick(b.call, s, 9.0, 0.0, 10.0);
    }
    b.data->build_event_order();

    Money observed{};
    bool  filled = false;
    run_session(*b.data, b.underlying, b.spot,
                [&](Ctx& ctx) -> StrategyTask {
                    co_await ctx.at("09:15");
                    const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
                    if (!call) co_return;
                    const auto pos = ctx.sell({*call}, 1, "short call");
                    if (!pos) co_return;
                    co_await ctx.after(500);
                    filled = ctx.portfolio().at(pos->id()).established();
                    observed = ctx.portfolio().at(pos->id()).pnl(ctx.market());
                },
                no_costs());

    // Guard against passing vacuously: the leg must actually have filled.
    CHECK(filled);

    // Marking a short at an absent offer of zero would claim the full premium as
    // profit. Falling back to the last trade at 10.00 against a 10.00 entry is
    // flat, which is the honest answer.
    CHECK(std::abs(observed.to_double()) < 1e-6);
}

// ---------------------------------------------------------------------------
// Order lifecycle
// ---------------------------------------------------------------------------

TEST(closing_a_position_cancels_its_unfilled_opening_orders) {
    Book b;
    for (int s = kOpen; s <= kOpen + 900; s += 10) {
        b.spot_tick(s, 100);
        b.opt_tick(b.call, s, 10.0, 10.5, 10.25);
        // The put quotes, so it is selectable, but is one-sided and therefore
        // never fillable.
        b.opt_tick(b.put, s, 0.0, 5.0, 2.5);
    }
    b.data->build_event_order();

    const auto result = run_session(
        *b.data, b.underlying, b.spot,
        [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:15");
            const auto legs = ctx.chain().straddle();
            if (legs.size() != 2) co_return;
            const auto pos = ctx.sell(legs, 1, "straddle");
            if (!pos) co_return;
            co_await ctx.after(300);
            pos->close();
            co_await ctx.after(300);
        },
        no_costs());

    // One leg filled and was closed; the other never filled and must not be
    // left queued to open after the position was closed.
    CHECK_EQ(result.unfilled_orders, std::size_t{0});
    CHECK_EQ(result.cancelled_orders, std::size_t{1});
    CHECK_EQ(result.trades, std::size_t{2});          // one entry, one exit
    CHECK_EQ(result.open_positions, std::size_t{0});
}

TEST(fills_larger_than_the_displayed_size_are_flagged) {
    Book b;
    for (int s = kOpen; s <= kOpen + 300; s += 10) {
        b.spot_tick(s, 100);
        b.opt_tick(b.call, s, 10.0, 10.5, 10.25, /*size=*/75);   // one lot showing
    }
    b.data->build_event_order();

    auto run_lots = [&](int lots) {
        return run_session(*b.data, b.underlying, b.spot,
                           [lots](Ctx& ctx) -> StrategyTask {
                               co_await ctx.at("09:15");
                               const auto call =
                                   ctx.chain().option(Price::from_double(100), Right::Call);
                               if (!call) co_return;
                               ctx.sell({*call}, lots, "short call");
                               co_await ctx.after(200);
                           },
                           no_costs());
    };

    CHECK_EQ(run_lots(1).oversized_fills, std::size_t{0});
    // Ten lots against one lot displayed would have walked the book in reality.
    CHECK_EQ(run_lots(10).oversized_fills, std::size_t{1});
}

// ---------------------------------------------------------------------------
// Costs
// ---------------------------------------------------------------------------

TEST(costs_are_charged_on_every_fill_and_reported_separately) {
    Book b;
    for (int s = kOpen; s <= kOpen + 600; s += 10) {
        b.spot_tick(s, 100);
        b.opt_tick(b.call, s, 100.0, 100.5, 100.25);
    }
    b.data->build_event_order();

    const StrategyFn fn = [](Ctx& ctx) -> StrategyTask {
        co_await ctx.at("09:15");
        const auto call = ctx.chain().option(Price::from_double(100), Right::Call);
        if (!call) co_return;
        const auto pos = ctx.sell({*call}, 1, "short call");
        if (!pos) co_return;
        co_await ctx.after(200);
        pos->close();
        co_await ctx.after(200);
    };

    const auto free_run = run_session(*b.data, b.underlying, b.spot, fn, no_costs());
    const auto real_run = run_session(*b.data, b.underlying, b.spot, fn, {});

    CHECK_EQ(real_run.trades, std::size_t{2});
    CHECK(free_run.costs.minor == 0);

    // Gross P&L is unaffected by costs; only the net moves.
    CHECK(real_run.realized == free_run.realized);
    CHECK(real_run.costs.minor > 0);
    CHECK(real_run.net_realized.minor < real_run.realized.minor);
    CHECK(real_run.net_realized == real_run.realized - real_run.costs);

    // Sold 75 at 100.00 and bought back at 100.50: a 37.50 gross loss that costs
    // turn into a materially larger one. A backtest reporting only the first
    // number is not reporting what the account would have done.
    CHECK(std::abs(real_run.realized.to_double() + 37.5) < 1e-6);
    CHECK(real_run.costs.to_double() > 40.0);
    CHECK(real_run.final_equity == real_run.net_realized + real_run.unrealized);
}

TEST(cost_of_a_round_trip_is_the_right_order_of_magnitude) {
    InstrumentRegistry reg;
    InstrumentSpec spec;
    spec.underlying = reg.intern_underlying("NIFTY");
    spec.kind       = InstrumentKind::Option;
    spec.expiry     = Date{20250703};
    spec.strike     = Price::from_double(25000);
    spec.right      = Right::Call;
    spec.lot_size   = 75;

    const IndianFnOCosts m;
    auto charge = [&](Side side) {
        return m.cost_of(CostContext{&spec, Price::from_double(200), 75, side, false, Price{}});
    };

    const Money sell = charge(Side::Sell);
    const Money buy  = charge(Side::Buy);

    // One lot of NIFTY at 200.00 premium is 15,000 of turnover per side. STT at
    // 0.15% lands on the sell alone and dominates it, so the sell side is much
    // the heavier of the two.
    CHECK(sell.to_double() > buy.to_double());
    CHECK(std::abs(sell.to_double() - 52.4) < 1.5);
    CHECK(std::abs(buy.to_double() - 30.4) < 1.5);

    // Roughly 0.55% of one side's turnover on a round trip, most of it statutory.
    const double round_trip = sell.to_double() + buy.to_double();
    CHECK(round_trip > 70.0);
    CHECK(round_trip < 100.0);
}

TEST(costs_scale_with_turnover_and_brokerage_can_be_removed) {
    InstrumentRegistry reg;
    InstrumentSpec spec;
    spec.underlying = reg.intern_underlying("NIFTY");
    spec.kind       = InstrumentKind::Option;
    spec.strike     = Price::from_double(25000);
    spec.right      = Right::Call;
    spec.lot_size   = 75;

    const IndianFnOCosts standard;
    const auto one  = standard.cost_of(
        CostContext{&spec, Price::from_double(200), 75, Side::Sell, false, Price{}});
    const auto ten  = standard.cost_of(
        CostContext{&spec, Price::from_double(200), 750, Side::Sell, false, Price{}});

    // Brokerage is flat per order, so ten lots cost less than ten single-lot
    // tickets — the percentage components scale, the ticket does not.
    CHECK(ten.to_double() < one.to_double() * 10.0);
    CHECK(ten.to_double() > one.to_double());

    // A zero-brokerage account is expressible without touching the engine.
    IndianFnORates free_rates;
    free_rates.brokerage_per_order = Money{0};
    const IndianFnOCosts zero_brokerage(free_rates);
    const auto cheap = zero_brokerage.cost_of(
        CostContext{&spec, Price::from_double(200), 75, Side::Sell, false, Price{}});
    CHECK(std::abs((one - cheap).to_double() - 20.0 * 1.18) < 0.05);
}

TEST(equity_separates_realized_costs_and_unrealized) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    // Deliberately leaves the position open at the bell.
    const auto result = run_session(
        *synth.data, synth.underlying, synth.spot, [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto legs = ctx.chain().straddle();
            if (legs.empty()) co_return;
            const auto pos = ctx.sell(legs, 1, "straddle");
            if (!pos) co_return;
            co_await ctx.at("15:29");
        });

    CHECK(result.open_positions > 0);
    CHECK(result.costs.minor > 0);

    // Nothing was closed, so realised is zero and the entire figure is a mark on
    // a position still open — which the result reports as its own line rather
    // than folding into a single number that reads like booked profit.
    CHECK(result.realized.minor == 0);
    CHECK(result.unrealized.minor != 0);
    CHECK(result.final_equity == result.net_realized + result.unrealized);
}
