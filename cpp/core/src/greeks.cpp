#include "volforge/greeks.hpp"

#include <algorithm>
#include <cmath>

namespace volforge {
namespace {

constexpr double kSqrt2   = 1.41421356237309504880;
constexpr double kInvSqrt2Pi = 0.39894228040143267794;

double norm_cdf(double x) { return 0.5 * std::erfc(-x / kSqrt2); }
double norm_pdf(double x) { return kInvSqrt2Pi * std::exp(-0.5 * x * x); }

// Value at expiry, or when volatility is zero — the option is worth its
// intrinsic and nothing else.
double intrinsic(double forward, double strike, double rate, double t, Right right) {
    const double df = std::exp(-rate * t);
    return df * (right == Right::Call ? std::max(0.0, forward - strike)
                                      : std::max(0.0, strike - forward));
}

struct D {
    double d1, d2, sqrt_t, df;
};

D compute_d(double forward, double strike, double t, double sigma, double rate) {
    D d{};
    d.sqrt_t = std::sqrt(t);
    d.df     = std::exp(-rate * t);
    const double vol_sqrt_t = sigma * d.sqrt_t;
    d.d1 = (std::log(forward / strike) + 0.5 * sigma * sigma * t) / vol_sqrt_t;
    d.d2 = d.d1 - vol_sqrt_t;
    return d;
}

bool degenerate(double forward, double strike, double t, double sigma) {
    return !(forward > 0.0) || !(strike > 0.0) || !(t > 0.0) || !(sigma > 0.0);
}

}  // namespace

double time_to_expiry(Timestamp now, Date expiry, int utc_offset_seconds,
                      int expiry_second_of_day) {
    const Timestamp end = timestamp_of(expiry, expiry_second_of_day, utc_offset_seconds);
    const double seconds = static_cast<double>(end.nanos - now.nanos) / 1e9;
    if (seconds <= 0.0) return 0.0;
    return seconds / (365.0 * 86400.0);
}

double black76_price(double forward, double strike, double t, double sigma, double rate,
                     Right right) {
    if (degenerate(forward, strike, t, sigma)) {
        return intrinsic(forward, strike, rate, std::max(0.0, t), right);
    }
    const D d = compute_d(forward, strike, t, sigma, rate);
    if (right == Right::Call) {
        return d.df * (forward * norm_cdf(d.d1) - strike * norm_cdf(d.d2));
    }
    return d.df * (strike * norm_cdf(-d.d2) - forward * norm_cdf(-d.d1));
}

GreekSet black76_greeks(double forward, double strike, double t, double sigma, double rate,
                        Right right) {
    GreekSet g;
    g.iv = sigma;

    if (degenerate(forward, strike, t, sigma)) {
        // At expiry an option is its intrinsic; delta is 0 or 1 with nothing in
        // between, and the higher-order Greeks are gone.
        g.price = intrinsic(forward, strike, rate, std::max(0.0, t), right);
        const bool itm = right == Right::Call ? forward > strike : forward < strike;
        g.delta = itm ? (right == Right::Call ? 1.0 : -1.0) : 0.0;
        return g;
    }

    const D d = compute_d(forward, strike, t, sigma, rate);
    const double pdf_d1 = norm_pdf(d.d1);

    g.price = black76_price(forward, strike, t, sigma, rate, right);
    g.delta = right == Right::Call ? d.df * norm_cdf(d.d1) : -d.df * norm_cdf(-d.d1);
    g.gamma = d.df * pdf_d1 / (forward * sigma * d.sqrt_t);
    g.vega  = d.df * forward * pdf_d1 * d.sqrt_t;

    // theta = -dV/dT. The volatility term is identical for calls and puts; only
    // the carry term differs, through the option's own value.
    const double decay = d.df * forward * pdf_d1 * sigma / (2.0 * d.sqrt_t);
    g.theta = (rate * g.price - decay) / 365.0;

    return g;
}

std::optional<double> implied_vol(double market_price, double forward, double strike, double t,
                                  double rate, Right right) {
    if (!(market_price > 0.0) || !(forward > 0.0) || !(strike > 0.0) || !(t > 0.0)) {
        return std::nullopt;
    }

    // A price below intrinsic or above the discounted bound is not produced by
    // any volatility. This is common on wide wings, and inventing a number to
    // fill the gap would put fabricated Greeks into strike selection.
    const double df  = std::exp(-rate * t);
    const double low = intrinsic(forward, strike, rate, t, right);
    const double high = right == Right::Call ? df * forward : df * strike;
    if (market_price < low - 1e-9 || market_price > high + 1e-9) return std::nullopt;

    constexpr double kMinVol = 1e-4;
    constexpr double kMaxVol = 6.0;

    // Bracket first. Price is monotonic in volatility, so bisection always
    // converges; Newton is used inside it for speed but never trusted alone,
    // since vega collapses far from the money and sends it wandering.
    double lo = kMinVol, hi = kMaxVol;
    const double p_lo = black76_price(forward, strike, t, lo, rate, right);
    const double p_hi = black76_price(forward, strike, t, hi, rate, right);

    // Scaled tolerance. A deep in-the-money option trades at its intrinsic, and
    // the zero-volatility price can round a hair above the quote, which would
    // otherwise reject a perfectly ordinary strike.
    const double tol = 1e-9 * std::max(1.0, std::abs(market_price));
    if (market_price > p_hi + tol || market_price < p_lo - tol) return std::nullopt;

    // At the intrinsic floor there is no time value to invert; volatility is
    // effectively zero, and reporting the floor is more useful than reporting
    // nothing for a strike that is genuinely all delta.
    if (market_price <= p_lo) return lo;

    double sigma = 0.2;
    for (int i = 0; i < 100; ++i) {
        const double price = black76_price(forward, strike, t, sigma, rate, right);
        const double diff  = price - market_price;
        if (std::abs(diff) < 1e-8) return sigma;

        if (diff > 0.0) hi = sigma; else lo = sigma;

        const D d = compute_d(forward, strike, t, sigma, rate);
        const double vega = df * forward * norm_pdf(d.d1) * d.sqrt_t;

        double next = vega > 1e-10 ? sigma - diff / vega : 0.5 * (lo + hi);
        if (!(next > lo && next < hi) || !std::isfinite(next)) next = 0.5 * (lo + hi);
        sigma = next;
    }
    return (hi - lo) < 1e-6 ? std::optional<double>(sigma) : std::nullopt;
}

std::optional<GreekSet> implied_greeks(double market_price, double forward, double strike,
                                       double t, double rate, Right right) {
    const auto iv = implied_vol(market_price, forward, strike, t, rate, right);
    if (!iv) return std::nullopt;
    return black76_greeks(forward, strike, t, *iv, rate, right);
}

std::optional<double> forward_from_parity(const ParityQuote* quotes, std::size_t count,
                                          double t, double rate) {
    if (quotes == nullptr || count == 0) return std::nullopt;

    // The strike where call and put are closest in value is the one nearest the
    // forward, and it is where parity is least distorted by the bid-ask spread
    // on a deep in-the-money leg.
    const ParityQuote* best = nullptr;
    double best_gap = 0.0;

    for (std::size_t i = 0; i < count; ++i) {
        const ParityQuote& q = quotes[i];
        if (!(q.strike > 0.0) || !(q.call > 0.0) || !(q.put > 0.0)) continue;
        const double gap = std::abs(q.call - q.put);
        if (best == nullptr || gap < best_gap) { best = &q; best_gap = gap; }
    }
    if (best == nullptr) return std::nullopt;

    const double fwd = best->strike + std::exp(rate * t) * (best->call - best->put);
    if (!(fwd > 0.0) || !std::isfinite(fwd)) return std::nullopt;
    return fwd;
}

}  // namespace volforge
