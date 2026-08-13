// A strategy running end to end.
//
//   sell the at-the-money straddle at 09:20
//   exit on a 30% loss, a 50% gain, or 15:15 — whichever comes first
//
// Run against generated data, since the vendor format is not settled. The point
// is the mechanism, not the P&L: watch the signal and fill timestamps in the
// trade log differ on every row.

#include "volforge/event_loop.hpp"
#include "volforge/synthetic.hpp"

#include <cstdio>
#include <string>

using namespace volforge;

namespace {

std::string clock_time(Timestamp ts) {
    const std::int64_t local = ts.seconds() + kISTOffsetSeconds;
    const std::int64_t sod   = ((local % 86400) + 86400) % 86400;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", sod / 3600, (sod / 60) % 60, sod % 60);
    return buf;
}

StrategyTask short_straddle(Ctx& ctx, double stop, double target) {
    co_await ctx.at("09:20");

    const auto legs = ctx.chain().straddle();
    if (legs.empty()) co_return;

    const auto pos = ctx.sell(legs, 1, "atm straddle");
    if (!pos) co_return;

    co_await (pos->pnl_pct_at_most(-stop)
              | pos->pnl_pct_at_least(target)
              | ctx.at("15:15"));

    pos->close();
    co_await (pos->closed() | ctx.at("15:29"));
}

}  // namespace

int main() {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    const double stop = 0.30, target = 0.50;

    const auto result = run_session(
        *synth.data, synth.underlying, synth.spot,
        [&](Ctx& ctx) { return short_straddle(ctx, stop, target); });

    std::printf("session %s\n\n", result.date.to_string().c_str());

    std::printf("  observations      %10zu\n", result.observations);
    std::printf("  timestamps        %10zu\n", result.steps);
    std::printf("  condition evals   %10zu\n", result.condition_evals);
    std::printf("  strategy resumes  %10zu   <- times Python-equivalent code ran\n",
                result.resumes);
    std::printf("  trades            %10zu\n", result.trades);
    std::printf("  illiquid fills    %10zu\n", result.illiquid_fills);
    std::printf("  oversized fills   %10zu\n", result.oversized_fills);
    std::printf("  unfilled orders   %10zu\n", result.unfilled_orders);
    std::printf("  open at close     %10zu\n", result.open_positions);
    std::printf("  strategy finished %10s\n\n", result.strategy_finished ? "yes" : "no");

    std::printf("  gross realized %11.2f\n", result.realized.to_double());
    std::printf("  costs          %11.2f\n", -result.costs.to_double());
    std::printf("  net realized   %11.2f\n", result.net_realized.to_double());
    std::printf("  unrealized     %11.2f\n", result.unrealized.to_double());
    std::printf("  final equity   %11.2f\n\n", result.final_equity.to_double());

    // The two timestamp columns are the point. A decision at 09:20:00 does not
    // execute at 09:20:00 — it executes against the next observation.
    std::printf("  %-9s %-9s %-5s %-9s %6s %5s %10s\n",
                "signal", "fill", "side", "strike", "right", "qty", "price");
    for (const TradeRecord& t : result.trade_log) {
        const InstrumentSpec& s = registry.spec(t.instrument);
        std::printf("  %-9s %-9s %-5s %9.0f %6s %5d %10.2f%s\n",
                    clock_time(t.signal_ts).c_str(), clock_time(t.fill_ts).c_str(),
                    t.side == Side::Buy ? "buy" : "sell",
                    s.strike.to_double(), to_string(s.right), t.qty, t.price.to_double(),
                    t.illiquid ? "   (wide spread)" : "");
    }
    return result.strategy_finished ? 0 : 1;
}
