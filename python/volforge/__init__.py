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


class Data:
    """A set of sessions to run against."""

    def __init__(self, series: Any, registry: Any) -> None:
        self._series = series
        # The registry owns every instrument the sessions point at, so it has to
        # outlive them. Holding it here is what keeps that true.
        self._registry = registry

    @property
    def dates(self) -> list[Date]:
        return list(self._series.dates)

    @property
    def expiries(self) -> list[Date]:
        return list(self._series.expiries)

    def __len__(self) -> int:
        return len(self._series.dates)

    def __repr__(self) -> str:
        d = self._series.dates
        if not d:
            return "Data(empty)"
        return f"Data({len(d)} sessions, {d[0]} to {d[-1]})"


def synthetic(sessions: int = 10, *, seed: int = 7, strikes_each_side: int = 10,
              step_seconds: int = 5, start: Date | None = None) -> Data:
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

    registry = _core.InstrumentRegistry()
    series = _core.make_synthetic_series(registry, cfg)
    return Data(series, registry)


def backtest(fn: Callable[..., Any], data: Data, *, config: BacktestConfig | None = None,
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

    return _core.run_backtest(data._series.source, data._series, factory,
                              config or BacktestConfig())
