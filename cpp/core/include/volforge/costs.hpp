// Transaction costs.
//
// Omitting these is one of the largest sources of overstated returns in Indian
// F&O. Statutory charges alone run to roughly 0.19% of premium on a round trip
// at current rates, which on a high-turnover intraday strategy decides whether
// the system makes money at all.
//
// Costs are a **policy**, not a constant. The rates below are data and can be
// edited; the whole calculation can be replaced by implementing CostPolicy,
// which is the extension point the Python layer will bind to for brokers whose
// structure does not fit this shape at all.
//
// RATES CHANGE, OFTEN AT BUDGETS. The defaults were checked against published
// schedules on 2026-08-13 and are cited in docs/costs-and-margin.md. Verify them
// against your own contract note before trusting a P&L figure.

#pragma once

#include "volforge/instrument.hpp"
#include "volforge/types.hpp"

#include <memory>

namespace volforge {

enum class Side : std::uint8_t;   // defined in portfolio.hpp

// What is being charged.
struct CostContext {
    const InstrumentSpec* instrument = nullptr;
    Price price;            // fill price (option premium)
    Qty   qty      = 0;     // contracts, always positive
    Side  side     = static_cast<Side>(0);
    bool  exercise = false; // settled at expiry rather than traded
    Price intrinsic;        // intrinsic at settlement, for exercise STT
};

class CostPolicy {
public:
    virtual ~CostPolicy() = default;
    [[nodiscard]] virtual Money cost_of(const CostContext& ctx) const = 0;
};

// Statutory charges and brokerage for NSE equity derivatives.
//
// Every field is a rate on premium turnover unless noted. Percentages are
// fractions: 0.0015 is 0.15%.
struct IndianFnORates {
    // Securities Transaction Tax, sell side only, on premium.
    // Raised from 0.10% to 0.15% in Budget 2026, effective 1 April 2026.
    double stt_sell_pct = 0.0015;

    // STT on exercised/assigned options is charged on **intrinsic value**, not
    // premium, and lands on the buyer. On a deep in-the-money option this dwarfs
    // every other charge.
    //
    // Not yet applied: expiry settlement is not implemented, so no position is
    // ever exercised. Present so the gap is visible rather than silent.
    double stt_exercise_pct = 0.0015;

    // NSE transaction charge on premium turnover.
    double transaction_pct = 0.0003553;

    // SEBI turnover fee, Rs 10 per crore.
    double sebi_pct = 0.000001;

    // Investor Protection Fund Trust, Rs 0.01 per crore. Tiny, included for
    // completeness rather than materiality.
    double ipft_pct = 0.000000001;

    // Stamp duty, buy side only: 0.003%, or Rs 300 per crore.
    double stamp_buy_pct = 0.00003;

    // GST applies to brokerage and exchange-level charges, never to STT or
    // stamp duty.
    double gst_pct = 0.18;

    // Flat per executed order. Broker-specific; Rs 20 is the common discount
    // rate. Set to zero for a zero-brokerage account.
    Money brokerage_per_order{2000};
};

class IndianFnOCosts final : public CostPolicy {
public:
    IndianFnOCosts() = default;
    explicit IndianFnOCosts(IndianFnORates rates) : rates_(rates) {}

    [[nodiscard]] Money cost_of(const CostContext& ctx) const override;

    [[nodiscard]] const IndianFnORates& rates() const { return rates_; }
    [[nodiscard]] IndianFnORates& rates() { return rates_; }

private:
    IndianFnORates rates_;
};

// Charges nothing. Useful for isolating strategy behaviour from cost drag, and
// for markets where this schedule does not apply.
class NoCosts final : public CostPolicy {
public:
    [[nodiscard]] Money cost_of(const CostContext&) const override { return Money{}; }
};

std::shared_ptr<const CostPolicy> default_cost_policy();

}  // namespace volforge
