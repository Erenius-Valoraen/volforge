#include "volforge/memory_source.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace volforge {

// ---------------------------------------------------------------------------
// MemorySessionData
// ---------------------------------------------------------------------------

MemorySessionData::MemorySessionData(Date date, const InstrumentRegistry& registry)
    : date_(date), registry_(&registry) {}

MemorySessionData::Series& MemorySessionData::series_for(InstrumentId id) {
    const auto key = index_of(id);
    auto [it, inserted] = series_.try_emplace(key);
    if (inserted) instruments_.push_back(id);
    return it->second;
}

void MemorySessionData::append(InstrumentId id, const Quote& q) {
    Series& s = series_for(id);
    if (!s.ts.empty() && q.ts < s.ts.back()) {
        throw std::invalid_argument(
            "MemorySessionData::append: observations must be non-decreasing in time");
    }
    s.ts.push_back(q.ts);
    s.last.push_back(q.last);
    s.bid.push_back(q.bid);
    s.ask.push_back(q.ask);
    s.bid_qty.push_back(q.bid_qty);
    s.ask_qty.push_back(q.ask_qty);
    s.last_qty.push_back(q.last_qty);
    s.open_interest.push_back(q.open_interest);
    ++total_;
    order_.clear();      // any cached merge order is now stale
    timeline_.clear();
}

QuoteColumns MemorySessionData::quotes(InstrumentId id) const {
    const auto it = series_.find(index_of(id));
    if (it == series_.end()) return QuoteColumns{};
    const Series& s = it->second;
    return QuoteColumns{s.ts, s.last, s.bid, s.ask,
                        s.bid_qty, s.ask_qty, s.last_qty, s.open_interest};
}

void MemorySessionData::build_event_order() {
    order_.clear();
    order_.reserve(total_);
    for (const auto& [key, s] : series_) {
        const auto id = static_cast<InstrumentId>(key);
        for (std::size_t i = 0; i < s.ts.size(); ++i) {
            order_.push_back(Event{s.ts[i], id, static_cast<std::uint32_t>(i)});
        }
    }
    // Stable ordering on (time, instrument, row). The instrument tiebreak keeps
    // replay deterministic when many instruments print in the same second, which
    // at 1-second resolution is most of them.
    timeline_.clear();
    std::sort(order_.begin(), order_.end(), [](const Event& a, const Event& b) {
        if (a.ts != b.ts) return a.ts < b.ts;
        if (a.instrument != b.instrument) return index_of(a.instrument) < index_of(b.instrument);
        return a.row < b.row;
    });

    timeline_.reserve(order_.size());
    for (const Event& e : order_) {
        if (timeline_.empty() || timeline_.back() != e.ts) timeline_.push_back(e.ts);
    }
    timeline_.shrink_to_fit();
}

// ---------------------------------------------------------------------------
// MemoryDataSource
// ---------------------------------------------------------------------------

MemoryDataSource::MemoryDataSource(InstrumentRegistry registry)
    : registry_(std::move(registry)) {}

void MemoryDataSource::add_session(std::shared_ptr<MemorySessionData> session) {
    if (!session) throw std::invalid_argument("MemoryDataSource::add_session: null session");
    sessions_[session->date().yyyymmdd] = std::move(session);
}

std::vector<Date> MemoryDataSource::sessions() const {
    std::vector<Date> out;
    out.reserve(sessions_.size());
    for (const auto& [d, _] : sessions_) out.push_back(Date{d});
    return out;   // std::map keeps these ascending
}

std::shared_ptr<SessionData> MemoryDataSource::load(Date d) {
    const auto it = sessions_.find(d.yyyymmdd);
    if (it == sessions_.end()) return nullptr;
    return it->second;
}

}  // namespace volforge
