// Multi-session backtesting.
//
// The strategy coroutine and the portfolio outlive a session; only the data is
// swapped. That is what makes a positional strategy expressible at all — a
// position opened on Monday is still open on Tuesday, and the strategy is still
// sitting at whatever it was awaiting.
//
// Written as a daily loop, a strategy repeats naturally, because `ctx.at()`
// resolves a time that has already passed onto the next *trading* session:
//
//     while (true) {
//         co_await ctx.at("09:20");
//         auto pos = ctx.sell(ctx.chain().straddle(), 1);
//         pos->exit_at("15:15");
//         co_await ctx.at("15:20");
//     }
//
// Three things do not survive a session boundary, by design:
//
//   - **Bar series and indicators.** They describe one session, and querying one
//     from a later day throws rather than returning yesterday's bars. Re-acquire
//     them inside the loop.
//   - **Working orders.** Exchange orders are day orders. Anything unfilled at
//     the close is cancelled rather than carried into a price the strategy never
//     saw.
//   - **Options past their expiry.** See below.
//
// What *does* survive: open positions, their attached risk rules, realised P&L,
// costs, and the carried mark on every leg — without which an overnight position
// would value at nothing until its first trade of the next day.

#pragma once

#include "volforge/event_loop.hpp"

#include <vector>

namespace volforge {

// What happens to options that reach their expiry still open.
//
// The default is to settle, because that is what happens in reality when a
// strategy does nothing. A strategy that wants something else — squaring off
// early, rolling to the next expiry, or anything at all — says so in its own
// code, by awaiting a time before expiry and acting. The engine does not guess
// what "roll" means, because the answer differs per strategy: same strike, same
// moneyness, same delta, or something else entirely.
enum class ExpiryHandling : std::uint8_t {
    // Cash settle at intrinsic against the settlement level. STT on exercise
    // falls on the buyer of an in-the-money option.
    Settle,

    // Refuse to hold into expiry: any position still open when its expiry
    // arrives is an error. For strategies that must never take settlement risk
    // and would rather fail loudly than discover it in the P&L.
    Forbid,
};

struct BacktestConfig : RunConfig {
    ExpiryHandling expiry = ExpiryHandling::Settle;

    // When on expiry day the settlement level is taken, and open contracts are
    // settled. NSE settles index options against the underlying's closing level.
    int settlement_second_of_day = 15 * 3600 + 30 * 60;
};

// One session's contribution, for equity curves and drawdown.
struct SessionSummary {
    Date        date;
    Money       realized_to_date{};   // gross, cumulative
    Money       costs_to_date{};
    Money       equity{};             // net realised plus open marks, cumulative
    Money       peak_margin{};        // within this session
    std::size_t trades          = 0;  // within this session
    std::size_t settled_legs    = 0;
    std::size_t open_positions  = 0;  // carried into the next session
};

struct BacktestResult {
    std::vector<Date> sessions;

    Money realized{};        // gross, before costs
    Money costs{};
    Money net_realized{};
    Money unrealized{};      // still-open positions at the end
    Money final_equity{};
    Money peak_margin{};     // across the whole run

    std::size_t observations     = 0;
    std::size_t steps            = 0;
    std::size_t resumes          = 0;
    std::size_t trades           = 0;
    std::size_t settled_legs     = 0;
    std::size_t illiquid_fills   = 0;
    std::size_t oversized_fills  = 0;
    std::size_t cancelled_orders = 0;
    std::size_t open_positions   = 0;
    bool        strategy_finished = false;

    std::vector<TradeRecord>    trade_log;
    std::vector<SessionSummary> daily;
};

// Runs one strategy across many sessions.
//
// `dates` defaults to every session the source knows about, ascending. Sessions
// are replayed in date order; supplying them out of order is an error, since a
// replay clock cannot run backwards.
BacktestResult run_backtest(DataSource& source, UnderlyingId underlying, InstrumentId spot,
                            const StrategyFn& strategy, const BacktestConfig& config = {},
                            std::vector<Date> dates = {});

}  // namespace volforge
