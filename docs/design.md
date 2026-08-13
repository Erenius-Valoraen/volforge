# Design rationale

This document records the architectural decisions behind volforge and the measurements
that justify them. It is meant to be falsifiable: where a decision rests on a number, the
number and how it was obtained are stated.

## 1. The source data

Measurements below come from a single vendor sample day (NIFTY options, 2025-07-01), which
is what the design was calibrated against.

| Property | Value |
|---|---|
| Rows | 6,545,762 |
| Instruments | 904 |
| Raw CSV | 520.1 MB |
| Zipped | 69.1 MB |
| Timestamp granularity | 1 second (`HH:MM:SS`) |
| Unique `(instrument, second)` pairs | 6,526,221 (99.7% of rows) |
| Rows with no trade (`LTQ = 0`) | 82.8% |
| Rows identical to previous | 0.8% |
| Order book depth | Top of book only |
| Rows per instrument | min 6, median 7,956, max 13,950 |

### Consequences

**This is not tick data.** It is a 1-second snapshot feed. Sub-second ordering does not
exist in the source, so queue position and intra-second sequencing are unknowable. The fill
model must not pretend otherwise — this is a hard ceiling on simulation fidelity, not an
implementation gap.

**Top-of-book only.** Any slippage beyond the touch price is a model assumption, not data.
Spreads on illiquid strikes are wide enough to matter: the sample contains deep-ITM quotes
around 2.5% wide. Filling at mid would manufacture returns that do not exist.

**Deduplication is not worth it.** Only 0.8% of rows repeat their predecessor exactly.

**No underlying series in the options feed.** Spot/futures data arrives separately. This
does not block Greeks — see §4.

## 2. Storage

**Status: parked.** The vendor's delivery format is not yet known, so the storage layer is
deliberately unspecified. The measurements below were taken to inform the decision when it
is made.

### Encoding measurements

Encoding prices as int32 paise, time as seconds-from-midnight, then delta-encoding and
compressing with zstd-9:

| Variant | Size | vs CSV | Bytes/row |
|---|---:|---:|---:|
| CSV (source) | 520.1 MB | 1.00× | 79.50 |
| Parquet, naive strings + floats + snappy | 48.8 MB | 10.7× | 7.46 |
| Int-encoded + zstd | 40.9 MB | 12.7× | 6.25 |
| Int-encoded + delta + zstd | **30.9 MB** | **16.8×** | **4.72** |

Query performance against the 30.9 MB file: full day (6.5M rows) loads in 0.59 s, a
three-column projection in 0.03 s, a single-instrument slice in 23 ms, and a vectorized
pass over all rows in 34 ms.

### Sort order costs 3.3×

| Sort key | Size | Bytes/row |
|---|---:|---:|
| `(instrument, second)` | 30.9 MB | 4.72 |
| `(second, instrument)` | 102.2 MB | 15.61 |

Storage should therefore group by instrument, while the event loop needs time order. The
two are reconciled in memory at load time — each instrument's rows are already time-sorted,
so a k-way merge of the per-instrument runs is O(n log k).

### Why the format matters less than it appears

At roughly 30 MB/day compressed, a 250-session year is ~7.7 GB. Decoded to working arrays
that is ~18 GB for all columns, but any real strategy touches three or four columns, i.e.
7–9 GB — which fits in memory.

That makes the correct sweep architecture *load each day once, run every configuration
against RAM*. Under that structure a 500-configuration annual sweep spends ~2.5 minutes on
I/O against hours of compute. **Storage format is not the sweep bottleneck; the engine is.**
This argues against over-investing in a bespoke format.

### Open question on data volume

The vendor states 45 GB for one year. The sample day implies ~130 GB/year raw or ~17 GB/year
zipped, so 45 GB reconciles with neither. Most likely the figure covers more than NIFTY
options, or the sample day (two sessions before a weekly expiry) is denser than average.
Worth confirming, as it determines whether a decoded year fits in memory.

## 3. Why the Python layer cannot be in the event loop

