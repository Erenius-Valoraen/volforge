// Multi-session behaviour and expiry settlement.
//
// The traps here are quiet ones: a position that reads as flat overnight, a time
// of day that resolves onto a weekend, a bar series that keeps answering for
// yesterday, settlement that double-counts. Each gets its own test.

#include "harness.hpp"

#include "volforge/backtest.hpp"
#include "volforge/synthetic.hpp"

#include <cmath>
#include <set>

using namespace volforge;

namespace {

constexpr int kOpen  = 9 * 3600 + 15 * 60;
constexpr int kClose = 15 * 3600 + 30 * 60;

// Two sessions with one option expiring on the second, driven tick by tick so
// settlement values can be asserted exactly.
struct TwoDay {
    InstrumentRegistry registry;
    UnderlyingId       underlying{};
    InstrumentId       spot = InstrumentId::Invalid;
    InstrumentId       call = InstrumentId::Invalid;
    InstrumentId       put  = InstrumentId::Invalid;
    std::shared_ptr<MemoryDataSource> source;

    Date day1{20250701};   // Tuesday
    Date day2{20250702};   // Wednesday, expiry

    TwoDay(double day1_spot, double day2_close_spot, double strike = 25000.0) {
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
            c.expiry     = day2;
            c.strike     = Price::from_double(strike);
            c.right      = r;
            c.lot_size   = 75;
            return registry.add(c);
        };
        call = opt(Right::Call);
        put  = opt(Right::Put);

        std::vector<std::shared_ptr<MemorySessionData>> sessions;
        sessions.push_back(build(day1, day1_spot, day1_spot, 120.0, 0));
        // Options start quoting fifteen minutes into day 2, so the first
        // observations of the session have no option price at all.
        sessions.push_back(build(day2, day1_spot, day2_close_spot, 40.0, 15 * 60));

        source = std::make_shared<MemoryDataSource>(registry);
        for (auto& s2 : sessions) source->add_session(std::move(s2));
    }

private:
    std::shared_ptr<MemorySessionData> build(Date date, double from, double to, double premium,
                                             int option_start_offset) {
        auto session = std::make_shared<MemorySessionData>(date, registry);
        const int span = kClose - kOpen;

        for (int t = 0; t <= span; t += 60) {
            const auto ts = timestamp_of(date, kOpen + t, kISTOffsetSeconds);
            const double px = from + (to - from) * (static_cast<double>(t) / span);

            Quote sq;
            sq.ts   = ts;
            sq.last = sq.bid = sq.ask = Price::from_double(px);
            sq.last_qty = 1;
            session->append(spot, sq);

            if (t < option_start_offset) continue;
            for (const InstrumentId id : {call, put}) {
                Quote q;
                q.ts   = ts;
                q.bid  = Price::from_double(premium);
                q.ask  = Price::from_double(premium + 1.0);
                q.last = Price::from_double(premium + 0.5);
                q.bid_qty = q.ask_qty = 750;
                session->append(id, q);
            }
        }
        session->build_event_order();
        return session;
    }
};

BacktestConfig no_costs() {
    BacktestConfig c;
    c.costs = std::make_shared<const NoCosts>();
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// Carrying across sessions
// ---------------------------------------------------------------------------

TEST(a_position_survives_overnight) {
    TwoDay d(25000, 25000);

    bool open_on_day2 = false;
    Date seen_on_day2{};

    const auto result = run_backtest(
        *d.source, d.underlying, d.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto pos = ctx.sell({d.call}, 1, "short call");
            if (!pos) co_return;

            // 09:20 has already gone today, so this resolves onto the next
            // trading session rather than firing again immediately.
            co_await ctx.at("09:20");
            seen_on_day2  = ctx.date();
            open_on_day2  = ctx.portfolio().at(pos->id()).open();
        },
        no_costs());

    CHECK(seen_on_day2 == d.day2);
    CHECK(open_on_day2);
    CHECK_EQ(result.sessions.size(), std::size_t{2});
}

