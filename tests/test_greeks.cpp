#include "harness.hpp"

#include "volforge/greeks.hpp"
#include "volforge/synthetic.hpp"
#include "volforge/event_loop.hpp"

#include <cmath>
#include <vector>

using namespace volforge;

namespace {
constexpr double kR = 0.065;
bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) < tol; }
}  // namespace

// ---------------------------------------------------------------------------
// Pricing identities
// ---------------------------------------------------------------------------

TEST(black76_respects_put_call_parity) {
    const double F = 25000, T = 0.05, sigma = 0.14;
    for (const double K : {23000.0, 24500.0, 25000.0, 25500.0, 27000.0}) {
        const double c = black76_price(F, K, T, sigma, kR, Right::Call);
        const double p = black76_price(F, K, T, sigma, kR, Right::Put);
        // C - P = e^{-rT}(F - K), exactly.
        CHECK(near(c - p, std::exp(-kR * T) * (F - K), 1e-8));
    }
}

TEST(price_is_monotonic_in_volatility_and_bounded) {
    const double F = 25000, K = 25000, T = 0.05;
    double prev = -1.0;
    for (double sigma = 0.02; sigma < 1.0; sigma += 0.02) {
        const double c = black76_price(F, K, T, sigma, kR, Right::Call);
        CHECK(c > prev);
        prev = c;
        CHECK(c <= std::exp(-kR * T) * F + 1e-9);
    }
}

TEST(at_expiry_an_option_is_worth_its_intrinsic_and_nothing_more) {
    CHECK(near(black76_price(25100, 25000, 0.0, 0.2, kR, Right::Call), 100.0, 1e-9));
    CHECK(near(black76_price(24900, 25000, 0.0, 0.2, kR, Right::Call), 0.0, 1e-9));
    CHECK(near(black76_price(24900, 25000, 0.0, 0.2, kR, Right::Put), 100.0, 1e-9));

    const auto g = black76_greeks(25100, 25000, 0.0, 0.2, kR, Right::Call);
    CHECK(near(g.delta, 1.0));
    CHECK(near(g.gamma, 0.0));
    CHECK(near(g.vega, 0.0));
}

// ---------------------------------------------------------------------------
// Greeks
// ---------------------------------------------------------------------------

TEST(greeks_have_the_right_signs_and_bounds) {
    const double F = 25000, T = 0.05, sigma = 0.14;

    for (const double K : {24000.0, 25000.0, 26000.0}) {
        const auto c = black76_greeks(F, K, T, sigma, kR, Right::Call);
        const auto p = black76_greeks(F, K, T, sigma, kR, Right::Put);

        CHECK(c.delta > 0.0 && c.delta < 1.0);
        CHECK(p.delta < 0.0 && p.delta > -1.0);
        CHECK(c.gamma > 0.0);
        CHECK(near(c.gamma, p.gamma, 1e-12));   // identical for both rights
        CHECK(c.vega > 0.0);
        CHECK(near(c.vega, p.vega, 1e-9));
        CHECK(c.theta < 0.0);                   // long options decay
    }
}

TEST(delta_matches_a_numerical_derivative) {
    const double F = 25000, K = 25200, T = 0.05, sigma = 0.14, h = 0.01;

    for (const Right r : {Right::Call, Right::Put}) {
        const double up   = black76_price(F + h, K, T, sigma, kR, r);
        const double down = black76_price(F - h, K, T, sigma, kR, r);
        const double numeric = (up - down) / (2 * h);
        CHECK(near(black76_greeks(F, K, T, sigma, kR, r).delta, numeric, 1e-6));
    }
}

TEST(gamma_and_vega_match_numerical_derivatives) {
    const double F = 25000, K = 25200, T = 0.05, sigma = 0.14;

    const double h = 0.5;
    const double d_up   = black76_greeks(F + h, K, T, sigma, kR, Right::Call).delta;
    const double d_down = black76_greeks(F - h, K, T, sigma, kR, Right::Call).delta;
    CHECK(near(black76_greeks(F, K, T, sigma, kR, Right::Call).gamma,
               (d_up - d_down) / (2 * h), 1e-8));

    const double dv = 1e-5;
    const double v_up   = black76_price(F, K, T, sigma + dv, kR, Right::Call);
    const double v_down = black76_price(F, K, T, sigma - dv, kR, Right::Call);
    CHECK(near(black76_greeks(F, K, T, sigma, kR, Right::Call).vega,
               (v_up - v_down) / (2 * dv), 1e-3));
}

TEST(theta_matches_the_change_in_value_over_a_day) {
    const double F = 25000, K = 25000, T = 0.05, sigma = 0.14;
    const double day = 1.0 / 365.0;

    const double now_price      = black76_price(F, K, T, sigma, kR, Right::Call);
    const double tomorrow_price = black76_price(F, K, T - day, sigma, kR, Right::Call);
    const double observed = tomorrow_price - now_price;

    // Theta is the instantaneous rate, so it slightly overstates a whole day of
    // decay; within a few percent is the correct agreement.
    const double theta = black76_greeks(F, K, T, sigma, kR, Right::Call).theta;
    CHECK(theta < 0.0);
    CHECK(std::abs(theta - observed) < std::abs(observed) * 0.05);
}

// ---------------------------------------------------------------------------
// Implied volatility
// ---------------------------------------------------------------------------

