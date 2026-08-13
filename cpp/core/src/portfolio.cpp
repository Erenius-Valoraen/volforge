#include "volforge/portfolio.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace volforge {

// ---------------------------------------------------------------------------
// Fills
// ---------------------------------------------------------------------------

std::optional<Price> CrossSpreadFill::fill(const Quote& q, Side side, Qty) const {
    // No two-sided market means no execution. Reporting a fill against a single
    // side would invent liquidity, and one-sided books are common on the wings.
    if (!q.two_sided()) return std::nullopt;
    if (q.ask < q.bid) return std::nullopt;   // crossed book, treat as untradeable
    return side == Side::Buy ? q.ask : q.bid;
}

// ---------------------------------------------------------------------------
// Position
// ---------------------------------------------------------------------------

bool Position::established() const {
    // Cancelled legs are not part of the position — an order that never filled
    // is not a holding. Requiring them to be "filled" would leave a position
    // with one cancelled leg permanently unestablished, which in turn would
    // disable every percentage stop on it while its surviving leg ran naked.
    bool any = false;
    for (const Leg& l : legs_) {
        if (l.cancelled()) continue;
        if (!l.filled()) return false;   // still working; not established yet
        any = true;
    }
    return any;
}

bool Position::open() const {
    return std::any_of(legs_.begin(), legs_.end(),
                       [](const Leg& l) { return l.live() || l.state == LegState::PendingOpen; });
}

bool Position::closed() const { return established() && !open(); }

Money Position::entry_notional() const {
    Money m{};
    for (const Leg& l : legs_) {
        if (!l.filled()) continue;
        m = m + notional(l.entry, l.qty);
    }
    return m;
}

// The price this leg would close at, or a negative sentinel when nothing is
// available. Never mid: a short marks at the ask it is bought back at, a long at
// the bid it is sold into.
Price Position::mark_for(const Leg& l, const MarketView& market) {
    const auto q = market.quote(l.instrument);

    if (q) {
        const Price side = l.qty < 0 ? q->ask : q->bid;
        if (side.minor > 0) return side;

        if (l.qty > 0) {
            // A long with no bid is worthless, and that is a real mark. Falling
            // back here would delete a total loss.
            return Price::from_minor(0);
        }
        // A short with no offer cannot be bought back at zero; claiming so would
        // book the whole premium as profit.
        if (q->last.minor > 0) return q->last;
    }

    // Nothing quoting: a quiet instrument, or a session that has not reached it
    // yet. Carry the last mark rather than valuing the position at nothing.
    if (l.last_mark_ts.nanos != 0 && l.last_mark.minor >= 0) return l.last_mark;
    if (l.filled()) return l.entry;
    return Price::from_minor(-1);
}

Money Position::pnl_of(std::size_t i, const MarketView& market) const {
    const Leg& l = legs_.at(i);
    if (!l.filled()) return Money{};

    if (l.state == LegState::Closed) {
        // Realised: what came in at entry, less what went out at exit.
        return notional(l.exit, l.qty) - notional(l.entry, l.qty);
    }

    Price mark = mark_for(l, market);
    if (mark.minor < 0) return Money{};   // no valuation available at all
    return notional(mark, l.qty) - notional(l.entry, l.qty);
}

double Position::pnl_pct_of(std::size_t i, const MarketView& market) const {
    const Leg& l = legs_.at(i);
    if (!l.filled()) return 0.0;
    const auto risk = std::abs(notional(l.entry, l.qty).minor);
    if (risk == 0) return 0.0;
    return static_cast<double>(pnl_of(i, market).minor) / static_cast<double>(risk);
}

Money Position::pnl(const MarketView& market) const {
    Money total{};
    for (std::size_t i = 0; i < legs_.size(); ++i) total = total + pnl_of(i, market);
    return total;
}

double Position::pnl_pct(const MarketView& market) const {
    if (!established()) return 0.0;
    const auto risk = std::abs(entry_notional().minor);
    if (risk == 0) return 0.0;
    return static_cast<double>(pnl(market).minor) / static_cast<double>(risk);
}

// ---------------------------------------------------------------------------
// Portfolio
// ---------------------------------------------------------------------------