TEST(an_overnight_position_is_not_valued_at_zero_on_the_next_open) {
    TwoDay d(25000, 25000);

    double first_of_day2 = 0.0;
    bool   sampled = false;

    run_backtest(*d.source, d.underlying, d.spot,
                 [&](Ctx& ctx) -> StrategyTask {
                     co_await ctx.at("09:20");
                     const auto pos = ctx.sell({d.call}, 1, "short call");
                     if (!pos) co_return;

                     // The very first observation of the next session. The option
                     // has not printed yet at this instant, so without a carried
                     // mark the position would read as flat.
                     co_await ctx.at("09:15");
                     first_of_day2 = ctx.portfolio().at(pos->id()).pnl_pct(ctx.market());
                     sampled = true;
                 },
                 no_costs());

    CHECK(sampled);
    // At this instant the option has not quoted yet today, so the valuation can
    // only come from the mark carried out of yesterday: sold at 120, marked at
    // the closing ask of 121. Exactly zero would mean the carry was lost.
    CHECK(first_of_day2 != 0.0);
    CHECK(std::abs(first_of_day2 + 75.0 / 9000.0) < 1e-6);
}

TEST(working_orders_do_not_survive_the_close) {
    TwoDay d(25000, 25000);

    // The put quotes one-sided on day 1 only in this scenario? No — instead
    // submit at the very last observation, so nothing is left to fill against.
    const auto result = run_backtest(
        *d.source, d.underlying, d.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("15:30");          // the session's final observation
            ctx.sell({d.call}, 1, "too late");
            co_await ctx.at("09:20");          // next session
        },
        no_costs());

    // The order could not fill before the bell, and a day order does not carry.
    CHECK(result.cancelled_orders > 0);
    for (const TradeRecord& t : result.trade_log) {
        CHECK(t.fill_ts.seconds() <= timestamp_of(d.day1, kClose, kISTOffsetSeconds).seconds());
    }
}

// ---------------------------------------------------------------------------
// Time of day across sessions
// ---------------------------------------------------------------------------

TEST(a_time_that_has_passed_resolves_onto_the_next_trading_session) {
    InstrumentRegistry registry;
    SyntheticSeriesConfig cfg;
    cfg.start    = Date{20250703};   // Thursday
    cfg.sessions = 4;                // Thu, Fri, Mon, Tue
    const auto series = make_synthetic_series(registry, cfg);

    std::vector<Date> woke_on;
    run_backtest(*series.source, series.underlying, series.spot,
                 [&](Ctx& ctx) -> StrategyTask {
                     for (int i = 0; i < 4; ++i) {
                         co_await ctx.at("10:00");
                         woke_on.push_back(ctx.date());
                     }
                 },
                 no_costs());

    CHECK_EQ(woke_on.size(), std::size_t{4});
    CHECK(woke_on[0] == Date{20250703});
    CHECK(woke_on[1] == Date{20250704});
    // The weekend is skipped: Friday is followed by Monday, not Saturday.
    CHECK(woke_on[2] == Date{20250707});
    CHECK(woke_on[3] == Date{20250708});
}

TEST(a_daily_loop_strategy_trades_every_session) {
    InstrumentRegistry registry;
    SyntheticSeriesConfig cfg;
    cfg.sessions = 6;
    const auto series = make_synthetic_series(registry, cfg);

    const auto result = run_backtest(
        *series.source, series.underlying, series.spot,
        [](Ctx& ctx) -> StrategyTask {
            while (true) {
                co_await ctx.at("09:30");
                const auto legs = ctx.chain().straddle();
                if (!legs.empty()) {
                    const auto pos = ctx.sell(legs, 1, "daily straddle");
                    if (pos) pos->exit_at("15:00");
                }
                co_await ctx.at("15:20");
            }
        },
        no_costs());

    CHECK_EQ(result.daily.size(), std::size_t{6});

    std::size_t traded_sessions = 0;
    for (const SessionSummary& s : result.daily) {
        if (s.trades > 0) ++traded_sessions;
    }
    CHECK(traded_sessions >= 5);

    // Flat every night, so nothing should ever reach settlement.
    CHECK_EQ(result.settled_legs, std::size_t{0});
    CHECK_EQ(result.open_positions, std::size_t{0});
}

