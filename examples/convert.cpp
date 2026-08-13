// Converts vendor day archives into the volforge day store.
//
//   convert <input> <output-dir> [--lot-size N] [--underlying NIFTY]
//           [--no-verify] [--force] [--limit N]
//
// `input` may be a single .zip, or a directory searched recursively for them.
// Nothing is ever expanded to disk: each archive is read in memory, one member
// at a time, and written straight out as a .vfday. A year of NIFTY options is
// 14 GB zipped and about 145 GB as CSV, so the intermediate is not something
// most machines can hold.
//
// Conversion is resumable. A day whose output already exists is skipped unless
// --force is given, so an interrupted run picks up where it stopped.

#include "volforge/gfdl_csv.hpp"
#include "volforge/vfday.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace volforge;
namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

namespace {

double seconds_since(SteadyClock::time_point t0) {
    return std::chrono::duration<double>(SteadyClock::now() - t0).count();
}

std::string human(double bytes) {
    const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (bytes >= 1024.0 && i < 4) { bytes /= 1024.0; ++i; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", bytes, unit[i]);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: convert <input.zip|dir> <output-dir> [--lot-size N] "
                    "[--underlying NAME] [--no-verify] [--force] [--limit N]\n");
        return 2;
    }

    const std::string input = argv[1];
    const fs::path    outdir = argv[2];

    GfdlLoadOptions options;
    bool verify = true, force = false;
    std::size_t limit = 0;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--lot-size" && i + 1 < argc) options.lot_size = std::atoi(argv[++i]);
        else if (arg == "--underlying" && i + 1 < argc) options.only_underlying = argv[++i];
        else if (arg == "--no-verify") verify = false;
        else if (arg == "--force") force = true;
        else if (arg == "--limit" && i + 1 < argc) limit = static_cast<std::size_t>(std::atoll(argv[++i]));
        else { std::printf("unknown argument: %s\n", arg.c_str()); return 2; }
    }

    std::vector<fs::path> archives;
    if (fs::is_directory(input)) {
        for (const auto& e : fs::recursive_directory_iterator(input)) {
            if (e.is_regular_file() && e.path().extension() == ".zip") archives.push_back(e.path());
        }
    } else if (fs::is_regular_file(input)) {
        archives.emplace_back(input);
    } else {
        std::printf("no such input: %s\n", input.c_str());
        return 1;
    }
    std::sort(archives.begin(), archives.end());
    if (limit != 0 && archives.size() > limit) archives.resize(limit);

    if (archives.empty()) { std::printf("nothing to convert\n"); return 1; }
    fs::create_directories(outdir);

    std::printf("%zu archive(s), lot size %d, verify %s\n\n", archives.size(),
                options.lot_size, verify ? "on" : "off");

    std::size_t converted = 0, skipped = 0, failed = 0;
    std::size_t total_rows = 0, total_out = 0, total_in = 0;
    std::vector<std::string> problems;
    const auto started = SteadyClock::now();

    for (std::size_t i = 0; i < archives.size(); ++i) {
        const fs::path& archive = archives[i];
        const auto t0 = SteadyClock::now();

        // The registry is per-day on purpose. A .vfday carries its own
        // instrument table, so nothing needs a shared registry across days, and
        // keeping them separate stops one bad archive from polluting the rest.
        InstrumentRegistry registry;

        GfdlDay day;
        try {
            day = load_gfdl_zip(registry, archive.string(), options);
        } catch (const std::exception& e) {
            problems.push_back(archive.filename().string() + ": " + e.what());
            ++failed;
            continue;
        }

        if (!day.session || day.rows_read == 0) {
            problems.push_back(archive.filename().string() + ": no readable rows");
            ++failed;
            continue;
        }

        const fs::path out = outdir / (day.date.to_string() + ".vfday");
        if (!force && fs::exists(out)) { ++skipped; continue; }

        VfdayStats stats;
        try {
            stats = write_vfday(*day.session, out.string(), verify);
        } catch (const std::exception& e) {
            problems.push_back(archive.filename().string() + ": " + e.what());
            ++failed;
            std::error_code ec;
            fs::remove(out, ec);   // never leave a half-written day behind
            continue;
        }

        const auto in_bytes = static_cast<double>(fs::file_size(archive));
        total_in += static_cast<std::size_t>(in_bytes);
        total_out += stats.bytes;
        total_rows += stats.rows;
        ++converted;

        std::printf("  [%3zu/%3zu] %s  %7zu rows  %6zu inst  %9s -> %9s  %.2f B/row  %5.1fs%s\n",
                    i + 1, archives.size(), day.date.to_string().c_str(), stats.rows,
                    stats.instruments, human(in_bytes).c_str(),
                    human(static_cast<double>(stats.bytes)).c_str(), stats.bytes_per_row,
                    seconds_since(t0), day.rows_skipped ? "  (rows skipped)" : "");
        std::fflush(stdout);

        for (const std::string& w : day.warnings) {
            problems.push_back(day.date.to_string() + ": " + w);
        }
    }

    const double elapsed = seconds_since(started);
    std::printf("\nconverted %zu, skipped %zu, failed %zu in %.1fs\n", converted, skipped,
                failed, elapsed);
    if (converted != 0) {
        std::printf("rows %zu   input %s   output %s   ratio %.1fx   %.2f bytes/row\n",
                    total_rows, human(static_cast<double>(total_in)).c_str(),
                    human(static_cast<double>(total_out)).c_str(),
                    static_cast<double>(total_in) / static_cast<double>(total_out),
                    static_cast<double>(total_out) / static_cast<double>(total_rows));
    }

    if (!problems.empty()) {
        std::printf("\n%zu problem(s):\n", problems.size());
        for (std::size_t i = 0; i < problems.size() && i < 40; ++i) {
            std::printf("  %s\n", problems[i].c_str());
        }
        if (problems.size() > 40) std::printf("  ... and %zu more\n", problems.size() - 40);
    }
    return failed == 0 ? 0 : 1;
}