Portfolio::Portfolio(std::shared_ptr<const FillModel> fills, std::int64_t execution_delay_nanos,
                     std::shared_ptr<const CostPolicy> costs)
    : fills_(std::move(fills)), delay_(execution_delay_nanos),
      costs_(costs ? std::move(costs) : default_cost_policy()) {
    if (!fills_) throw std::invalid_argument("Portfolio: null fill model");
    if (delay_ < 0) throw std::invalid_argument("Portfolio: negative execution delay");
}

void Portfolio::submit(PendingOrder order) {
    order.from_rule = closing_from_rule_;
    pending_.push_back(order);
}

PositionId Portfolio::submit_open(std::string label,
                                  const std::vector<InstrumentId>& instruments,
                                  Side side, Qty qty_per_leg, Timestamp now) {
    if (instruments.empty()) throw std::invalid_argument("submit_open: no legs");
    if (qty_per_leg <= 0) throw std::invalid_argument("submit_open: quantity must be positive");

    const auto id = static_cast<PositionId>(positions_.size());
    positions_.emplace_back(id, std::move(label));
    Position& pos = positions_.back();

    const Qty signed_qty = side == Side::Sell ? -qty_per_leg : qty_per_leg;

    for (const InstrumentId inst : instruments) {
        Leg leg;
        leg.instrument = inst;
        leg.qty        = signed_qty;
        leg.state      = LegState::PendingOpen;
        pos.legs().push_back(leg);

        submit(PendingOrder{id, pos.legs().size() - 1, inst, side, qty_per_leg, now,
                            Timestamp{now.nanos + delay_}, false});
    }
    return id;
}

void Portfolio::submit_close(PositionId id, Timestamp now) {
    Position& pos = at(id);

    // Cancel opening orders that have not filled. Leaving them queued would let
    // a leg open *after* the position was closed, silently leaving a naked
    // position the strategy never asked for and never manages.
    std::vector<PendingOrder> keep;
    keep.reserve(pending_.size());
    for (const PendingOrder& o : pending_) {
        if (o.position == id && !o.closing) {
            pos.legs()[o.leg_index].state = LegState::Cancelled;
            ++cancelled_orders_;
            continue;
        }
        keep.push_back(o);
    }
    pending_.swap(keep);

    for (std::size_t i = 0; i < pos.legs().size(); ++i) {
        if (pos.legs()[i].state == LegState::Open) submit_close_leg(id, i, now);
    }
}

void Portfolio::submit_close_leg(PositionId id, std::size_t leg_index, Timestamp now) {
    Position& pos = at(id);
    Leg& leg = pos.legs().at(leg_index);
    if (leg.state != LegState::Open) return;

    leg.state = LegState::PendingClose;
    submit(PendingOrder{id, leg_index, leg.instrument, leg.closing_side(),
                        static_cast<Qty>(std::abs(leg.qty)), now,
                        Timestamp{now.nanos + delay_}, true});
}

void Portfolio::process_pending(const MarketView& market) {
    if (pending_.empty()) return;

    const Timestamp now = market.now();
    std::vector<PendingOrder> still_pending;
    still_pending.reserve(pending_.size());

    for (const PendingOrder& order : pending_) {
        if (now < order.eligible_at) {           // not yet executable
            still_pending.push_back(order);
            continue;
        }

        const auto q = market.quote(order.instrument);
        if (!q) {                                 // instrument has not printed
            still_pending.push_back(order);
            continue;
        }

        const auto price = fills_->fill(*q, order.side, order.qty);
        if (!price) {                             // no tradeable market; rest
            still_pending.push_back(order);
            continue;
        }

        const double mid = q->mid().to_double();
        const bool illiquid =
            mid > 0.0 && q->spread().to_double() / mid > fills_->illiquid_spread_ratio();
        if (illiquid) ++illiquid_fills_;

        // Displayed size at the touch. The feed carries top of book only, so an
        // order larger than what is shown would in reality walk into levels we
        // cannot see. It is filled at the touch and flagged, because silently
        // filling size that was never displayed is a liquidity claim the data
        // does not support.
        const Qty displayed = order.side == Side::Buy ? q->ask_qty : q->bid_qty;
        const bool oversized = displayed > 0 && order.qty > displayed;
        if (oversized) ++oversized_fills_;

        const CostContext cc{&market.registry().spec(order.instrument), *price,
                             order.qty, order.side, false, Price{}};
        const Money cost = costs_->cost_of(cc);
        total_costs_ = total_costs_ + cost;

        Position& pos = positions_[static_cast<std::size_t>(order.position)];
        Leg& leg = pos.legs()[order.leg_index];

        if (order.closing) {
            leg.exit    = *price;
            leg.exit_ts = now;
            leg.state   = LegState::Closed;
            realized_   = realized_ + (notional(leg.exit, leg.qty) - notional(leg.entry, leg.qty));
        } else {
            leg.entry    = *price;
            leg.entry_ts = now;
            leg.state    = LegState::Open;
        }

        trades_.push_back(TradeRecord{order.submitted_at, now, order.position, order.instrument,
                                      order.side, order.qty, *price, cost, illiquid,
                                      order.from_rule, oversized});
    }

    pending_.swap(still_pending);
}

