"""Tests for the Python authoring layer.

The property that matters most is the one in `test_python_stays_out_of_the_hot_loop`:
the whole design rests on Python being entered a handful of times per session
while conditions are checked millions of times natively. Everything else here is
about the boundary behaving predictably.
"""

import pytest

import volforge as vf


@pytest.fixture(scope="module")
def data():
    return vf.synthetic(sessions=5)


@vf.strategy
async def daily_straddle(ctx, stop=0.40, entry="09:30", exit_time="15:10"):
    while True:
        await ctx.at(entry)
        legs = ctx.chain().straddle()
        if legs:
            pos = ctx.sell(legs, lots=1, label="straddle")
            if pos is not None:
                pos.stop_loss(stop)
                pos.exit_at(exit_time)
        await ctx.at("15:20")


# ---------------------------------------------------------------------------
# The point of the whole design
# ---------------------------------------------------------------------------

def test_python_stays_out_of_the_hot_loop(data):
    result = vf.backtest(daily_straddle, data)

    assert result.observations > 1_000_000
    # Twice a session: one entry decision, one wait for the close.
    assert result.resumes <= 2 * len(data) + 2
    assert result.trades > 0


def test_a_daily_loop_trades_every_session(data):
    result = vf.backtest(daily_straddle, data)

    assert len(result.daily) == len(data)
    assert sum(1 for s in result.daily if s.trades > 0) >= len(data) - 1
    # Flat every night, so nothing reaches settlement.
    assert result.settled_legs == 0
    assert result.open_positions == 0


def test_runs_are_reproducible(data):
    a = vf.backtest(daily_straddle, data)
    b = vf.backtest(daily_straddle, data)

    assert a.trades == b.trades
    assert a.realized == b.realized
    assert [t.price for t in a.trade_log] == [t.price for t in b.trade_log]


# ---------------------------------------------------------------------------
# Parameters belong to the experiment, not the strategy
# ---------------------------------------------------------------------------

def test_parameters_are_passed_from_outside(data):
    tight = vf.backtest(daily_straddle, data, stop=0.10)
    loose = vf.backtest(daily_straddle, data, stop=0.90)

    # A tighter stop cannot produce fewer exits than a looser one.
    assert tight.trades >= loose.trades
    assert tight.realized != loose.realized


def test_a_misspelled_parameter_is_an_error(data):
    with pytest.raises(TypeError, match="has no parameter"):
        vf.backtest(daily_straddle, data, stopp=0.30)


def test_a_plain_function_is_rejected_at_definition():
    with pytest.raises(TypeError, match="async def"):
        @vf.strategy
        def not_a_coroutine(ctx):
            return None


# ---------------------------------------------------------------------------
# Conditions
# ---------------------------------------------------------------------------

def test_conditions_compose(data):
    seen = []

    @vf.strategy
    async def combined(ctx):
        await (ctx.at("09:30") | ctx.at("10:00"))
        seen.append("or")
        await (ctx.at("11:00") & ctx.at("10:00"))
        seen.append("and")

    vf.backtest(combined, data)
    assert seen == ["or", "and"]


def test_awaiting_something_that_is_not_a_condition_fails_clearly(data):
    import asyncio

    @vf.strategy
    async def confused(ctx):
        await asyncio.sleep(0)   # meaningless inside a backtest

    with pytest.raises(Exception) as excinfo:
        vf.backtest(confused, data)
    assert "condition" in str(excinfo.value).lower()


def test_an_error_in_strategy_code_propagates(data):
    @vf.strategy
    async def broken(ctx):
        await ctx.at("09:30")
        raise ValueError("deliberate")

    with pytest.raises(ValueError, match="deliberate"):
        vf.backtest(broken, data)


# ---------------------------------------------------------------------------
# Market access
# ---------------------------------------------------------------------------

