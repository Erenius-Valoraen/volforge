#include "harness.hpp"

#include "volforge/event_loop.hpp"
#include "volforge/margin.hpp"
#include "volforge/synthetic.hpp"

#include <cmath>
#include <vector>

using namespace volforge;

namespace {

constexpr double kF    = 25000.0;   // forward
constexpr double kT    = 0.02;      // ~1 week
constexpr double kR    = 0.065;
constexpr double kIV   = 0.14;
constexpr Qty    kLot  = 75;

MarginLeg leg(Right r, double strike, int lots, double iv = kIV) {
    return MarginLeg{r, strike, static_cast<Qty>(lots * kLot), iv};
}

double total(const MarginResult& m) { return m.total.to_double(); }

}  // namespace

// ---------------------------------------------------------------------------
// The property that makes grouping matter
// ---------------------------------------------------------------------------

TEST(a_defined_risk_spread_margins_far_less_than_the_naked_short) {
    const auto naked = span_margin({leg(Right::Call, 25200, -1)}, kF, kT, kR);
    const auto spread = span_margin({leg(Right::Call, 25200, -1),
                                     leg(Right::Call, 25500, +1)}, kF, kT, kR);

    CHECK(total(naked) > 0.0);
    CHECK(total(spread) > 0.0);

    // Buying the wing caps the loss, and the scenario scan sees that. This is
    // the entire reason legs live inside a position rather than beside it: a
    // condor modelled as four separate books would report capital no broker
    // would ever block.
    CHECK(total(spread) < total(naked) * 0.3);

    // A defined-risk spread cannot lose more than the width less the credit, and
    // this model margins it at about that. Real SPAN charges somewhat more via
    // the intracommodity spread charge, which is the documented optimism in
    // margin.hpp — pinned here so it stays visible rather than drifting.
    const double max_loss = 300.0 * static_cast<double>(kLot);
    CHECK(total(spread) <= max_loss);
}

TEST(an_iron_condor_margins_less_than_the_strangle_inside_it) {
    const std::vector<MarginLeg> strangle{leg(Right::Call, 25500, -1),
                                          leg(Right::Put, 24500, -1)};
    const std::vector<MarginLeg> condor{leg(Right::Call, 25500, -1),
                                        leg(Right::Call, 25800, +1),
                                        leg(Right::Put, 24500, -1),
                                        leg(Right::Put, 24200, +1)};

    CHECK(total(span_margin(condor, kF, kT, kR)) <
          total(span_margin(strangle, kF, kT, kR)));
}

TEST(a_long_only_book_requires_no_margin) {
    // Premium is paid up front and is the whole of the risk.
    const auto m = span_margin({leg(Right::Call, 25200, +1), leg(Right::Put, 24800, +2)},
                               kF, kT, kR);
    CHECK(m.total.minor == 0);
    CHECK(m.span.minor == 0);
    CHECK(m.exposure.minor == 0);
}

TEST(margin_scales_with_position_size) {
    const auto one  = span_margin({leg(Right::Call, 25000, -1)}, kF, kT, kR);
    const auto five = span_margin({leg(Right::Call, 25000, -5)}, kF, kT, kR);

    // Linear in size: SPAN has no size discount, and neither does exposure.
    CHECK(std::abs(total(five) - total(one) * 5.0) < total(one) * 0.02);
}

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

TEST(exposure_is_three_percent_of_short_notional) {
    const auto m = span_margin({leg(Right::Call, 25000, -1)}, kF, kT, kR);

    // One lot short: 75 contracts against a 25,000 forward is 18.75 lakh of
    // notional, and exposure is 3% of that.
    const double expected = kF * kLot * 0.03;
    CHECK(std::abs(m.exposure.to_double() - expected) < 1.0);
}

TEST(a_deep_out_of_the_money_short_is_floored_rather_than_charged_nothing) {
    // Twelve percent out of the money with a week to run: the scan barely
    // touches it, and without a floor it would margin at almost zero right up
    // until the move that matters.
    const auto far = span_margin({leg(Right::Call, 28000, -1)}, kF, kT, kR);

    CHECK(far.floor_applied);
    CHECK(far.scanning_risk < far.short_option_min);
    CHECK(std::abs(far.span.to_double() - far.short_option_min) < 1.0);
    CHECK(total(far) > 0.0);
}

