# volforge

A backtesting engine for options, built for people who need to trust the result.

> **Status: design phase.** The architecture below is settled; implementation has not started.
> Nothing here is usable yet.

## Why another backtester

Most retail options backtesters are wrong in ways you can't see. They fill at mid-price,
they leak future data into indicators, they model a four-leg spread as four unrelated
orders, and they ignore margin entirely — which makes short-premium returns off by an
order of magnitude.

volforge takes the opposite position: **the engine is open so that the assumptions behind
your results are auditable.** If a backtest here says a strategy made money, you should be
able to read exactly which fill, margin, and causality rules produced that number.

## Design principles

**Look-ahead is impossible, not merely discouraged.** The context object at time *T* has no
accessor for data after *T*. Custom indicators are pure functions, so the engine verifies
causality automatically by checking `f(data[:k]) == f(data)[:k]` for random `k`. An
indicator that peeks at the future is rejected, and the check cannot be disabled.

**Fills are pessimistic by default.** Source data is 1-second top-of-book snapshots, so
sub-second queue position is unknowable and the engine never pretends otherwise. The
default model crosses the spread and warns on fills in illiquid strikes.

**Margin is a first-class model.** Positional short-premium strategies are dominated by
capital usage. Any engine that reports ROI without modelling SPAN + exposure margin is
reporting fiction.

**Every run is reproducible.** Runs emit a manifest — data version, engine version, config
hash, fill model — so two results are always comparable.

**Multi-leg positions are one object.** A condor has a net delta, a combined P&L and a
single stop. It is not four independent orders.

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

```python
from volforge import Strategy, params

class ShortStraddle(Strategy):
    entry_time = params.Time("09:20")
    stop_pct   = params.Float(0.30, sweep=[0.2, 0.3, 0.4, 0.5])
    target_pct = params.Float(0.50)

    def setup(self, ctx):
        ctx.schedule(self.entry_time, self.enter)

    def enter(self, ctx):
        legs = ctx.chain(dte=0).atm().straddle()
        pos  = ctx.sell(legs, lots=1)
        pos.stop_loss(pct=self.stop_pct)       # evaluated natively, every second
        pos.take_profit(pct=self.target_pct)
        pos.exit_at("15:15")
```

Python runs twice per day here. The stop is evaluated 22,500 times per day in C++.

Because parameters are *declared* rather than hardcoded, sweeps are free — the engine knows
the parameter space, parallelises across cores, and reuses each day's decoded data across
every configuration. You never write a sweep loop.

See [docs/strategy-api.md](docs/strategy-api.md) for the full authoring model, and
[docs/design.md](docs/design.md) for the architecture rationale and measurements.

## Scope

v1 targets intraday and overnight positional strategies on index options, with Greeks and
implied volatility computed natively.

The storage layer is deliberately unspecified pending a vendor data-format decision; see
[docs/design.md](docs/design.md) for the measurements that will inform it.

## License

Apache-2.0. Strategies you write on top of volforge are yours and are not covered by it.
