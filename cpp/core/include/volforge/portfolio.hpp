// Positions, legs, orders and fills.
//
// A position is a container of legs and is the unit that risk rules and metrics
// resolve against. Applied to a position they use the combined view; applied to a
// leg they affect only that leg. Grouping is not presentational — margin on a
// defined-risk structure is materially lower than the sum of its legs, so a
// condor modelled as four positions reports capital no broker would block.
//
// Orders are *pending* between submission and fill. A strategy that learns a
// price at T cannot also have traded on it at T, so an order never fills against
// the observation that triggered it. See docs/execution-semantics.md section 4.

#pragma once

#include "volforge/costs.hpp"
#include "volforge/data_source.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace volforge {

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

// ---------------------------------------------------------------------------
// Fills
// ---------------------------------------------------------------------------

// How an order becomes a price.
//
// The default crosses the spread, which is not conservatism for its own sake:
// the feed carries 1-second top-of-book snapshots, so sub-second queue position
// is unknowable. Filling at mid claims an execution the data cannot support.
class FillModel {
public:
    virtual ~FillModel() = default;

    // The fill price, or nullopt when there is no market to trade against.
    [[nodiscard]] virtual std::optional<Price> fill(const Quote& q, Side side, Qty qty) const = 0;

    // Spread as a fraction of mid, beyond which a fill is flagged. Deep wings in
    // the sample day quote 2.5% wide, and filling there silently tells a story
    // about liquidity that did not exist.
    [[nodiscard]] virtual double illiquid_spread_ratio() const { return 0.02; }
};

class CrossSpreadFill final : public FillModel {
public:
    [[nodiscard]] std::optional<Price> fill(const Quote& q, Side side, Qty) const override;
};

// ---------------------------------------------------------------------------
// Costs
// ---------------------------------------------------------------------------

// Transaction costs live in costs.hpp as an injectable policy, so a broker with
// a different structure — or the Python layer — can replace the calculation
// outright rather than only tuning its rates.

// ---------------------------------------------------------------------------
// Legs
// ---------------------------------------------------------------------------

enum class LegState : std::uint8_t {
    PendingOpen,   // submitted, not yet filled
    Open,
    PendingClose,
    Closed,
    Cancelled,     // opening order withdrawn before it filled
};

struct Leg {
    InstrumentId instrument = InstrumentId::Invalid;
    Qty          qty        = 0;      // signed: negative is short
    LegState     state      = LegState::PendingOpen;

    Price     entry;
    Timestamp entry_ts{};
    Price     exit;
    Timestamp exit_ts{};

    // Last price this leg could be marked at, carried forward.
    //
    // Overnight this is the only thing standing between a held position and a
    // P&L of zero: on the next session's first observations the instrument has
    // not printed yet, so quoting it returns nothing. Without a carried mark the
    // position would silently value at nil until its first trade of the day.
    Price     last_mark;
    Timestamp last_mark_ts{};
    bool      settled = false;   // closed by expiry rather than by an order

    [[nodiscard]] bool is_short() const { return qty < 0; }
    [[nodiscard]] bool filled() const {
        return state == LegState::Open || state == LegState::PendingClose ||
               state == LegState::Closed;
    }
    [[nodiscard]] bool live() const {
        return state == LegState::Open || state == LegState::PendingClose;
    }
    [[nodiscard]] bool cancelled() const { return state == LegState::Cancelled; }
    [[nodiscard]] Side closing_side() const { return qty < 0 ? Side::Buy : Side::Sell; }
};

using PositionId = std::int32_t;

class Position {
public:
    Position(PositionId id, std::string label) : id_(id), label_(std::move(label)) {}

    [[nodiscard]] PositionId id() const { return id_; }
    [[nodiscard]] const std::string& label() const { return label_; }
    [[nodiscard]] const std::vector<Leg>& legs() const { return legs_; }
    [[nodiscard]] std::vector<Leg>& legs() { return legs_; }

