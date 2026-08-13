// Conditions.
//
// A condition is a predicate over market state that the engine evaluates on the
// strategy's behalf. This is the mechanism that keeps strategy code out of the
// hot loop: a strategy suspends on a condition and the engine checks it, so the
// authoring language is only entered when something the strategy asked about
// actually happens.
//
// Conditions are evaluated once per distinct timestamp rather than once per
// observation. Market state is only coherent at a timestamp boundary — several
// instruments print in the same second and a position's P&L is meaningless
// halfway through applying them.

#pragma once

#include "volforge/data_source.hpp"

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace volforge {

class Portfolio;

// What a condition is allowed to see. Deliberately the time-bounded MarketView
// rather than SessionData: a condition cannot look forward either.
struct EvalCtx {
    const MarketView* market    = nullptr;
    const Portfolio*  portfolio = nullptr;
    Timestamp         now{};
};

// A composable predicate.
//
// Backed by std::function for now. The indirection is irrelevant at ~22,500
// evaluations per session, and specialising the common shapes into branch-free
// native forms is a later optimisation rather than a design change.
class Cond {
public:
    using Fn = std::function<bool(const EvalCtx&)>;

    Cond() = default;
    explicit Cond(Fn fn) : fn_(std::make_shared<Fn>(std::move(fn))) {}

    [[nodiscard]] bool valid() const { return fn_ != nullptr; }
    [[nodiscard]] bool eval(const EvalCtx& ctx) const { return fn_ && (*fn_)(ctx); }

    friend Cond operator|(Cond a, Cond b);
    friend Cond operator&(Cond a, Cond b);
    friend Cond operator!(Cond a);

private:
    std::shared_ptr<Fn> fn_;
};

// Generic escape hatch, and the building block for everything below.
inline Cond when(Cond::Fn fn) { return Cond(std::move(fn)); }

// True from `t` onward. The engine may not step exactly onto t — the feed is
// irregular — so this is "at or after", never "equals".
Cond at_or_after(Timestamp t);

// Seconds since midnight, local session time. Accepts "HH:MM" and "HH:MM:SS".
int parse_time_of_day(std::string_view hhmm);

}  // namespace volforge