TEST(a_bar_series_from_an_earlier_session_refuses_to_answer) {
    InstrumentRegistry registry;
    SyntheticSeriesConfig cfg;
    cfg.sessions = 2;
    const auto series = make_synthetic_series(registry, cfg);

    bool threw = false;
    run_backtest(*series.source, series.underlying, series.spot,
                 [&](Ctx& ctx) -> StrategyTask {
                     co_await ctx.at("10:00");
                     const auto stale = ctx.spot_bars(60);   // day 1's bars
                     co_await ctx.at("10:00");               // now day 2
                     try {
                         (void)stale->known_count(ctx.now());
                     } catch (const std::exception&) {
                         threw = true;
                     }
                 },
                 no_costs());

    // Silently serving yesterday's bars is the failure this guards against.
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// Settlement
// ---------------------------------------------------------------------------

TEST(an_in_the_money_short_settles_at_intrinsic) {
    // Spot closes at 25,300 against a 25,000 strike: the call is 300 in.
    TwoDay d(25000, 25300);

    const auto result = run_backtest(
        *d.source, d.underlying, d.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            ctx.sell({d.call}, 1, "short call");
            co_await ctx.at("15:29");   // held into expiry deliberately
        },
        no_costs());

    CHECK_EQ(result.settled_legs, std::size_t{1});
    CHECK_EQ(result.open_positions, std::size_t{0});

    const TradeRecord& settle = result.trade_log.back();
    CHECK(settle.settled);
    CHECK(std::abs(settle.price.to_double() - 300.0) < 1e-6);

    // Sold at 120, bought back by settlement at 300: a loss of 180 a contract.
    CHECK(std::abs(result.realized.to_double() + 180.0 * 75) < 1e-6);
}

TEST(an_out_of_the_money_short_expires_worthless_and_keeps_the_premium) {
    // Spot closes below the strike, so the call is worth nothing.
    TwoDay d(25000, 24700);

    const auto result = run_backtest(
        *d.source, d.underlying, d.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            ctx.sell({d.call}, 1, "short call");
            co_await ctx.at("15:29");
        },
        no_costs());

    CHECK_EQ(result.settled_legs, std::size_t{1});
    CHECK(std::abs(result.trade_log.back().price.to_double()) < 1e-9);

    // The whole 120 of premium is kept.
    CHECK(std::abs(result.realized.to_double() - 120.0 * 75) < 1e-6);
}

TEST(exercise_charges_the_buyer_and_not_the_writer) {
    // Same in-the-money settlement, once long and once short, with costs on.
    auto run_side = [](bool long_side) {
        TwoDay d(25000, 25300);
        return run_backtest(*d.source, d.underlying, d.spot,
                            [&, long_side](Ctx& ctx) -> StrategyTask {
                                co_await ctx.at("09:20");
                                if (long_side) ctx.buy({d.call}, 1, "long call");
                                else           ctx.sell({d.call}, 1, "short call");
                                co_await ctx.at("15:29");
                            },
                            BacktestConfig{});
    };

    const auto as_buyer  = run_side(true);
    const auto as_writer = run_side(false);

    // STT on exercise is charged on intrinsic value and falls on the buyer.
    // 300 * 75 = 22,500 of intrinsic at 0.15% is about 33.75.
    const double buyer_settlement_cost =
        as_buyer.trade_log.back().cost.to_double();
    CHECK(std::abs(buyer_settlement_cost - 33.75) < 0.5);

    CHECK(as_writer.trade_log.back().cost.minor == 0);
}

