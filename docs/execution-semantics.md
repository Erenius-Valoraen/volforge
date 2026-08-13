# Execution semantics

Most look-ahead bugs are not someone reading `close[-1]`. They come from three
confusions that feel harmless and quietly invalidate a backtest:

1. Treating a value's **label** as the time it became **knowable**.
2. Leaving the **confirmation policy** implicit, so a signal silently means either
   "price touched this" or "the bar closed beyond this" depending on how the engine
   happened to be written.
3. Collapsing **signal time** and **fill time**, so an order executes at the very
   print that triggered it — or worse, at a price from earlier in the bar.

This document fixes all three explicitly. Nothing here is a default the engine
picks for you; where two answers are both legitimate, the strategy states which
one it means.

## 1. Label time vs knowable time

Every bar carries three timestamps, and they are not interchangeable:

| | Meaning |
|---|---|
| `open_time` | The label. Bars are **open-stamped**, so the 09:20 one-minute bar covers 09:20:00–09:20:59. |
| `close_time` | `open_time + duration`. The 09:20 one-minute bar closes at 09:21:00. |
| `known_at` | When the value could first have been observed. For a completed bar this is `close_time`. |

**Series gate on `known_at`, never on the label.** A one-hour bar labelled 09:15
is not readable at 09:30, 09:45 or 10:00 — it becomes readable at 10:15. Engines
that index by label leak an hour of future into every decision and produce
backtests that cannot be reproduced live.

This is why `bars("5min").close` returns the last *completed* bar, and why
reading the bar in progress requires writing `.forming` on purpose. See
[series-and-timeframes.md](series-and-timeframes.md).

## 2. Indicators inherit knowability

An indicator computed over bars is knowable only when the bars it consumed are.

This is where the subtle version of the bug lives. Consider a 20-period Bollinger
band on one-minute bars, evaluated at 09:20:37:

- A band over the last 20 **completed** bars is knowable. Every input closed at or
  before 09:20:00.
- A band over 19 completed bars **plus the bar currently forming** is not. Its
  mean and standard deviation both depend on where 09:20 eventually closes, which
  is information from 09:21:00.

So intrabar comparisons use the **last completed** band, held flat across the
forming minute, and it steps at each bar close. Nobody wrote `[-1]`, and the
second version still leaks the future.

The general rule: **an indicator's `known_at` is the `known_at` of the most recent
input it consumed.**

## 3. Confirmation policy

"Enter when price crosses the upper band" has two legitimate readings, and the
difference is not cosmetic — it changes which trades exist at all.

### Instant

Enter at the exact second the level is crossed. Evaluated at base resolution
against the last completed band. **If the minute later closes back below the
band, the position was still taken** — the cross happened, and the strategy said
it wanted the cross.

```python
await cross_above(spot.last, bb(m1, 20).upper)          # confirm="instant" (default)
```

### Bar-close confirmation

Enter only if a bar *closes* beyond the level. The signal becomes true at the
bar's `close_time` — that is, `open_time + duration`, since bars are
open-stamped. A bar that pokes above and comes back **produces no trade**.

```python
await cross_above(m1.close, bb(m1, 20).upper, confirm="bar_close")
```

### Worked example

One-minute bars. Price crosses the upper band at 09:20:37, then falls back and the
09:20 bar closes below it.

| | Instant | Bar-close |
|---|---|---|
| Band used | Last completed (bars ≤ 09:20:00) | Bars ≤ 09:21:00, including the 09:20 bar |
| Signal time | 09:20:37 | — |
| Trade taken | Yes | **No** |

Had the bar closed above the band instead, bar-close confirmation would signal at
**09:21:00**, not 09:20 and not 09:20:37.

Neither answer is more correct. Silently picking one is what makes a backtester
untrustworthy, so `confirm` is part of the condition rather than an engine
setting, and it is visible in the strategy source.

## 4. Signal time is not fill time

A signal becoming true is not an execution. The engine keeps them separate:

```
signal true at T  →  order submitted at T  →  fills at first market data ≥ T + execution_delay
```

Orders are **pending** between submission and fill. They cannot fill against the
observation that triggered them, because a strategy that learns a price at T
cannot also have traded on it at T. Filling on the triggering print is the single
most flattering bug a backtester can have.

This is enforced by the shape of the loop rather than by a setting. At each
timestamp the engine advances the clock, **fills eligible orders, and only then
enters strategy code**. An order submitted at step T therefore cannot fill before
step T+1, because step T's fill pass has already run. No configuration defeats
this.

`execution_delay` only ever *adds* to that structural minimum, and defaults to
zero — meaning "the next observation", which on this feed is one second later.
Raising it models a slower path to the exchange. There is no way to lower it
below one observation, so "assume a resting order already at the touch" is not
expressible, because the data cannot support the claim.