def test_chain_and_greeks_are_reachable(data):
    captured = {}

    @vf.strategy
    async def inspect(ctx):
        await ctx.at("11:00")
        chain = ctx.chain()
        captured["strikes"] = len(chain.strikes)
        captured["atm"] = chain.atm_strike
        captured["spot"] = ctx.spot_price

        call = chain.option(chain.atm_strike, vf.CALL)
        captured["greeks"] = ctx.greeks(call)

        sixteen_delta = chain.by_delta(0.16, vf.PUT)
        captured["by_delta"] = ctx.greeks(sixteen_delta)

    vf.backtest(inspect, data)

    assert captured["strikes"] > 5
    assert captured["atm"] is not None
    assert abs(captured["atm"] - captured["spot"]) < 100

    g = captured["greeks"]
    assert 0.0 < g.iv < 3.0
    assert 0.0 < g.delta < 1.0
    assert g.theta < 0.0

    # Selected from a discrete ladder, so near the target rather than on it.
    assert abs(abs(captured["by_delta"].delta) - 0.16) < 0.15


def test_margin_is_reported_and_released(data):
    seen = {}

    @vf.strategy
    async def held(ctx):
        await ctx.at("10:00")
        legs = ctx.chain().straddle()
        pos = ctx.sell(legs, lots=1)
        await ctx.after(300)
        seen["while_open"] = ctx.margin().total
        pos.close()
        await (pos.closed() | ctx.at("15:20"))
        await ctx.after(120)
        seen["after_close"] = ctx.margin().total

    vf.backtest(held, data)
    assert seen["while_open"] > 50_000
    assert seen["after_close"] == 0.0


# ---------------------------------------------------------------------------
# Costs are configurable from Python
# ---------------------------------------------------------------------------

def test_costs_can_be_tuned_from_python(data):
    default_cfg = vf.BacktestConfig()

    free = vf.BacktestConfig()
    free.costs = vf.NoCosts()

    zero_brokerage = vf.BacktestConfig()
    rates = vf.CostRates()
    rates.brokerage_per_order = 0.0
    zero_brokerage.costs = vf.IndianCosts(rates)

    a = vf.backtest(daily_straddle, data, config=default_cfg)
    b = vf.backtest(daily_straddle, data, config=free)
    c = vf.backtest(daily_straddle, data, config=zero_brokerage)

    # Gross P&L is a property of the trades and does not move with the cost model.
    assert a.realized == b.realized == c.realized

    assert b.costs == 0.0
    assert a.costs > c.costs > 0.0

    # Removing a flat 20 per order removes exactly that, plus its GST.
    assert abs((a.costs - c.costs) - a.trades * 20.0 * 1.18) < 0.5


def test_a_budget_rate_change_moves_only_the_tax(data):
    base = vf.CostRates()
    hiked = vf.CostRates()
    hiked.stt_sell_pct = base.stt_sell_pct * 2

    cfg_a, cfg_b = vf.BacktestConfig(), vf.BacktestConfig()
    cfg_a.costs = vf.IndianCosts(base)
    cfg_b.costs = vf.IndianCosts(hiked)

    a = vf.backtest(daily_straddle, data, config=cfg_a)
    b = vf.backtest(daily_straddle, data, config=cfg_b)

    assert b.costs > a.costs
    assert a.realized == b.realized


def test_margin_parameters_can_be_tuned_from_python(data):
    tight, wide = vf.BacktestConfig(), vf.BacktestConfig()

    p = vf.SpanParameters()
    p.price_scan_pct = 0.02
    tight.margin = vf.SpanMargin(p)

    q = vf.SpanParameters()
    q.price_scan_pct = 0.07
    wide.margin = vf.SpanMargin(q)

    a = vf.backtest(daily_straddle, data, config=tight)
    b = vf.backtest(daily_straddle, data, config=wide)

    assert b.peak_margin > a.peak_margin


# ---------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------

def test_result_accounting_reconciles(data):
    r = vf.backtest(daily_straddle, data)

    assert abs(r.net_realized - (r.realized - r.costs)) < 1e-6
    assert abs(r.final_equity - (r.net_realized + r.unrealized)) < 1e-6
    assert sum(s.trades for s in r.daily) == r.trades

    for t in r.trade_log:
        if not t.settled:
            # An order can never fill on the observation that triggered it.
            assert t.fill_seconds > t.signal_seconds
