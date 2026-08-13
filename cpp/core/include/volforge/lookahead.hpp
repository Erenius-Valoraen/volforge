// Whole-strategy look-ahead detection.
//
// The theorem this rests on: **a causal strategy's behaviour over [0, T] cannot
// depend on any data after T.** So truncate the session at T, replay, and compare
// the trades against the full run's trades up to T. If they differ in any field —
// which trades happened, when they were signalled, when they filled, at what
// price — something read the future.
//
// This catches leaks no unit test is aimed at, because it makes no assumption
// about *where* the leak is. An indicator averaging over the forming bar, a
// chain query selecting a strike that lists later in the day, a condition
// peeking at a bar before it closes, a fill taken from the print that triggered
// it — all of them change behaviour under truncation, and none of them need to
// be anticipated.
//
// Honest limitations, since a detector that overstates itself is worse than none:
//
//   - It only sees leaks that change *trades*. A strategy that computes a biased
//     signal but never acts on it passes.
//   - Coverage is only as good as the cutoffs sampled. A leak in a five-minute
//     window nobody truncated inside is invisible.
//   - A clean report means "no leak was observed at these cutoffs", never "this
//     strategy is causal".

#pragma once

#include "volforge/event_loop.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace volforge {

// A session clipped to observations at or before a cutoff.
//
// Nothing is copied: per-instrument columns are already sorted by time, so
// truncation is a prefix of each span.
class TruncatedSession final : public SessionData {
public:
    TruncatedSession(const SessionData& inner, Timestamp cutoff);

    [[nodiscard]] Date date() const override { return inner_->date(); }
    [[nodiscard]] const InstrumentRegistry& registry() const override { return inner_->registry(); }
    [[nodiscard]] std::span<const InstrumentId> instruments() const override { return kept_; }
    [[nodiscard]] QuoteColumns quotes(InstrumentId) const override;
    [[nodiscard]] std::size_t total_observations() const override { return total_; }
    // Deliberately none: the parent's cached order covers the untruncated
    // session, so the cursor merges instead.

    [[nodiscard]] Timestamp cutoff() const { return cutoff_; }

private:
    const SessionData*                        inner_;
    Timestamp                                 cutoff_;
    std::vector<InstrumentId>                 kept_;
    std::map<std::int32_t, std::size_t>       counts_;
    std::size_t                               total_ = 0;
};

struct LookaheadViolation {
    Timestamp   cutoff;
    std::string detail;
};

struct LookaheadReport {
    bool                            clean = true;
    std::size_t                     cutoffs_tested = 0;
    std::size_t                     trades_compared = 0;
    std::vector<LookaheadViolation> violations;
};

// Replays the strategy against progressively truncated sessions and compares.
//
// `cutoffs` sampling is deterministic in `seed`, so a failing report reproduces.
LookaheadReport check_lookahead(const SessionData& session, UnderlyingId underlying,
                                InstrumentId spot, const StrategyFn& strategy,
                                const RunConfig& config = {}, int cutoffs = 40,
                                std::uint64_t seed = 12345);

}  // namespace volforge