    // True once every leg has filled. P&L is meaningless before this.
    [[nodiscard]] bool established() const;
    [[nodiscard]] bool open() const;     // any leg still live
    [[nodiscard]] bool closed() const;   // established, and nothing live

    // Premium paid (positive) or collected (negative) across filled legs.
    [[nodiscard]] Money entry_notional() const;

    // Mark-to-market at *exit-side* prices: a short leg marks at the ask it would
    // be bought back at, a long leg at the bid it would be sold into. Marking at
    // mid reports profit that closing would not realise.
    [[nodiscard]] Money pnl(const MarketView& market) const;

    // P&L as a fraction of premium at risk — the familiar "30% of credit
    // collected" stop. Zero until the position is established.
    [[nodiscard]] double pnl_pct(const MarketView& market) const;

    [[nodiscard]] Money pnl_of(std::size_t leg_index, const MarketView& market) const;

    // The price a leg would close at right now, or a negative sentinel when no
    // valuation is available. Falls back to the carried mark, then to entry.
    [[nodiscard]] static Price mark_for(const Leg& leg, const MarketView& market);

    // Leg-level P&L as a fraction of that leg's own premium. Risk rules applied
    // to a leg resolve against this; rules applied to the position resolve
    // against pnl_pct above.
    [[nodiscard]] double pnl_pct_of(std::size_t leg_index, const MarketView& market) const;

private:
    PositionId       id_;
    std::string      label_;
    std::vector<Leg> legs_;
};

// ---------------------------------------------------------------------------
// Orders
// ---------------------------------------------------------------------------

struct PendingOrder {
    PositionId   position   = -1;
    std::size_t  leg_index  = 0;
    InstrumentId instrument = InstrumentId::Invalid;
    Side         side       = Side::Buy;
    Qty          qty        = 0;
    Timestamp    submitted_at{};
    Timestamp    eligible_at{};   // cannot fill before this
    bool         closing    = false;
    bool         from_rule  = false;
};

// A standing exit condition attached to a position or one of its legs.
//
// Risk rules are *orders*, not signals. They are evaluated by the engine at base
// resolution on every observation, regardless of what timeframe the strategy
// reasons in and regardless of what the strategy currently happens to be
// awaiting. A stop that is only checked when the strategy is looking at it is
// not a stop.
struct RiskRule {
    enum class Kind : std::uint8_t { StopLossPct, TakeProfitPct, ExitAt };

    Kind        kind = Kind::StopLossPct;
    PositionId  position = -1;
    std::optional<std::size_t> leg;   // unset = the whole position
    double      threshold = 0.0;      // magnitude, as a fraction of premium
    Timestamp   deadline{};           // for ExitAt
    bool        fired = false;
};

struct TradeRecord {
    Timestamp    signal_ts{};    // when the strategy decided
    Timestamp    fill_ts{};      // when it actually executed
    PositionId   position   = -1;
    InstrumentId instrument = InstrumentId::Invalid;
    Side         side       = Side::Buy;
    Qty          qty        = 0;
    Price        price;
    Money        cost;                 // statutory charges and brokerage
    bool         illiquid   = false;   // filled across a suspiciously wide spread
    bool         from_rule  = false;   // closed by an attached risk rule
    bool         oversized  = false;   // larger than the size displayed at the touch
    bool         settled    = false;   // cash settlement at expiry, not an order
};

// ---------------------------------------------------------------------------
// Portfolio
// ---------------------------------------------------------------------------

class Portfolio {
public:
    // `execution_delay` is the minimum gap between a decision and a fill. One
    // base interval by default. Zero is permitted but means "a resting order was
    // already at the touch", which is a claim about infrastructure rather than a
    // parameter to tune, so it is recorded in the run manifest.
    Portfolio(std::shared_ptr<const FillModel> fills, std::int64_t execution_delay_nanos,
              std::shared_ptr<const CostPolicy> costs = nullptr);