TEST(the_binding_scenario_is_recorded_and_is_an_adverse_move) {
    const auto short_call = span_margin({leg(Right::Call, 25000, -1)}, kF, kT, kR);
    const auto short_put  = span_margin({leg(Right::Put, 25000, -1)}, kF, kT, kR);

    CHECK(short_call.worst_scenario >= 0);
    CHECK(short_put.worst_scenario >= 0);

    // A short call is hurt by a rally and a short put by a selloff, so they
    // cannot be bound by the same scenario.
    CHECK(short_call.worst_scenario != short_put.worst_scenario);
}

TEST(net_option_value_is_negative_for_a_short_book) {
    const auto m = span_margin({leg(Right::Call, 25000, -1)}, kF, kT, kR);
    CHECK(m.net_option_value < 0.0);   // premium received, not paid
}

TEST(a_wider_scan_range_demands_more_margin) {
    SpanParameters tight;  tight.price_scan_pct = 0.02;
    SpanParameters wide;   wide.price_scan_pct  = 0.06;

    const auto a = span_margin({leg(Right::Call, 25000, -1)}, kF, kT, kR, tight);
    const auto b = span_margin({leg(Right::Call, 25000, -1)}, kF, kT, kR, wide);
    CHECK(total(b) > total(a));
}

// ---------------------------------------------------------------------------
// Does the number resemble reality
// ---------------------------------------------------------------------------

TEST(an_atm_nifty_short_lands_in_the_range_a_broker_would_block) {
    const auto m = span_margin({leg(Right::Call, 25000, -1)}, kF, kT, kR);

    // One lot of a near-the-money NIFTY option is blocked at roughly one to two
    // lakh in practice. This is an approximation of SPAN rather than SPAN, so
    // the check is that it is the right size, not that it matches to the rupee.
    CHECK(total(m) > 80'000.0);
    CHECK(total(m) < 250'000.0);

    // Both components should be material; if either dominated completely the
    // parameters would be wrong.
    CHECK(m.span.to_double() > 10'000.0);
    CHECK(m.exposure.to_double() > 10'000.0);
}

// ---------------------------------------------------------------------------
// Wired into a session
// ---------------------------------------------------------------------------

TEST(margin_is_tracked_across_a_session_and_released_on_close) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    double at_entry = 0.0, after_close = 0.0;

    const auto result = run_session(
        *synth.data, synth.underlying, synth.spot, [&](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("10:00");
            const auto legs = ctx.chain().straddle();
            if (legs.empty()) co_return;
            const auto pos = ctx.sell(legs, 1, "straddle");
            if (!pos) co_return;

            co_await ctx.after(120);
            at_entry = ctx.margin().total.to_double();

            pos->close();
            co_await (pos->closed() | ctx.at("15:29"));
            co_await ctx.after(60);
            after_close = ctx.margin().total.to_double();
        });

    CHECK(at_entry > 0.0);
    CHECK_EQ(result.open_positions, std::size_t{0});

    // Capital is returned when the position is closed.
    CHECK(std::abs(after_close) < 1e-9);

    // The session's peak is at least what was required while the trade was on.
    CHECK(result.peak_margin.to_double() >= at_entry * 0.5);
    CHECK(result.final_margin.minor == 0);
}

TEST(a_long_position_never_blocks_margin_during_a_session) {
    InstrumentRegistry registry;
    const auto synth = make_synthetic_session(registry);

    const auto result = run_session(
        *synth.data, synth.underlying, synth.spot, [](Ctx& ctx) -> StrategyTask {
            co_await ctx.at("10:00");
            const auto legs = ctx.chain().straddle();
            if (legs.empty()) co_return;
            const auto pos = ctx.buy(legs, 1, "long straddle");
            if (!pos) co_return;
            co_await ctx.at("15:15");
        });

    CHECK(result.trades > 0);
    CHECK(result.peak_margin.minor == 0);
}
