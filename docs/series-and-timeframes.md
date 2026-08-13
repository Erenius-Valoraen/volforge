# Series, history and timeframes

## Historical access

Series support backward indexing, where `[0]` is the current value:

```python
spot.close[1]                      # previous value
spot.close[1] > spot.close[2]      # comparison over history
ta.rsi(spot.close, 14)[1]          # indicator history
```

Three rules make this safe and unsurprising:

**Negative indices raise.** `close[-1]` is a look-ahead request, so it is an error rather
than a value. There is no accessor for the future anywhere in the API.

**Insufficient history yields `nan`.** Asking for `close[500]` on the 3rd bar produces `nan`
rather than an exception or a silently wrong number. `nan` propagates through arithmetic, and
`series.valid` guards where a strategy needs to branch on availability.

**There is no history limit.** Indicators are precomputed arrays and `[n]` is an array index,
so depth is bounded by the data itself rather than by an arbitrary buffer. (Pine caps this
at 5,000 bars and reports the overflow as a confusing `max_bars_back` error.)

### Two evaluation contexts

The same expression means something slightly different depending on where it appears, and
this is deliberate:

```python
await spot.close[1] > spot.close[2]   # a condition: evaluated continuously until true
if spot.close[1] > spot.close[2]:     # a value: evaluated once, right now
```

Expressions build lazy condition objects. Using one in a boolean or numeric context resolves
it against the current simulation time; awaiting it compiles it to a native predicate and
suspends. Both readings are the natural one for their context.

### What "one step back" means

Pine never has to answer this because chart bars are regular. Options data is not: an
illiquid strike may not update for 30 seconds, so "the previous value" and "the value one
second ago" are different questions.

Every series therefore carries an explicit timebase:

| Series kind | `[1]` means |
|---|---|
| Raw quote/snapshot series | The previous **observation**, irregular in time |
| Bar series (`bars("1min")`) | The previous **bar**, regular on the session grid |

Where a strategy needs elapsed time rather than observation count, `series.ago(seconds=30)`
is explicit about it. The ambiguity is resolved by the series knowing what it is, never by
convention.

## Timeframes

Source data is 1-second snapshots. Strategies that want candles resample:

```python
m5  = bars("5min")                        # index/spot bars
m1  = bars("1min", of=pos.call)           # per-instrument bars
await m5.close > ta.highest(m5.high, 20)
```

### Only completed bars are visible

This is the single most important decision here, and it is what Pine gets wrong badly enough
to have a documented category of bug named after it.

A 5-minute bar starting at 09:20 is not complete until 09:25. If a strategy reads that bar's
close at 09:22, it is reading a value that did not exist yet. In Pine this is the `request.
security()` lookahead problem, whose correct usage is the genuinely obscure incantation of an
offset of `[1]` combined with `lookahead_on`, and whose incorrect usage silently produces
beautiful backtests that cannot be traded.

In volforge, **higher-timeframe series expose only closed bars.** There is no lookahead flag
to set wrong. Reading the in-progress bar requires naming it:

```python
m5.close          # last completed bar — always safe
m5.forming.close  # the bar currently building — explicit, and never the default
```

### Session-aligned boundaries

Bar boundaries follow the trading session, not wall-clock arithmetic. With an NSE session
opening at 09:15, 5-minute bars run 09:15–09:20, 09:20–09:25 and so on. Alignment is a
property of the session calendar so that instruments, expiries and venues with different
hours stay correct.

### Resampling is lazy and per-instrument

Contract selection happens at runtime, so which of ~900 instruments need bars cannot be known
in advance. Series are therefore resampled on first access and cached for the session. A
strategy touching four strikes pays for four, not nine hundred.

### Option bars carry more than OHLC

An option bar aggregates quotes, not just trades, so it exposes bid, ask and open interest at
bar close alongside OHLC and summed volume. Which price drives OHLC is explicit, because both
answers are defensible and they differ materially on illiquid strikes:

```python
bars("1min", of=leg, price="ltp")   # last traded — stale when nothing trades
bars("1min", of=leg, price="mid")   # bid/ask midpoint — continuous but not tradable
```

This matters more than it looks: 82.8% of rows in the sample day carry no trade, so
LTP-based bars on a quiet strike can be minutes stale while the quote moves. Bars expose
`staleness` so a strategy can refuse to act on a dead print.

### The event loop stays at base resolution

Replay always runs at the source's 1-second resolution. Bar series update at their
boundaries, and a strategy awaiting a 5-minute condition simply wakes at 5-minute marks.
Resampling changes what a strategy *sees*, never how the engine steps.
