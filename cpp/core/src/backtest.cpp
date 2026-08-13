#include "volforge/backtest.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace volforge {
namespace {

// The level options settle against. NSE settles index options on the closing
// level of the underlying, so the spot's last print of the session is used, and
// the parity forward stands in when there is no index feed.
std::optional<double> settlement_level(const Ctx& ctx, InstrumentId spot, Date expiry) {
    if (valid(spot)) {
        const auto q = ctx.market().quote(spot);
        if (q && q->last.minor > 0) return q->last.to_double();
    }

    // Parity on the expiring series is no help here: at the bell its time to
    // expiry is zero, so there is no forward to recover. Fall back to the
    // nearest series that still has life in it, which prices off the same
    // underlying.
    if (const auto own = ctx.forward(expiry)) return own;
    for (const Date d : ctx.expiries()) {
        if (d <= expiry) continue;
        if (const auto f = ctx.forward(d)) return f;
    }
    return std::nullopt;
}

// Expiries that have arrived and still have something open against them.
//
// Driven by what is actually held rather than by the session date, so an expiry
// falling on a holiday — or a session simply missing from the data — still
// settles at the next session rather than leaving a position open forever.
std::set<std::int32_t> due_expiries(const Portfolio& portfolio,
                                    const InstrumentRegistry& registry, Date today) {
    std::set<std::int32_t> due;
    for (std::size_t i = 0; i < portfolio.size(); ++i) {
        const Position& pos = portfolio.at(static_cast<PositionId>(i));
        for (const Leg& leg : pos.legs()) {
            if (!leg.live()) continue;
            const InstrumentSpec& spec = registry.spec(leg.instrument);
            if (!spec.is_option() || !spec.expiry.valid()) continue;
            if (spec.expiry <= today) due.insert(spec.expiry.yyyymmdd);
        }
    }
    return due;
}

}  // namespace

