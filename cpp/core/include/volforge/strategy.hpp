// The strategy coroutine and the context it is handed.
//
// A strategy reads as straight-line script and suspends on conditions the engine
// evaluates natively. This is the C++ substrate; the Python layer will drive the
// same machinery, which is why the shape here deliberately mirrors the API in
// docs/strategy-api.md.
//
//     StrategyTask short_straddle(Ctx& ctx, double stop) {
//         co_await ctx.at("09:20");
//         auto pos = ctx.sell(ctx.chain().straddle(), 1);
//         co_await (ctx.pnl_pct_at_most(pos, -stop) | ctx.at("15:15"));
//         ctx.close(pos);
//     }

#pragma once

#include "volforge/condition.hpp"
#include "volforge/portfolio.hpp"

#include <coroutine>
#include <exception>
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
        Cond               pending;   // what the strategy is currently waiting on
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

// Suspends until the condition holds.
struct CondAwaiter {
    Cond cond;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(StrategyTask::Handle h) const { h.promise().pending = cond; }
    void await_resume() const noexcept {}
};

inline CondAwaiter operator co_await(Cond c) { return CondAwaiter{std::move(c)}; }

// ---------------------------------------------------------------------------
// Chain access
// ---------------------------------------------------------------------------

class Ctx;

// The option chain for one expiry, as seen right now.
//
// Selection happens at runtime because the tradable universe changes daily and
// instruments are chosen by query, not named in advance.
class ChainView {
public:
    ChainView(const Ctx& ctx, Date expiry);

    [[nodiscard]] Date expiry() const { return expiry_; }
    [[nodiscard]] const std::vector<Price>& strikes() const { return strikes_; }

    // Nearest listed strike to spot, or nullopt if spot has not printed.
    [[nodiscard]] std::optional<Price> atm_strike() const;

    // Nearest strike, shifted by `offset` strike steps.
    [[nodiscard]] std::optional<Price> strike_at(int offset) const;

    [[nodiscard]] std::optional<InstrumentId> option(Price strike, Right right) const;

    // Both legs at one strike. Empty if either leg is unavailable, because a
    // half-built straddle is a different position than the one asked for.
    [[nodiscard]] std::vector<InstrumentId> straddle(int offset = 0) const;

    // Call and put `width` strike steps either side of the money.
    [[nodiscard]] std::vector<InstrumentId> strangle(int width) const;

private:
    const Ctx*         ctx_;
    Date               expiry_;
    std::vector<Price> strikes_;
};

// ---------------------------------------------------------------------------
// Strategy context
// ---------------------------------------------------------------------------

// What a strategy is handed. Everything reachable from here respects the
// look-ahead boundary: market() is a MarketView, never SessionData.
class Ctx {
public:
    Ctx(const MarketView& market, Portfolio& portfolio, const InstrumentRegistry& registry,
        UnderlyingId underlying, InstrumentId spot, Date date, int utc_offset_seconds);

    [[nodiscard]] Timestamp now() const { return market_->now(); }
    [[nodiscard]] const MarketView& market() const { return *market_; }
    [[nodiscard]] Portfolio& portfolio() { return *portfolio_; }
    [[nodiscard]] const Portfolio& portfolio() const { return *portfolio_; }
    [[nodiscard]] const InstrumentRegistry& registry() const { return *registry_; }
    [[nodiscard]] InstrumentId spot() const { return spot_; }
    [[nodiscard]] UnderlyingId underlying() const { return underlying_; }
    [[nodiscard]] Date date() const { return date_; }

    [[nodiscard]] std::optional<Price> spot_price() const;

    // Chain for the nearest expiry at or after the session date, or for a
    // specific expiry.
    [[nodiscard]] ChainView chain() const;
    [[nodiscard]] ChainView chain(Date expiry) const;

    // Expiries known for this underlying, ascending.
    [[nodiscard]] std::vector<Date> expiries() const;

    // --- orders -----------------------------------------------------------

    std::optional<PositionId> sell(const std::vector<InstrumentId>& legs, int lots,
                                   std::string label = "position");
    std::optional<PositionId> buy(const std::vector<InstrumentId>& legs, int lots,
                                  std::string label = "position");
    void close(PositionId id);
    void close_leg(PositionId id, std::size_t leg_index);

    // --- conditions -------------------------------------------------------

    [[nodiscard]] Cond at(std::string_view hhmm) const;
    [[nodiscard]] Cond pnl_pct_at_most(PositionId id, double pct) const;
    [[nodiscard]] Cond pnl_pct_at_least(PositionId id, double pct) const;
    [[nodiscard]] Cond position_closed(PositionId id) const;

    // Lot size for an instrument, so `lots` means what a broker means by it.
    [[nodiscard]] Qty lot_size(InstrumentId id) const;

private:
    const MarketView*         market_;
    Portfolio*                portfolio_;
    const InstrumentRegistry* registry_;
    UnderlyingId              underlying_;
    InstrumentId              spot_;
    Date                      date_;
    int                       utc_offset_;
};

}  // namespace volforge
