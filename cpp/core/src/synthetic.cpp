#include "volforge/synthetic.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace volforge {
namespace {

// Width, in index points, over which option time value decays away from the
// money. Chosen so a NIFTY ATM straddle prints in the low hundreds and the wings
// go near-worthless around ten strikes out.
constexpr double kTimeValueWidth = 250.0;

// Width governing how quickly quote updates thin out away from the money. The
// real feed shows ATM strikes updating almost every second while distant wings
// sit still for minutes, and reproducing that is what makes the irregular
// timebase testable.
constexpr double kActivityWidth = 400.0;

struct OptionLeg {
    InstrumentId id;
    double       strike;      // index points
    Right        right;
    int          first_sec;   // wing strikes start printing late
    std::int64_t oi;
};

double intrinsic(double spot, double strike, Right r) {
    return r == Right::Call ? std::max(0.0, spot - strike)
                            : std::max(0.0, strike - spot);
}

}  // namespace

SyntheticSession make_synthetic_session(InstrumentRegistry& registry,
                                        const SyntheticConfig& cfg) {
    SyntheticSession out;
    out.underlying = registry.intern_underlying(cfg.underlying);

    InstrumentSpec spot_spec;
    spot_spec.underlying = out.underlying;
    spot_spec.kind       = InstrumentKind::Spot;
    spot_spec.lot_size   = 1;
    out.spot = registry.add(spot_spec);

    const double spot_open   = cfg.spot_open.to_double();
    const double strike_step = cfg.strike_step.to_double();

    std::vector<OptionLeg> legs;
    legs.reserve(static_cast<std::size_t>(cfg.strikes_each_side) * 4 + 2);

    std::mt19937_64 rng(cfg.seed);

    for (int k = -cfg.strikes_each_side; k <= cfg.strikes_each_side; ++k) {
        const double strike = std::round((spot_open + k * strike_step) / strike_step) * strike_step;
        for (const Right right : {Right::Call, Right::Put}) {
            InstrumentSpec s;
            s.underlying = out.underlying;
            s.kind       = InstrumentKind::Option;
            s.expiry     = cfg.expiry;
            s.strike     = Price::from_double(strike);
            s.right      = right;
            s.lot_size   = cfg.lot_size;

            // Distant strikes wake up later, so tests exercise the "instrument
            // has not printed yet" path rather than assuming full coverage.
            const int dist = std::abs(k);
            const int first = cfg.session_open_sec + std::min(dist * dist * 20, 3600);

            legs.push_back(OptionLeg{registry.add(s), strike, right, first,
                                     20000 + static_cast<std::int64_t>(rng() % 60000)});
        }
    }

    auto session = std::make_shared<MemorySessionData>(cfg.date, registry);

    // Days to expiry, used to scale time value and its decay across the session.
    const double dte = std::max(
        1.0, static_cast<double>(days_from_civil(cfg.expiry.year(),
                                                 static_cast<unsigned>(cfg.expiry.month()),
                                                 static_cast<unsigned>(cfg.expiry.day()))
                                 - days_from_civil(cfg.date.year(),
                                                   static_cast<unsigned>(cfg.date.month()),
                                                   static_cast<unsigned>(cfg.date.day()))));

    std::normal_distribution<double>       step(0.0, 1.2);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    const int total_secs = cfg.session_close_sec - cfg.session_open_sec;
    double spot = spot_open;

    for (int t = 0; t <= total_secs; ++t) {
        const int  sec = cfg.session_open_sec + t;
        const auto ts  = timestamp_of(cfg.date, sec, kISTOffsetSeconds);

        spot += step(rng);

        {
            Quote q;
            q.ts       = ts;
            q.last     = Price::from_double(spot);
            q.bid      = Price::from_double(spot - 0.25);
            q.ask      = Price::from_double(spot + 0.25);
            q.bid_qty  = 0;
            q.ask_qty  = 0;
            q.last_qty = 1;
            session->append(out.spot, q);
        }

        // Fraction of the option's life still ahead, used for time-value decay.
        const double progress  = static_cast<double>(t) / static_cast<double>(total_secs);
        const double remaining = std::max(0.02, (dte - progress) / dte);

        for (const OptionLeg& leg : legs) {
            if (sec < leg.first_sec) continue;

            const double dist = leg.strike - spot;

            const double activity = std::clamp(
                std::exp(-0.5 * (dist / kActivityWidth) * (dist / kActivityWidth)), 0.02, 1.0);
            if (unit(rng) > activity) continue;

            const double tv = 120.0 * std::sqrt(remaining) *
                              std::exp(-0.5 * (dist / kTimeValueWidth) * (dist / kTimeValueWidth));
            const double fair = std::max(0.05, intrinsic(spot, leg.strike, leg.right) + tv);

            // Spreads widen away from the money and in proportion to premium,
            // which is what makes mid-price fills fantasy on the wings.
            const double half_spread =
                std::max(0.025, fair * 0.004 + std::abs(dist) / strike_step * 0.05);

            Quote q;
            q.ts  = ts;
            q.bid = Price::from_double(std::max(0.05, fair - half_spread));
            q.ask = Price::from_double(fair + half_spread);

            // 17% of observations carry a trade, matching the 82.8% of rows in
            // the sample day that report a quote change and nothing more.
            const bool traded = unit(rng) < 0.17;
            q.last     = Price::from_double(fair);
            q.last_qty = traded ? static_cast<Qty>(cfg.lot_size * (1 + (rng() % 8))) : 0;

            q.bid_qty = static_cast<Qty>(cfg.lot_size * (1 + (rng() % 20)));
            q.ask_qty = static_cast<Qty>(cfg.lot_size * (1 + (rng() % 20)));
            q.open_interest = leg.oi;

            session->append(leg.id, q);
        }
    }

    session->build_event_order();
    out.data = std::move(session);
    return out;
}

}  // namespace volforge