    // Submits the opening legs. Returns the position id immediately; the legs are
    // PendingOpen until process_pending fills them at a later timestamp.
    PositionId submit_open(std::string label, const std::vector<InstrumentId>& instruments,
                           Side side, Qty qty_per_leg, Timestamp now);

    // Records a fresh mark for every open leg that is quoting. Called once per
    // observation by the loop, so a position always has a valuation even when
    // its instrument has gone quiet or a new session has not reached it yet.
    void refresh_marks(const MarketView& market);

    // Cash-settles every open leg expiring on `expiry` against the settlement
    // level of the underlying. Legs finish Closed at their intrinsic value.
    //
    // This is what happens when a strategy does nothing, because it is what
    // happens in reality. STT on exercise falls on the buyer and is charged on
    // intrinsic value, not premium.
    std::size_t settle_expiry(const MarketView& market, Date expiry, double settlement_level,
                              Timestamp when);

    // Cancels orders that have not filled by the close. Exchange orders are day
    // orders; carrying them overnight would fill against a price the strategy
    // never saw.
    std::size_t cancel_working_orders();

    void submit_close(PositionId id, Timestamp now);
    void submit_close_leg(PositionId id, std::size_t leg_index, Timestamp now);

    // --- attached risk rules ----------------------------------------------

    void attach_stop_loss(PositionId id, std::optional<std::size_t> leg, double pct);
    void attach_take_profit(PositionId id, std::optional<std::size_t> leg, double pct);
    void attach_exit_at(PositionId id, std::optional<std::size_t> leg, Timestamp when);

    // Evaluated once per observation, before strategy code runs. Any rule that
    // fires submits a closing order through the ordinary path, so a stop is
    // filled by exactly the same machinery — and under exactly the same timing
    // guarantee — as a discretionary exit.
    void process_risk_rules(const MarketView& market);

    [[nodiscard]] std::size_t rules_fired() const { return rules_fired_; }

    // Fills whatever has become eligible. Called by the event loop once per
    // timestamp, before conditions are evaluated.
    void process_pending(const MarketView& market);

    [[nodiscard]] const Position& at(PositionId id) const;
    [[nodiscard]] Position& at(PositionId id);
    [[nodiscard]] std::size_t size() const { return positions_.size(); }
    [[nodiscard]] std::size_t pending_orders() const { return pending_.size(); }

    // Gross realised P&L, before costs.
    [[nodiscard]] Money realized() const { return realized_; }
    [[nodiscard]] Money costs() const { return total_costs_; }
    [[nodiscard]] Money net_realized() const { return realized_ - total_costs_; }
    [[nodiscard]] Money unrealized(const MarketView& market) const;
    [[nodiscard]] Money equity(const MarketView& market) const;

    [[nodiscard]] const std::vector<TradeRecord>& trades() const { return trades_; }
    [[nodiscard]] std::size_t illiquid_fills() const { return illiquid_fills_; }
    [[nodiscard]] std::size_t oversized_fills() const { return oversized_fills_; }
    [[nodiscard]] std::size_t cancelled_orders() const { return cancelled_orders_; }
    [[nodiscard]] std::size_t settled_legs() const { return settled_legs_; }
    [[nodiscard]] std::int64_t execution_delay_nanos() const { return delay_; }

private:
    void submit(PendingOrder order);

    std::shared_ptr<const FillModel> fills_;
    std::int64_t                     delay_;
    std::vector<Position>            positions_;
    std::vector<PendingOrder>        pending_;
    std::vector<RiskRule>            rules_;
    std::vector<TradeRecord>         trades_;
    Money                            realized_{};
    std::shared_ptr<const CostPolicy> costs_;
    Money                            total_costs_{};
    std::size_t                      illiquid_fills_  = 0;
    std::size_t                      oversized_fills_ = 0;
    std::size_t                      cancelled_orders_ = 0;
    std::size_t                      settled_legs_    = 0;
    std::size_t                      rules_fired_    = 0;
    bool                             closing_from_rule_ = false;
};

}  // namespace volforge
