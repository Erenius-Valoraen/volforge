#include "volforge/strategy.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace volforge {

// ---------------------------------------------------------------------------
// ChainView
// ---------------------------------------------------------------------------

ChainView::ChainView(const Ctx& ctx, Date expiry) : ctx_(&ctx), expiry_(expiry) {
    for (const InstrumentSpec& s : ctx.registry().all()) {
        if (!s.is_option()) continue;
        if (s.underlying != ctx.underlying()) continue;
        if (s.expiry != expiry) continue;
        // As-of the decision: a strike listed later in the session does not
        // exist yet, and picking it because the registry knows about it would be
        // look-ahead in the instrument universe rather than in the prices.
        if (!ctx.market().has_quote(s.id)) continue;
        strikes_.push_back(s.strike);
    }
    std::sort(strikes_.begin(), strikes_.end());
    strikes_.erase(std::unique(strikes_.begin(), strikes_.end()), strikes_.end());
}

std::optional<Price> ChainView::atm_strike() const {
    const auto spot = ctx_->spot_price();
    if (!spot || strikes_.empty()) return std::nullopt;

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
    const auto id = ctx_->registry().find_option(ctx_->underlying(), expiry_, strike, right);
    if (!id) return std::nullopt;
    if (!ctx_->market().has_quote(*id)) return std::nullopt;   // not tradeable yet
    return id;
}

