// The day store: one file per trading session.
//
// This layer is the one place where a silent error is unrecoverable. Every
// figure a backtest reports is derived from these bytes, so a flipped bit does
// not produce an obviously broken run — it produces a confident, plausible,
// wrong one. The design is therefore built around detecting damage rather than
// around going fast, and the speed is a consequence of the layout rather than of
// skipping checks.
//
// What that means concretely:
//
//   - **Self-describing.** The file carries its own instrument table, so
//     reloading it reconstructs the same contracts regardless of what any
//     registry happens to contain. Instrument ids are never stored; identity is.
//   - **Checksummed at every level.** Header, instrument table, timeline,
//     directory and each instrument's column blob carry their own CRC. A blob is
//     verified immediately before it is decoded, so corruption is caught at the
//     point of use rather than trusted at the point of open.
//   - **Bounds-checked everywhere.** Every offset and length is validated
//     against the actual file size before it is followed. A malformed file is
//     rejected, never read past.
//   - **Verified on write.** The writer reads its own output back and compares
//     it against the source, value for value, before reporting success.
//
// Memory is bounded by what a strategy touches, not by the size of the data.
// Instrument columns are decoded on first access and cached for the session;
// a strategy trading four strikes decodes four, not nine hundred. The session
// timeline is stored separately so stepping through time never requires
// decoding a single quote.

#pragma once

#include "volforge/data_source.hpp"
#include "volforge/instrument.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace volforge {

// Bumped whenever the layout changes in a way older readers cannot handle.
constexpr std::uint32_t kVfdayVersion = 3;

struct VfdayStats {
    std::size_t instruments = 0;
    std::size_t rows        = 0;
    std::size_t timestamps  = 0;   // distinct
    std::size_t bytes       = 0;
    double      bytes_per_row = 0.0;
};

// Writes one session.
//
// `verify` re-reads the finished file and compares every value against the
// source before returning. It roughly doubles write time and is on by default,
// because a conversion that silently mangled a column would not be discovered
// until a backtest reported something odd months later.
VfdayStats write_vfday(const SessionData& session, const std::string& path,
                       bool verify = true);

// A session backed by a file on disk.
//
// Opening reads only the header, instrument table, timeline and directory —
// kilobytes, not megabytes. Quote columns are decoded on first request and held
// for the lifetime of the session.
class VfdaySession final : public SessionData {
public:
    VfdaySession() = default;
    ~VfdaySession() override;
    VfdaySession(const VfdaySession&) = delete;
    VfdaySession& operator=(const VfdaySession&) = delete;

    // Throws if the file is missing, truncated, the wrong version, or fails any
    // checksum. Registers the file's instruments into `registry`.
    static std::shared_ptr<VfdaySession> open(const std::string& path,
                                              InstrumentRegistry& registry);

    [[nodiscard]] Date date() const override { return date_; }
    [[nodiscard]] const InstrumentRegistry& registry() const override { return *registry_; }
    [[nodiscard]] std::span<const InstrumentId> instruments() const override {
        return instruments_;
    }
    [[nodiscard]] QuoteColumns quotes(InstrumentId) const override;
    [[nodiscard]] std::size_t total_observations() const override { return total_rows_; }

    // Distinct observation times, ascending. Stepping through a session needs
    // only this, so a replay that never touches an instrument never decodes one.
    [[nodiscard]] std::span<const Timestamp> timeline() const override { return timeline_; }

    // Whether the instrument had printed by `t`, answered from the directory
    // without decoding anything. Chain scans depend on this being cheap.
    [[nodiscard]] bool printed_by(InstrumentId id, Timestamp t) const override;

    [[nodiscard]] std::size_t decoded_instruments() const { return decoded_.size(); }
    [[nodiscard]] std::size_t decoded_bytes() const;

private:
    struct Entry {
        std::int32_t  table_index = 0;
        std::uint32_t row_count   = 0;
        std::int64_t  blob_offset = 0;
        std::uint32_t blob_bytes  = 0;
        std::uint32_t blob_crc    = 0;
        Timestamp     first{};
        Timestamp     last{};
    };

    struct Columns {
        std::vector<Timestamp>    ts;
        std::vector<Price>        last, bid, ask;
        std::vector<Qty>          bid_qty, ask_qty, last_qty;
        std::vector<std::int64_t> open_interest;
    };

    const Columns& materialise(InstrumentId id) const;

    std::string                       path_;
    Date                              date_;
    const InstrumentRegistry*         registry_ = nullptr;
    std::size_t                       total_rows_ = 0;

    std::vector<InstrumentId>         instruments_;      // in file order
    std::map<std::int32_t, Entry>     entries_;          // keyed by InstrumentId index
    std::vector<Timestamp>            timeline_;

    mutable std::map<std::int32_t, Columns> decoded_;
    mutable void*                     file_ = nullptr;   // FILE*, opened lazily
};

// A DataSource over a directory of .vfday files.
class VfdaySource final : public DataSource {
public:
    // Scans `directory` for .vfday files and reads each header. Files that fail
    // validation are reported rather than skipped silently.
    VfdaySource(const std::string& directory, InstrumentRegistry& registry);

    [[nodiscard]] const InstrumentRegistry& registry() const override { return *registry_; }
    [[nodiscard]] std::vector<Date> sessions() const override;
    [[nodiscard]] std::shared_ptr<SessionData> load(Date) override;

    [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }

private:
    InstrumentRegistry*                 registry_;
    std::map<std::int32_t, std::string> files_;    // date -> path
    std::map<std::int32_t, std::shared_ptr<VfdaySession>> cache_;
    std::vector<std::string>            problems_;
};

// CRC-32 (IEEE), exposed for tests.
std::uint32_t crc32(const void* data, std::size_t bytes, std::uint32_t seed = 0);

}  // namespace volforge
