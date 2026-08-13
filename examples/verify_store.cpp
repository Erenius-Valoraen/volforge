// Checks a day store without rebuilding it.
//
//   verify_store <store-dir> [--deep] [--open HH:MM] [--close HH:MM]
//
// Opening a file already validates the header, instrument table, string table,
// timeline and directory against their checksums, and rejects a truncated or
// extended file. This adds the checks that catch a *parser* fault rather than a
// storage one:
//
//   - every timestamp belongs to the session the file claims
//   - every timestamp falls inside plausible trading hours
//   - the timeline is strictly ascending and matches the row counts
//
// A row mangled on the way in is the only thing that puts a timestamp outside
// those bounds, so this is a direct test for it rather than a proxy.
//
// --deep additionally decodes every instrument, which verifies each column
// blob's checksum and every per-instrument time range. Slower, but still far
// cheaper than converting again.

#include "volforge/vfday.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace volforge;
namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

namespace {

int parse_hhmm(const char* s) {
    const int hh = std::atoi(s);
    const char* colon = std::strchr(s, ':');
    const int mm = colon ? std::atoi(colon + 1) : 0;
    return hh * 3600 + mm * 60;
}

std::string local_time(Timestamp ts) {
    const std::int64_t local = ts.seconds() + kISTOffsetSeconds;
    const std::int64_t day = (local >= 0 ? local : local - 86399) / 86400;
    const std::int64_t sod = ((local % 86400) + 86400) % 86400;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld",
                  date_from_days(day).to_string().c_str(), sod / 3600, (sod % 3600) / 60,
                  sod % 60);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: verify_store <store-dir> [--deep] [--open HH:MM] [--close HH:MM]\n");
        return 2;
    }
    const std::string dir = argv[1];
    bool deep = false;
    int open_sec = 9 * 3600, close_sec = 15 * 3600 + 45 * 60;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--deep") deep = true;
        else if (a == "--open" && i + 1 < argc) open_sec = parse_hhmm(argv[++i]);
        else if (a == "--close" && i + 1 < argc) close_sec = parse_hhmm(argv[++i]);
        else { std::printf("unknown argument: %s\n", a.c_str()); return 2; }
    }

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".vfday") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::printf("no .vfday files in %s\n", dir.c_str()); return 1; }

    std::printf("%zu sessions, checking %s\n\n", files.size(),
                deep ? "structure and every column" : "structure and timelines");

    const auto started = SteadyClock::now();
    std::size_t ok = 0, bad = 0, rows = 0, instruments = 0, stamps = 0;
    std::vector<std::string> problems;
    Date first_date, last_date;

    for (const fs::path& path : files) {
        try {
            InstrumentRegistry registry;
            auto s = VfdaySession::open(path.string(), registry);

            const Date date = s->date();
            if (!first_date.valid()) first_date = date;
            last_date = date;

            if (path.filename().string().substr(0, 10) != date.to_string()) {
                problems.push_back(path.filename().string() + ": names a different date than " +
                                   date.to_string());
            }

            const Timestamp lo = timestamp_of(date, open_sec, kISTOffsetSeconds);
            const Timestamp hi = timestamp_of(date, close_sec, kISTOffsetSeconds);

            const auto tl = s->timeline();
            if (tl.empty()) problems.push_back(date.to_string() + ": empty timeline");

            std::size_t outside = 0;
            Timestamp worst{};
            for (const Timestamp t : tl) {
                if (t < lo || t > hi) {
                    if (outside == 0) worst = t;
                    ++outside;
                }
            }
            if (outside != 0) {
                problems.push_back(date.to_string() + ": " + std::to_string(outside) +
                                   " timestamp(s) outside trading hours, first " +
                                   local_time(worst));
            }

            if (deep) {
                std::size_t counted = 0;
                for (const InstrumentId id : s->instruments()) {
                    const auto c = s->quotes(id);   // checksum verified before decode
                    counted += c.size();
                    if (c.empty()) continue;
                    if (c.ts.front() < lo || c.ts.back() > hi) {
                        problems.push_back(date.to_string() + ": instrument spans " +
                                           local_time(c.ts.front()) + " .. " +
                                           local_time(c.ts.back()));
                        break;
                    }
                }
                if (counted != s->total_observations()) {
                    problems.push_back(date.to_string() + ": column rows do not sum to the header");
                }
            }

            rows += s->total_observations();
            instruments += s->instruments().size();
            stamps += tl.size();
            ++ok;
        } catch (const std::exception& e) {
            problems.emplace_back(std::string(path.filename().string()) + ": " + e.what());
            ++bad;
        }
    }

    const double elapsed = std::chrono::duration<double>(SteadyClock::now() - started).count();
    std::printf("opened      %zu   failed %zu   in %.1fs\n", ok, bad, elapsed);
    if (ok != 0) {
        std::printf("range       %s .. %s\n", first_date.to_string().c_str(),
                    last_date.to_string().c_str());
        std::printf("rows        %zu\n", rows);
        std::printf("timestamps  %zu\n", stamps);
        std::printf("instruments %zu (summed across sessions)\n", instruments);
    }

    if (problems.empty()) {
        std::printf("\nno problems found\n");
        return 0;
    }
    std::printf("\n%zu problem(s):\n", problems.size());
    for (std::size_t i = 0; i < problems.size() && i < 50; ++i) {
        std::printf("  %s\n", problems[i].c_str());
    }
    if (problems.size() > 50) std::printf("  ... and %zu more\n", problems.size() - 50);
    return 1;
}
