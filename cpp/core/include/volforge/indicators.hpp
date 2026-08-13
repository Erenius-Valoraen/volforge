// Indicators over bars.
//
// The rule that makes these safe: **an indicator's known_at is the known_at of
// the most recent input it consumed.** A value computed from bars 0..i is
// knowable exactly when bar i closes, and not one moment sooner.
//
// This is where the subtle version of look-ahead lives. A 20-period band over
// 19 completed bars *plus the bar currently forming* depends on where that bar
// eventually closes, so using it intrabar is reading the future — even though
// nobody wrote a negative index. Indicators here are computed over completed
// bars only, so an intrabar comparison sees the last completed value held flat
// across the forming interval, stepping at each bar close.

#pragma once

#include "volforge/bars.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace volforge {

// A value per bar, gated on that bar's close_time.
class BarIndicator {
public:
    BarIndicator(std::shared_ptr<const BarSeries> bars, std::vector<double> values);

    // The value from the most recent bar completed at `now`, stepped `back` bars
    // earlier. nullopt before warmup completes or when no bar has closed yet.
    [[nodiscard]] std::optional<double> at(Timestamp now, int back = 0) const;

    [[nodiscard]] const BarSeries& bars() const { return *bars_; }
    [[nodiscard]] const std::vector<double>& values() const { return values_; }

private:
    std::shared_ptr<const BarSeries> bars_;
    std::vector<double>              values_;
};

using IndicatorPtr = std::shared_ptr<const BarIndicator>;

// Simple moving average of bar closes. Values before warmup are NaN, which
// propagates to nullopt rather than silently becoming a partial average — a
// 20-period mean of 3 bars is a different statistic wearing the same name.
IndicatorPtr sma(std::shared_ptr<const BarSeries> bars, int period);

// Population standard deviation of bar closes over the same window.
IndicatorPtr stdev(std::shared_ptr<const BarSeries> bars, int period);

struct BollingerBands {
    IndicatorPtr middle;
    IndicatorPtr upper;
    IndicatorPtr lower;
};

BollingerBands bollinger(std::shared_ptr<const BarSeries> bars, int period, double k);

}  // namespace volforge
