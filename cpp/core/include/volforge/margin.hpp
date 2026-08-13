// Margin.
//
// NSE requires SPAN + Exposure on short option and futures positions. Long
// options require no margin beyond the premium already paid.
//
// SPAN is a scenario engine, not a formula: the portfolio is fully revalued
// under a grid of joint moves in the underlying and in volatility, and the
// requirement is the worst outcome. That structure is reproduced here exactly,
// using Black-76 revaluation — which is why a defined-risk spread gets a
// materially smaller number than its legs would separately, and why the relative
// margins across structures come out right.
//
// WHAT THIS IS NOT. NSE Clearing publishes a risk parameter file every day
// carrying the actual scan ranges, and the real calculation additionally applies
// intracommodity spread charges, delivery charges and a net-option-value
// adjustment. Without that file the absolute rupee figure is an approximation.
// Feed real scan ranges into SpanParameters and it converges on the real thing;
// treat the default parameters as a sensible NIFTY-shaped guess, not as NSE's
// answer.
//
// KNOWN BIAS, and it runs one way. A naked short comes out close to what a
// broker actually blocks. Defined-risk spreads come out **low**, because NSE's
// intracommodity spread charge is not modelled and exposure is netted across
// legs — so a vertical or a condor is margined at roughly its worst case rather
// than somewhat above it. Return on capital for spread strategies is therefore
// flattering, and by a wider margin than for naked shorts.
// See docs/costs-and-margin.md.

#pragma once

#include "volforge/types.hpp"

#include <memory>
#include <vector>

namespace volforge {

struct SpanParameters {
    // Price scan range as a fraction of the underlying. NSE derives this from
    // 3.5 standard deviations scaled by the margin period of risk, subject to a
    // floor; for NIFTY it typically lands in the 3-6% band.
    double price_scan_pct = 0.035;

    // Volatility scan range, as a relative change in implied volatility.
    double vol_scan_pct = 0.10;

    // The two extreme scenarios move price by this multiple of the scan range,
    // and only a fraction of the resulting loss is charged.
    double extreme_multiplier = 2.0;
    double extreme_weight     = 0.33;

    // Floor on short option positions, as a fraction of notional, so that deep
    // out-of-the-money shorts are never charged near zero.
    double short_option_minimum_pct = 0.015;

    // Exposure margin on short positions, as a fraction of notional. 3% for
    // index futures and index options.
    double exposure_pct = 0.03;
};

// One option position, for margining.
struct MarginLeg {
    Right  right  = Right::Call;
    double strike = 0.0;
    Qty    qty    = 0;     // signed: negative is short. In contracts.
    double iv     = 0.0;   // annualised
};

struct MarginResult {
    Money  span{};
    Money  exposure{};
    Money  total{};

    double scanning_risk    = 0.0;   // worst-case loss across the scenario grid
    double short_option_min = 0.0;   // the floor, when it binds
    double net_option_value = 0.0;   // long premium less short premium
    int    worst_scenario   = -1;    // which scenario bound, for auditability
    bool   floor_applied    = false;
};

// Sixteen scenarios: price unchanged, and at one third, two thirds and the full
// scan range either way, each with volatility up and down; plus two extreme
// moves charged at a reduced weight.
constexpr int kSpanScenarios = 16;

// Margins a portfolio of options on one underlying and expiry.
//
// `forward` and `t` come from the chain, so this needs no index feed. Returns a
// zero requirement when nothing is short: a long book has already paid for its
// risk in premium.
MarginResult span_margin(const std::vector<MarginLeg>& legs, double forward, double t,
                         double rate, const SpanParameters& params = {});

class MarginModel {
public:
    virtual ~MarginModel() = default;
    [[nodiscard]] virtual MarginResult requirement(const std::vector<MarginLeg>& legs,
                                                   double forward, double t,
                                                   double rate) const = 0;
};

class SpanMarginModel final : public MarginModel {
public:
    SpanMarginModel() = default;
    explicit SpanMarginModel(SpanParameters p) : params_(p) {}

    [[nodiscard]] MarginResult requirement(const std::vector<MarginLeg>& legs, double forward,
                                           double t, double rate) const override {
        return span_margin(legs, forward, t, rate, params_);
    }

    [[nodiscard]] const SpanParameters& params() const { return params_; }
    [[nodiscard]] SpanParameters& params() { return params_; }

private:
    SpanParameters params_;
};

std::shared_ptr<const MarginModel> default_margin_model();

}  // namespace volforge
