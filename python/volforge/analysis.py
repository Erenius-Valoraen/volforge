"""Performance statistics for a backtest result.

Analytics run once per backtest, so they live in Python by design — this is the
part of the system where clarity is worth more than speed.

Every figure here is computed from *net* equity, after costs. A statistic quoted
on gross P&L flatters a strategy by exactly the amount its costs would have
taken, which on a high-turnover options book is most of it.

Two things are deliberately not hidden:

- **Return is quoted against peak margin**, the largest requirement the run ever
  posted, because that is the capital the position actually tied up. Quoting
  against average margin, or against premium collected, produces a larger number
  that no broker would have let you trade on.
- **Ratios from 253 observations are noisy.** A Sharpe from one year is an
  estimate with a wide interval, not a property of the strategy. `summary()`
  says so rather than printing a number that invites more confidence than it
  deserves.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any

# NSE trades about 250 sessions a year.
SESSIONS_PER_YEAR = 250


@dataclass
class Drawdown:
    """The worst peak-to-trough fall in net equity."""

    depth: float = 0.0          # currency, always <= 0
    peak_equity: float = 0.0
    trough_equity: float = 0.0
    peak_date: Any = None
    trough_date: Any = None
    recovered_date: Any = None  # None if still under water at the end
    sessions_under_water: int = 0

    @property
    def recovered(self) -> bool:
        return self.recovered_date is not None


@dataclass
class Performance:
    sessions: int = 0
    trades: int = 0
    traded_sessions: int = 0

    gross: float = 0.0
    costs: float = 0.0
    net: float = 0.0
    unrealized: float = 0.0
    final_equity: float = 0.0

    peak_margin: float = 0.0
    return_on_peak: float = 0.0        # percent
    cost_ratio: float = 0.0            # costs / gross, as a fraction

    daily_pnl: list[float] = field(default_factory=list)
    equity_curve: list[float] = field(default_factory=list)
    dates: list[Any] = field(default_factory=list)

    winning_sessions: int = 0
    losing_sessions: int = 0
    flat_sessions: int = 0
    win_rate: float = 0.0              # of sessions that traded
    average_win: float = 0.0
    average_loss: float = 0.0
    profit_factor: float = 0.0
    best_session: float = 0.0
    worst_session: float = 0.0
    longest_losing_streak: int = 0

    volatility: float = 0.0            # annualised, as a fraction of peak margin
    sharpe: float = 0.0                # annualised, zero risk-free
    drawdown: Drawdown = field(default_factory=Drawdown)

    settled_legs: int = 0
    illiquid_fills: int = 0
    oversized_fills: int = 0

    def summary(self) -> str:
        """A readable report, with the caveats attached rather than implied."""
        lines = []
        add = lines.append

        add(f"{self.sessions} sessions, {self.trades} trades on "
            f"{self.traded_sessions} of them")
        if self.dates:
            add(f"{self.dates[0]} .. {self.dates[-1]}")
        add("")

        add(f"  gross            {self.gross:>14,.2f}")
        add(f"  costs            {-self.costs:>14,.2f}"
            f"   ({100 * self.cost_ratio:.0f}% of gross)" if self.gross > 0
            else f"  costs            {-self.costs:>14,.2f}")
        add(f"  net realised     {self.net:>14,.2f}")
        if abs(self.unrealized) > 1e-9:
            add(f"  unrealised       {self.unrealized:>14,.2f}   (still open at the end)")
        add(f"  final equity     {self.final_equity:>14,.2f}")
        add("")

        add(f"  peak margin      {self.peak_margin:>14,.2f}")
        add(f"  return on peak   {self.return_on_peak:>13.2f}%")
        add("")

        d = self.drawdown
        add(f"  max drawdown     {d.depth:>14,.2f}"
            f"   ({100 * abs(d.depth) / self.peak_margin:.1f}% of peak margin)"
            if self.peak_margin > 0 else f"  max drawdown     {d.depth:>14,.2f}")
        if d.peak_date is not None:
            state = (f"recovered {d.recovered_date}" if d.recovered
                     else "never recovered")
            add(f"                   {d.peak_date} -> {d.trough_date}, {state}")
            add(f"                   {d.sessions_under_water} sessions under water")
        add("")

        add(f"  win rate         {100 * self.win_rate:>13.1f}%"
            f"   ({self.winning_sessions}W / {self.losing_sessions}L)")
        add(f"  average win      {self.average_win:>14,.2f}")
        add(f"  average loss     {self.average_loss:>14,.2f}")
        add(f"  profit factor    {self.profit_factor:>14.2f}")
        add(f"  best / worst     {self.best_session:>14,.2f} / {self.worst_session:,.2f}")
        add(f"  worst streak     {self.longest_losing_streak:>14d} losing sessions")
        add("")

        add(f"  volatility       {100 * self.volatility:>13.1f}%   annualised, on peak margin")
        add(f"  sharpe           {self.sharpe:>14.2f}   zero risk-free")
        if self.sessions < 2 * SESSIONS_PER_YEAR:
            add(f"                   from {self.sessions} sessions - a wide interval, "
                f"not a property of the strategy")
        add("")

        flags = []
        if self.settled_legs:
            flags.append(f"{self.settled_legs} legs settled at expiry")
        if self.illiquid_fills:
            flags.append(f"{self.illiquid_fills} fills across a wide spread")
        if self.oversized_fills:
            flags.append(f"{self.oversized_fills} fills larger than the size shown")
        if flags:
            add("  worth knowing:   " + "; ".join(flags))

        return "\n".join(lines)


def _drawdown(equity: list[float], dates: list[Any]) -> Drawdown:
    """Worst peak-to-trough fall, and whether it ever came back."""
    out = Drawdown()
    if not equity:
        return out

    peak = equity[0]
    peak_i = 0
    worst_i = worst_peak_i = 0
    worst = 0.0

    for i, value in enumerate(equity):
        if value > peak:
            peak = value
            peak_i = i
        fall = value - peak
        if fall < worst:
            worst = fall
            worst_i = i
            worst_peak_i = peak_i

    if worst >= 0.0:
        return out

    out.depth = worst
    out.peak_equity = equity[worst_peak_i]
    out.trough_equity = equity[worst_i]
    out.peak_date = dates[worst_peak_i] if worst_peak_i < len(dates) else None
    out.trough_date = dates[worst_i] if worst_i < len(dates) else None

    for j in range(worst_i + 1, len(equity)):
        if equity[j] >= out.peak_equity:
            out.recovered_date = dates[j] if j < len(dates) else None
            out.sessions_under_water = j - worst_peak_i
            break
    else:
        out.sessions_under_water = len(equity) - worst_peak_i

    return out


def analyze(result: Any) -> Performance:
    """Turns a backtest result into statistics you can judge it by."""
    p = Performance()

    p.sessions = len(result.daily)
    p.trades = result.trades
    p.traded_sessions = sum(1 for s in result.daily if s.trades)
    p.gross = result.realized
    p.costs = result.costs
    p.net = result.net_realized
    p.unrealized = result.unrealized
    p.final_equity = result.final_equity
    p.peak_margin = result.peak_margin
    p.settled_legs = result.settled_legs
    p.illiquid_fills = result.illiquid_fills
    p.oversized_fills = result.oversized_fills

    if p.gross > 0:
        p.cost_ratio = p.costs / p.gross
    if p.peak_margin > 0:
        p.return_on_peak = 100.0 * p.final_equity / p.peak_margin

    p.dates = [s.date for s in result.daily]
    p.equity_curve = [s.equity for s in result.daily]

    previous = 0.0
    for value in p.equity_curve:
        p.daily_pnl.append(value - previous)
        previous = value

    if not p.daily_pnl:
        return p

    # Only sessions that traded count towards win rate; a flat day the strategy
    # sat out is not a loss, and counting it as a win is worse.
    wins = [x for x in p.daily_pnl if x > 0]
    losses = [x for x in p.daily_pnl if x < 0]
    p.winning_sessions = len(wins)
    p.losing_sessions = len(losses)
    p.flat_sessions = p.sessions - len(wins) - len(losses)

    decided = len(wins) + len(losses)
    if decided:
        p.win_rate = len(wins) / decided
    if wins:
        p.average_win = sum(wins) / len(wins)
        p.best_session = max(wins)
    if losses:
        p.average_loss = sum(losses) / len(losses)
        p.worst_session = min(losses)
    total_loss = -sum(losses)
    p.profit_factor = (sum(wins) / total_loss) if total_loss > 0 else float("inf")

    streak = 0
    for x in p.daily_pnl:
        streak = streak + 1 if x < 0 else 0
        p.longest_losing_streak = max(p.longest_losing_streak, streak)

    p.drawdown = _drawdown(p.equity_curve, p.dates)

    # Returns are expressed against the capital the strategy actually tied up.
    if p.peak_margin > 0 and len(p.daily_pnl) > 1:
        returns = [x / p.peak_margin for x in p.daily_pnl]
        mean = sum(returns) / len(returns)
        variance = sum((r - mean) ** 2 for r in returns) / (len(returns) - 1)
        sd = math.sqrt(variance)
        p.volatility = sd * math.sqrt(SESSIONS_PER_YEAR)
        if sd > 0:
            p.sharpe = (mean / sd) * math.sqrt(SESSIONS_PER_YEAR)

    return p
