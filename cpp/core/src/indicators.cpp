#include "volforge/indicators.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace volforge {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::vector<double> closes_of(const BarSeries& bars) {
    std::vector<double> out;
    out.reserve(bars.all().size());
    for (const Bar& b : bars.all()) out.push_back(b.close.to_double());
    return out;
}

}  // namespace

BarIndicator::BarIndicator(std::shared_ptr<const BarSeries> bars, std::vector<double> values)
    : bars_(std::move(bars)), values_(std::move(values)) {
    if (!bars_) throw std::invalid_argument("BarIndicator: null bar series");
    if (values_.size() != bars_->all().size()) {
        throw std::invalid_argument("BarIndicator: one value per bar required");
    }
}

std::optional<double> BarIndicator::at(Timestamp now, int back) const {
    if (back < 0) {
        throw std::invalid_argument("BarIndicator::at: negative offset reads the future");
    }
    // The gate. known_count counts bars that have *closed*, so a value derived
    // from the forming bar is unreachable by construction.
    const std::size_t k = bars_->known_count(now);
    if (k == 0) return std::nullopt;

    const auto steps = static_cast<std::size_t>(back);
    if (steps >= k) return std::nullopt;

    const double v = values_[k - 1 - steps];
    if (std::isnan(v)) return std::nullopt;   // still warming up
    return v;
}

IndicatorPtr sma(std::shared_ptr<const BarSeries> bars, int period) {
    if (period <= 0) throw std::invalid_argument("sma: period must be positive");
    const auto closes = closes_of(*bars);

    std::vector<double> out(closes.size(), kNaN);
    double running = 0.0;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        running += closes[i];
        if (i >= static_cast<std::size_t>(period)) running -= closes[i - period];
        if (i + 1 >= static_cast<std::size_t>(period)) out[i] = running / period;
    }
    return std::make_shared<const BarIndicator>(std::move(bars), std::move(out));
}

IndicatorPtr stdev(std::shared_ptr<const BarSeries> bars, int period) {
    if (period <= 0) throw std::invalid_argument("stdev: period must be positive");
    const auto closes = closes_of(*bars);

    std::vector<double> out(closes.size(), kNaN);
    for (std::size_t i = 0; i + 1 >= static_cast<std::size_t>(period) && i < closes.size(); ++i) {
        if (i + 1 < static_cast<std::size_t>(period)) continue;
        const std::size_t start = i + 1 - static_cast<std::size_t>(period);

        double mean = 0.0;
        for (std::size_t j = start; j <= i; ++j) mean += closes[j];
        mean /= period;

        double acc = 0.0;
        for (std::size_t j = start; j <= i; ++j) {
            const double d = closes[j] - mean;
            acc += d * d;
        }
        out[i] = std::sqrt(acc / period);
    }
    return std::make_shared<const BarIndicator>(std::move(bars), std::move(out));
}

BollingerBands bollinger(std::shared_ptr<const BarSeries> bars, int period, double k) {
    if (period <= 0) throw std::invalid_argument("bollinger: period must be positive");

    const auto closes = closes_of(*bars);
    std::vector<double> mid(closes.size(), kNaN);
    std::vector<double> up(closes.size(), kNaN);
    std::vector<double> lo(closes.size(), kNaN);

    for (std::size_t i = 0; i < closes.size(); ++i) {
        if (i + 1 < static_cast<std::size_t>(period)) continue;
        const std::size_t start = i + 1 - static_cast<std::size_t>(period);

        double mean = 0.0;
        for (std::size_t j = start; j <= i; ++j) mean += closes[j];
        mean /= period;

        double acc = 0.0;
        for (std::size_t j = start; j <= i; ++j) {
            const double d = closes[j] - mean;
            acc += d * d;
        }
        const double sd = std::sqrt(acc / period);

        mid[i] = mean;
        up[i]  = mean + k * sd;
        lo[i]  = mean - k * sd;
    }

    return BollingerBands{
        std::make_shared<const BarIndicator>(bars, std::move(mid)),
        std::make_shared<const BarIndicator>(bars, std::move(up)),
        std::make_shared<const BarIndicator>(bars, std::move(lo)),
    };
}

}  // namespace volforge