BacktestResult run_backtest(DataSource& source, UnderlyingId underlying, InstrumentId spot,
                            const StrategyFn& strategy, const BacktestConfig& config,
                            std::vector<Date> dates) {
    if (dates.empty()) dates = source.sessions();
    if (!std::is_sorted(dates.begin(), dates.end())) {
        throw std::invalid_argument("run_backtest: sessions must be in ascending date order");
    }

    auto fills = config.fills ? config.fills
                              : std::static_pointer_cast<const FillModel>(
                                    std::make_shared<const CrossSpreadFill>());

    // Clock, portfolio, context and the strategy itself all outlive a session.
    ReplayClock clock;
    Portfolio   portfolio(fills, config.execution_delay_nanos, config.costs);

    BacktestResult result;
    if (dates.empty()) return result;

    // Session and view are held here rather than inside the loop so that the
    // context never points at a destroyed object between sessions.
    std::shared_ptr<SessionData> session = source.load(dates.front());
    if (!session) throw std::runtime_error("run_backtest: first session failed to load");
    std::optional<MarketView> market;
    market.emplace(*session, clock);

    Ctx ctx(*session, *market, portfolio, underlying, spot, dates.front(), kISTOffsetSeconds,
            config.session_open_sec, config.rate, config.margin);
    ctx.set_calendar(&dates);

    StrategyTask task = strategy(ctx);
    if (!task.valid()) throw std::invalid_argument("run_backtest: strategy produced no task");
    task.resume();

    const std::int64_t margin_interval =
        static_cast<std::int64_t>(std::max(1, config.margin_sample_seconds)) * 1'000'000'000;

    std::size_t trades_seen = 0;

    for (const Date date : dates) {
        auto next = source.load(date);
        if (!next) continue;   // a gap in the data is not an error

        // Order matters: drop the old view, take the new session, build the new
        // view, then rebind. Nothing reads the context in between.
        market.reset();
        session = std::move(next);
        market.emplace(*session, clock);
        ctx.bind_session(*session, *market, date);

        Money     session_peak_margin{};
        Timestamp next_margin{std::numeric_limits<std::int64_t>::min()};

        EventCursor cursor(*session);
        Event       ev;
        Timestamp   last{std::numeric_limits<std::int64_t>::min()};

        while (cursor.next(ev)) {
            ++result.observations;
            if (ev.ts == last) continue;
            last = ev.ts;

            clock.advance_to(ev.ts);
            ++result.steps;

            portfolio.process_pending(*market);

            // Before anything reads a valuation. A carried mark is what keeps an
            // overnight position from reading as flat on the next session's first
            // observations, when its instrument has not yet printed.
            portfolio.refresh_marks(*market);

            portfolio.process_risk_rules(*market);

            if (ev.ts >= next_margin) {
                const MarginResult m = ctx.margin();
                if (m.total.minor > session_peak_margin.minor) session_peak_margin = m.total;
                next_margin = Timestamp{ev.ts.nanos + margin_interval};
            }

            const EvalCtx eval{&*market, &portfolio, ev.ts};
            int guard = 0;
            while (!task.done() && task.pending().valid()) {
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

        // --- end of session --------------------------------------------------

        const Timestamp settle_at =
            timestamp_of(date, config.settlement_second_of_day, kISTOffsetSeconds);
        if (settle_at > clock.now()) clock.advance_to(settle_at);

        std::size_t settled_today = 0;
        for (const std::int32_t yyyymmdd : due_expiries(portfolio, session->registry(), date)) {
            const Date expiry{yyyymmdd};

            if (config.expiry == ExpiryHandling::Forbid) {
                throw std::runtime_error(
                    "a position was still open at expiry and ExpiryHandling::Forbid is set");
            }

            const auto level = settlement_level(ctx, spot, expiry);
            if (!level) {
                // Refusing to invent a settlement level is the honest failure:
                // settling at a guessed number would put fabricated P&L into the
                // result and look exactly like a real trade.
                throw std::runtime_error(
                    "cannot settle expiry: no underlying level and no parity forward available");
            }
            settled_today += portfolio.settle_expiry(*market, expiry, *level, clock.now());
        }

        // Day orders. Anything unfilled expires with the session rather than
        // carrying into a price the strategy never saw.
        portfolio.cancel_working_orders();

        SessionSummary summary;
        summary.date             = date;
        summary.realized_to_date = portfolio.realized();
        summary.costs_to_date    = portfolio.costs();
        summary.equity           = portfolio.equity(*market);
        summary.peak_margin      = session_peak_margin;
        summary.trades           = portfolio.trades().size() - trades_seen;
        summary.settled_legs     = settled_today;
        trades_seen              = portfolio.trades().size();

        for (std::size_t i = 0; i < portfolio.size(); ++i) {
            if (portfolio.at(static_cast<PositionId>(i)).open()) ++summary.open_positions;
        }
        result.daily.push_back(summary);
        result.sessions.push_back(date);

        if (session_peak_margin.minor > result.peak_margin.minor) {
            result.peak_margin = session_peak_margin;
        }

    }

    result.strategy_finished = task.done();
    result.realized          = portfolio.realized();
    result.costs             = portfolio.costs();
    result.net_realized      = portfolio.net_realized();
    result.trades            = portfolio.trades().size();
    result.settled_legs      = portfolio.settled_legs();
    result.illiquid_fills    = portfolio.illiquid_fills();
    result.oversized_fills   = portfolio.oversized_fills();
    result.cancelled_orders  = portfolio.cancelled_orders();
    result.trade_log         = portfolio.trades();

    result.unrealized  = result.daily.empty() ? Money{}
                                              : result.daily.back().equity - result.net_realized;
    result.final_equity = result.net_realized + result.unrealized;

    for (std::size_t i = 0; i < portfolio.size(); ++i) {
        if (portfolio.at(static_cast<PositionId>(i)).open()) ++result.open_positions;
    }
    return result;
}

}  // namespace volforge
