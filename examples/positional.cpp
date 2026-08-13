// A positional strategy across many sessions.
//
//   sell the ATM straddle in the weekly expiry, hold it overnight, and roll into
//   the next expiry rather than taking settlement
//
// The roll is ordinary strategy code. The engine has no idea what "roll" means —
// same strike, same moneyness, same delta are all defensible answers, and the
// answer differs per strategy — so it settles by default and gets out of the way
// when a strategy wants something else.
//
// Run against generated sessions, since the vendor format is not settled.

#include "volforge/backtest.hpp"
#include "volforge/synthetic.hpp"

#include <cstdio>

using namespace volforge;

namespace {

StrategyTask rolled_straddle(Ctx& ctx, double stop, int roll_days_before) {
    std::optional<PositionRef> held;
    std::optional<Date>        held_expiry;

    while (true) {
        co_await ctx.at("09:30");

        // A stop may have taken the position out overnight without the strategy
        // being anywhere near it. Notice that before deciding anything else.
        if (held && ctx.portfolio().at(held->id()).closed()) {
            held.reset();
            held_expiry.reset();
        }

        // Roll when the held expiry is close enough to matter.
        if (held && held_expiry) {
            const Date roll_on = add_days(*held_expiry, -roll_days_before);
            if (ctx.date() >= roll_on) {
                held->close();
                held.reset();
                held_expiry.reset();
            }
        }

        if (!held) {
            const auto expiry = ctx.next_expiry(roll_days_before + 1);
            if (expiry) {
                const auto legs = ctx.chain(*expiry).straddle();
                if (!legs.empty()) {
                    held = ctx.sell(legs, 1, "straddle");
                    if (held) {
                        held_expiry = expiry;
                        // Armed continuously from here, including overnight.
                        held->stop_loss(stop);
                    }
                }
            }
        }

        co_await ctx.at("15:20");
    }
}

}  // namespace

int main() {
    InstrumentRegistry registry;
    SyntheticSeriesConfig cfg;
    cfg.sessions = 20;
    const auto series = make_synthetic_series(registry, cfg);

    const auto result = run_backtest(
        *series.source, series.underlying, series.spot,
        [](Ctx& ctx) { return rolled_straddle(ctx, 0.60, 1); });

    std::printf("%zu sessions, %s to %s\n\n", result.sessions.size(),
                result.sessions.front().to_string().c_str(),
                result.sessions.back().to_string().c_str());

    std::printf("  observations   %12zu\n", result.observations);
    std::printf("  strategy wakes %12zu\n", result.resumes);
    std::printf("  trades         %12zu\n", result.trades);
    std::printf("  settled at expiry %9zu\n", result.settled_legs);
    std::printf("  cancelled orders  %9zu\n\n", result.cancelled_orders);

    std::printf("  gross realized %12.2f\n", result.realized.to_double());
    std::printf("  costs          %12.2f\n", -result.costs.to_double());
    std::printf("  net realized   %12.2f\n", result.net_realized.to_double());
    std::printf("  unrealized     %12.2f\n", result.unrealized.to_double());
    std::printf("  final equity   %12.2f\n", result.final_equity.to_double());
    std::printf("  peak margin    %12.2f\n", result.peak_margin.to_double());
    if (result.peak_margin.minor > 0) {
        std::printf("  return on peak %11.2f%%\n\n",
                    100.0 * result.final_equity.to_double() / result.peak_margin.to_double());
    }

    std::printf("  %-12s %10s %10s %8s %6s\n", "session", "equity", "margin", "trades", "open");
    for (const SessionSummary& s : result.daily) {
        std::printf("  %-12s %10.0f %10.0f %8zu %6zu\n", s.date.to_string().c_str(),
                    s.equity.to_double(), s.peak_margin.to_double(), s.trades,
                    s.open_positions);
    }
    return 0;
}