TEST(implied_vol_round_trips_across_strikes_and_tenors) {
    const double F = 25000;
    for (const double T : {0.002, 0.02, 0.08, 0.3}) {
        for (const double K : {22000.0, 24000.0, 25000.0, 26000.0, 28000.0}) {
            for (const double sigma : {0.08, 0.15, 0.35, 0.9}) {
                for (const Right r : {Right::Call, Right::Put}) {
                    const double px = black76_price(F, K, T, sigma, kR, r);
                    if (px < 0.01) continue;   // numerically worthless, nothing to solve
                    const auto iv = implied_vol(px, F, K, T, kR, r);
                    CHECK(iv.has_value());
                    if (!iv) continue;

                    // The invariant that always holds: re-pricing at the solved
                    // volatility reproduces the input.
                    CHECK(near(black76_price(F, K, T, *iv, kR, r), px, 1e-6));

                    // Recovering sigma itself is only well conditioned where
                    // vega is meaningful. Far out of the money and close to
                    // expiry a wide band of volatilities price identically, so
                    // demanding an exact sigma there would be testing the
                    // arithmetic of the machine rather than the solver.
                    if (black76_greeks(F, K, T, sigma, kR, r).vega > 1.0) {
                        CHECK(near(*iv, sigma, 1e-4));
                    }
                }
            }
        }
    }
}

TEST(implied_vol_refuses_prices_no_volatility_can_produce) {
    const double F = 25000, K = 25000, T = 0.05;

    // Below intrinsic.
    CHECK(!implied_vol(1.0, F, 20000, T, kR, Right::Call).has_value());
    // Above the discounted forward.
    CHECK(!implied_vol(F * 1.5, F, K, T, kR, Right::Call).has_value());
    // Non-positive, and expired.
    CHECK(!implied_vol(0.0, F, K, T, kR, Right::Call).has_value());
    CHECK(!implied_vol(100.0, F, K, 0.0, kR, Right::Call).has_value());
}

// ---------------------------------------------------------------------------
// Forward from parity — no index feed required
// ---------------------------------------------------------------------------

TEST(parity_recovers_the_forward_from_the_chain_alone) {
    const double F = 25137.5, T = 0.05, sigma = 0.14;

    std::vector<ParityQuote> chain;
    for (double K = 24000; K <= 26000; K += 50) {
        chain.push_back(ParityQuote{K, black76_price(F, K, T, sigma, kR, Right::Call),
                                    black76_price(F, K, T, sigma, kR, Right::Put)});
    }

    const auto fwd = forward_from_parity(chain.data(), chain.size(), T, kR);
    CHECK(fwd.has_value());
    if (fwd) CHECK(near(*fwd, F, 1e-6));
}

TEST(parity_reports_nothing_when_the_chain_is_unusable) {
    CHECK(!forward_from_parity(nullptr, 0, 0.05, kR).has_value());

    std::vector<ParityQuote> one_sided{{25000.0, 100.0, 0.0}};
    CHECK(!forward_from_parity(one_sided.data(), one_sided.size(), 0.05, kR).has_value());
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

TEST(time_to_expiry_counts_down_to_the_closing_bell) {
    const Date expiry{20250703};
    const auto t0 = time_to_expiry(timestamp_of(Date{20250701}, 9 * 3600 + 15 * 60,
                                                kISTOffsetSeconds),
                                   expiry, kISTOffsetSeconds);
    const auto t1 = time_to_expiry(timestamp_of(Date{20250702}, 9 * 3600 + 15 * 60,
                                                kISTOffsetSeconds),
                                   expiry, kISTOffsetSeconds);
    CHECK(t0 > t1);
    CHECK(near(t0 - t1, 1.0 / 365.0, 1e-9));   // exactly one calendar day apart

    // Zero at and after the bell on expiry day.
    CHECK(near(time_to_expiry(timestamp_of(expiry, 15 * 3600 + 30 * 60, kISTOffsetSeconds),
                              expiry, kISTOffsetSeconds), 0.0));
    CHECK(near(time_to_expiry(timestamp_of(expiry, 16 * 3600, kISTOffsetSeconds), expiry,
                              kISTOffsetSeconds), 0.0));
}

// ---------------------------------------------------------------------------
// Wired into a session
// ---------------------------------------------------------------------------

TEST(greeks_and_delta_selection_work_on_a_live_chain) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    bool got_forward = false, got_greeks = false;
    double picked_delta = 0.0;
    bool picked = false;

    run_session(*synth.data, synth.underlying, synth.spot, [&](Ctx& ctx) -> StrategyTask {
        co_await ctx.at("11:00");

        const auto fwd = ctx.forward(Date{20250703});
        if (fwd) {
            got_forward = true;
            // Recovered from the chain alone; should sit near the generator's spot.
            const auto spot = ctx.spot_price();
            if (spot) CHECK(std::abs(*fwd - spot->to_double()) < 400.0);
        }

        const auto chain = ctx.chain();
        const auto atm = chain.atm_strike();
        if (atm) {
            const auto call = chain.option(*atm, Right::Call);
            if (call) {
                const auto g = ctx.greeks(*call);
                if (g) {
                    got_greeks = true;
                    CHECK(g->iv > 0.0 && g->iv < 3.0);
                    CHECK(g->delta > 0.0 && g->delta < 1.0);
                    CHECK(g->gamma > 0.0);
                    CHECK(g->theta < 0.0);
                }
            }
        }

        const auto sel = chain.by_delta(0.25, Right::Call);
        if (sel) {
            const auto g = ctx.greeks(*sel);
            if (g) { picked = true; picked_delta = std::abs(g->delta); }
        }
    });

    CHECK(got_forward);
    CHECK(got_greeks);
    CHECK(picked);
    // Chosen from a discrete strike ladder, so it lands near the target rather
    // than on it.
    if (picked) CHECK(std::abs(picked_delta - 0.25) < 0.15);
}
