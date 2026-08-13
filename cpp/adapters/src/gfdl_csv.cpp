#include "volforge/gfdl_csv.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace volforge {
namespace {

constexpr std::array<const char*, 12> kMonths = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                                "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

int month_from(std::string_view s) {
    for (int i = 0; i < 12; ++i) {
        if (s.size() == 3 && s[0] == kMonths[static_cast<std::size_t>(i)][0] &&
            s[1] == kMonths[static_cast<std::size_t>(i)][1] &&
            s[2] == kMonths[static_cast<std::size_t>(i)][2]) {
            return i + 1;
        }
    }
    return 0;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// --- field scanners ---------------------------------------------------------
//
// Hand-rolled rather than iostreams or std::stod: this runs 6.5 million times a
// day, and the standard conversions are two orders of magnitude slower than the
// work actually being done.

std::int64_t scan_int(const char*& p, const char* end) {
    std::int64_t v = 0;
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) { neg = (*p == '-'); ++p; }
    while (p < end && is_digit(*p)) { v = v * 10 + (*p - '0'); ++p; }
    return neg ? -v : v;
}

// Decimal with at most two places, returned in minor units.
std::int32_t scan_price_minor(const char*& p, const char* end) {
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) { neg = (*p == '-'); ++p; }

    std::int64_t whole = 0;
    while (p < end && is_digit(*p)) { whole = whole * 10 + (*p - '0'); ++p; }

    std::int64_t frac = 0;
    int digits = 0;
    if (p < end && *p == '.') {
        ++p;
        while (p < end && is_digit(*p)) {
            if (digits < 2) { frac = frac * 10 + (*p - '0'); ++digits; }
            ++p;   // anything past two places is below the tick grid
        }
    }
    while (digits < 2) { frac *= 10; ++digits; }

    const std::int64_t v = whole * 100 + frac;
    return static_cast<std::int32_t>(neg ? -v : v);
}

void skip_field(const char*& p, const char* end) {
    while (p < end && *p != ',' && *p != '\n' && *p != '\r') ++p;
    if (p < end && *p == ',') ++p;
}

bool at_separator(const char* p, const char* end) {
    return p >= end || *p == ',' || *p == '\n' || *p == '\r';
}

}  // namespace

ParsedSymbol parse_gfdl_symbol(std::string_view ticker) {
    ParsedSymbol out;

    if (const auto dot = ticker.find('.'); dot != std::string_view::npos) {
        ticker = ticker.substr(0, dot);          // drop the ".NFO" segment
    }
    if (ticker.size() < 12) return out;

    // Right is the final two characters.
    const std::string_view tail = ticker.substr(ticker.size() - 2);
    if (tail == "CE") out.right = Right::Call;
    else if (tail == "PE") out.right = Right::Put;
    else return out;
    ticker.remove_suffix(2);

    // Underlying runs up to the first digit.
    std::size_t i = 0;
    while (i < ticker.size() && !is_digit(ticker[i])) ++i;
    if (i == 0 || i + 7 >= ticker.size()) return out;
    out.underlying = std::string(ticker.substr(0, i));

    // Then DDMMMYY, then the strike.
    const std::string_view rest = ticker.substr(i);
    if (rest.size() < 8) return out;
    if (!is_digit(rest[0]) || !is_digit(rest[1])) return out;

    const int day   = (rest[0] - '0') * 10 + (rest[1] - '0');
    const int month = month_from(rest.substr(2, 3));
    if (month == 0) return out;
    if (!is_digit(rest[5]) || !is_digit(rest[6])) return out;
    const int year = 2000 + (rest[5] - '0') * 10 + (rest[6] - '0');

    const std::string_view strike_text = rest.substr(7);
    if (strike_text.empty()) return out;
    for (const char c : strike_text) {
        if (!is_digit(c) && c != '.') return out;
    }

    const char* sp = strike_text.data();
    out.strike = Price::from_minor(scan_price_minor(sp, strike_text.data() + strike_text.size()));
    if (out.strike.minor <= 0) return out;

    if (day < 1 || day > 31) return out;
    out.expiry = Date{year * 10000 + month * 100 + day};
    out.ok = true;
    return out;
}

