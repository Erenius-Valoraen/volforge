# Costs and margin

Both are configuration, not constants. Rates change at budgets and exchange
circulars, and margin parameters change daily. This document records what the
defaults are, where they came from, and — more importantly — where they are
known to be wrong.

## Transaction costs

Verified against published schedules on **2026-08-13**. Sources at the bottom.

| Charge | Rate | Side | Basis |
|---|---|---|---|
| STT | **0.15%** | Sell | Premium |
| STT on exercise | 0.15% | Buy | **Intrinsic value**, not premium |
| NSE transaction charge | 0.03553% | Both | Premium |
| SEBI turnover fee | ₹10 per crore | Both | Premium |
| IPFT | ₹0.01 per crore | Both | Premium |
| Stamp duty | 0.003% (₹300/crore) | Buy | Premium |
| GST | 18% | Both | On brokerage + transaction + SEBI + IPFT |
| Brokerage | ₹20 per executed order | Both | Flat; broker-specific |

GST applies to brokerage and exchange-level charges. It does **not** apply to
STT or stamp duty.

### What this costs in practice

One lot of NIFTY at ₹200 premium is ₹15,000 of turnover per side:

| | Sell | Buy |
|---|---:|---:|
| Brokerage | 20.00 | 20.00 |
| Transaction + SEBI + IPFT | 5.34 | 5.34 |
| GST | 4.56 | 4.56 |
| STT / stamp duty | 22.50 | 0.45 |
| **Total** | **52.40** | **30.35** |

A round trip is roughly **₹83**, or about 0.28% of round-trip turnover. Small per
trade, decisive across a few hundred of them.

### Corrections made

Two of the defaults shipped in the first version were already stale, and one
charge was missing entirely:

- **STT was 0.10%.** Budget 2026 raised it to 0.15% effective 1 April 2026, so
  the engine was understating the single largest statutory charge by a third.
- **Transaction charge was 0.035%.** The published rate is 0.03553%.
- **IPFT was missing.** Immaterial in size, but it was simply absent.

### Known gap: exercise

STT on exercised options is charged on **intrinsic value**, not premium, and
falls on the buyer. On a deep in-the-money option held to expiry this dwarfs
every other charge on the trade.

It is **not applied**, because expiry settlement is not implemented — nothing is
ever exercised. The rate exists in `IndianFnORates` so the gap is visible in code
rather than silent. Any strategy that would hold ITM options into expiry will
have its costs understated until settlement lands.

## Margin

NSE requires **SPAN + Exposure** on short option and futures positions. Long
options require no margin beyond the premium already paid.

### What is implemented

SPAN is a scenario engine, not a formula. The portfolio is fully revalued under
16 joint moves in the underlying and in volatility, and the requirement is the
worst outcome:

- price unchanged, and at ⅓, ⅔ and the full scan range in each direction
- each paired with volatility up and down
- plus two extreme moves at twice the scan range, charged at 33% weight

That structure is reproduced exactly, using Black-76 revaluation. It is why a
defined-risk spread gets a materially smaller number than its legs would
separately, and why the *relative* margins across structures come out right.

Positions are grouped **by expiry**, so a calendar is not margined as if its legs
were unrelated.

On top of the scan:

- **Short option minimum** — a floor as a fraction of net short notional, so a
  deep out-of-the-money short is not charged near zero right up until the move
  that matters.
- **Exposure margin** — 3% of net short notional, matching the index rate.

### What is not implemented, and which way it errs

**NSE Clearing publishes a risk parameter file every trading day** carrying the
actual scan ranges. Without it, the scan ranges here are a NIFTY-shaped guess
(3.5% price, 10% relative volatility). Feed the real parameters into
`SpanParameters` and the model converges on the real thing.

The real calculation also applies intracommodity spread charges, delivery
charges and a net-option-value adjustment that are not modelled.

**The bias runs one way, and it is not uniform:**

| Structure | This model | Reality | Direction |
|---|---|---|---|
| Naked short, 1 lot ATM | ~₹1.05 lakh | ~₹1.2–1.7 lakh | Slightly low |
| Vertical spread | ≈ max loss | Somewhat above max loss | **Low** |
| Iron condor | ≈ max loss of one side | Materially above | **Low** |

Spread margins are understated because exposure is netted across legs and the
intracommodity spread charge is absent. **Return on capital for spread
strategies is therefore flattering**, and by more than for naked shorts. Naked
short-premium strategies are roughly right.

## Overriding both

Both are injectable policies, which is the extension point the Python layer will
bind to.

Tune the rates:

```cpp
IndianFnORates rates;
rates.brokerage_per_order = Money{0};      // zero-brokerage account
rates.stt_sell_pct        = 0.0020;        // next budget
config.costs = std::make_shared<const IndianFnOCosts>(rates);
```

Replace the calculation entirely, for a broker whose structure does not fit this
shape at all:

```cpp
class MyBrokerCosts final : public CostPolicy {
    Money cost_of(const CostContext& ctx) const override { /* ... */ }
};
```

Margin works the same way — set `SpanParameters` from the day's risk parameter
file, or implement `MarginModel` outright:

```cpp
SpanParameters p;
p.price_scan_pct = 0.042;                  // from NSE's file for that day
config.margin = std::make_shared<const SpanMarginModel>(p);
```

Margin is sampled every 60 seconds rather than on every observation, since it
needs an implied-volatility solve per leg and a per-second figure would cost far
more than the precision is worth. `RunResult` reports peak and final
requirement.

## Sources

- [STT rates and Budget 2026 changes — ClearTax](https://cleartax.in/s/securities-transaction-tax-stt)
- [STT rates 2026 — Bajaj Finserv](https://www.bajajfinserv.in/securities-transaction-tax)
- [Charges list (F&O) — Zerodha](https://zerodha.com/charges/)
- [Revision in transaction charges — NSE circular](https://nsearchives.nseindia.com/content/circulars/FA73061.pdf)
- [Margins — NSE Clearing](https://www.nseclearing.in/risk-management/equity-derivatives/margins)
- [NSCCL SPAN — NSE Clearing](https://www.nseclearing.in/risk-management/equity-derivatives/nsccl-span)
- [SPAN risk parameters — NSE Clearing](https://www.nseclearing.in/risk-management/equity-derivatives/span-risk-parameters)