At ~6.5M events/day and roughly 1 µs per Python call, a per-event Python callback costs
~27 minutes per annual run — which would negate the native core entirely. A pure-Python
loop was measured at 114 ns/row doing nothing but integer addition; realistic strategy
logic runs 0.5–2 µs/row. Equivalent C++ runs at 2–10 ns/row.

The resolution is that **Python configures the loop rather than running inside it.**

Strategies are coroutines. `await <condition>` suspends the strategy, and the condition
compiles to a native predicate evaluated in C++. Python resumes only when it fires, so a
strategy wakes a handful of times per session while its conditions are checked 22,500 times.

Coroutines were chosen over callback registration because a callback model cannot express
sequence: "enter, wait, adjust, exit" has to be shredded across disconnected methods with
state flags threaded between them. Suspension is also precisely the fact the engine needs —
*this strategy is dormant until condition C holds* — so the abstraction and the
implementation want the same thing.

Conditions that cannot compile to native code are rejected unless explicitly wrapped in
`slow()`. The cliff is therefore impossible to hit by accident, and every `slow()` condition
is reported with its measured cost in the run summary.

## 4. Greeks without spot data

Implied volatility does not require the index feed. Put-call parity recovers the forward
from the option chain itself:

```
F = K + (C − P)·e^(rT)
```

evaluated at the ATM strike. The Greeks module therefore takes a pluggable spot source and
uses a parity-derived forward until an index series is available. The missing underlying
data does not block v1.

## 5. Custom indicators at native speed

The key observation is that **indicators are pure functions of the price series.** SMA,
RSI, ATR, VWAP, Bollinger and IV rank depend on no strategy state — not position, not entry
price, not P&L. They therefore do not belong in the event loop at all: they are computed
vectorized, once per day, before replay begins, and the loop reads a precomputed column.

This eliminates the performance problem for the large majority of custom indicators, since
NumPy over a day of data runs at native speed.

### Tiers

| Tier | Author writes | Speed | Use |
|---|---|---|---|
| 0 | Built-in library | Native | Standard indicators |
| 1 | Vectorized NumPy | ~Native | Default for custom |
| 2 | Numba-JIT scalar loop | ~1–2× C | Recursive/stateful math |
| 3 | Composed native primitives | Native | Terse expressions |
| 4 | Plain Python callback | Slow | Everything else |

Tier selection is automatic, and **the engine reports the tier and measured cost of each
indicator** so the author can see exactly what is expensive rather than guessing.

### Causality validation

Precomputing over a whole day creates a look-ahead risk — a centered window, or
normalisation by the session high, would silently leak the future. Because indicators are
pure functions, this is checkable: evaluate `f(data[:k])` and compare against `f(data)[:k]`
for random `k`. Any mismatch means the indicator reads future data, and it is rejected.
This check is mandatory.

## 6. Extensibility

Requirements cannot be enumerated in advance, so nothing is a fixed menu. Indicators, fill
models, position sizers, exit rules, chain selectors and data adapters are registries
behind stable interfaces.

Extensions declare an optional native form and fall back to Python when they cannot:

```python
@register.exit_rule("delta_breach")
class DeltaBreach(ExitRule):
    def compiles_to(self):                  # optional native path
        return native.abs(native.delta()) > self.threshold
    def evaluate(self, pos, ctx):           # always-available fallback
        return abs(pos.delta) > self.threshold
```

This lets user extensions be fast when possible without ever being blocked when not.

Chain selection is arbitrary Python by design. It runs a handful of times per day over ~900
contracts — a few thousand predicate calls — so there is no reason to constrain it to a DSL.

## 7. Positions and legs

A position is a container of legs, and **every risk rule and metric resolves at the level it
is applied to** — position level uses the combined view, leg level affects only that leg,
and the two mix within one position. Closing a leg leaves the remainder live.

The grouping is not presentational. Margin on a defined-risk structure is materially lower
than the sum of its legs priced independently, so modelling a condor as four separate
positions would report capital usage no broker would charge, and therefore the wrong return
on capital. Net Greeks, combined P&L percentage and position-level stops are likewise only
meaningful against the group.

This is why legs are addressable *within* a position rather than being separate positions
that happen to be related.

## 8. Live parity