GfdlDay load_gfdl_day(InstrumentRegistry& registry, const std::string& directory,
                      const GfdlLoadOptions& options) {
    namespace fs = std::filesystem;

    GfdlDay out;
    if (!fs::is_directory(directory)) {
        out.warnings.push_back("not a directory: " + directory);
        return out;
    }

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".csv" && ext != ".CSV") continue;
        files.push_back(entry.path());
    }
    // Deterministic order, so instrument ids are stable run to run.
    std::sort(files.begin(), files.end());

    std::set<std::int32_t> expiries;
    std::string buffer;

    for (const fs::path& path : files) {
        const ParsedSymbol sym = parse_gfdl_symbol(path.filename().string());
        if (!sym.ok) {
            ++out.files_skipped;
            out.warnings.push_back("unparsable symbol: " + path.filename().string());
            continue;
        }
        if (!options.only_underlying.empty() && sym.underlying != options.only_underlying) {
            ++out.files_skipped;
            continue;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            ++out.files_skipped;
            out.warnings.push_back("unreadable: " + path.filename().string());
            continue;
        }
        buffer.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (buffer.empty()) { ++out.files_skipped; continue; }

        out.underlying = registry.intern_underlying(sym.underlying);

        InstrumentSpec spec;
        spec.underlying = out.underlying;
        spec.kind       = InstrumentKind::Option;
        spec.expiry     = sym.expiry;
        spec.strike     = sym.strike;
        spec.right      = sym.right;
        spec.lot_size   = options.lot_size;
        spec.tick_size  = options.tick_size;
        const InstrumentId id = registry.add(spec);

        expiries.insert(sym.expiry.yyyymmdd);

        const char* p   = buffer.data();
        const char* end = p + buffer.size();

        // Skip the header line if present.
        if (end - p > 6 && std::strncmp(p, "Ticker", 6) == 0) {
            while (p < end && *p != '\n') ++p;
            if (p < end) ++p;
        }

        Date      row_date;
        Timestamp previous{};
        bool      have_previous = false;

        while (p < end) {
            if (*p == '\r' || *p == '\n') { ++p; continue; }

            skip_field(p, end);                                   // Ticker

            // Date, dd/mm/yyyy
            const std::int64_t dd = scan_int(p, end);
            if (p < end && *p == '/') ++p;
            const std::int64_t mm = scan_int(p, end);
            if (p < end && *p == '/') ++p;
            const std::int64_t yyyy = scan_int(p, end);
            if (p < end && *p == ',') ++p;

            // Time, HH:MM:SS
            const std::int64_t hh = scan_int(p, end);
            if (p < end && *p == ':') ++p;
            const std::int64_t mi = scan_int(p, end);
            if (p < end && *p == ':') ++p;
            const std::int64_t ss = scan_int(p, end);
            if (p < end && *p == ',') ++p;

            Quote q;
            q.last = Price::from_minor(scan_price_minor(p, end));
            if (p < end && *p == ',') ++p;
            q.bid = Price::from_minor(scan_price_minor(p, end));
            if (p < end && *p == ',') ++p;
            q.bid_qty = static_cast<Qty>(scan_int(p, end));
            if (p < end && *p == ',') ++p;
            q.ask = Price::from_minor(scan_price_minor(p, end));
            if (p < end && *p == ',') ++p;
            q.ask_qty = static_cast<Qty>(scan_int(p, end));
            if (p < end && *p == ',') ++p;
            q.last_qty = static_cast<Qty>(scan_int(p, end));
            if (p < end && *p == ',') ++p;
            q.open_interest = scan_int(p, end);

            while (p < end && *p != '\n') ++p;
            if (p < end) ++p;

            if (yyyy < 1970 || mm < 1 || mm > 12 || dd < 1 || dd > 31 ||
                hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 59) {
                ++out.rows_skipped;
                continue;
            }

            const Date d{static_cast<std::int32_t>(yyyy * 10000 + mm * 100 + dd)};
            if (!row_date.valid()) row_date = d;

            q.ts = timestamp_of(d, static_cast<int>(hh * 3600 + mi * 60 + ss),
                                options.utc_offset_seconds);

            // The feed is time-ordered within a file; anything else is corrupt
            // and is dropped rather than silently reordered.
            if (have_previous && q.ts < previous) { ++out.rows_skipped; continue; }
            previous = q.ts;
            have_previous = true;

            if (!out.session) {
                out.session = std::make_shared<MemorySessionData>(d, registry);
                out.date = d;
            }
            out.session->append(id, q);
            ++out.rows_read;
        }

        ++out.files_read;
        if (row_date.valid() && out.date.valid() && row_date != out.date) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s carries date %s, expected %s",
                          path.filename().string().c_str(), row_date.to_string().c_str(),
                          out.date.to_string().c_str());
            out.warnings.push_back(buf);
        }
    }

    for (const std::int32_t e : expiries) out.expiries.push_back(Date{e});

    if (out.session) out.session->build_event_order();
    return out;
}

}  // namespace volforge
