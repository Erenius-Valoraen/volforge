#include "volforge/lookahead.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <random>
#include <set>

namespace volforge {

// ---------------------------------------------------------------------------
// TruncatedSession
// ---------------------------------------------------------------------------

TruncatedSession::TruncatedSession(const SessionData& inner, Timestamp cutoff)
    : inner_(&inner), cutoff_(cutoff) {
    for (const InstrumentId id : inner.instruments()) {
        const QuoteColumns cols = inner.quotes(id);
        if (cols.empty()) continue;

        const std::size_t idx = cols.index_at_or_before(cutoff);
        if (idx == QuoteColumns::npos) continue;   // nothing yet at the cutoff

        const std::size_t n = idx + 1;
        counts_.emplace(index_of(id), n);
        kept_.push_back(id);
        total_ += n;
    }
}

QuoteColumns TruncatedSession::quotes(InstrumentId id) const {
    const auto it = counts_.find(index_of(id));
    if (it == counts_.end()) return QuoteColumns{};

    const QuoteColumns full = inner_->quotes(id);
    const std::size_t n = it->second;
    return QuoteColumns{
        full.ts.first(n),      full.last.first(n),     full.bid.first(n),
        full.ask.first(n),     full.bid_qty.first(n),  full.ask_qty.first(n),
        full.last_qty.first(n), full.open_interest.first(n),
    };
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

namespace {

bool same_trade(const TradeRecord& a, const TradeRecord& b) {
    return a.signal_ts == b.signal_ts && a.fill_ts == b.fill_ts &&
           a.instrument == b.instrument && a.side == b.side && a.qty == b.qty &&
           a.price == b.price;
}

std::string describe(const TradeRecord& t) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "signal=%lld fill=%lld inst=%d %s qty=%d px=%.2f",
                  static_cast<long long>(t.signal_ts.seconds()),
                  static_cast<long long>(t.fill_ts.seconds()), index_of(t.instrument),
                  t.side == Side::Buy ? "buy" : "sell", t.qty, t.price.to_double());
    return buf;
}

}  // namespace

LookaheadReport check_lookahead(const SessionData& session, UnderlyingId underlying,
                                InstrumentId spot, const StrategyFn& strategy,
                                const RunConfig& config, int cutoffs, std::uint64_t seed) {
    LookaheadReport report;

    const RunResult full = run_session(session, underlying, spot, strategy, config);

    // Collect the session's distinct timestamps to sample cutoffs from. Cutting
    // between observations would test nothing the neighbouring cut does not.
    std::vector<Timestamp> times;
    {
        EventCursor cursor(session);
        Event ev;
        Timestamp last{std::numeric_limits<std::int64_t>::min()};
        while (cursor.next(ev)) {
            if (ev.ts == last) continue;
            last = ev.ts;
            times.push_back(ev.ts);
        }
    }
    if (times.size() < 2) return report;

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, times.size() - 1);

    std::set<std::int64_t> chosen;
    // Always cut immediately around every fill: if a fill can be influenced by
    // later data, the boundary next to it is where that shows up most sharply.
    for (const TradeRecord& t : full.trade_log) {
        const auto it = std::lower_bound(times.begin(), times.end(), t.fill_ts);
        if (it != times.end()) chosen.insert(it->nanos);
        if (it != times.begin()) chosen.insert(std::prev(it)->nanos);
    }
    while (static_cast<int>(chosen.size()) < cutoffs) {
        chosen.insert(times[pick(rng)].nanos);
        if (chosen.size() >= times.size()) break;
    }

    for (const std::int64_t nanos : chosen) {
        const Timestamp cutoff{nanos};
        ++report.cutoffs_tested;

        TruncatedSession clipped(session, cutoff);
        if (clipped.instruments().empty()) continue;

        const RunResult partial = run_session(clipped, underlying, spot, strategy, config);

        std::vector<TradeRecord> expected;
        for (const TradeRecord& t : full.trade_log) {
            if (t.fill_ts <= cutoff) expected.push_back(t);
        }

        report.trades_compared += expected.size();

        if (partial.trade_log.size() != expected.size()) {
            report.clean = false;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "truncated run produced %zu trades, full run had %zu by this point",
                          partial.trade_log.size(), expected.size());
            report.violations.push_back({cutoff, buf});
            continue;
        }

        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (same_trade(expected[i], partial.trade_log[i])) continue;
            report.clean = false;
            report.violations.push_back(
                {cutoff, "trade differs under truncation: full[" + describe(expected[i]) +
                             "] vs truncated[" + describe(partial.trade_log[i]) + "]"});
            break;
        }
    }

    return report;
}

}  // namespace volforge
