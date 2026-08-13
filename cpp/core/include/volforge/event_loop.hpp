// The event loop.
//
// Order of operations at each distinct timestamp, and the order matters:
//
//   1. advance the clock, which is what makes new observations visible at all
//   2. fill any orders that have become eligible
//   3. evaluate the strategy's pending condition, resuming it if it holds
//
// Filling before resuming is the structural half of "an order cannot execute
// against the observation that triggered it". A strategy that decides at step T
// has its order filled no earlier than step T+1, because step T's fill pass has
// already run by the time the strategy is entered. No configuration can defeat
// this; execution_delay only ever adds to it. See docs/execution-semantics.md.
//
// Conditions are evaluated once per timestamp rather than once per observation.
// Market state is only coherent at a timestamp boundary: several instruments
// print in the same second, and a position's P&L is meaningless halfway through
// applying them.

#pragma once

#include "volforge/margin.hpp"
#include "volforge/strategy.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace volforge {

struct RunConfig {
    // Additional delay between a decision and the earliest fill, on top of the
    // structural one-observation minimum. Zero means "the next observation".
    std::int64_t execution_delay_nanos = 0;

    // Defaults to CrossSpreadFill when null.
    std::shared_ptr<const FillModel> fills;

    // A strategy that resumes this many times within one timestamp is spinning
    // on an always-true condition rather than waiting for the market.
    int max_resumes_per_step = 64;

    // Session open, local time. Bar boundaries are anchored here so they follow
    // the trading session rather than wall-clock arithmetic.
    int session_open_sec = 9 * 3600 + 15 * 60;

    // Annualised risk-free rate used for discounting and implied volatility.
    double rate = 0.065;

    // Statutory charges and brokerage. Verify the rates against your broker;
    // they are configuration, not constants.
    std::shared_ptr<const CostPolicy> costs;

    // Margin model. Defaults to a SPAN-style scenario scan.
    std::shared_ptr<const MarginModel> margin;

    // How often margin is re-evaluated, in seconds. Every observation would mean
    // an implied-volatility solve per leg per second for no useful precision.
    int margin_sample_seconds = 60;
};

struct RunResult {
    Date        date;
    Money       realized{};        // gross, before costs
    Money       costs{};           // statutory charges and brokerage
    Money       net_realized{};    // realized - costs
    Money       unrealized{};      // open positions marked at exit-side prices
    Money       final_equity{};    // net_realized + unrealized
    std::size_t observations     = 0;   // rows walked
    std::size_t steps            = 0;   // distinct timestamps
    std::size_t condition_evals  = 0;
    std::size_t resumes          = 0;   // times strategy code was entered
    std::size_t trades           = 0;
    std::size_t illiquid_fills   = 0;   // filled across a very wide spread
    std::size_t oversized_fills  = 0;   // larger than the size displayed at the touch
    std::size_t cancelled_orders = 0;   // opening orders withdrawn on close

    Money       peak_margin{};          // largest requirement seen during the session
    Money       final_margin{};         // requirement at the bell
    std::size_t rules_fired      = 0;   // attached stops/targets that triggered
    std::size_t unfilled_orders  = 0;   // still pending at session end
    std::size_t open_positions   = 0;   // still live at session end
    bool        strategy_finished = false;

    // Every fill, carrying both the time the strategy decided and the time it
    // actually executed. Those columns are never equal, and being able to read
    // them side by side is what makes the execution model auditable.
    std::vector<TradeRecord> trade_log;
};

using StrategyFn = std::function<StrategyTask(Ctx&)>;

// Runs one strategy over one session.
RunResult run_session(const SessionData& session, UnderlyingId underlying, InstrumentId spot,
                      const StrategyFn& strategy, const RunConfig& config = {});

}  // namespace volforge
