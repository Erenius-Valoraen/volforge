#include "volforge/strategy.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace volforge {

// ---------------------------------------------------------------------------
// ChainView
// ---------------------------------------------------------------------------

ChainView::ChainView(const Ctx& ctx, Date expiry) : ctx_(&ctx), expiry_(expiry) {
    for (const InstrumentSpec& s : ctx.registry().all()) {
        if (!s.is_option()) continue;
        if (s.underlying != ctx.underlying()) continue;
        if (s.expiry != expiry) continue;
        strikes_.push_back(s.strike);
    }
    std::sort(strikes_.begin(), strikes_.end());
    strikes_.erase(std::unique(strikes_.begin(), strikes_.end()), strikes_.end());
}

std::optional<Price> ChainView::atm_strike() const {
    const auto spot = ctx_->spot_price();
    if (!spot || strikes_.empty()) return std::nullopt;

    // Nearest listed strike, not nearest round number — the chain decides what
    // exists, and a strategy asking for "the money" means a tradeable strike.
    auto best = strikes_.begin();
    auto best_gap = std::abs(best->minor - spot->minor);
    for (auto it = strikes_.begin(); it != strikes_.end(); ++it) {
        const auto gap = std::abs(it->minor - spot->minor);
        if (gap < best_gap) { best = it; best_gap = gap; }
    }
    return *best;
}

std::optional<Price> ChainView::strike_at(int offset) const {
    const auto atm = atm_strike();
    if (!atm) return std::nullopt;

    const auto it = std::lower_bound(strikes_.begin(), strikes_.end(), *atm);
    const auto idx = static_cast<std::ptrdiff_t>(std::distance(strikes_.begin(), it)) + offset;
    if (idx < 0 || static_cast<std::size_t>(idx) >= strikes_.size()) return std::nullopt;
    return strikes_[static_cast<std::size_t>(idx)];
}

std::optional<InstrumentId> ChainView::option(Price strike, Right right) const {
    return ctx_->registry().find_option(ctx_->underlying(), expiry_, strike, right);
}

std::vector<InstrumentId> ChainView::straddle(int offset) const {
    const auto k = strike_at(offset);
    if (!k) return {};
    const auto call = option(*k, Right::Call);
    const auto put  = option(*k, Right::Put);
    // A half-built straddle is a different position than the one asked for, so
    // an incomplete structure is refused rather than silently traded.
    if (!call || !put) return {};
    return {*call, *put};
}

std::vector<InstrumentId> ChainView::strangle(int width) const {
    const auto kc = strike_at(+width);
    const auto kp = strike_at(-width);
    if (!kc || !kp) return {};
    const auto call = option(*kc, Right::Call);
    const auto put  = option(*kp, Right::Put);
    if (!call || !put) return {};
    return {*call, *put};
}

// ---------------------------------------------------------------------------
// Ctx
// ---------------------------------------------------------------------------

Ctx::Ctx(const MarketView& market, Portfolio& portfolio, const InstrumentRegistry& registry,
         UnderlyingId underlying, InstrumentId spot, Date date, int utc_offset_seconds)
    : market_(&market), portfolio_(&portfolio), registry_(&registry),
      underlying_(underlying), spot_(spot), date_(date), utc_offset_(utc_offset_seconds) {}

std::optional<Price> Ctx::spot_price() const {
    const auto q = market_->quote(spot_);
    if (!q) return std::nullopt;
    return q->last;
}

std::vector<Date> Ctx::expiries() const { return registry_->expiries(underlying_); }

ChainView Ctx::chain(Date expiry) const { return ChainView(*this, expiry); }

ChainView Ctx::chain() const {
    const auto all = expiries();
    for (const Date d : all) {
        if (d >= date_) return ChainView(*this, d);
    }
    if (all.empty()) throw std::runtime_error("Ctx::chain: no expiries for underlying");
    return ChainView(*this, all.back());
}

Qty Ctx::lot_size(InstrumentId id) const { return registry_->spec(id).lot_size; }

namespace {

Qty contracts_for(const InstrumentRegistry& reg, const std::vector<InstrumentId>& legs, int lots) {
    if (legs.empty()) throw std::invalid_argument("order has no legs");
    if (lots <= 0) throw std::invalid_argument("lots must be positive");

    const Qty lot = reg.spec(legs.front()).lot_size;
    for (const InstrumentId id : legs) {
        if (reg.spec(id).lot_size != lot) {
            // Mixing lot sizes across legs makes "lots" ambiguous, and guessing
            // is how position sizing silently goes wrong.
            throw std::invalid_argument("legs have differing lot sizes; size them explicitly");
        }
    }
    return static_cast<Qty>(lot * lots);
}

}  // namespace

std::optional<PositionId> Ctx::sell(const std::vector<InstrumentId>& legs, int lots,
                                    std::string label) {
    if (legs.empty()) return std::nullopt;
    const Qty qty = contracts_for(*registry_, legs, lots);
    return portfolio_->submit_open(std::move(label), legs, Side::Sell, qty, now());
}

std::optional<PositionId> Ctx::buy(const std::vector<InstrumentId>& legs, int lots,
                                   std::string label) {
    if (legs.empty()) return std::nullopt;
    const Qty qty = contracts_for(*registry_, legs, lots);
    return portfolio_->submit_open(std::move(label), legs, Side::Buy, qty, now());
}

void Ctx::close(PositionId id) { portfolio_->submit_close(id, now()); }

void Ctx::close_leg(PositionId id, std::size_t leg_index) {
    portfolio_->submit_close_leg(id, leg_index, now());
}

Cond Ctx::at(std::string_view hhmm) const {
    return at_or_after(timestamp_of(date_, parse_time_of_day(hhmm), utc_offset_));
}

Cond Ctx::pnl_pct_at_most(PositionId id, double pct) const {
    return Cond([id, pct](const EvalCtx& c) {
        const Position& p = c.portfolio->at(id);
        // Before every leg has filled there is no meaningful P&L to compare, and
        // treating an unfilled position as flat would fire a stop instantly.
        if (!p.established()) return false;
        return p.pnl_pct(*c.market) <= pct;
    });
}

Cond Ctx::pnl_pct_at_least(PositionId id, double pct) const {
    return Cond([id, pct](const EvalCtx& c) {
        const Position& p = c.portfolio->at(id);
        if (!p.established()) return false;
        return p.pnl_pct(*c.market) >= pct;
    });
}

Cond Ctx::position_closed(PositionId id) const {
    return Cond([id](const EvalCtx& c) { return c.portfolio->at(id).closed(); });
}

}  // namespace volforge
