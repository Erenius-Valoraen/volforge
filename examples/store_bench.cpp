// Where does the time go, and what would a binary store actually save?
//
// Answers three questions with measurements rather than projections:
//   1. How much of the CSV load is I/O, how much is parsing, how much is the
//      ingest path (map lookups and vector growth)?
//   2. How fast is writing the decoded session to a flat columnar file?
//   3. How fast is reading it back?
//
// The binary format used here is deliberately the simplest thing that could
// work: on-disk layout identical to what QuoteColumns wants in memory, so
// reading is one sequential read and a pointer fix-up, with no parse at all.

#include "volforge/gfdl_csv.hpp"
#include "volforge/codec.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

using namespace volforge;
namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

namespace {

double seconds_since(SteadyClock::time_point t0) {
    return std::chrono::duration<double>(SteadyClock::now() - t0).count();
}

struct Header {
    char          magic[8];       // "VFDAY001"
    std::int32_t  date;
    std::int32_t  instrument_count;
    std::int64_t  row_count;
};

struct Entry {
    std::int32_t instrument;
    std::int32_t row_count;
    std::int64_t offset;          // bytes from the start of the payload
};

// One column block per instrument, laid out exactly as the engine consumes it.
constexpr std::size_t kBytesPerRow = sizeof(Timestamp) + 3 * sizeof(Price) +
                                     3 * sizeof(Qty) + sizeof(std::int64_t);

void write_binary(const SessionData& session, const std::string& path) {
    std::ofstream out(path, std::ios::binary);

    const auto instruments = session.instruments();
    Header h{};
    std::memcpy(h.magic, "VFDAY001", 8);
    h.date = session.date().yyyymmdd;
    h.instrument_count = static_cast<std::int32_t>(instruments.size());
    h.row_count = static_cast<std::int64_t>(session.total_observations());
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));

    std::vector<Entry> directory;
    directory.reserve(instruments.size());
    std::int64_t offset = 0;
    for (const InstrumentId id : instruments) {
        const auto cols = session.quotes(id);
        directory.push_back(Entry{index_of(id), static_cast<std::int32_t>(cols.size()), offset});
        offset += static_cast<std::int64_t>(cols.size() * kBytesPerRow);
    }
    out.write(reinterpret_cast<const char*>(directory.data()),
              static_cast<std::streamsize>(directory.size() * sizeof(Entry)));

    auto put = [&](const void* data, std::size_t bytes) {
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    };
    for (const InstrumentId id : instruments) {
        const auto c = session.quotes(id);
        const std::size_t n = c.size();
        put(c.ts.data(), n * sizeof(Timestamp));
        put(c.last.data(), n * sizeof(Price));
        put(c.bid.data(), n * sizeof(Price));
        put(c.ask.data(), n * sizeof(Price));
        put(c.bid_qty.data(), n * sizeof(Qty));
        put(c.ask_qty.data(), n * sizeof(Qty));
        put(c.last_qty.data(), n * sizeof(Qty));
        put(c.open_interest.data(), n * sizeof(std::int64_t));
    }
}

// Reads the whole file in one go and hands back spans into it. No parsing, no
// per-row work, no allocation beyond the single buffer.
struct BinaryDay {
    std::vector<char> blob;
    std::vector<Entry> directory;
    Header header{};
    std::size_t columns_wired = 0;
};

BinaryDay read_binary(const std::string& path) {
    BinaryDay day;

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    day.blob.resize(size);
    in.read(day.blob.data(), static_cast<std::streamsize>(size));

    std::memcpy(&day.header, day.blob.data(), sizeof(Header));
    day.directory.resize(static_cast<std::size_t>(day.header.instrument_count));
    std::memcpy(day.directory.data(), day.blob.data() + sizeof(Header),
                day.directory.size() * sizeof(Entry));

    // Wiring up the column spans is the entire "decode" step.
    const char* payload = day.blob.data() + sizeof(Header) +
                          day.directory.size() * sizeof(Entry);
    for (const Entry& e : day.directory) {
        const char* p = payload + e.offset;
        const std::size_t n = static_cast<std::size_t>(e.row_count);
        QuoteColumns cols{
            {reinterpret_cast<const Timestamp*>(p), n},
            {reinterpret_cast<const Price*>(p + n * 8), n},
            {reinterpret_cast<const Price*>(p + n * 12), n},
            {reinterpret_cast<const Price*>(p + n * 16), n},
            {reinterpret_cast<const Qty*>(p + n * 20), n},
            {reinterpret_cast<const Qty*>(p + n * 24), n},
            {reinterpret_cast<const Qty*>(p + n * 28), n},
            {reinterpret_cast<const std::int64_t*>(p + n * 32), n},
        };
        if (!cols.empty()) ++day.columns_wired;
    }
    return day;
}

// --- codec-compressed variant ------------------------------------------------

