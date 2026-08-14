"""Tests for the performance statistics.

These matter more than they look: a drawdown or win rate that is quietly wrong
does not fail loudly, it just makes a bad strategy look tradeable.
"""

import math
import pytest

import volforge as vf


class FakeSession:
    def __init__(self, date, equity, trades=1):
        self.date = date
        self.equity = equity
        self.trades = trades
        self.peak_margin = 0.0
        self.open_positions = 0


class FakeResult:
    """A backtest result with a chosen equity curve."""

    def __init__(self, equity, peak_margin=100_000.0, gross=0.0, costs=0.0):
        self.daily = [FakeSession(f"d{i}", e) for i, e in enumerate(equity)]
        self.trades = 2 * len(equity)
        self.realized = gross if gross else (equity[-1] if equity else 0.0)
        self.costs = costs
        self.net_realized = self.realized - costs
        self.unrealized = 0.0
        self.final_equity = equity[-1] if equity else 0.0
        self.peak_margin = peak_margin
        self.settled_legs = 0
        self.illiquid_fills = 0
        self.oversized_fills = 0


def test_daily_pnl_is_the_difference_of_the_equity_curve():
    p = vf.analyze(FakeResult([100.0, 250.0, 200.0, 400.0]))
    assert p.daily_pnl == [100.0, 150.0, -50.0, 200.0]
    assert p.equity_curve == [100.0, 250.0, 200.0, 400.0]


def test_drawdown_measures_peak_to_trough_not_start_to_end():
    # Rises to 500, falls to 100, recovers past the old peak.
    p = vf.analyze(FakeResult([100.0, 500.0, 300.0, 100.0, 600.0]))
    d = p.drawdown
    assert d.depth == pytest.approx(-400.0)
    assert d.peak_equity == pytest.approx(500.0)
    assert d.trough_equity == pytest.approx(100.0)
    assert d.recovered
    assert d.recovered_date == "d4"


def test_a_drawdown_still_open_at_the_end_is_reported_as_unrecovered():
    p = vf.analyze(FakeResult([100.0, 500.0, 200.0]))
    assert not p.drawdown.recovered
    assert p.drawdown.recovered_date is None
    assert p.drawdown.depth == pytest.approx(-300.0)


def test_a_curve_that_only_rises_has_no_drawdown():
    p = vf.analyze(FakeResult([10.0, 20.0, 30.0]))
    assert p.drawdown.depth == 0.0
    assert not p.drawdown.recovered


def test_win_rate_ignores_sessions_that_did_nothing():
    # Equity flat on the third session: neither a win nor a loss.
    p = vf.analyze(FakeResult([100.0, 50.0, 50.0, 150.0]))
    assert p.winning_sessions == 2      # +100, +100
    assert p.losing_sessions == 1       # -50
    assert p.flat_sessions == 1
    assert p.win_rate == pytest.approx(2 / 3)


def test_profit_factor_and_averages():
    p = vf.analyze(FakeResult([100.0, 300.0, 200.0]))   # +100, +200, -100
    assert p.average_win == pytest.approx(150.0)
    assert p.average_loss == pytest.approx(-100.0)
    assert p.profit_factor == pytest.approx(3.0)
    assert p.best_session == pytest.approx(200.0)
    assert p.worst_session == pytest.approx(-100.0)


def test_profit_factor_is_infinite_when_nothing_lost():
    p = vf.analyze(FakeResult([100.0, 200.0]))
    assert math.isinf(p.profit_factor)


def test_longest_losing_streak():
    p = vf.analyze(FakeResult([100.0, 90.0, 80.0, 70.0, 200.0, 190.0]))
    assert p.longest_losing_streak == 3


def test_sharpe_is_positive_for_a_rising_curve_and_negative_for_a_falling_one():
    up = vf.analyze(FakeResult([100.0, 190.0, 310.0, 380.0, 500.0]))
    down = vf.analyze(FakeResult([-100.0, -190.0, -310.0, -380.0, -500.0]))
    assert up.sharpe > 0
    assert down.sharpe < 0
    assert up.volatility > 0


def test_a_flat_curve_has_no_volatility_and_no_sharpe():
    p = vf.analyze(FakeResult([0.0, 0.0, 0.0]))
    assert p.volatility == 0.0
    assert p.sharpe == 0.0


def test_costs_are_reported_against_gross():
    p = vf.analyze(FakeResult([500.0], gross=1000.0, costs=500.0))
    assert p.cost_ratio == pytest.approx(0.5)
    assert p.net == pytest.approx(500.0)


def test_an_empty_run_does_not_divide_by_zero():
    p = vf.analyze(FakeResult([]))
    assert p.sessions == 0
    assert p.sharpe == 0.0
    assert p.drawdown.depth == 0.0
    assert isinstance(p.summary(), str)


def test_summary_renders_and_stays_ascii():
    p = vf.analyze(FakeResult([100.0, 500.0, 200.0], gross=1000.0, costs=800.0))
    text = p.summary()
    text.encode("ascii")            # would raise if a stray dash crept back in
    assert "max drawdown" in text
    assert "win rate" in text
    assert "not a property of the strategy" in text   # the small-sample caveat
