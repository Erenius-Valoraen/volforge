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

## 5. What the engine forbids

- Reading a bar before its `close_time`, unless `.forming` is written explicitly.
- Computing an indicator over the forming bar and using it intrabar.
- Filling an order against the observation that triggered it.
- Filling a bar-confirmed signal at any price from within that bar.
- A condition whose confirmation policy is ambiguous — `cross_above` on a bar
  series requires `confirm` to be stated.

## 6. Status

The order timing model in section 4 is implemented. Bars, indicators and the
`confirm` policy in sections 1–3 are specified here and not yet built; they are
the next layer, and the interfaces above are shaped to accept them.
