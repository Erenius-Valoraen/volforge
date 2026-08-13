# Strategy authoring

A strategy is a coroutine. It reads as straight-line script, while the conditions it waits
on are compiled to native code and evaluated in C++.

```python
@strategy
async def short_straddle(stop=0.30, target=0.50, entry="09:20"):
    await clock.at(entry)
    pos = sell(chain(dte=0).atm().straddle(), lots=1)
    await (pos.pnl_pct <= -stop) | (pos.pnl_pct >= target) | clock.at("15:15")
    close(pos)
```

Python wakes twice per session here. The condition behind that `await` is evaluated 22,500
times per session natively. See [design.md](design.md#3-why-the-python-layer-cannot-be-in-the-event-loop)
for why this split exists.

## Why coroutines

A callback-registration model cannot express sequence. "Enter, wait, adjust, exit" has to be
shredded across disconnected methods with state flags threaded between them, which is what
makes most backtesting APIs painful once a strategy becomes real.

`await` restores the sequence without putting Python in the hot loop, because suspension is
exactly what the engine needs to know: *this strategy is dormant until condition C holds.*

## Parameters

Parameters are ordinary function arguments with defaults. There are no wrapper types and no
declaration of a search space — a strategy does not know that sweeps exist.

```python
@strategy
async def short_straddle(stop: float = 0.30, target: float = 0.50, entry: Time = "09:20"):
```

Annotations are optional and used for validation when present.

## Awaiting conditions

Conditions are expressions over series. They compose with `|` and `&`:

```python
await clock.at("09:20")                       # time
await spot.close > ta.highest(spot.high, 20)  # indicator series
await abs(pos.delta) > 50                     # position state
await pos.closed | clock.at("15:15")          # either
```

`any_of(...)` and `all_of(...)` are available where a chain of operators would read poorly.

## Positions and legs

A position is a container of legs. **Every risk rule and every metric resolves at the level
you ask for it**: applied to the position it uses the combined view, applied to a leg it
affects only that leg. The two mix freely.

```python
@strategy
async def strangle(stop=0.30, leg_stop=0.60):
    await clock.at("09:20")
    pos = sell(chain(dte=0).strangle(delta=0.20), lots=1)

    pos.stop_loss(pct=stop)              # combined position P&L
    pos.call.stop_loss(pct=leg_stop)     # this leg only
    pos.put.stop_loss(pct=leg_stop)      # this leg only

    await pos.closed | clock.at("15:15")
    close(pos)
```

Metrics follow the same rule — `pos.delta` is net across legs, `pos.call.delta` is that leg
alone. Likewise `pnl`, `pnl_pct`, `theta`, `vega`, `premium`.

Legs are addressable several ways:

```python
pos.legs[0]                  # by index
pos.calls, pos.puts          # by right
pos.call, pos.put            # singular form when unambiguous
pos.short_call, pos.long_put # by role, in recognised structures
pos.leg(strike=23000)        # by strike
```

Closing a leg leaves the rest of the position live, which is what adjustment strategies
need:

```python
close(pos.call)     # continues as a naked short put
```

### Why not just use two positions

Because grouping is not bookkeeping — it changes the numbers. Margin on a defined-risk
structure is materially lower than the sum of its legs treated separately, and P&L
percentage, net Greeks and combined stops are only meaningful against the group. A short
straddle held as two independent positions would report capital usage that does not match
what a broker would actually block.

## Contract selection

The tradable universe changes daily and instruments are chosen **by query at runtime**, not
named in advance. "Sell the 0.30-delta call" does not identify a symbol.

```python
chain(dte=0).atm().straddle()
chain(dte=0).calls.delta(0.30)
chain(expiry="weekly").puts.otm(pct=2)
chain(dte=2).atm(offset=+2)
chain(dte=0).iron_condor(short_delta=0.16, wing=200)
```

Selection is unconstrained Python, because it runs perhaps 5–50 times per session over ~900
contracts — a few thousand predicate calls, which is free:

```python
chain(dte=0).where(lambda c: c.delta < 0.30 and c.oi > 10_000) \
            .max_by(lambda c: c.premium / c.spread)
```

## Indicators

Indicators are pure functions of the price series, so they are computed vectorized ahead of
replay and the event loop reads a precomputed column. Custom indicators therefore reach
native speed without anyone writing C++.

```python
@indicator
def atr(high, low, close, n=14):
    tr = np.maximum(high - low, np.abs(high - shift(close, 1)))
    return rolling_mean(tr, n)
```

For recursions that will not vectorize:

```python
@indicator(jit=True)
def adaptive_ema(close, alpha):
    out, acc = np.empty_like(close), close[0]
    for i in range(len(close)):
        acc = alpha[i] * close[i] + (1 - alpha[i]) * acc
        out[i] = acc
    return out
```

Every indicator is validated for causality before use — see
[design.md](design.md#5-custom-indicators-at-native-speed).

## The escape hatch is opt-in

Conditions that cannot compile to native code are **rejected unless explicitly marked**.
This makes the performance cliff impossible to hit by accident:

```python
await slow(lambda: my_arbitrary_python_check(pos))
```

`slow()` is deliberately blunt. The run summary reports every `slow()` condition with its
measured cost, so a strategy can never be quietly slow — it is either fast or visibly
marked.

## Research is separate

A strategy describes what it *is*. An experiment describes what you are *investigating*.
They live in different files and neither imports the other's concerns.

```python
# strategies/short_straddle.py
@strategy
async def short_straddle(stop=0.30, target=0.50, entry="09:20"):
    ...
```

```python
# research/stop_sensitivity.py
from strategies.short_straddle import short_straddle

grid = sweep(short_straddle,
             stop=[0.2, 0.3, 0.4, 0.5],
             target=arange(0.3, 0.8, 0.1),
             data=nifty("2025"))

grid.heatmap("stop", "target", metric="sharpe")
```

Sweeps validate against the function signature, so a misspelled parameter is an error rather
than a silently ignored keyword. The same strategy can be swept different ways in different
experiments without touching the strategy file.

## Live parity

The same strategy source is intended to run against live data unchanged, and the coroutine
model is chosen partly because it already describes an event-driven system rather than a
replay.

Three constraints exist in the design specifically to keep that promise:

- **Every indicator has both a vectorized and a streaming implementation**, with an
  equivalence test asserting they agree. Backtests precompute; live must compute
  incrementally. Without enforced agreement, the two would silently diverge.
- **Clock, data and execution are injected interfaces**, never globals bound to replay.
  Switching to live swaps the implementations, not the strategy.
- **Orders are event-driven in both modes.** Fills arrive as events even in backtest, so
  partial fills and rejections do not require different strategy code.

What will *not* transfer for free, and should not be pretended otherwise:

- **Latency.** Backtest has none; live has some. Results will differ regardless of API.
- **Crash recovery.** A live strategy that dies must resume with open positions and its
  place in its own control flow. A coroutine's position in its execution is state that has
  to be made recoverable. This is the genuinely hard part of live support and is not yet
  designed.

## What the engine will not let you do

- Read data from after the current simulation time. There is no accessor for it.
- Register an indicator that fails causality validation.
- Use a non-compilable condition without marking it `slow()`.
- Fill at mid-price by default, or fill in an illiquid strike without a warning.
- Report positional returns without a margin model.
