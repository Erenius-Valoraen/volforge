// Adapter for GFDL (Global Datafeeds) NFO tick CSV.
//
// One file per traded symbol per day:
//
//     NIFTY03JUL2523000CE.NFO.csv
//     Ticker,Date,Time,LTP,BuyPrice,BuyQty,SellPrice,SellQty,LTQ,OpenInterest
//     NIFTY03JUL2523000CE.NFO,01/07/2025,10:11:50,2551,2539.85,75,2603.55,975,150,34575
//
// Things the vendor's FAQ makes explicit, and which this adapter has to respect:
//
//   - **Only symbols that traded that day have a file.** The instrument universe
//     is therefore "strikes that traded", not "strikes that were listed". A
//     strike quoted all session but never hit is simply absent.
//   - **`Time` is the last trade time**, not a snapshot clock, and resolution is
//     one second.
//   - **`LTQ = 0` does not mean no trade.** The exchange emits records with zero
//     quantity to keep the day's OHLC intact, and a trade landing at the end of a
//     second can have its price in one record and its quantity in the next. Any
//     "did a trade happen" logic built on `LTQ > 0` alone will be wrong.
//   - **`OpenInterest` updates every three minutes**, and reads 0 for the first
//     three minutes of a newly listed strike. Zero does not mean "no interest".
//
// Lot size is not in the feed. It is a property of the contract at the time and
// has to be supplied; NIFTY was 75 through 2025.

#pragma once

#include "volforge/memory_source.hpp"

#include <string>
#include <vector>

namespace volforge {

struct GfdlLoadOptions {
    // Contracts carry no lot size in the feed, and it has changed over time
    // (NIFTY has been 25, then 50, then 75). Getting it wrong scales every
    // position and every P&L figure, so it is required rather than guessed.
    Qty lot_size = 75;

    Price tick_size = Price::from_minor(5);   // 0.05
    int   utc_offset_seconds = kISTOffsetSeconds;

    // Skip files whose underlying is not this, when set.
    std::string only_underlying;
};

struct GfdlDay {
    std::shared_ptr<MemorySessionData> session;
    UnderlyingId underlying{};
    Date         date;

    std::size_t files_read   = 0;
    std::size_t files_skipped = 0;
    std::size_t rows_read    = 0;
    std::size_t rows_skipped = 0;   // malformed, or out of time order

    // Distinct expiries seen, ascending.
    std::vector<Date> expiries;

    // Anything the caller should know about. Kept rather than logged, because a
    // silent adapter is how bad data becomes a confident backtest.
    std::vector<std::string> warnings;
};

// Parses one day's directory of CSVs.
//
// `registry` accumulates instruments and must outlive the returned session.
GfdlDay load_gfdl_day(InstrumentRegistry& registry, const std::string& directory,
                      const GfdlLoadOptions& options = {});

// Parses one day straight out of a vendor .zip, without expanding it to disk.
//
// This is the path that matters at scale: a year of NIFTY options is 14 GB
// zipped and roughly 145 GB as CSV, so writing the intermediate out is not an
// option on most machines. Files are decompressed one at a time, so peak memory
// is one CSV plus the session being built.
GfdlDay load_gfdl_zip(InstrumentRegistry& registry, const std::string& zip_path,
                      const GfdlLoadOptions& options = {});

// Decomposes a vendor ticker such as "NIFTY03JUL2523000CE.NFO".
struct ParsedSymbol {
    std::string  underlying;
    Date         expiry;
    Price        strike;
    Right        right = Right::Call;
    bool         ok = false;
};

ParsedSymbol parse_gfdl_symbol(std::string_view ticker);

}  // namespace volforge
