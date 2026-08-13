# Strategy authoring

The design goal is that writing a strategy feels like writing Python, while the work
happens in C++. This is achievable because the parts of a strategy an author actually
writes run rarely — see the frequency table in [design.md](design.md#3-why-the-python-layer-cannot-be-in-the-event-loop).

## The model: declare intent

A strategy describes *what should happen*. The engine evaluates those declarations
natively. Python is called a handful of times per day.

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
        pos.stop_loss(pct=self.stop_pct)
        pos.take_profit(pct=self.target_pct)
        pos.exit_at("15:15")
```

`stop_loss` and `take_profit` are not Python callbacks. They are declarations the engine
evaluates every second in native code.

## Parameters and sweeps

Parameters are declared, not hardcoded, so the engine knows the search space. Sweeps are
therefore free — parallelised across cores, with each day's decoded data reused across
every configuration:

```python
results = backtest(ShortStraddle, data, period="2025")   # single run
results = sweep(ShortStraddle, data, period="2025")      # every declared combination
```

You never write a sweep loop.

## Contract selection

Options differ fundamentally from equities: the tradable universe changes daily, and
instruments are chosen **by query at runtime**, not named in advance. "Sell the 0.30-delta
call" does not identify a symbol.

Selection is therefore a first-class part of the API, and unconstrained:

```python
ctx.chain(dte=0).calls.delta(0.30)              # by Greek
ctx.chain(expiry="weekly").puts.otm(pct=2)      # by moneyness
ctx.chain(dte=2).atm(offset=+2)                 # by strike step
ctx.chain(dte=0).iron_condor(short_delta=0.16, wing=200)
```

Arbitrary predicates are supported because selection is cheap — a few thousand calls per
day:

```python
ctx.chain(dte=0).where(lambda c: c.delta < 0.30 and c.oi > 10_000) \
                .max_by(lambda c: c.premium / c.spread)
```

## Positions are single objects

A multi-leg position has a net delta, a combined P&L and one stop. It is not a collection
of independent orders:

```python
pos = ctx.sell(ctx.chain(dte=0).iron_condor(short_delta=0.16, wing=200))
pos.delta          # net across all four legs
pos.pnl            # combined
pos.stop_loss(pct=0.30)   # applies to the position, not per leg
```

## Custom indicators

Indicators are pure functions of the price series and are computed vectorized before replay
begins, so they run at native speed without the author writing any C++.

**Tier 1 — vectorized (the default):**

```python
@indicator
def atr(high, low, close, n=14):
    tr = np.maximum(high - low, np.abs(high - shift(close, 1)))
    return rolling_mean(tr, n)
```

**Tier 2 — JIT, for recursions that will not vectorize:**

```python
@indicator(jit=True)
def adaptive_ema(close, alpha):
    out, acc = np.empty_like(close), close[0]
    for i in range(len(close)):
        acc = alpha[i] * close[i] + (1 - alpha[i]) * acc
        out[i] = acc
    return out
```

Plain Python — a loop and an accumulator — running within ~2× of hand-written C++.

Every indicator is checked for causality before use. An indicator whose output for
`data[:k]` disagrees with the first `k` values of its output for the full series is reading
future data and is rejected.

## The escape hatch

When logic genuinely cannot be declared, a Python callback is available. It is explicit,
and its cost is reported rather than hidden:

```python
@on_event(every="30s", scope="position")   # engine reports: ~750 py-calls/day
def manage(self, ctx, pos):
    if abs(pos.delta) > 50:
        ctx.hedge(pos, with_=ctx.future())
```

The fast path is the default; the slow path is opt-in and labelled.

## Extending the engine

Indicators, fill models, position sizers, exit rules, chain selectors and data adapters are
all registries. An extension may declare a native form and falls back to Python otherwise:

```python
@register.exit_rule("delta_breach")
class DeltaBreach(ExitRule):
    def compiles_to(self):
        return native.abs(native.delta()) > self.threshold
    def evaluate(self, pos, ctx):
        return abs(pos.delta) > self.threshold
```

## What the engine will not let you do

- Read data from after the current simulation time. There is no accessor for it.
- Register an indicator that fails causality validation.
- Fill at mid-price by default, or fill in an illiquid strike without a warning.
- Report positional returns without a margin model.
