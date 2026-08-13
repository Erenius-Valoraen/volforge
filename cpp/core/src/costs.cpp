#include "volforge/costs.hpp"
#include "volforge/portfolio.hpp"   // for Side

#include <cmath>

namespace volforge {

Money IndianFnOCosts::cost_of(const CostContext& ctx) const {
    // Settlement is not a brokered order. Only the exercise levy applies, and it
    // is charged on intrinsic value to the buyer of an in-the-money option; a
    // writer being assigned pays none of it. Brokers differ on whether they bill
    // a ticket for expiry, and the common case is that they do not.
    if (ctx.exercise) {
        const double intrinsic = ctx.intrinsic.to_double() * static_cast<double>(ctx.qty);
        if (!(intrinsic > 0.0)) return Money{};
        return Money{static_cast<std::int64_t>(
            std::llround(intrinsic * rates_.stt_exercise_pct * 100.0))};
    }

    const double premium = ctx.price.to_double() * static_cast<double>(ctx.qty);
    const double brokerage = static_cast<double>(rates_.brokerage_per_order.minor) / 100.0;

    if (!(premium > 0.0)) {
        // A zero-premium fill still costs a ticket, and GST rides on it.
        return Money{static_cast<std::int64_t>(
            std::llround(brokerage * (1.0 + rates_.gst_pct) * 100.0))};
    }

    // Exchange-level charges, which GST applies to.
    const double exchange = premium * (rates_.transaction_pct + rates_.sebi_pct +
                                       rates_.ipft_pct);
    const double gst = (brokerage + exchange) * rates_.gst_pct;

    // Statutory levies, which GST does not apply to. STT lands on the seller of
    // premium; stamp duty on the buyer.
    double statutory = 0.0;
    if (ctx.side == Side::Sell) {
        statutory = premium * rates_.stt_sell_pct;
    } else {
        statutory = premium * rates_.stamp_buy_pct;
    }

    const double total = brokerage + exchange + gst + statutory;
    return Money{static_cast<std::int64_t>(std::llround(total * 100.0))};
}

std::shared_ptr<const CostPolicy> default_cost_policy() {
    static const auto policy = std::make_shared<const IndianFnOCosts>();
    return policy;
}

}  // namespace volforge
