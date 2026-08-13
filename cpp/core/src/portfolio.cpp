#include "volforge/portfolio.hpp"

#include <algorithm>
#include <cmath>
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
    return !legs_.empty() &&
           std::all_of(legs_.begin(), legs_.end(), [](const Leg& l) { return l.filled(); });
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

Money Position::pnl_of(std::size_t i, const MarketView& market) const {
    const Leg& l = legs_.at(i);
    if (!l.filled()) return Money{};

    if (l.state == LegState::Closed) {
        // Realised: what came in at entry, less what went out at exit.
        return notional(l.exit, l.qty) - notional(l.entry, l.qty);
    }

    const auto q = market.quote(l.instrument);
    if (!q) return Money{};

    // Mark at the side that would close this leg, not at mid.
    const Price mark = l.qty < 0 ? q->ask : q->bid;
    if (mark.minor <= 0) return Money{};
    return notional(mark, l.qty) - notional(l.entry, l.qty);
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

Portfolio::Portfolio(std::shared_ptr<const FillModel> fills, std::int64_t execution_delay_nanos)
    : fills_(std::move(fills)), delay_(execution_delay_nanos) {
    if (!fills_) throw std::invalid_argument("Portfolio: null fill model");
    if (delay_ < 0) throw std::invalid_argument("Portfolio: negative execution delay");
}

void Portfolio::submit(PendingOrder order) { pending_.push_back(order); }

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
                                      order.side, order.qty, *price, illiquid});
    }

    pending_.swap(still_pending);
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
    return realized_ + unrealized(market);
}

}  // namespace volforge
