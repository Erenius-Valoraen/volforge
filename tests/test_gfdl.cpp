#include "harness.hpp"

#include "volforge/gfdl_csv.hpp"

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
