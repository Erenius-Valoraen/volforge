// Pricing, implied volatility and Greeks.
//
// Priced off the **forward**, not spot (Black-76). Indian index options are
// priced against the futures, and the forward is recoverable from the option
// chain itself by put-call parity:
//
//     F = K + e^{rT}(C - P)
//
// evaluated at the strike where |C - P| is smallest, which is the strike nearest
// the money. That means Greeks work with no index or futures feed at all — a
// useful property, since the options feed does not carry one.
//
// Everything here is a pure function of inputs the caller supplies. Causality is
// the caller's problem and is handled where these are wired to a MarketView: the
// quotes fed in come from the time-bounded view, so a Greek can never be computed
// from a price that has not printed.

#pragma once

#include "volforge/types.hpp"

#include <optional>

namespace volforge {

struct GreekSet {
    double iv    = 0.0;   // annualised, as a fraction (0.18 == 18%)
    double price = 0.0;   // model price at that iv
    double delta = 0.0;   // per 1.0 move in the forward
    double gamma = 0.0;   // per 1.0 move in the forward
    double vega  = 0.0;   // per 1.00 (100 vol points) change in iv
    double theta = 0.0;   // per calendar day
};

// Year fraction to expiry on ACT/365, from `now` to 15:30 local on expiry day.
// Returns 0 at or after expiry.
double time_to_expiry(Timestamp now, Date expiry, int utc_offset_seconds,
                      int expiry_second_of_day = 15 * 3600 + 30 * 60);

// Black-76 price of an option on a forward.
double black76_price(double forward, double strike, double t, double sigma, double rate,
                     Right right);

// Greeks at a given volatility.
GreekSet black76_greeks(double forward, double strike, double t, double sigma, double rate,
                        Right right);

// Implied volatility from a market price.
//
// Returns nullopt when no volatility reproduces the price — a quote below
// intrinsic, above the forward, or otherwise unsolvable. Deep wings with wide
// spreads routinely land there, and reporting a fabricated number would be worse
// than reporting none.
std::optional<double> implied_vol(double market_price, double forward, double strike, double t,
                                  double rate, Right right);

// Full set from a market price: solves iv, then evaluates Greeks at it.
std::optional<GreekSet> implied_greeks(double market_price, double forward, double strike,
                                       double t, double rate, Right right);

// One (call, put) pair at a strike, for the parity solve.
struct ParityQuote {
    double strike = 0.0;
    double call   = 0.0;
    double put    = 0.0;
};

// Forward implied by put-call parity, using the strike where |C - P| is smallest.
std::optional<double> forward_from_parity(const ParityQuote* quotes, std::size_t count,
                                          double t, double rate);

}  // namespace volforge
