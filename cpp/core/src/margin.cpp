#include "volforge/margin.hpp"

#include "volforge/greeks.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace volforge {
namespace {

// The scenario grid. Each entry is (price move as a fraction of the scan range,
// volatility move as a fraction of the vol scan range, weight).
//
// This mirrors SPAN's array: the underlying at rest, at a third, two thirds and
// the full scan range in each direction, each paired with volatility up and
// down, plus two extreme moves charged at a reduced weight because a move that
// large is not expected to be liquidated into.
struct Scenario {
    double price_fraction;
    double vol_fraction;
    double weight;
};

std::array<Scenario, kSpanScenarios> build_scenarios(double extreme_multiplier,
                                                     double extreme_weight) {
    return {{
        {0.0, +1.0, 1.0},   {0.0, -1.0, 1.0},
        {+1.0 / 3.0, +1.0, 1.0}, {+1.0 / 3.0, -1.0, 1.0},
        {-1.0 / 3.0, +1.0, 1.0}, {-1.0 / 3.0, -1.0, 1.0},
        {+2.0 / 3.0, +1.0, 1.0}, {+2.0 / 3.0, -1.0, 1.0},
        {-2.0 / 3.0, +1.0, 1.0}, {-2.0 / 3.0, -1.0, 1.0},
        {+1.0, +1.0, 1.0},  {+1.0, -1.0, 1.0},
        {-1.0, +1.0, 1.0},  {-1.0, -1.0, 1.0},
        {+extreme_multiplier, 0.0, extreme_weight},
        {-extreme_multiplier, 0.0, extreme_weight},
    }};
}

double portfolio_value(const std::vector<MarginLeg>& legs, double forward, double t,
                       double rate, double price_shift, double vol_scale) {
    double value = 0.0;
    for (const MarginLeg& leg : legs) {
        const double sigma = std::max(1e-4, leg.iv * vol_scale);
        const double px = black76_price(forward + price_shift, leg.strike, t, sigma, rate,
                                        leg.right);
        value += px * static_cast<double>(leg.qty);
    }
    return value;
}

}  // namespace

MarginResult span_margin(const std::vector<MarginLeg>& legs, double forward, double t,
                         double rate, const SpanParameters& p) {
    MarginResult out;
    if (legs.empty() || !(forward > 0.0)) return out;

    // A book with nothing short posts no margin: the premium on a long option is
    // paid up front and is the whole of the risk.
    const bool has_short = std::any_of(legs.begin(), legs.end(),
                                       [](const MarginLeg& l) { return l.qty < 0; });
    if (!has_short) return out;

    const double base = portfolio_value(legs, forward, t, rate, 0.0, 1.0);
    out.net_option_value = base;

    const double scan = forward * p.price_scan_pct;
    const auto scenarios = build_scenarios(p.extreme_multiplier, p.extreme_weight);

    for (int i = 0; i < kSpanScenarios; ++i) {
        const Scenario& s = scenarios[static_cast<std::size_t>(i)];
        const double shifted = portfolio_value(legs, forward, t, rate, s.price_fraction * scan,
                                               1.0 + s.vol_fraction * p.vol_scan_pct);
        // A loss to the holder is a fall in portfolio value.
        const double loss = (base - shifted) * s.weight;
        if (loss > out.scanning_risk) {
            out.scanning_risk  = loss;
            out.worst_scenario = i;
        }
    }

    // Exposure and the floor are charged on the **net** short position, so long
    // options offset shorts contract for contract within an expiry. Charging
    // gross would make a fully hedged spread post the same exposure as a naked
    // short, which is neither what a broker blocks nor what the risk is.
    Qty short_contracts = 0, long_contracts = 0;
    for (const MarginLeg& leg : legs) {
        if (leg.qty < 0) short_contracts += -leg.qty; else long_contracts += leg.qty;
    }
    const Qty net_short = std::max<Qty>(0, short_contracts - long_contracts);
    const double short_notional = forward * static_cast<double>(net_short);

    out.short_option_min = short_notional * p.short_option_minimum_pct;

    double span_value = out.scanning_risk;
    if (out.short_option_min > span_value) {
        span_value = out.short_option_min;
        out.floor_applied = true;
    }

    const double exposure = short_notional * p.exposure_pct;

    out.span     = Money{static_cast<std::int64_t>(std::llround(span_value * 100.0))};
    out.exposure = Money{static_cast<std::int64_t>(std::llround(exposure * 100.0))};
    out.total    = out.span + out.exposure;
    return out;
}

std::shared_ptr<const MarginModel> default_margin_model() {
    static const auto model = std::make_shared<const SpanMarginModel>();
    return model;
}

}  // namespace volforge
