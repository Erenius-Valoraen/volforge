#include "harness.hpp"

#include "volforge/gfdl_csv.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace volforge;

TEST(vendor_symbols_decompose_correctly) {
    const auto weekly = parse_gfdl_symbol("NIFTY03JUL2523000CE.NFO");
    CHECK(weekly.ok);
    CHECK(weekly.underlying == "NIFTY");
    CHECK(weekly.expiry == Date{20250703});
    CHECK(weekly.strike == Price::from_double(23000));
    CHECK(weekly.right == Right::Call);

    // Monthly contracts use the same DDMMMYY shape, so there is no second format
    // to special-case — verified against all 904 files in the sample day.
    const auto monthly = parse_gfdl_symbol("NIFTY31JUL2525000PE.NFO");
    CHECK(monthly.ok);
    CHECK(monthly.expiry == Date{20250731});
    CHECK(monthly.right == Right::Put);

    // Long-dated contracts are present in the sample and must parse too.
    const auto leap = parse_gfdl_symbol("NIFTY28DEC2826000CE.NFO");
    CHECK(leap.ok);
    CHECK(leap.expiry == Date{20281228});

    // Works without the exchange suffix.
    CHECK(parse_gfdl_symbol("NIFTY03JUL2523000CE").ok);
}

TEST(a_malformed_symbol_is_rejected_rather_than_guessed) {
    CHECK(!parse_gfdl_symbol("garbage").ok);
    CHECK(!parse_gfdl_symbol("").ok);
    CHECK(!parse_gfdl_symbol("NIFTY03JUL25CE.NFO").ok);          // no strike
    CHECK(!parse_gfdl_symbol("NIFTY03XXX2523000CE.NFO").ok);     // no such month
    CHECK(!parse_gfdl_symbol("NIFTY03JUL2523000XX.NFO").ok);     // neither CE nor PE
    CHECK(!parse_gfdl_symbol("03JUL2523000CE.NFO").ok);          // no underlying
    CHECK(!parse_gfdl_symbol("NIFTY99JUL2523000CE.NFO").ok);     // impossible day
}

TEST(other_underlyings_parse_without_being_taught_about_them) {
    const auto bn = parse_gfdl_symbol("BANKNIFTY31JUL2556000CE.NFO");
    CHECK(bn.ok);
    CHECK(bn.underlying == "BANKNIFTY");
    CHECK(bn.strike == Price::from_double(56000));

    const auto stock = parse_gfdl_symbol("RELIANCE31JUL251500PE.NFO");
    CHECK(stock.ok);
    CHECK(stock.underlying == "RELIANCE");
    CHECK(stock.right == Right::Put);
}

TEST(loading_a_missing_directory_reports_rather_than_crashes) {
    InstrumentRegistry registry;
    const auto day = load_gfdl_day(registry, "definitely-not-a-real-directory");
    CHECK(day.session == nullptr);
    CHECK(!day.warnings.empty());
}

// ---------------------------------------------------------------------------
// Corrupt rows, found in real vendor data
// ---------------------------------------------------------------------------

namespace {

// Writes a CSV and loads it through the directory path, returning the session.
struct OneFile {
    std::filesystem::path dir;
    InstrumentRegistry    registry;
    GfdlDay               day;

    OneFile(const char* stem, const std::string& body) {
        dir = std::filesystem::temp_directory_path() / ("volforge_gfdl_" + std::string(stem));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "NIFTY27JAN2627250CE.NFO.csv", std::ios::binary) << body;
        day = load_gfdl_day(registry, dir.string());
    }
    ~OneFile() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};

constexpr const char* kHeader =
    "Ticker,Date,Time,LTP,BuyPrice,BuyQty,SellPrice,SellQty,LTQ,OpenInterest\n";

std::string row(const char* date, const char* time) {
    return std::string("NIFTY27JAN2627250CE.NFO,") + date + "," + time +
           ",1.4,1.35,69940,1.4,54730,0,256360\n";
}

}  // namespace

TEST(a_mistyped_year_is_rejected_rather_than_wrapped_into_the_future) {
    // Straight from the vendor's 2026-01-22 archive: one row reads 20266.
    // Multiplied out to nanoseconds that overflows a signed 64-bit count and
    // wraps to the year 2144, which silently reorders the whole session and
    // makes the replay clock run backwards a day later.
    OneFile f("badyear", std::string(kHeader) + row("22/01/2026", "09:15:00") +
                             row("22/01/20266", "09:26:42") +
                             row("22/01/2026", "09:28:06"));

    CHECK(f.day.session != nullptr);
    CHECK_EQ(f.day.rows_read, std::size_t{2});
    CHECK_EQ(f.day.rows_skipped, std::size_t{1});

    const auto tl = f.day.session->timeline();
    CHECK(!tl.empty());
    // Everything must land inside the session it claims to be.
    const Timestamp lo = timestamp_of(f.day.date, 0, kISTOffsetSeconds);
    const Timestamp hi = timestamp_of(f.day.date, 24 * 3600, kISTOffsetSeconds);
    for (const Timestamp t : tl) CHECK(t >= lo && t < hi);
}

TEST(a_row_from_another_day_is_rejected) {
    // One archive is one session, so a stray date is damage rather than data.
    OneFile f("otherday", std::string(kHeader) + row("22/01/2026", "09:15:00") +
                              row("23/01/2026", "09:16:00") +
                              row("22/01/2026", "09:17:00"));

    CHECK_EQ(f.day.rows_read, std::size_t{2});
    CHECK_EQ(f.day.rows_skipped, std::size_t{1});
    CHECK(f.day.date == Date{20260122});
}

TEST(a_truncated_final_row_does_not_become_a_midnight_print) {
    // The same archive ends one file mid-row, with no time field at all.
    // Reading the missing fields as zero would plant a print at midnight.
    OneFile f("truncated", std::string(kHeader) + row("22/01/2026", "09:15:00") +
                               "NIFTY27JAN2627250CE.NFO,22/01/2026");

    CHECK_EQ(f.day.rows_read, std::size_t{1});
    CHECK_EQ(f.day.rows_skipped, std::size_t{1});
}

TEST(a_truncated_row_is_rejected_even_with_nothing_after_it_to_catch_it) {
    // The order check would have caught the case above by accident, because a
    // later row came after it. Alone, only the field count saves it.
    OneFile f("truncalone", std::string(kHeader) + "NIFTY27JAN2627250CE.NFO,22/01/2026");

    CHECK_EQ(f.day.rows_read, std::size_t{0});
    CHECK_EQ(f.day.rows_skipped, std::size_t{1});
    CHECK(f.day.session == nullptr);
}

TEST(a_row_with_too_few_or_too_many_fields_is_rejected) {
    OneFile f("fields", std::string(kHeader) + row("22/01/2026", "09:15:00") +
                            "NIFTY27JAN2627250CE.NFO,22/01/2026,09:16:00,1.4\n" +
                            "NIFTY27JAN2627250CE.NFO,22/01/2026,09:17:00,1,2,3,4,5,6,7,8\n");
    CHECK_EQ(f.day.rows_read, std::size_t{1});
    CHECK_EQ(f.day.rows_skipped, std::size_t{2});
}

TEST(an_out_of_range_time_is_rejected) {
    OneFile f("badtime", std::string(kHeader) + row("22/01/2026", "09:15:00") +
                             row("22/01/2026", "25:00:00") +
                             row("22/01/2026", "09:16:61"));
    CHECK_EQ(f.day.rows_read, std::size_t{1});
    CHECK_EQ(f.day.rows_skipped, std::size_t{2});
}