Fills themselves cross the spread and warn on illiquid strikes, for reasons in
[design.md](design.md#1-the-source-data).

### Consequence for bar-close entries

A bar-close signal fires at `close_time`, so the fill is at `close_time +
execution_delay` — **never at the bar's open**, and never at the close price that
produced the signal. Filling a close-confirmed signal at that bar's open is the
classic version of this bug: it trades at 09:20:00 on information from 09:21:00.

## 5. Fill resolution never inherits signal resolution

A strategy may reason in any timeframe it likes. **Execution always runs at the
finest resolution the data provides**, and the two are entirely independent.

This matters most for stops, because a stop is not a signal — it is an order that
happens to be conditional. An engine that evaluates stops at the strategy's
timeframe will exit an hourly strategy at the top of the hour, at a price nobody
could have got, an average of half an hour after the level was breached. That
single mistake can make a losing strategy look profitable.

So risk rules are **attached to a position**, not awaited by the strategy:

```python
pos.stop_loss(pct=0.30)      # armed from here on, checked every observation
pos.take_profit(pct=0.50)
pos.exit_at("15:15")
pos.call.stop_loss(pct=0.60) # this leg only
```

Two properties follow, and both are tested:

**Armed continuously.** A strategy can attach a stop and then go and wait on
something else entirely — an hourly bar close, a time hours away — and the stop
stays live. A stop that is only checked while the strategy happens to be looking
at it is not a stop.

**Evaluated at base resolution.** A strategy signalling on hourly bars still has
its stop checked on every observation. The test for this fires a stop at
10:33:17 — deliberately neither an hour nor a minute boundary — while the
strategy itself is awaiting an hourly-bar event.

Rules are routed through the ordinary order path, so a stop is filled by the same
fill model under the same timing guarantee as a discretionary exit: it crosses
the spread, and it cannot fill on the observation that triggered it.

### What a stop is *not*, on this data

An attached stop is a **monitored** stop: the engine observes the breach and
sends a market order. It is not a resting stop order at the exchange filling at
the stop price. With 1-second top-of-book snapshots, intra-second dynamics are
unknowable, so a resting stop cannot be simulated honestly — and a fill *at* the
stop price would be exactly the flattering fiction this document exists to
prevent.

## 6. What the engine forbids

- Reading a bar before its `close_time`, unless `.forming` is written explicitly.
- Computing an indicator over the forming bar and using it intrabar.
- Filling an order against the observation that triggered it.
- Filling a bar-confirmed signal at any price from within that bar.
- A condition whose confirmation policy is ambiguous — `cross_above` on a bar
  series requires `confirm` to be stated.

## 7. The detector

Rules are only worth what enforces them, so the engine ships a whole-strategy
look-ahead check built on one theorem:

> A causal strategy's behaviour over [0, T] cannot depend on any data after T.

`check_lookahead` truncates the session at many cutoffs, replays the strategy
against each prefix, and compares the resulting trades against the full run's
trades up to that cutoff. Every field must match — which trades happened, when
they were signalled, when they filled, at what price. Cutoffs are sampled
deterministically and always include the boundaries either side of every fill.

The value of this approach is that it assumes nothing about *where* a leak might
be. An indicator averaging over the forming bar, a chain query selecting a strike
that lists later in the day, a condition reading a bar before it closes, a fill
taken from the triggering print — all of them change behaviour under truncation,
and none of them have to be anticipated.

It is guarded by its own tests: two deliberately cheating strategies are checked
to make sure the detector flags them. A detector that cannot catch a known leak
is worse than none, because it manufactures confidence.

### What it does not prove

- It only sees leaks that change **trades**. A strategy computing a biased signal
  it never acts on passes.
- Coverage is only as good as the cutoffs sampled.
- A clean report means "no leak was observed at these cutoffs", never "this
  strategy is causal".

## 8. Status

Sections 1–4 and the detector are implemented and tested: bar timing, indicator
knowability, both confirmation policies, order timing, and truncation-based
detection.

Worked example from the test suite, using twenty flat one-minute bars that put
the upper band at exactly 100, then a minute that touches 105 and closes back at
99, then a minute closing at 110:

| | Instant | Bar-close |
|---|---|---|
| Signals at | 09:35:30 — the second of the touch | 09:37:00 |
| The 09:35 poke | Traded | **Ignored** — it closed back below |
| Fill | 09:35:31 | 09:37:01 |

Note that the bar-close signal lands at 09:37:00, which is the 09:36 bar's
`open_time + 60s`. Not 09:36, and not anywhere inside the bar that produced it.
