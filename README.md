# volforge

A backtesting engine for options, built for people who need to trust the result.

> **Status: early, but it runs.** Strategies execute end to end in C++ — data interface,
> event loop, coroutines, bars and multi-timeframe, indicators, orders, fills, position P&L
> and transaction costs, plus a whole-strategy look-ahead detector.
>
> Greeks, implied volatility and delta-based strike selection are in, priced off a forward
> recovered from the chain by put-call parity — so they need no index feed at all.
>
> Margin is modelled with a SPAN-style 16-scenario revaluation plus exposure, so defined-risk
> spreads margin far below their legs. The storage layer and the Python layer are not written
> yet, so this is not usable for real backtesting.
>
> Working examples: [short_straddle.cpp](examples/short_straddle.cpp),
> [bollinger.cpp](examples/bollinger.cpp).
>
> The storage layer is deliberately unspecified pending a vendor format decision; the engine
> is written against the abstraction, not the format.

## Why another backtester

Most retail options backtesters are wrong in ways you can't see. They fill at mid-price,
they leak future data into indicators, they model a four-leg spread as four unrelated
orders, and they ignore margin entirely — which makes short-premium returns off by an
order of magnitude.

volforge takes the opposite position: **the engine is open so that the assumptions behind
your results are auditable.** If a backtest here says a strategy made money, you should be
able to read exactly which fill, margin, and causality rules produced that number.

## Design principles

**Look-ahead is checked, not asserted.** `check_lookahead` replays a strategy against
progressively truncated sessions and compares the trades, resting on a simple theorem: a
causal strategy's behaviour over [0, T] cannot depend on data after T. It assumes nothing
about where a leak might be, and it is guarded by its own tests — two deliberately cheating
strategies verify the detector actually catches leaks.

**Look-ahead is hard to write by accident.** There is no accessor for data after the
current simulation time — `close[-1]` raises rather than returning a value. Custom indicators
are pure functions, so the engine verifies causality automatically by checking
`f(data[:k]) == f(data)[:k]` for random `k`, and rejects any indicator that peeks. Higher
timeframes expose only *completed* bars, so the repainting class of bug has no entry point:
reading a bar that is still forming requires writing `.forming` on purpose.

**Fill resolution never inherits signal resolution.** A strategy may reason in hourly bars;
execution always runs at the finest resolution the data provides. Stops are *orders*, not
signals — attached to a position, armed continuously, and checked on every observation even
while the strategy is awaiting something else entirely. An engine that checks stops at the
strategy's timeframe exits an hourly strategy at the top of the hour, at a price nobody could
have got.

**Fills are pessimistic by default.** Source data is 1-second top-of-book snapshots, so
sub-second queue position is unknowable and the engine never pretends otherwise. The
default model crosses the spread and warns on fills in illiquid strikes.

**Margin is a first-class model.** Positional short-premium strategies are dominated by
capital usage. Any engine that reports ROI without modelling SPAN + exposure margin is
reporting fiction.

**Every run is reproducible.** Runs emit a manifest — data version, engine version, config
hash, fill model — so two results are always comparable.

**Risk rules resolve at the level you apply them.** A position is a container of legs.
Applied to the position, a stop uses the combined view; applied to a leg, it affects only
that leg; the two mix freely. Grouping is not bookkeeping — margin on a defined-risk
structure is materially lower than the sum of its legs, so a condor modelled as four
independent orders reports capital usage a broker would never charge.

## Architecture

The performance rule that drives the layering: **cost is determined by call frequency.**

| Component | Calls/day | Runs in |
|---|---:|---|
| Indicator computation | ~6,500,000 | Native (vectorized / JIT) |
| Stop, target, trailing evaluation | ~22,500 | C++ |
| Chain selection | ~5–50 | Python |
| Entry / exit decisions | ~5–50 | Python |
| Reporting and analytics | 1 | Python |

Python is therefore free for most of what a strategy author actually writes. Only
indicators and continuous position monitoring need to be native.

```
volforge-core   (C++)      data store, event loop, fills, portfolio, margin, Greeks/IV
volforge-ta     (C++/py)   streaming indicators
volforge        (Python)   strategy API, chain queries, sweeps, reporting
────────────────────────────────────────────────────────────────────────────
your strategies (private)  imports the above, ships nothing back
```

## What a strategy looks like

A strategy is a coroutine. It reads as straight-line script, while the conditions it waits
on compile to native code and run in C++.

```python
@strategy
async def short_straddle(stop=0.30, target=0.50, entry="09:20"):
    await clock.at(entry)
    pos = sell(chain(dte=0).atm().straddle(), lots=1)
    await (pos.pnl_pct <= -stop) | (pos.pnl_pct >= target) | clock.at("15:15")
    close(pos)
```

Python wakes twice per session. The condition behind that `await` is evaluated 22,500 times
per session natively.

Risk rules attach at whatever level you want, and mix:

```python
pos.stop_loss(pct=0.30)           # combined position P&L
pos.call.stop_loss(pct=0.60)      # that leg only
close(pos.call)                   # continues as a naked short put
```

**Research lives outside the strategy.** A strategy describes what it *is*; an experiment
describes what you're investigating. The strategy file has no idea sweeps exist.

```python
# research/stop_sensitivity.py
grid = sweep(short_straddle,
             stop=[0.2, 0.3, 0.4, 0.5],
             target=arange(0.3, 0.8, 0.1),
             data=nifty("2025"))

grid.heatmap("stop", "target", metric="sharpe")
```

The same strategy can be swept different ways in different experiments without anyone
editing it. Sweeps are parallelised across cores and reuse each day's decoded data across
every configuration.

Documentation:

- [docs/strategy-api.md](docs/strategy-api.md) — the full authoring model
- [docs/series-and-timeframes.md](docs/series-and-timeframes.md) — history access, resampling, multi-timeframe
- [docs/costs-and-margin.md](docs/costs-and-margin.md) — verified charge rates, SPAN methodology, and where both are known to be wrong
- [docs/design.md](docs/design.md) — architecture rationale and measurements
- [docs/execution-semantics.md](docs/execution-semantics.md) — signal time vs fill time, confirmation policy, and where look-ahead really comes from
- [docs/pinescript-lessons.md](docs/pinescript-lessons.md) — what we're deliberately doing differently, and why

## Scope

v1 targets intraday and overnight positional strategies on index options, with Greeks and
implied volatility computed natively.

The storage layer is deliberately unspecified pending a vendor data-format decision; see
[docs/design.md](docs/design.md) for the measurements that will inform it.

## License

Apache-2.0. Strategies you write on top of volforge are yours and are not covered by it.
