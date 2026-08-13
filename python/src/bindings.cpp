// Python bindings.
//
// The interesting part is the strategy bridge. A Python `async def` must drive
// the C++ event loop without Python ending up in the hot path, and the shape
// that achieves it is a C++ coroutine driving the Python one:
//
//     while (true) {
//         auto cond = send(coro);   // Python runs to its next `await`
//         if (!cond) co_return;     // the coroutine finished
//         co_await *cond;           // evaluated natively until it fires
//     }
//
// So Python executes only when a condition it asked about actually holds — a
// handful of times per session — while the condition behind each `await` is
// checked thousands of times in C++. `await` works because a bound Cond returns
// `iter((self,))` from __await__, which yields itself once and then stops.
//
// The GIL is held for the whole run. The hot loop is C++ and does not want it,
// but releasing it would mean re-acquiring on every resume for no benefit, and a
// backtest has no other Python thread to unblock.

#include "volforge/backtest.hpp"
#include "volforge/indicators.hpp"
#include "volforge/synthetic.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <optional>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;
using namespace volforge;

namespace {

// --- instrument ids cross the boundary as plain ints -------------------------

std::vector<int> to_ints(const std::vector<InstrumentId>& ids) {
    std::vector<int> out;
    out.reserve(ids.size());
    for (const InstrumentId id : ids) out.push_back(index_of(id));
    return out;
}

std::vector<InstrumentId> to_ids(const std::vector<int>& ints) {
    std::vector<InstrumentId> out;
    out.reserve(ints.size());
    for (const int i : ints) out.push_back(static_cast<InstrumentId>(i));
    return out;
}

std::optional<int> to_int(const std::optional<InstrumentId>& id) {
    if (!id) return std::nullopt;
    return index_of(*id);
}

// --- the strategy bridge -----------------------------------------------------

// Advances a Python coroutine to its next `await` and returns what it is waiting
// on. nullopt means the coroutine ran to completion.
std::optional<Cond> send_to_coroutine(const nb::object& coro) {
    PyObject* result = PyObject_CallMethod(coro.ptr(), "send", "O", Py_None);

    if (result == nullptr) {
        if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
            PyErr_Clear();
            return std::nullopt;
        }
        throw nb::python_error();   // a real error in strategy code
    }

    nb::object yielded = nb::steal(result);
    if (!nb::isinstance<Cond>(yielded)) {
        throw nb::type_error(
            "a strategy may only await a volforge condition; awaiting anything else "
            "(asyncio.sleep, an HTTP call) has no meaning inside a backtest");
    }
    return nb::cast<Cond>(yielded);
}

StrategyTask drive_python(nb::object coro) {
    while (true) {
        auto pending = send_to_coroutine(coro);
        if (!pending) co_return;
        co_await *pending;
    }
}

// Wraps a Python callable of one argument (the context) into a StrategyFn.
StrategyFn make_strategy(nb::object factory) {
    return [factory](Ctx& ctx) -> StrategyTask {
        nb::object coro = factory(nb::cast(&ctx, nb::rv_policy::reference));
        if (!nb::hasattr(coro, "send")) {
            throw nb::type_error(
                "strategy function must be `async def` — it returned something that is "
                "not a coroutine");
        }
        return drive_python(std::move(coro));
    };
}

}  // namespace

