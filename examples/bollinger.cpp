// Bollinger breakout on one-minute bars, run under both confirmation policies,
// then checked for look-ahead.
//
// The same strategy source, changing only `confirm`:
//
//   Instant    enter at the exact second spot crosses the upper band, compared
//              against the last *completed* band. If the minute closes back
//              inside the band, the trade still happened.
//
//   BarClose   enter only if a one-minute bar *closes* above the band. The
//              signal fires at open_time + 60s, so a bar that pokes through and
//              retreats produces nothing.
//
// Both are legitimate. They produce different trades, which is the point.

#include "volforge/lookahead.hpp"
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

StrategyTask breakout(Ctx& ctx, Confirm confirm) {
    const auto m1 = ctx.spot_bars(60);
    const auto bb = bollinger(m1, 20, 2.0);

    // Wait for the band to warm up. Before 20 bars have closed the band is
    // undefined, and cross_above reports nothing rather than comparing against
    // a partial statistic wearing the same name.
    co_await ctx.at("09:40");

    co_await (ctx.cross_above(bb.upper, confirm) | ctx.at("15:00"));
    if (ctx.now() >= timestamp_of(ctx.date(), 15 * 3600, kISTOffsetSeconds)) co_return;

    const auto k = ctx.chain().atm_strike();
    if (!k) co_return;
    const auto call = ctx.chain().option(*k, Right::Call);
    if (!call) co_return;

    const auto pos = ctx.buy({*call}, 1, "breakout call");
    if (!pos) co_return;

    co_await (pos->pnl_pct_at_least(0.25) | pos->pnl_pct_at_most(-0.20)
              | ctx.after(600) | ctx.at("15:15"));
    pos->close();
    co_await (pos->closed() | ctx.at("15:29"));
}

void report(const char* name, const SyntheticSession& synth, InstrumentRegistry& registry,
            Confirm confirm) {
    const StrategyFn fn = [confirm](Ctx& ctx) { return breakout(ctx, confirm); };

    const auto result = run_session(*synth.data, synth.underlying, synth.spot, fn);

    std::printf("%s\n", name);
    std::printf("  resumes %zu   trades %zu   realized %10.2f\n",
                result.resumes, result.trades, result.realized.to_double());

    for (const TradeRecord& t : result.trade_log) {
        const InstrumentSpec& s = registry.spec(t.instrument);
        std::printf("    signal %s  fill %s  %-4s %.0f %s  @ %8.2f\n",
                    clock_time(t.signal_ts).c_str(), clock_time(t.fill_ts).c_str(),
                    t.side == Side::Buy ? "buy" : "sell", s.strike.to_double(),
                    to_string(s.right), t.price.to_double());
    }

    const auto audit = check_lookahead(*synth.data, synth.underlying, synth.spot, fn, {}, 60);
    std::printf("  look-ahead: %s  (%zu cutoffs, %zu trade comparisons)\n",
                audit.clean ? "CLEAN" : "VIOLATIONS FOUND", audit.cutoffs_tested,
                audit.trades_compared);
    for (const auto& v : audit.violations) {
        std::printf("    at %s: %s\n", clock_time(v.cutoff).c_str(), v.detail.c_str());
    }
    std::printf("\n");
}

}  // namespace

int main() {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    std::printf("session %s, bollinger(20, 2.0) on 1-minute spot bars\n\n",
                synth.data->date().to_string().c_str());

    report("instant execution  (fill at the second of the cross)", synth, registry,
           Confirm::Instant);
    report("close confirmation (fill after the bar closes above)", synth, registry,
           Confirm::BarClose);

    return 0;
}