void write_codec(const SessionData& session, const std::string& path) {
    std::vector<std::uint8_t> blob;
    std::vector<std::int64_t> scratch;
    std::vector<Entry> directory;

    auto encode = [&](auto span) {
        scratch.clear();
        scratch.reserve(span.size());
        for (const auto& v : span) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, Timestamp>) {
                scratch.push_back(v.nanos);
            } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, Price>) {
                scratch.push_back(v.minor);
            } else {
                scratch.push_back(static_cast<std::int64_t>(v));
            }
        }
        encode_column(scratch.data(), scratch.size(), blob);
    };

    for (const InstrumentId id : session.instruments()) {
        const auto c = session.quotes(id);
        directory.push_back(Entry{index_of(id), static_cast<std::int32_t>(c.size()),
                                  static_cast<std::int64_t>(blob.size())});
        encode(c.ts); encode(c.last); encode(c.bid); encode(c.ask);
        encode(c.bid_qty); encode(c.ask_qty); encode(c.last_qty); encode(c.open_interest);
    }

    Header h{};
    std::memcpy(h.magic, "VFDAY002", 8);
    h.date = session.date().yyyymmdd;
    h.instrument_count = static_cast<std::int32_t>(directory.size());
    h.row_count = static_cast<std::int64_t>(session.total_observations());

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(directory.data()),
              static_cast<std::streamsize>(directory.size() * sizeof(Entry)));
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
}

std::size_t read_codec(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<char> raw(size);
    in.read(raw.data(), static_cast<std::streamsize>(size));

    Header h{};
    std::memcpy(&h, raw.data(), sizeof(Header));
    std::vector<Entry> directory(static_cast<std::size_t>(h.instrument_count));
    std::memcpy(directory.data(), raw.data() + sizeof(Header),
                directory.size() * sizeof(Entry));

    const auto* payload =
        reinterpret_cast<const std::uint8_t*>(raw.data()) + sizeof(Header) +
        directory.size() * sizeof(Entry);
    const std::size_t payload_size = size - sizeof(Header) - directory.size() * sizeof(Entry);

    std::size_t values = 0;
    std::vector<std::int64_t> column;
    for (const Entry& e : directory) {
        const std::uint8_t* p = payload + e.offset;
        std::size_t remaining = payload_size - static_cast<std::size_t>(e.offset);
        for (int col = 0; col < 8; ++col) {
            const std::size_t used = decode_column(p, remaining, column);
            if (used == 0) return values;   // corrupt
            p += used;
            remaining -= used;
            values += column.size();
        }
    }
    return values;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: store_bench <gfdl-day-directory> [out.vfday]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string bin = argc > 2 ? argv[2] : "day.vfday";

    // --- 1. raw I/O only -----------------------------------------------------
    std::size_t bytes = 0, files = 0;
    auto t0 = SteadyClock::now();
    {
        std::string buf;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (!e.is_regular_file() || e.path().extension() != ".csv") continue;
            std::ifstream in(e.path(), std::ios::binary);
            buf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            bytes += buf.size();
            ++files;
        }
    }
    const double io = seconds_since(t0);

    // --- 2. full CSV load ----------------------------------------------------
    InstrumentRegistry registry;
    t0 = SteadyClock::now();
    GfdlDay day = load_gfdl_day(registry, dir);
    const double csv = seconds_since(t0);
    if (!day.session) { std::printf("no session loaded\n"); return 1; }

    // --- 3. write and read the binary form -----------------------------------
    t0 = SteadyClock::now();
    write_binary(*day.session, bin);
    const double write = seconds_since(t0);

    t0 = SteadyClock::now();
    BinaryDay reread = read_binary(bin);
    const double read = seconds_since(t0);

    // --- 3b. codec-compressed form -------------------------------------------
    const std::string packed = bin + ".packed";
    t0 = SteadyClock::now();
    write_codec(*day.session, packed);
    const double cwrite = seconds_since(t0);

    t0 = SteadyClock::now();
    const std::size_t decoded_values = read_codec(packed);
    const double cread = seconds_since(t0);

    // --- 4. replay, for scale ------------------------------------------------
    t0 = SteadyClock::now();
    std::size_t walked = 0;
    {
        EventCursor cursor(*day.session);
        Event ev;
        while (cursor.next(ev)) ++walked;
    }
    const double replay = seconds_since(t0);

    const double mb = static_cast<double>(bytes) / 1e6;
    const double binmb = static_cast<double>(fs::file_size(bin)) / 1e6;

    std::printf("source        %6.1f MB across %zu files\n", mb, files);
    std::printf("rows          %zu\n\n", day.rows_read);

    std::printf("  read files only     %7.2f s   %6.0f MB/s\n", io, mb / io);
    std::printf("  full CSV load       %7.2f s   (parse + ingest = %.2f s)\n", csv, csv - io);
    std::printf("  write binary        %7.2f s   -> %.1f MB\n", write, binmb);
    std::printf("  read binary         %7.2f s   %6.0f MB/s\n", read, binmb / read);
    std::printf("  replay all events   %7.2f s   %zu events\n\n", replay, walked);

    const double cmb = static_cast<double>(fs::file_size(packed)) / 1e6;
    std::printf("  write packed        %7.2f s   -> %.1f MB  (%.2f bytes/row)\n",
                cwrite, cmb, cmb * 1e6 / static_cast<double>(day.rows_read));
    std::printf("  read packed         %7.2f s   %zu values decoded\n\n", cread,
                decoded_values);

    std::printf("  vs CSV        raw %5.1fx faster    packed %5.1fx faster\n",
                csv / read, csv / cread);
    std::printf("  size          CSV %6.0f MB    raw %6.0f MB    packed %6.1f MB\n",
                mb, binmb, cmb);
    std::printf("  197 sessions  CSV %5.1f min    raw %5.1f s / %4.1f GB    "
                "packed %5.1f s / %4.1f GB\n",
                csv * 197 / 60.0, read * 197, binmb * 197 / 1000.0,
                cread * 197, cmb * 197 / 1000.0);
    return 0;
}