TEST(squaring_off_before_expiry_avoids_settlement_entirely) {
    TwoDay d(25000, 25300);

    const auto result = run_backtest(
        *d.source, d.underlying, d.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto pos = ctx.sell({d.call}, 1, "short call");
            if (!pos) co_return;
            // Out before the bell on expiry day. exit_at would mean *today*,
            // which for a carried position is almost never what is meant.
            pos->exit_at_on(d.day2, "15:00");
            co_await ctx.at("15:29");
        },
        no_costs());

    CHECK_EQ(result.settled_legs, std::size_t{0});
    CHECK_EQ(result.open_positions, std::size_t{0});
    // Closed at the ask of 41, having sold at 120.
    CHECK(result.realized.to_double() > 0.0);
}

TEST(a_roll_is_ordinary_strategy_code) {
    InstrumentRegistry registry;
    SyntheticSeriesConfig cfg;
    cfg.sessions = 8;
    const auto series = make_synthetic_series(registry, cfg);

    int rolls = 0;

    const auto result = run_backtest(
        *series.source, series.underlying, series.spot,
        [&](Ctx& ctx) -> StrategyTask {
            std::optional<PositionRef> held;
            std::optional<Date>        held_expiry;

            while (true) {
                co_await ctx.at("09:30");

                // At least one day out: on expiry day itself, today's expiry is
                // the thing being rolled out of, not into.
                const auto expiry = ctx.next_expiry(1);
                if (!expiry) continue;

                // Close whatever is about to expire and reopen further out. The
                // engine has no idea what "roll" means; the strategy does.
                if (held && held_expiry && *held_expiry <= ctx.date()) {
                    held->close();
                    held.reset();
                    ++rolls;
                }

                if (!held) {
                    const auto legs = ctx.chain(*expiry).straddle();
                    if (!legs.empty()) {
                        held = ctx.sell(legs, 1, "rolled straddle");
                        held_expiry = expiry;
                    }
                }
                co_await ctx.at("15:20");
            }
        },
        no_costs());

    CHECK(rolls > 0);
    // Rolled out of every expiry rather than being settled into one.
    CHECK_EQ(result.settled_legs, std::size_t{0});
}