// ---------------------------------------------------------------------------
// Marks, settlement and end of day
// ---------------------------------------------------------------------------

void Portfolio::refresh_marks(const MarketView& market) {
    const Timestamp now = market.now();
    for (Position& pos : positions_) {
        for (Leg& leg : pos.legs()) {
            if (!leg.live()) continue;
            const auto q = market.quote(leg.instrument);
            if (!q) continue;

            const Price side = leg.qty < 0 ? q->ask : q->bid;
            if (side.minor > 0) {
                leg.last_mark = side;
            } else if (leg.qty > 0) {
                leg.last_mark = Price::from_minor(0);   // a long with no bid is worthless
            } else if (q->last.minor > 0) {
                leg.last_mark = q->last;                // short with no offer
            } else {
                continue;                               // nothing usable; keep the old mark
            }
            leg.last_mark_ts = now;
        }
    }
}

std::size_t Portfolio::settle_expiry(const MarketView& market, Date expiry,
                                     double settlement_level, Timestamp when) {
    std::size_t settled = 0;

    for (Position& pos : positions_) {
        for (std::size_t i = 0; i < pos.legs().size(); ++i) {
            Leg& leg = pos.legs()[i];
            if (!leg.live()) continue;

            const InstrumentSpec& spec = market.registry().spec(leg.instrument);
            if (!spec.is_option() || spec.expiry != expiry) continue;

            const double strike = spec.strike.to_double();
            const double value  = spec.right == Right::Call
                                      ? std::max(0.0, settlement_level - strike)
                                      : std::max(0.0, strike - settlement_level);
            const Price settle_px = Price::from_double(value);

            leg.exit     = settle_px;
            leg.exit_ts  = when;
            leg.state    = LegState::Closed;
            leg.settled  = true;
            realized_ = realized_ + (notional(settle_px, leg.qty) - notional(leg.entry, leg.qty));

            // STT on exercise falls on the buyer of an in-the-money option and is
            // charged on intrinsic value. A writer being assigned pays none of it.
            Money cost{};
            if (leg.qty > 0 && value > 0.0) {
                const CostContext cc{&spec, settle_px, leg.qty, Side::Sell, true, settle_px};
                cost = costs_->cost_of(cc);
                total_costs_ = total_costs_ + cost;
            }

            TradeRecord rec;
            rec.signal_ts  = when;
            rec.fill_ts    = when;
            rec.position   = pos.id();
            rec.instrument = leg.instrument;
            rec.side       = leg.closing_side();
            rec.qty        = static_cast<Qty>(std::abs(leg.qty));
            rec.price      = settle_px;
            rec.cost       = cost;
            rec.settled    = true;
            trades_.push_back(rec);

            ++settled;
            ++settled_legs_;
        }
    }

    // Orders working against an instrument that has just settled can never fill.
    std::vector<PendingOrder> keep;
    keep.reserve(pending_.size());
    for (const PendingOrder& o : pending_) {
        const InstrumentSpec& spec = market.registry().spec(o.instrument);
        if (spec.is_option() && spec.expiry == expiry) {
            Leg& leg = positions_[static_cast<std::size_t>(o.position)].legs()[o.leg_index];
            if (!o.closing && leg.state == LegState::PendingOpen) leg.state = LegState::Cancelled;
            ++cancelled_orders_;
            continue;
        }
        keep.push_back(o);
    }
    pending_.swap(keep);

    return settled;
}

