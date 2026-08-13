// The strategy coroutine and the context it is handed.
//
// A strategy reads as straight-line script and suspends on conditions the engine
// evaluates natively. This is the C++ substrate; the Python layer will drive the
// same machinery, which is why the shape mirrors docs/strategy-api.md.

#pragma once

#include "volforge/condition.hpp"
#include "volforge/indicators.hpp"
#include "volforge/portfolio.hpp"

#include <coroutine>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace volforge {

// ---------------------------------------------------------------------------
// The coroutine
// ---------------------------------------------------------------------------

class StrategyTask {
public:
    struct promise_type {
        Cond               pending;
        std::exception_ptr error;

        StrategyTask get_return_object() {
            return StrategyTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        static std::suspend_always initial_suspend() noexcept { return {}; }
        static std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    using Handle = std::coroutine_handle<promise_type>;

    StrategyTask() = default;
    explicit StrategyTask(Handle h) : h_(h) {}

    StrategyTask(const StrategyTask&) = delete;
    StrategyTask& operator=(const StrategyTask&) = delete;
    StrategyTask(StrategyTask&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    StrategyTask& operator=(StrategyTask&& o) noexcept {
        if (this != &o) { destroy(); h_ = std::exchange(o.h_, {}); }
        return *this;
    }
    ~StrategyTask() { destroy(); }

    [[nodiscard]] bool valid() const { return h_ != nullptr; }
    [[nodiscard]] bool done() const { return !h_ || h_.done(); }
    [[nodiscard]] const Cond& pending() const { return h_.promise().pending; }

    void resume() {
        h_.promise().pending = Cond{};
        h_.resume();
        if (h_.done() && h_.promise().error) std::rethrow_exception(h_.promise().error);
    }

private:
    void destroy() { if (h_) { h_.destroy(); h_ = {}; } }
    Handle h_{};
};

struct CondAwaiter {
    Cond cond;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(StrategyTask::Handle h) const { h.promise().pending = cond; }
    void await_resume() const noexcept {}
};

inline CondAwaiter operator co_await(Cond c) { return CondAwaiter{std::move(c)}; }

// ---------------------------------------------------------------------------
// Confirmation policy
// ---------------------------------------------------------------------------

// "Enter when price crosses the band" has two legitimate readings that produce
// different trade sets. Neither is more correct, so the strategy states which it
// means rather than inheriting an engine default.
enum class Confirm : std::uint8_t {
    // Fire at the exact observation the level is crossed, evaluated at base
    // resolution against the last *completed* indicator value. If the bar later
    // closes back inside the band, the trade still happened — the cross did.
    Instant,

    // Fire only when a bar *closes* beyond the level. The signal becomes true
    // when that bar closes, i.e. at open_time + interval. A bar that pokes
    // through and retreats produces nothing.
    BarClose,
};

// ---------------------------------------------------------------------------
// Chain access
// ---------------------------------------------------------------------------

class Ctx;

// The option chain for one expiry, as seen right now.
//
// Only strikes that have actually quoted at or before now are visible. Strikes
// are listed intraday as spot moves, and selecting one from the registry because
// it exists *later in the session* is look-ahead of the survivorship kind — the
// universe itself must be as-of the decision.
class ChainView {
public:
    ChainView(const Ctx& ctx, Date expiry);

    [[nodiscard]] Date expiry() const { return expiry_; }
    [[nodiscard]] const std::vector<Price>& strikes() const { return strikes_; }

    [[nodiscard]] std::optional<Price> atm_strike() const;
    [[nodiscard]] std::optional<Price> strike_at(int offset) const;
    [[nodiscard]] std::optional<InstrumentId> option(Price strike, Right right) const;

    // Both legs at one strike. Empty if either is unavailable, because a
    // half-built straddle is a different position than the one asked for.
    [[nodiscard]] std::vector<InstrumentId> straddle(int offset = 0) const;
    [[nodiscard]] std::vector<InstrumentId> strangle(int width) const;

private:
    const Ctx*         ctx_;
    Date               expiry_;
    std::vector<Price> strikes_;   // only those quoting at or before now
};

// ---------------------------------------------------------------------------
// Strategy context
// ---------------------------------------------------------------------------

class Ctx {
public:
    Ctx(const SessionData& session, const MarketView& market, Portfolio& portfolio,
        UnderlyingId underlying, InstrumentId spot, Date date, int utc_offset_seconds,
        int session_open_sec);

    [[nodiscard]] Timestamp now() const { return market_->now(); }
    [[nodiscard]] const MarketView& market() const { return *market_; }
    [[nodiscard]] Portfolio& portfolio() { return *portfolio_; }
    [[nodiscard]] const Portfolio& portfolio() const { return *portfolio_; }
    [[nodiscard]] const InstrumentRegistry& registry() const { return session_->registry(); }
    [[nodiscard]] InstrumentId spot() const { return spot_; }
    [[nodiscard]] UnderlyingId underlying() const { return underlying_; }
    [[nodiscard]] Date date() const { return date_; }
    [[nodiscard]] Timestamp session_anchor() const { return anchor_; }

    [[nodiscard]] std::optional<Price> spot_price() const;

    [[nodiscard]] ChainView chain() const;
    [[nodiscard]] ChainView chain(Date expiry) const;
    [[nodiscard]] std::vector<Date> expiries() const;

    // --- bars and indicators ---------------------------------------------
    //
    // Built once from the full session and cached, but every accessor gates on
    // close_time, so precomputing cannot leak: a bar that has not closed is
    // unreachable regardless of what is in memory.

    [[nodiscard]] std::shared_ptr<const BarSeries> bars(
        InstrumentId id, int interval_seconds, BarPrice source = BarPrice::Last) const;

    [[nodiscard]] std::shared_ptr<const BarSeries> spot_bars(int interval_seconds) const;

    // --- orders -----------------------------------------------------------

    std::optional<PositionId> sell(const std::vector<InstrumentId>& legs, int lots,
                                   std::string label = "position");
    std::optional<PositionId> buy(const std::vector<InstrumentId>& legs, int lots,
                                  std::string label = "position");
    void close(PositionId id);
    void close_leg(PositionId id, std::size_t leg_index);

    // --- conditions -------------------------------------------------------

    [[nodiscard]] Cond at(std::string_view hhmm) const;

    // A relative deadline, measured from now.
    [[nodiscard]] Cond after(int seconds) const;
    [[nodiscard]] Cond pnl_pct_at_most(PositionId id, double pct) const;
    [[nodiscard]] Cond pnl_pct_at_least(PositionId id, double pct) const;
    [[nodiscard]] Cond position_closed(PositionId id) const;

    // Spot crossing above or below an indicator level, under the stated policy.
    [[nodiscard]] Cond cross_above(IndicatorPtr level, Confirm confirm) const;
    [[nodiscard]] Cond cross_below(IndicatorPtr level, Confirm confirm) const;

    [[nodiscard]] Qty lot_size(InstrumentId id) const;

private:
    const SessionData*        session_;
    const MarketView*         market_;
    Portfolio*                portfolio_;
    UnderlyingId              underlying_;
    InstrumentId              spot_;
    Date                      date_;
    int                       utc_offset_;
    Timestamp                 anchor_;

    // Keyed by (instrument, interval, price source).
    mutable std::map<std::tuple<std::int32_t, int, int>,
                     std::shared_ptr<const BarSeries>> bar_cache_;
};

}  // namespace volforge