NB_MODULE(_volforge, m) {
    m.doc() = "volforge core";

    // ---------------------------------------------------------------------
    // Values
    // ---------------------------------------------------------------------

    nb::class_<Date>(m, "Date")
        .def(nb::init<>())
        .def("__init__", [](Date* d, int yyyymmdd) { new (d) Date{yyyymmdd}; }, "yyyymmdd"_a)
        .def_prop_ro("year", &Date::year)
        .def_prop_ro("month", &Date::month)
        .def_prop_ro("day", &Date::day)
        .def_prop_ro("yyyymmdd", [](const Date& d) { return d.yyyymmdd; })
        .def("__str__", &Date::to_string)
        .def("__repr__", [](const Date& d) { return "Date(" + d.to_string() + ")"; })
        .def("__eq__", [](const Date& a, const Date& b) { return a == b; })
        .def("__lt__", [](const Date& a, const Date& b) { return a < b; })
        .def("__le__", [](const Date& a, const Date& b) { return a <= b; })
        .def("__gt__", [](const Date& a, const Date& b) { return a > b; })
        .def("__ge__", [](const Date& a, const Date& b) { return a >= b; })
        .def("__hash__", [](const Date& d) { return static_cast<Py_hash_t>(d.yyyymmdd); })
        .def("plus_days", [](const Date& d, int n) { return add_days(d, n); }, "days"_a);

    nb::enum_<Right>(m, "Right")
        .value("CALL", Right::Call)
        .value("PUT", Right::Put);

    nb::enum_<Confirm>(m, "Confirm")
        .value("INSTANT", Confirm::Instant)
        .value("BAR_CLOSE", Confirm::BarClose);

    nb::enum_<ExpiryHandling>(m, "ExpiryHandling")
        .value("SETTLE", ExpiryHandling::Settle)
        .value("FORBID", ExpiryHandling::Forbid);

    nb::enum_<BarPrice>(m, "BarPrice")
        .value("LAST", BarPrice::Last)
        .value("MID", BarPrice::Mid);

    // ---------------------------------------------------------------------
    // Conditions
    // ---------------------------------------------------------------------

    nb::class_<Cond>(m, "Cond")
        .def("__or__", [](const Cond& a, const Cond& b) { return a | b; })
        .def("__and__", [](const Cond& a, const Cond& b) { return a & b; })
        .def("__invert__", [](const Cond& a) { return !a; })
        // `await cond` calls this, drives the returned iterator, and the single
        // yielded value propagates out to whoever is driving the coroutine.
        .def("__await__", [](nb::object self) { return nb::iter(nb::make_tuple(self)); });

    // ---------------------------------------------------------------------
    // Pricing and risk
    // ---------------------------------------------------------------------

    nb::class_<GreekSet>(m, "Greeks")
        .def_ro("iv", &GreekSet::iv)
        .def_ro("price", &GreekSet::price)
        .def_ro("delta", &GreekSet::delta)
        .def_ro("gamma", &GreekSet::gamma)
        .def_ro("vega", &GreekSet::vega)
        .def_ro("theta", &GreekSet::theta)
        .def("__repr__", [](const GreekSet& g) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Greeks(iv=%.4f delta=%.4f theta=%.2f)", g.iv,
                          g.delta, g.theta);
            return std::string(buf);
        });

    nb::class_<MarginResult>(m, "Margin")
        .def_prop_ro("span", [](const MarginResult& m2) { return m2.span.to_double(); })
        .def_prop_ro("exposure", [](const MarginResult& m2) { return m2.exposure.to_double(); })
        .def_prop_ro("total", [](const MarginResult& m2) { return m2.total.to_double(); })
        .def_ro("scanning_risk", &MarginResult::scanning_risk)
        .def_ro("floor_applied", &MarginResult::floor_applied);

    // ---------------------------------------------------------------------
    // Costs
    // ---------------------------------------------------------------------

    nb::class_<IndianFnORates>(m, "CostRates")
        .def(nb::init<>())
        .def_rw("stt_sell_pct", &IndianFnORates::stt_sell_pct)
        .def_rw("stt_exercise_pct", &IndianFnORates::stt_exercise_pct)
        .def_rw("transaction_pct", &IndianFnORates::transaction_pct)
        .def_rw("sebi_pct", &IndianFnORates::sebi_pct)
        .def_rw("ipft_pct", &IndianFnORates::ipft_pct)
        .def_rw("stamp_buy_pct", &IndianFnORates::stamp_buy_pct)
        .def_rw("gst_pct", &IndianFnORates::gst_pct)
        .def_prop_rw(
            "brokerage_per_order",
            [](const IndianFnORates& r) { return r.brokerage_per_order.to_double(); },
            [](IndianFnORates& r, double v) {
                r.brokerage_per_order = Money{static_cast<std::int64_t>(std::llround(v * 100.0))};
            });

    nb::class_<CostPolicy>(m, "CostPolicy");
    nb::class_<IndianFnOCosts, CostPolicy>(m, "IndianCosts")
        .def(nb::init<>())
        .def(nb::init<IndianFnORates>(), "rates"_a);
    nb::class_<NoCosts, CostPolicy>(m, "NoCosts").def(nb::init<>());

    nb::class_<SpanParameters>(m, "SpanParameters")
        .def(nb::init<>())
        .def_rw("price_scan_pct", &SpanParameters::price_scan_pct)
        .def_rw("vol_scan_pct", &SpanParameters::vol_scan_pct)
        .def_rw("short_option_minimum_pct", &SpanParameters::short_option_minimum_pct)
        .def_rw("exposure_pct", &SpanParameters::exposure_pct);

    nb::class_<MarginModel>(m, "MarginModel");
    nb::class_<SpanMarginModel, MarginModel>(m, "SpanMargin")
        .def(nb::init<>())
        .def(nb::init<SpanParameters>(), "params"_a);

    // ---------------------------------------------------------------------
    // Positions
    // ---------------------------------------------------------------------

    nb::class_<Position>(m, "Position")
        .def_prop_ro("id", &Position::id)
        .def_prop_ro("label", &Position::label)
        .def_prop_ro("open", &Position::open)
        .def_prop_ro("closed", &Position::closed)
        .def_prop_ro("established", &Position::established)
        .def_prop_ro("leg_count", [](const Position& p) { return p.legs().size(); });

    nb::class_<LegRef>(m, "Leg")
        .def_prop_ro("index", &LegRef::index)
        .def("stop_loss", &LegRef::stop_loss, "pct"_a)
        .def("take_profit", &LegRef::take_profit, "pct"_a)
        .def("close", &LegRef::close);

    nb::class_<PositionRef>(m, "PositionRef")
        .def_prop_ro("id", &PositionRef::id)
        .def_prop_ro("leg_count", &PositionRef::leg_count)
        .def("leg", &PositionRef::leg, "index"_a)
        .def("stop_loss", &PositionRef::stop_loss, "pct"_a)
        .def("take_profit", &PositionRef::take_profit, "pct"_a)
        .def("exit_at", &PositionRef::exit_at, "hhmm"_a)
        .def("exit_at_on", &PositionRef::exit_at_on, "date"_a, "hhmm"_a)
        .def("close", &PositionRef::close)
        .def("closed", &PositionRef::closed)
        .def("pnl_pct_at_most", &PositionRef::pnl_pct_at_most, "pct"_a)
        .def("pnl_pct_at_least", &PositionRef::pnl_pct_at_least, "pct"_a);

    // ---------------------------------------------------------------------
    // Chain
    // ---------------------------------------------------------------------

    nb::class_<ChainView>(m, "Chain")
        .def_prop_ro("expiry", &ChainView::expiry)
        .def_prop_ro("strikes",
                     [](const ChainView& c) {
                         std::vector<double> out;
                         for (const Price p : c.strikes()) out.push_back(p.to_double());
                         return out;
                     })
        .def_prop_ro("atm_strike",
                     [](const ChainView& c) -> std::optional<double> {
                         const auto k = c.atm_strike();
                         if (!k) return std::nullopt;
                         return k->to_double();
                     })
        .def("strike_at",
             [](const ChainView& c, int offset) -> std::optional<double> {
                 const auto k = c.strike_at(offset);
                 if (!k) return std::nullopt;
                 return k->to_double();
             },
             "offset"_a)
        .def("option",
             [](const ChainView& c, double strike, Right r) {
                 return to_int(c.option(Price::from_double(strike), r));
             },
             "strike"_a, "right"_a)
        .def("straddle", [](const ChainView& c, int off) { return to_ints(c.straddle(off)); },
             "offset"_a = 0)
        .def("strangle", [](const ChainView& c, int w) { return to_ints(c.strangle(w)); },
             "width"_a)
        .def("by_delta",
             [](const ChainView& c, double target, Right r) {
                 return to_int(c.by_delta(target, r));
             },
             "delta"_a, "right"_a)
        .def("strangle_by_delta",
             [](const ChainView& c, double t) { return to_ints(c.strangle_by_delta(t)); },
             "delta"_a);

    // ---------------------------------------------------------------------
    // Indicators
    // ---------------------------------------------------------------------

    nb::class_<BarSeries>(m, "BarSeries")
        .def("known_count", &BarSeries::known_count, "now"_a);

    nb::class_<BarIndicator>(m, "Indicator");

    m.def("sma", &sma, "bars"_a, "period"_a);
    m.def("stdev", &stdev, "bars"_a, "period"_a);
    m.def("bollinger",
          [](std::shared_ptr<const BarSeries> bars, int period, double k) {
              const auto b = bollinger(std::move(bars), period, k);
              return nb::make_tuple(b.middle, b.upper, b.lower);
          },
          "bars"_a, "period"_a, "k"_a = 2.0);

    // ---------------------------------------------------------------------
    // Context
    // ---------------------------------------------------------------------

    nb::class_<Portfolio>(m, "Portfolio")
        .def_prop_ro("realized", [](const Portfolio& p) { return p.realized().to_double(); })
        .def_prop_ro("costs", [](const Portfolio& p) { return p.costs().to_double(); })
        .def_prop_ro("size", &Portfolio::size)
        .def("at", nb::overload_cast<PositionId>(&Portfolio::at, nb::const_), "id"_a,
             nb::rv_policy::reference_internal);

    nb::class_<Ctx>(m, "Ctx")
        .def_prop_ro("date", &Ctx::date)
        .def_prop_ro("portfolio", nb::overload_cast<>(&Ctx::portfolio),
                     nb::rv_policy::reference_internal)
        .def_prop_ro("spot", [](const Ctx& c) { return index_of(c.spot()); })
        .def_prop_ro("spot_price",
                     [](const Ctx& c) -> std::optional<double> {
                         const auto p = c.spot_price();
                         if (!p) return std::nullopt;
                         return p->to_double();
                     })
        .def_prop_ro("expiries", &Ctx::expiries)

        .def("at", &Ctx::at, "hhmm"_a)
        .def("at_on", &Ctx::at_on, "date"_a, "hhmm"_a)
        .def("after", &Ctx::after, "seconds"_a)

        .def("chain", nb::overload_cast<>(&Ctx::chain, nb::const_))
        .def("chain", nb::overload_cast<Date>(&Ctx::chain, nb::const_), "expiry"_a)
        .def("next_expiry", &Ctx::next_expiry, "min_days_ahead"_a = 0)

        .def("forward", &Ctx::forward, "expiry"_a)
        .def("greeks", [](const Ctx& c, int id) { return c.greeks(static_cast<InstrumentId>(id)); },
             "instrument"_a)
        .def("margin", &Ctx::margin)

        .def("bars",
             [](const Ctx& c, int id, int seconds, BarPrice src) {
                 return c.bars(static_cast<InstrumentId>(id), seconds, src);
             },
             "instrument"_a, "interval_seconds"_a, "price"_a = BarPrice::Last)
        .def("spot_bars", &Ctx::spot_bars, "interval_seconds"_a)
        .def("cross_above", &Ctx::cross_above, "level"_a, "confirm"_a)
        .def("cross_below", &Ctx::cross_below, "level"_a, "confirm"_a)

        .def("sell",
             [](Ctx& c, const std::vector<int>& legs, int lots, std::string label) {
                 return c.sell(to_ids(legs), lots, std::move(label));
             },
             "legs"_a, "lots"_a = 1, "label"_a = "position")
        .def("buy",
             [](Ctx& c, const std::vector<int>& legs, int lots, std::string label) {
                 return c.buy(to_ids(legs), lots, std::move(label));
             },
             "legs"_a, "lots"_a = 1, "label"_a = "position")

        .def("instrument",
             [](const Ctx& c, int id) {
                 const InstrumentSpec& s = c.registry().spec(static_cast<InstrumentId>(id));
                 nb::dict d;
                 d["strike"] = s.strike.to_double();
                 d["right"]  = s.right;
                 d["expiry"] = s.expiry;
                 d["lot_size"] = s.lot_size;
                 d["is_option"] = s.is_option();
                 return d;
             },
             "instrument"_a);

    // ---------------------------------------------------------------------
    // Running
    // ---------------------------------------------------------------------

    nb::class_<TradeRecord>(m, "Trade")
        .def_prop_ro("signal_seconds", [](const TradeRecord& t) { return t.signal_ts.seconds(); })
        .def_prop_ro("fill_seconds", [](const TradeRecord& t) { return t.fill_ts.seconds(); })
        .def_prop_ro("instrument", [](const TradeRecord& t) { return index_of(t.instrument); })
        .def_prop_ro("is_buy", [](const TradeRecord& t) { return t.side == Side::Buy; })
        .def_ro("qty", &TradeRecord::qty)
        .def_prop_ro("price", [](const TradeRecord& t) { return t.price.to_double(); })
        .def_prop_ro("cost", [](const TradeRecord& t) { return t.cost.to_double(); })
        .def_ro("illiquid", &TradeRecord::illiquid)
        .def_ro("from_rule", &TradeRecord::from_rule)
        .def_ro("settled", &TradeRecord::settled);

    nb::class_<SessionSummary>(m, "SessionSummary")
        .def_ro("date", &SessionSummary::date)
        .def_prop_ro("equity", [](const SessionSummary& s) { return s.equity.to_double(); })
        .def_prop_ro("peak_margin",
                     [](const SessionSummary& s) { return s.peak_margin.to_double(); })
        .def_ro("trades", &SessionSummary::trades)
        .def_ro("open_positions", &SessionSummary::open_positions);

    nb::class_<BacktestResult>(m, "BacktestResult")
        .def_ro("sessions", &BacktestResult::sessions)
        .def_prop_ro("realized", [](const BacktestResult& r) { return r.realized.to_double(); })
        .def_prop_ro("costs", [](const BacktestResult& r) { return r.costs.to_double(); })
        .def_prop_ro("net_realized",
                     [](const BacktestResult& r) { return r.net_realized.to_double(); })
        .def_prop_ro("unrealized", [](const BacktestResult& r) { return r.unrealized.to_double(); })
        .def_prop_ro("final_equity",
                     [](const BacktestResult& r) { return r.final_equity.to_double(); })
        .def_prop_ro("peak_margin",
                     [](const BacktestResult& r) { return r.peak_margin.to_double(); })
        .def_ro("observations", &BacktestResult::observations)
        .def_ro("resumes", &BacktestResult::resumes)
        .def_ro("trades", &BacktestResult::trades)
        .def_ro("settled_legs", &BacktestResult::settled_legs)
        .def_ro("illiquid_fills", &BacktestResult::illiquid_fills)
        .def_ro("oversized_fills", &BacktestResult::oversized_fills)
        .def_ro("open_positions", &BacktestResult::open_positions)
        .def_ro("strategy_finished", &BacktestResult::strategy_finished)
        .def_ro("trade_log", &BacktestResult::trade_log)
        .def_ro("daily", &BacktestResult::daily);

    nb::class_<BacktestConfig>(m, "BacktestConfig")
        .def(nb::init<>())
        .def_rw("execution_delay_nanos", &BacktestConfig::execution_delay_nanos)
        .def_rw("rate", &BacktestConfig::rate)
        .def_rw("costs", &BacktestConfig::costs)
        .def_rw("margin", &BacktestConfig::margin)
        .def_rw("margin_sample_seconds", &BacktestConfig::margin_sample_seconds)
        .def_rw("session_open_sec", &BacktestConfig::session_open_sec)
        .def_rw("expiry", &BacktestConfig::expiry);

    // ---------------------------------------------------------------------
    // Data
    // ---------------------------------------------------------------------

    nb::class_<InstrumentRegistry>(m, "InstrumentRegistry")
        .def(nb::init<>())
        .def_prop_ro("size", &InstrumentRegistry::size);

    nb::class_<DataSource>(m, "DataSource");
    nb::class_<MemoryDataSource, DataSource>(m, "MemoryDataSource")
        .def("sessions", &MemoryDataSource::sessions);

    nb::class_<SyntheticSeriesConfig>(m, "SyntheticConfig")
        .def(nb::init<>())
        .def_rw("start", &SyntheticSeriesConfig::start)
        .def_rw("sessions", &SyntheticSeriesConfig::sessions)
        .def_rw("strikes_each_side", &SyntheticSeriesConfig::strikes_each_side)
        .def_rw("step_seconds", &SyntheticSeriesConfig::step_seconds)
        .def_rw("seed", &SyntheticSeriesConfig::seed);

    nb::class_<SyntheticSeries>(m, "SyntheticSeries")
        .def_ro("source", &SyntheticSeries::source)
        .def_ro("dates", &SyntheticSeries::dates)
        .def_ro("expiries", &SyntheticSeries::expiries)
        .def_prop_ro("spot", [](const SyntheticSeries& s) { return index_of(s.spot); });

    m.def("make_synthetic_series", &make_synthetic_series, "registry"_a, "config"_a,
          nb::rv_policy::move);

    m.def(
        "run_backtest",
        [](DataSource& source, const SyntheticSeries& series, nb::object strategy,
           const BacktestConfig& config) {
            return run_backtest(source, series.underlying, series.spot,
                                make_strategy(std::move(strategy)), config);
        },
        "source"_a, "series"_a, "strategy"_a, "config"_a);
}