TEST(forbidding_expiry_exposure_fails_loudly) {
    TwoDay d(25000, 25300);

    BacktestConfig cfg = no_costs();
    cfg.expiry = ExpiryHandling::Forbid;

    bool threw = false;
    try {
        run_backtest(*d.source, d.underlying, d.spot,
                     [&](Ctx& ctx) -> StrategyTask {
                         co_await ctx.at("09:20");
                         ctx.sell({d.call}, 1, "short call");
                         co_await ctx.at("15:29");
                     },
                     cfg);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// Accounting across the run
// ---------------------------------------------------------------------------

TEST(daily_summaries_reconcile_with_the_final_result) {
    InstrumentRegistry registry;
    SyntheticSeriesConfig cfg;
    cfg.sessions = 6;
    const auto series = make_synthetic_series(registry, cfg);

    const auto result = run_backtest(
        *series.source, series.underlying, series.spot,
        [](Ctx& ctx) -> StrategyTask {
            while (true) {
                co_await ctx.at("09:30");
                const auto legs = ctx.chain().straddle();
                if (!legs.empty()) {
                    const auto pos = ctx.sell(legs, 1, "straddle");
                    if (pos) { pos->stop_loss(0.35); pos->exit_at("15:00"); }
                }
                co_await ctx.at("15:20");
            }
        });

    CHECK_EQ(result.daily.size(), std::size_t{6});

    // Cumulative figures only ever move forward.
    for (std::size_t i = 1; i < result.daily.size(); ++i) {
        CHECK(result.daily[i].costs_to_date.minor >= result.daily[i - 1].costs_to_date.minor);
    }

    // The last day's cumulative figures are the run's figures.
    const SessionSummary& last = result.daily.back();
    CHECK(last.realized_to_date == result.realized);
    CHECK(last.costs_to_date == result.costs);
    CHECK(result.net_realized == result.realized - result.costs);
    CHECK(result.final_equity == result.net_realized + result.unrealized);

    // Trades are counted once, whether attributed to a day or to the run.
    std::size_t summed = 0;
    for (const SessionSummary& s : result.daily) summed += s.trades;
    CHECK_EQ(summed, result.trades);
}

TEST(settlement_closes_each_leg_exactly_once) {
    TwoDay d(25000, 25300);

    const auto result = run_backtest(
        *d.source, d.underlying, d.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            ctx.sell({d.call, d.put}, 1, "straddle");
            co_await ctx.at("15:29");
        },
        no_costs());

    CHECK_EQ(result.settled_legs, std::size_t{2});

    std::set<std::int32_t> settled_instruments;
    std::size_t settle_records = 0;
    for (const TradeRecord& t : result.trade_log) {
        if (!t.settled) continue;
        ++settle_records;
        settled_instruments.insert(index_of(t.instrument));
    }
    CHECK_EQ(settle_records, std::size_t{2});
    CHECK_EQ(settled_instruments.size(), std::size_t{2});

    // Short call 300 in the money, short put worthless: -180 and +120 a contract.
    CHECK(std::abs(result.realized.to_double() - (-180.0 + 120.0) * 75) < 1e-6);
}

TEST(a_multi_session_run_is_reproducible) {
    InstrumentRegistry r1, r2;
    SyntheticSeriesConfig cfg;
    cfg.sessions = 5;
    const auto a = make_synthetic_series(r1, cfg);
    const auto b = make_synthetic_series(r2, cfg);

    const StrategyFn fn = [](Ctx& ctx) -> StrategyTask {
        while (true) {
            co_await ctx.at("09:30");
            const auto legs = ctx.chain().strangle(2);
            if (!legs.empty()) {
                const auto pos = ctx.sell(legs, 1, "strangle");
                if (pos) pos->stop_loss(0.30);
            }
            co_await ctx.at("15:10");
        }
    };

    const auto ra = run_backtest(*a.source, a.underlying, a.spot, fn);
    const auto rb = run_backtest(*b.source, b.underlying, b.spot, fn);

    CHECK_EQ(ra.trades, rb.trades);
    CHECK(ra.realized == rb.realized);
    CHECK(ra.costs == rb.costs);
    CHECK(ra.peak_margin == rb.peak_margin);
    CHECK_EQ(ra.trade_log.size(), rb.trade_log.size());
    for (std::size_t i = 0; i < ra.trade_log.size(); ++i) {
        CHECK(ra.trade_log[i].fill_ts == rb.trade_log[i].fill_ts);
        CHECK(ra.trade_log[i].price == rb.trade_log[i].price);
    }
}

TEST(sessions_must_be_supplied_in_order) {
    TwoDay d(25000, 25000);
    bool threw = false;
    try {
        run_backtest(*d.source, d.underlying, d.spot,
                     [](Ctx& ctx) -> StrategyTask { co_await ctx.at("15:29"); },
                     no_costs(), {d.day2, d.day1});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// Defects found by review, pinned so they cannot come back
// ---------------------------------------------------------------------------

namespace {

// One session where a chosen leg quotes one-sided and therefore never fills,
// plus a following session where everything is tradeable again.
struct PartialFill {
    InstrumentRegistry registry;
    UnderlyingId       underlying{};
    InstrumentId       spot = InstrumentId::Invalid;
    InstrumentId       call = InstrumentId::Invalid;
    InstrumentId       put  = InstrumentId::Invalid;
    std::shared_ptr<MemoryDataSource> source;

    Date day1{20250707};   // Monday
    Date day2{20250708};

    PartialFill() {
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
            c.expiry     = Date{20250710};   // Thursday, beyond both sessions
            c.strike     = Price::from_double(25000);
            c.right      = r;
            c.lot_size   = 75;
            return registry.add(c);
        };
        call = opt(Right::Call);
        put  = opt(Right::Put);

        std::vector<std::shared_ptr<MemorySessionData>> built;
        built.push_back(build(day1));
        built.push_back(build(day2));
        source = std::make_shared<MemoryDataSource>(registry);
        for (auto& b : built) source->add_session(std::move(b));
    }

private:
    std::shared_ptr<MemorySessionData> build(Date date) {
        auto session = std::make_shared<MemorySessionData>(date, registry);
        for (int t = 0; t <= kClose - kOpen; t += 60) {
            const auto ts = timestamp_of(date, kOpen + t, kISTOffsetSeconds);

            Quote sq;
            sq.ts = ts;
            sq.last = sq.bid = sq.ask = Price::from_double(25000);
            sq.last_qty = 1;
            session->append(spot, sq);

            Quote c;
            c.ts = ts;
            c.bid = Price::from_double(100);
            c.ask = Price::from_double(101);
            c.last = Price::from_double(100.5);
            c.bid_qty = c.ask_qty = 750;
            session->append(call, c);

            // The put shows no bid, so a sell order against it can never fill.
            Quote p;
            p.ts = ts;
            p.bid = Price::from_double(0);
            p.ask = Price::from_double(90);
            p.last = Price::from_double(85);
            p.bid_qty = 0;
            p.ask_qty = 750;
            session->append(put, p);
        }
        session->build_event_order();
        return session;
    }
};

}  // namespace

TEST(a_position_with_a_cancelled_leg_is_still_managed) {
    PartialFill f;

    bool established_on_day2 = false;
    double pct_on_day2 = 0.0;

    const auto result = run_backtest(
        *f.source, f.underlying, f.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto pos = ctx.sell({f.call, f.put}, 1, "straddle");
            if (!pos) co_return;

            co_await ctx.at("09:20");   // next session
            const Position& p = ctx.portfolio().at(pos->id());
            established_on_day2 = p.established();
            pct_on_day2 = p.pnl_pct(ctx.market());
        },
        no_costs());

    CHECK_EQ(result.cancelled_orders, std::size_t{1});   // the put never filled

    // The surviving call leg is a real holding. Treating the position as
    // unestablished would report a P&L of zero and disable every percentage
    // stop on it, leaving that leg running naked and unprotected.
    CHECK(established_on_day2);
    CHECK(pct_on_day2 != 0.0);
}

TEST(a_stop_that_could_not_fill_by_the_bell_is_re_armed_next_session) {
    PartialFill f;

    const auto result = run_backtest(
        *f.source, f.underlying, f.spot,
        [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:20");
            const auto pos = ctx.sell({f.call}, 1, "short call");
            if (!pos) co_return;

            // Fires at the final observation of day 1, so the exit order has
            // nothing left to fill against before the close.
            pos->exit_at("15:30");
            co_await ctx.at("15:00");   // day 2
        },
        no_costs());

    // The abandoned exit must not leave the rule spent. Re-armed, it gets done
    // on the next session rather than the position carrying on unprotected.
    CHECK_EQ(result.open_positions, std::size_t{0});
    CHECK_EQ(result.trades, std::size_t{2});
    CHECK(result.trade_log.back().fill_ts >
          timestamp_of(f.day1, kClose, kISTOffsetSeconds));
}

TEST(settlement_works_without_an_index_feed) {
    InstrumentRegistry registry;
    SyntheticSeriesConfig cfg;
    cfg.start    = Date{20250707};   // Monday
    cfg.sessions = 5;                // through Friday, past Thursday's expiry
    const auto series = make_synthetic_series(registry, cfg);

    // No spot instrument at all: the settlement level has to come from the chain
    // itself. On expiry day the expiring series has no time left to invert, so
    // this only works if a later series is consulted.
    const auto result = run_backtest(
        *series.source, series.underlying, InstrumentId::Invalid,
        [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("09:30");
            const auto expiry = ctx.next_expiry();
            if (!expiry) co_return;
            const auto legs = ctx.chain(*expiry).straddle();
            if (legs.empty()) co_return;
            ctx.sell(legs, 1, "held to expiry");
            co_await ctx.at("09:30");   // stay in through settlement
            co_await ctx.at("09:30");
            co_await ctx.at("09:30");
            co_await ctx.at("09:30");
        },
        no_costs());

    CHECK_EQ(result.settled_legs, std::size_t{2});
    CHECK_EQ(result.open_positions, std::size_t{0});
}