std::size_t Portfolio::cancel_working_orders() {
    const std::size_t n = pending_.size();
    std::set<PositionId> exits_abandoned;

    for (const PendingOrder& o : pending_) {
        Leg& leg = positions_[static_cast<std::size_t>(o.position)].legs()[o.leg_index];
        if (o.closing) {
            // The exit did not get done. The leg is still held, so it goes back
            // to Open rather than lingering as PendingClose forever.
            if (leg.state == LegState::PendingClose) {
                leg.state = LegState::Open;
                exits_abandoned.insert(o.position);
            }
        } else if (leg.state == LegState::PendingOpen) {
            leg.state = LegState::Cancelled;
        }
    }
    pending_.clear();
    cancelled_orders_ += n;

    // Re-arm the rules whose exits were abandoned. A stop that fired and then
    // failed to fill has not done its job; leaving it spent would carry the
    // position overnight believing it was protected.
    for (RiskRule& rule : rules_) {
        if (!rule.fired) continue;
        if (exits_abandoned.count(rule.position) == 0) continue;
        if (!at(rule.position).open()) continue;
        rule.fired = false;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Risk rules
// ---------------------------------------------------------------------------

void Portfolio::attach_stop_loss(PositionId id, std::optional<std::size_t> leg, double pct) {
    if (pct <= 0.0) throw std::invalid_argument("stop_loss: pct must be positive magnitude");
    (void)at(id);   // validates the id, throws if unknown
    rules_.push_back(RiskRule{RiskRule::Kind::StopLossPct, id, leg, pct, Timestamp{}, false});
}

void Portfolio::attach_take_profit(PositionId id, std::optional<std::size_t> leg, double pct) {
    if (pct <= 0.0) throw std::invalid_argument("take_profit: pct must be positive magnitude");
    (void)at(id);
    rules_.push_back(RiskRule{RiskRule::Kind::TakeProfitPct, id, leg, pct, Timestamp{}, false});
}

void Portfolio::attach_exit_at(PositionId id, std::optional<std::size_t> leg, Timestamp when) {
    (void)at(id);
    rules_.push_back(RiskRule{RiskRule::Kind::ExitAt, id, leg, 0.0, when, false});
}

void Portfolio::process_risk_rules(const MarketView& market) {
    if (rules_.empty()) return;

    const Timestamp now = market.now();

    for (RiskRule& rule : rules_) {
        if (rule.fired) continue;

        Position& pos = at(rule.position);
        if (!pos.open()) { rule.fired = true; continue; }

        // A rule on an unfilled position must not fire. Treating "not yet
        // established" as flat would trigger every stop the instant it attached.
        if (rule.leg) {
            if (rule.leg >= pos.legs().size()) { rule.fired = true; continue; }
            if (pos.legs()[*rule.leg].state != LegState::Open) continue;
        } else if (!pos.established()) {
            continue;
        }

        bool trigger = false;
        switch (rule.kind) {
            case RiskRule::Kind::StopLossPct: {
                const double pnl = rule.leg ? pos.pnl_pct_of(*rule.leg, market)
                                            : pos.pnl_pct(market);
                trigger = pnl <= -rule.threshold;
                break;
            }
            case RiskRule::Kind::TakeProfitPct: {
                const double pnl = rule.leg ? pos.pnl_pct_of(*rule.leg, market)
                                            : pos.pnl_pct(market);
                trigger = pnl >= rule.threshold;
                break;
            }
            case RiskRule::Kind::ExitAt:
                trigger = now >= rule.deadline;
                break;
        }
        if (!trigger) continue;

        rule.fired = true;
        ++rules_fired_;

        // Routed through the ordinary order path: same fill model, same timing
        // guarantee. A stop is an order, not a special case.
        closing_from_rule_ = true;
        if (rule.leg) {
            submit_close_leg(rule.position, *rule.leg, now);
        } else {
            submit_close(rule.position, now);
        }
        closing_from_rule_ = false;
    }
}

const Position& Portfolio::at(PositionId id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= positions_.size()) {
        throw std::out_of_range("Portfolio::at: unknown position");
    }
    return positions_[static_cast<std::size_t>(id)];
}

Position& Portfolio::at(PositionId id) {
    return const_cast<Position&>(std::as_const(*this).at(id));
}

Money Portfolio::unrealized(const MarketView& market) const {
    Money m{};
    for (const Position& p : positions_) {
        for (std::size_t i = 0; i < p.legs().size(); ++i) {
            if (p.legs()[i].state == LegState::Closed) continue;
            m = m + p.pnl_of(i, market);
        }
    }
    return m;
}

Money Portfolio::equity(const MarketView& market) const {
    // Net of costs. Gross figures are available separately so the difference is
    // always visible rather than buried.
    return realized_ - total_costs_ + unrealized(market);
}


}  // namespace volforge
