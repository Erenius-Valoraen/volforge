# What to learn from Pine Script

Pine Script is the most widely used strategy language in existence, so its rough edges are
unusually well documented. This records which of them volforge is designed to avoid, and
which are not really Pine's fault.

A fair distinction up front: **many Pine limits exist because it runs on TradingView's
servers for millions of concurrent users.** Execution timeouts, plot caps and script size
limits are multi-tenancy costs, not language design errors. volforge runs on your machine, so
it simply does not inherit them. Those are listed for completeness, not as criticism.

The lessons worth actually learning are the design ones: repainting, unsafe defaults, and the
type system.

## Design problems worth avoiding

### Repainting

TradingView documents three distinct categories: historical-versus-realtime calculation
differences, plotting into the past, and dataset variation across account tiers. The first is
the dangerous one — on an unconfirmed bar, `close` and every indicator derived from it are
still moving, so signals computed live differ from the same signals recomputed over history.

The recommended mitigations are all *advisory*: add `and barstate.isconfirmed`, reference
`[1]` instead of live values, prefer `open` over `close`. Every one of them is something a
user must remember.

**volforge decision.** Causality is enforced rather than advised. Indicators are validated by
checking `f(data[:k]) == f(data)[:k]`, higher timeframes expose only completed bars, and there
is no accessor for data after the current simulation time. A user cannot forget to do the safe
thing, because the unsafe thing is not reachable.

### Lookahead defaults on higher-timeframe data

The correct way to read higher-timeframe data without leaking the future in Pine is an offset
of `[1]` combined with `lookahead = barmerge.lookahead_on`. That incantation is not
discoverable, its failure mode is silent, and the resulting backtests look excellent.

**volforge decision.** `bars("5min").close` is the last *completed* bar and always safe. The
in-progress bar requires writing `.forming` explicitly. There is no flag to get wrong, and
the dangerous option is longer to type than the safe one. See
[series-and-timeframes.md](series-and-timeframes.md#only-completed-bars-are-visible).

### The type qualifier hierarchy

Pine values carry qualifiers `const < input < simple < series`, and many built-ins demand a
`simple int` for parameters like length. The consequence is that `ta.sma(close, dynamicLen)`
is rejected when `dynamicLen` varies across bars — so adaptive-period indicators, which are a
legitimate and common technique, cannot be expressed with the standard library at all.

This is the limitation most likely to bite a serious strategy author, and it is invisible
until you hit it.

**volforge decision.** Indicator parameters may be series. `ta.sma(close, dynamic_len)` is
valid. Where a varying window has no efficient vectorized form, it falls through to the JIT
tier, which is an ordinary Python loop compiled to native code — so the expressive ceiling is
a performance question, never a "this cannot be written" question.

### `varip` and `calc_on_every_tick`

Both are documented as inherently repainting, and strategies using `calc_on_every_tick = true`
do not reproduce under historical backtest.

**volforge decision.** There is one execution model. Backtest and live differ in their data
source and their fill realism, not in when strategy code runs. The constraints that keep this
true are in [design.md](design.md#8-live-parity).

### No options support

Pine has no concept of an option chain, Greeks, implied volatility, multi-leg positions or
margin. Strategies that need them are simply not writable.

**volforge decision.** These are the core domain model rather than additions, which is the
reason this project exists.

## Limits that come from hosting, not design

volforge does not inherit these, and Pine is not wrong to have them.

| Pine limit | volforge |
|---|---|
| 5,000-bar history buffer (`max_bars_back`) | No limit — `[n]` is an array index into precomputed data |
| 40 `request.*()` calls per script | No limit — options require hundreds of instruments by nature |
| 20–40 s execution timeout | None |
| 500 ms per-bar loop timeout | None |
| 64 plots; 500 lines/boxes/labels | None |
| 80,000-token compiled script cap | None |
| 9,000 orders (200,000 deep backtesting) | None |

## What Pine gets right

Worth stating, since the goal is to beat it rather than dismiss it.

Terseness is a real feature: `close[1]` says exactly what it means, and any design that needs
five lines to express the same idea is worse regardless of its other merits. Implicit
series semantics — writing what looks like scalar code and having it run across all bars — is
why Pine is learnable in an afternoon. Both are preserved here.

What Pine cannot do is express sequence. "Enter, wait, adjust, exit" becomes a scatter of
state flags, which is why volforge uses coroutines instead. See
[strategy-api.md](strategy-api.md#why-coroutines).

## Sources

- [Repainting — Pine Script docs](https://www.tradingview.com/pine-script-docs/concepts/repainting/)
- [Other timeframes and data — Pine Script docs](https://www.tradingview.com/pine-script-docs/concepts/other-timeframes-and-data/)
- [Type system — Pine Script docs](https://www.tradingview.com/pine-script-docs/language/type-system)
- [The Main Limitations of Pine Script on TradingView — Quant Nomad](https://quantnomad.com/the-main-limitations-of-pine-script-on-tradingview/)
- [Functions Allowing Series As Length — PineCoders FAQ](https://es.tradingview.com/script/kY5hhjA7-Functions-Allowing-Series-As-Length-PineCoders-FAQ)