The same strategy source should eventually run against live data unchanged. The coroutine
model already describes an event-driven system rather than a replay, which is most of what
that requires — but three constraints must hold from the start or the promise cannot be
kept retroactively:

- **Every indicator has both a vectorized and a streaming implementation, with an
  equivalence test asserting they agree.** Backtests precompute over the day; live must
  compute incrementally. Without an enforced check the two implementations drift, and the
  divergence would surface as unexplained live-versus-backtest mismatch.
- **Clock, data and execution are injected interfaces**, never globals bound to replay.
  Going live swaps implementations, not strategies.
- **Orders are event-driven in both modes.** Fills arrive as events even in backtest, so
  partial fills and rejections need no separate strategy code path.

Two things will not transfer for free and should not be papered over. **Latency** exists
live and not in replay, so results differ regardless of API fidelity. **Crash recovery** is
the genuinely hard part: a live strategy that dies must resume holding open positions *and*
at the right point in its own control flow, which makes a coroutine's execution position
recoverable state. That is not yet designed, and it is the main open question for live
support.

## 9. v1 scope

Intraday **and** overnight positional strategies on index options.

Positional scope makes several things mandatory rather than optional:

- **Margin modelling** (SPAN + exposure). Capital usage dominates returns for short-premium
  positional strategies.
- **Expiry rolls** and position lifecycle across sessions.
- **Overnight gap handling** and carry.

Greeks and implied volatility are in v1. Contract selection is unconstrained — users select
by delta, moneyness, premium, strike offset, open interest, spread, or any predicate they
write.

## 10. Accounting

A backtest is only worth what its P&L means, so the accounting path was audited
specifically for anything that flatters returns. Four defects were found and
fixed; they are recorded here because each one is easy to reintroduce.

### Marking

Open legs mark at the price that would **close** them: a short at the ask it
would be bought back at, a long at the bid it would be sold into. Marking at mid
reports profit that closing would not realise.

Two edge cases matter more than they look:

- **A long option with no bid is worth nothing, and the loss is total.** An
  earlier version returned zero P&L when the closing side was absent, which
  deleted a 100% loss outright and stopped any percentage stop from ever firing.
- **A short option with no offer cannot be bought back at zero.** Marking it
  there would book the entire premium as profit. It falls back to the last trade,
  and failing that refuses to claim a gain at all.

### Costs

Transaction costs are charged on every fill. Omitting them is one of the largest
sources of overstated returns in Indian F&O: statutory charges alone run to
roughly 0.14% of premium on a round trip, which decides whether a high-turnover
intraday strategy makes money at all. In the sample straddle run they turn a
gross loss of ₹5,587.50 into ₹5,719.30; on the Bollinger run they are about a
third of the gross result.

`CostModel` covers STT (sell side), exchange transaction charges, SEBI turnover
fees, stamp duty (buy side), GST, and per-order brokerage. **The default rates
were current at the time of writing and are configuration, not constants** —
verify them against your broker's contract note.

Results report `realized` (gross), `costs`, `net_realized`, `unrealized` and
`final_equity` as separate lines, so the difference between what a strategy
earned and what an account would have kept is never buried in one number.

### Order lifecycle

Closing a position **cancels its unfilled opening orders**. Previously a queued
leg could fill after the position was closed, silently leaving a naked position
the strategy never asked for and would never manage.

### Size against displayed liquidity

The feed carries top of book only, so an order larger than the displayed size
would in reality walk into levels that cannot be seen. Such fills happen at the
touch but are **counted and reported** as `oversized_fills`, because silently
filling size that was never displayed is a liquidity claim the data cannot
support.

### Known limitations

- Oversized fills are flagged, not rejected or penalised. With no depth data,
  any slippage model beyond the touch would be invention.
- Margin is not modelled yet. Positional short-premium returns are dominated by
  capital usage, so return-on-capital is not yet meaningful.
- Percentage stops are meaningless on a structure whose net premium is near zero,
  since the denominator vanishes. Such positions need absolute stops, which do
  not exist yet.
- Vendor open interest reflects the previous day's close rather than live
  positioning, so any selection keyed on OI is using a stale figure.