std::vector<InstrumentId> ChainView::straddle(int offset) const {
    const auto k = strike_at(offset);
    if (!k) return {};
    const auto call = option(*k, Right::Call);
    const auto put  = option(*k, Right::Put);
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

Ctx::Ctx(const SessionData& session, const MarketView& market, Portfolio& portfolio,
         UnderlyingId underlying, InstrumentId spot, Date date, int utc_offset_seconds,
         int session_open_sec)
    : session_(&session), market_(&market), portfolio_(&portfolio),
      underlying_(underlying), spot_(spot), date_(date), utc_offset_(utc_offset_seconds),
      anchor_(timestamp_of(date, session_open_sec, utc_offset_seconds)) {}

std::optional<Price> Ctx::spot_price() const {
    const auto q = market_->quote(spot_);
    if (!q) return std::nullopt;
    return q->last;
}

std::vector<Date> Ctx::expiries() const { return registry().expiries(underlying_); }

ChainView Ctx::chain(Date expiry) const { return ChainView(*this, expiry); }

ChainView Ctx::chain() const {
    const auto all = expiries();
    for (const Date d : all) {
        if (d >= date_) return ChainView(*this, d);
    }
    if (all.empty()) throw std::runtime_error("Ctx::chain: no expiries for underlying");
    return ChainView(*this, all.back());
}

std::shared_ptr<const BarSeries> Ctx::bars(InstrumentId id, int interval_seconds,
                                           BarPrice source) const {
    if (interval_seconds <= 0) throw std::invalid_argument("Ctx::bars: interval must be positive");

    const auto key = std::make_tuple(index_of(id), interval_seconds, static_cast<int>(source));
    if (const auto it = bar_cache_.find(key); it != bar_cache_.end()) return it->second;

    auto series = std::make_shared<const BarSeries>(BarSeries::build(
        session_->quotes(id), static_cast<std::int64_t>(interval_seconds) * 1'000'000'000,
        anchor_, source));
    bar_cache_.emplace(key, series);
    return series;
}

std::shared_ptr<const BarSeries> Ctx::spot_bars(int interval_seconds) const {
    return bars(spot_, interval_seconds, BarPrice::Last);
}

Qty Ctx::lot_size(InstrumentId id) const { return registry().spec(id).lot_size; }

namespace {

Qty contracts_for(const InstrumentRegistry& reg, const std::vector<InstrumentId>& legs, int lots) {
    if (legs.empty()) throw std::invalid_argument("order has no legs");
    if (lots <= 0) throw std::invalid_argument("lots must be positive");

    const Qty lot = reg.spec(legs.front()).lot_size;
    for (const InstrumentId id : legs) {
        if (reg.spec(id).lot_size != lot) {
            throw std::invalid_argument("legs have differing lot sizes; size them explicitly");
        }
    }
    return static_cast<Qty>(lot * lots);
}

}  // namespace

std::optional<PositionRef> Ctx::sell(const std::vector<InstrumentId>& legs, int lots,
                                     std::string label) {
    if (legs.empty()) return std::nullopt;
    const Qty qty = contracts_for(registry(), legs, lots);
    return PositionRef(this, portfolio_->submit_open(std::move(label), legs, Side::Sell, qty,
                                                     now()));
}

std::optional<PositionRef> Ctx::buy(const std::vector<InstrumentId>& legs, int lots,
                                    std::string label) {
    if (legs.empty()) return std::nullopt;
    const Qty qty = contracts_for(registry(), legs, lots);
    return PositionRef(this, portfolio_->submit_open(std::move(label), legs, Side::Buy, qty,
                                                     now()));
}

// ---------------------------------------------------------------------------
// Position handles
// ---------------------------------------------------------------------------

std::size_t PositionRef::leg_count() const {
    return ctx_->portfolio().at(id_).legs().size();
}

void PositionRef::stop_loss(double pct) const {
    ctx_->portfolio().attach_stop_loss(id_, std::nullopt, pct);
}
void PositionRef::take_profit(double pct) const {
    ctx_->portfolio().attach_take_profit(id_, std::nullopt, pct);
}
void PositionRef::exit_at(std::string_view hhmm) const {
    ctx_->portfolio().attach_exit_at(
        id_, std::nullopt,
        timestamp_of(ctx_->date(), parse_time_of_day(hhmm), kISTOffsetSeconds));
}
void PositionRef::close() const { ctx_->close(id_); }

Cond PositionRef::closed() const { return ctx_->position_closed(id_); }
Cond PositionRef::pnl_pct_at_most(double pct) const { return ctx_->pnl_pct_at_most(id_, pct); }
Cond PositionRef::pnl_pct_at_least(double pct) const { return ctx_->pnl_pct_at_least(id_, pct); }

void LegRef::stop_loss(double pct) const { ctx_->portfolio().attach_stop_loss(pos_, leg_, pct); }
void LegRef::take_profit(double pct) const { ctx_->portfolio().attach_take_profit(pos_, leg_, pct); }
void LegRef::close() const { ctx_->close_leg(pos_, leg_); }

void Ctx::close(PositionId id) { portfolio_->submit_close(id, now()); }

void Ctx::close_leg(PositionId id, std::size_t leg_index) {
    portfolio_->submit_close_leg(id, leg_index, now());
}

Cond Ctx::at(std::string_view hhmm) const {
    return at_or_after(timestamp_of(date_, parse_time_of_day(hhmm), utc_offset_));
}

Cond Ctx::after(int seconds) const {
    return at_or_after(Timestamp{now().nanos + static_cast<std::int64_t>(seconds) * 1'000'000'000});
}

Cond Ctx::pnl_pct_at_most(PositionId id, double pct) const {
    return Cond([id, pct](const EvalCtx& c) {
        const Position& p = c.portfolio->at(id);
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

// ---------------------------------------------------------------------------
// Crossings
// ---------------------------------------------------------------------------

namespace {

// Instant crossing, defined purely from data so it needs no evaluation history.
//
// A stateful "was below, now above" flag would be wrong here: conditions are
// short-circuited inside `a | b`, so a flag on `b` would miss transitions
// whenever `a` happened to be true. Comparing two adjacent observations, each
// against the level knowable *at that observation*, is stateless and exact.
Cond instant_cross(InstrumentId inst, IndicatorPtr level, bool above) {
    return Cond([inst, level, above](const EvalCtx& c) {
        const auto now_q  = c.market->quote(inst, 0);
        const auto prev_q = c.market->quote(inst, 1);
        if (!now_q || !prev_q) return false;

        const auto lvl_now  = level->at(c.now, 0);
        const auto lvl_prev = level->at(prev_q->ts, 0);
        if (!lvl_now || !lvl_prev) return false;

        const double p_now  = now_q->last.to_double();
        const double p_prev = prev_q->last.to_double();

        return above ? (p_prev <= *lvl_prev && p_now > *lvl_now)
                     : (p_prev >= *lvl_prev && p_now < *lvl_now);
    });
}

// Bar-close confirmation: the most recently *completed* bar closed beyond the
// level while the one before it did not.
//
// This becomes true when that bar closes — open_time + interval — so an order
// submitted on it fills at the next observation after the close, never at any
// price from inside the bar that produced the signal.
Cond bar_close_cross(IndicatorPtr level, bool above) {
    return Cond([level, above](const EvalCtx& c) {
        const BarSeries& bars = level->bars();
        const Bar* b0 = bars.completed(c.now, 0);
        const Bar* b1 = bars.completed(c.now, 1);
        if (!b0 || !b1) return false;

        const auto l0 = level->at(c.now, 0);
        const auto l1 = level->at(c.now, 1);
        if (!l0 || !l1) return false;

        const double c0 = b0->close.to_double();
        const double c1 = b1->close.to_double();

        return above ? (c1 <= *l1 && c0 > *l0) : (c1 >= *l1 && c0 < *l0);
    });
}

}  // namespace

Cond Ctx::cross_above(IndicatorPtr level, Confirm confirm) const {
    if (!level) throw std::invalid_argument("cross_above: null indicator");
    return confirm == Confirm::Instant ? instant_cross(spot_, std::move(level), true)
                                       : bar_close_cross(std::move(level), true);
}

Cond Ctx::cross_below(IndicatorPtr level, Confirm confirm) const {
    if (!level) throw std::invalid_argument("cross_below: null indicator");
    return confirm == Confirm::Instant ? instant_cross(spot_, std::move(level), false)
                                       : bar_close_cross(std::move(level), false);
}

}  // namespace volforge
