"""volforge — an options backtesting engine.

A strategy is an ordinary ``async def``. It reads as straight-line script while
the conditions it waits on are compiled to native code and evaluated in C++::

    from volforge import strategy, backtest, synthetic

    @strategy
    async def short_straddle(ctx, stop=0.30, target=0.50, entry="09:20"):
        await ctx.at(entry)
        pos = ctx.sell(ctx.chain().straddle(), lots=1)
        if pos is None:
            return
        pos.stop_loss(stop)
        pos.take_profit(target)
        pos.exit_at("15:15")
        await ctx.at("15:20")

    data = synthetic(sessions=10)
    result = backtest(short_straddle, data, stop=0.35)

Python is entered only when a condition the strategy asked about actually holds
— a handful of times per session — while that condition is checked thousands of
times natively. Nothing about the authoring style changes that.

Parameters are ordinary function arguments with defaults. A strategy does not
know that sweeps exist; an experiment passes values in from outside.
"""

from __future__ import annotations

import inspect
from typing import Any, Callable

from . import _volforge as _core
from ._volforge import (  # re-exported value types
    BacktestConfig,
    BarPrice,
    Confirm,
    CostRates,
    Date,
    ExpiryHandling,
    IndianCosts,
    NoCosts,
    Right,
    SpanMargin,
    SpanParameters,
)

CALL = Right.CALL
PUT = Right.PUT

__all__ = [
    "strategy",
    "backtest",
    "synthetic",
    "load_gfdl",
    "parse_symbol",
    "describe",
    "BacktestConfig",
    "BarPrice",
    "Confirm",
    "CostRates",
    "Date",
    "ExpiryHandling",
    "IndianCosts",
    "NoCosts",
    "Right",
    "SpanMargin",
    "SpanParameters",
    "CALL",
    "PUT",
    "bollinger",
    "sma",
    "stdev",
]

sma = _core.sma
stdev = _core.stdev
bollinger = _core.bollinger


def strategy(fn: Callable[..., Any]) -> Callable[..., Any]:
    """Marks a coroutine function as a strategy.

    The check is worth having: a plain ``def`` would return a value rather than a
    coroutine, and the failure would otherwise surface deep inside the engine as
    a confusing type error rather than here, at the definition.
    """
    if not inspect.iscoroutinefunction(fn):
        raise TypeError(
            f"@strategy expects `async def`, but {fn.__name__} is a plain function. "
            "A strategy suspends on `await`, so it has to be a coroutine."
        )
    fn.__volforge_strategy__ = True  # type: ignore[attr-defined]
    return fn


def _describe(dataset: Any) -> str:
    d = dataset.dates
    if not d:
        return "Data(empty)"
    span = f"{d[0]}" if len(d) == 1 else f"{d[0]} to {d[-1]}"
    return f"Data({len(d)} session{'s' if len(d) != 1 else ''}, {span}, {dataset.instruments} instruments)"


describe = _describe


def synthetic(sessions: int = 10, *, seed: int = 7, strikes_each_side: int = 10,
              step_seconds: int = 5, start: Date | None = None) -> Any:
    """Generated sessions, for development while real data is unavailable.

    Deliberately reproduces the awkward properties of a real feed rather than an
    idealised one: mostly quote-only observations, activity thinning away from
    the money, wings that list late, spreads wide enough that mid-price fills
    would be fiction, and overnight gaps.
    """
    cfg = _core.SyntheticConfig()
    cfg.sessions = sessions
    cfg.seed = seed
    cfg.strikes_each_side = strikes_each_side
    cfg.step_seconds = step_seconds
    if start is not None:
        cfg.start = start
    return _core.make_synthetic(cfg)


def load_gfdl(directory: str, *, lot_size: int, only_underlying: str = "") -> Any:
    """Loads one day of GFDL NFO tick CSVs.

    ``lot_size`` is required rather than defaulted: the feed does not carry it,
    it has changed over time (NIFTY has been 25, then 50, then 75), and getting
    it wrong scales every position and every P&L figure by a constant.

    Note that only symbols which *traded* that day have a file, so the instrument
    universe is "strikes that traded", not "strikes that were listed".
    """
    return _core.load_gfdl(directory, lot_size, only_underlying)


def parse_symbol(ticker: str) -> dict:
    """Decomposes a vendor ticker such as ``NIFTY03JUL2523000CE.NFO``."""
    return _core.parse_symbol(ticker)


def backtest(fn: Callable[..., Any], data: Any, *, config: BacktestConfig | None = None,
             **params: Any) -> Any:
    """Runs a strategy across every session in ``data``.

    Strategy parameters are passed here rather than baked into the strategy, so
    the same strategy can be run different ways without being edited.
    """
    if not inspect.iscoroutinefunction(fn):
        raise TypeError(
            f"{getattr(fn, '__name__', fn)} is not a coroutine function; "
            "decorate it with @strategy and declare it `async def`"
        )

    # Fail on a misspelled parameter here rather than silently ignoring it and
    # reporting the results of a configuration nobody asked for.
    signature = inspect.signature(fn)
    accepted = set(signature.parameters)
    unknown = set(params) - accepted
    if unknown:
        raise TypeError(
            f"{fn.__name__}() has no parameter(s) {sorted(unknown)}; "
            f"it accepts {sorted(accepted - {'ctx'})}"
        )

    def factory(ctx: Any) -> Any:
        return fn(ctx, **params)

    return _core.run_backtest(data, factory, config or BacktestConfig())
