#include "volforge/event_loop.hpp"

#include <stdexcept>

namespace volforge {

RunResult run_session(const SessionData& session, UnderlyingId underlying, InstrumentId spot,
                      const StrategyFn& strategy, const RunConfig& config) {
    auto fills = config.fills ? config.fills
                              : std::static_pointer_cast<const FillModel>(
                                    std::make_shared<const CrossSpreadFill>());

    ReplayClock clock;
    MarketView  market(session, clock);
    Portfolio   portfolio(fills, config.execution_delay_nanos, config.costs);

    Ctx ctx(session, market, portfolio, underlying, spot, session.date(), kISTOffsetSeconds,
            config.session_open_sec, config.rate);

    StrategyTask task = strategy(ctx);
    if (!task.valid()) throw std::invalid_argument("run_session: strategy produced no task");

    RunResult result;
    result.date = session.date();

    // Enter the strategy once so it runs up to its first suspension. It has not
    // seen any market data yet, which is correct: whatever it awaits will be
    // evaluated for the first time at the session's first timestamp.
    task.resume();

    EventCursor cursor(session);
    Event       ev;
    Timestamp   last{std::numeric_limits<std::int64_t>::min()};

    while (cursor.next(ev)) {
        ++result.observations;
        if (ev.ts == last) continue;   // same timestamp, already stepped
        last = ev.ts;

        clock.advance_to(ev.ts);
        ++result.steps;

        portfolio.process_pending(market);

        // Attached risk rules are evaluated here — every observation, at the
        // finest resolution the data provides, regardless of what timeframe the
        // strategy reasons in or what it currently happens to be awaiting.
        portfolio.process_risk_rules(market);

        const EvalCtx eval{&market, &portfolio, ev.ts};
        int guard = 0;
        while (!task.done() && task.pending().valid()) {
            ++result.condition_evals;
            if (!task.pending().eval(eval)) break;

            task.resume();
            ++result.resumes;

            if (++guard > config.max_resumes_per_step) {
                throw std::runtime_error(
                    "strategy resumed repeatedly within one timestamp: a condition is "
                    "always true, so it is spinning rather than waiting for the market");
            }
        }
    }

    // Anything still pending at the bell never traded. Reporting it rather than
    // force-filling keeps the run honest: a strategy that could not get out is a
    // result, not a rounding error.
    result.strategy_finished = task.done();
    result.unfilled_orders   = portfolio.pending_orders();
    result.realized          = portfolio.realized();
    result.costs             = portfolio.costs();
    result.net_realized      = portfolio.net_realized();
    result.unrealized        = portfolio.unrealized(market);
    result.final_equity      = portfolio.equity(market);
    result.trades            = portfolio.trades().size();
    result.illiquid_fills    = portfolio.illiquid_fills();
    result.oversized_fills   = portfolio.oversized_fills();
    result.cancelled_orders  = portfolio.cancelled_orders();
    result.trade_log         = portfolio.trades();
    result.rules_fired       = portfolio.rules_fired();

    for (std::size_t i = 0; i < portfolio.size(); ++i) {
        if (portfolio.at(static_cast<PositionId>(i)).open()) ++result.open_positions;
    }
    return result;
}

}  // namespace volforge
