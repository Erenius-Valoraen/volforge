#include "volforge/vfday.hpp"

#include "volforge/codec.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace volforge {
namespace {

constexpr char kMagic[8] = {'V', 'F', 'D', 'A', 'Y', '0', '0', '3'};

#pragma pack(push, 1)
struct FileHeader {
    char          magic[8];
    std::uint32_t version;
    std::uint32_t flags;
    std::int32_t  date;
    std::int32_t  instrument_count;
    std::int64_t  total_rows;

    std::int64_t  table_offset;     std::uint32_t table_bytes;     std::uint32_t table_crc;
    std::int64_t  strings_offset;   std::uint32_t strings_bytes;   std::uint32_t strings_crc;
    std::int64_t  timeline_offset;  std::uint32_t timeline_count;  std::uint32_t timeline_crc;
    std::int64_t  directory_offset; std::uint32_t directory_bytes; std::uint32_t directory_crc;

    std::int64_t  file_bytes;       // whole file, so truncation is detectable
    std::uint32_t header_crc;       // over every byte preceding it
    std::uint32_t reserved;
};

struct TableEntry {
    std::uint32_t underlying_offset;
    std::uint32_t underlying_length;
    std::uint8_t  kind;
    std::uint8_t  right;
    std::uint16_t reserved;
    std::int32_t  expiry;
    std::int32_t  strike_minor;
    std::int32_t  lot_size;
    std::int32_t  tick_minor;
};

struct DirEntry {
    std::int32_t  table_index;
    std::uint32_t row_count;
    std::int64_t  blob_offset;
    std::uint32_t blob_bytes;
    std::uint32_t blob_crc;
    std::int64_t  first_ts;
    std::int64_t  last_ts;
};
#pragma pack(pop)

constexpr std::size_t kColumnsPerInstrument = 8;

[[noreturn]] void fail(const std::string& path, const std::string& why) {
    throw std::runtime_error("vfday: " + path + ": " + why);
}

const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t bytes, std::uint32_t seed) {
    const auto& table = crc_table();
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

namespace {

template <typename T, typename Fn>
void encode_into(std::vector<std::uint8_t>& blob, std::span<const T> values,
                 std::vector<std::int64_t>& scratch, Fn to_i64) {
    scratch.clear();
    scratch.reserve(values.size());
    for (const T& v : values) scratch.push_back(to_i64(v));
    encode_column(scratch.data(), scratch.size(), blob);
}

bool columns_equal(const QuoteColumns& a, const QuoteColumns& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a.ts[i] != b.ts[i] || a.last[i] != b.last[i] || a.bid[i] != b.bid[i] ||
            a.ask[i] != b.ask[i] || a.bid_qty[i] != b.bid_qty[i] ||
            a.ask_qty[i] != b.ask_qty[i] || a.last_qty[i] != b.last_qty[i] ||
            a.open_interest[i] != b.open_interest[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

VfdayStats write_vfday(const SessionData& session, const std::string& path, bool verify) {
    const auto instruments = session.instruments();

    std::vector<std::uint8_t> blob;
    std::vector<DirEntry>     directory;
    std::vector<TableEntry>   table;
    std::vector<char>         strings;
    std::vector<std::int64_t> scratch;
    std::map<std::string, std::uint32_t> interned;

    directory.reserve(instruments.size());
    table.reserve(instruments.size());

    for (const InstrumentId id : instruments) {
        const QuoteColumns c = session.quotes(id);
        if (c.empty()) continue;

        const InstrumentSpec& spec = session.registry().spec(id);
        const std::string name(session.registry().underlying_name(spec.underlying));

        auto it = interned.find(name);
        if (it == interned.end()) {
            const auto offset = static_cast<std::uint32_t>(strings.size());
            strings.insert(strings.end(), name.begin(), name.end());
            it = interned.emplace(name, offset).first;
        }

        table.push_back(TableEntry{it->second, static_cast<std::uint32_t>(name.size()),
                                   static_cast<std::uint8_t>(spec.kind),
                                   static_cast<std::uint8_t>(spec.right), 0,
                                   spec.expiry.yyyymmdd, spec.strike.minor, spec.lot_size,
                                   spec.tick_size.minor});

        // Absolute, because that is what the reader validates and seeks to.
        const auto blob_start = static_cast<std::int64_t>(sizeof(FileHeader) + blob.size());

        encode_into<Timestamp>(blob, c.ts, scratch, [](Timestamp t) { return t.nanos; });
        encode_into<Price>(blob, c.last, scratch, [](Price p) { return std::int64_t{p.minor}; });
        encode_into<Price>(blob, c.bid, scratch, [](Price p) { return std::int64_t{p.minor}; });
        encode_into<Price>(blob, c.ask, scratch, [](Price p) { return std::int64_t{p.minor}; });
        encode_into<Qty>(blob, c.bid_qty, scratch, [](Qty q) { return std::int64_t{q}; });
        encode_into<Qty>(blob, c.ask_qty, scratch, [](Qty q) { return std::int64_t{q}; });
        encode_into<Qty>(blob, c.last_qty, scratch, [](Qty q) { return std::int64_t{q}; });
        encode_into<std::int64_t>(blob, c.open_interest, scratch, [](std::int64_t v) { return v; });

        const auto blob_bytes = static_cast<std::uint32_t>(
            sizeof(FileHeader) + blob.size() - static_cast<std::size_t>(blob_start));
        directory.push_back(DirEntry{
            static_cast<std::int32_t>(table.size() - 1), static_cast<std::uint32_t>(c.size()),
            blob_start, blob_bytes,
            crc32(blob.data() + (static_cast<std::size_t>(blob_start) - sizeof(FileHeader)),
                  blob_bytes),
            c.ts.front().nanos, c.ts.back().nanos});
    }

    std::vector<std::int64_t> timeline;
    for (const Timestamp t : session.timeline()) timeline.push_back(t.nanos);
    if (timeline.empty()) {
        // Fall back to deriving it, so a source without a cached timeline still
        // produces a complete file.
        EventCursor cursor(session);
        Event ev;
        while (cursor.next(ev)) {
            if (timeline.empty() || timeline.back() != ev.ts.nanos) timeline.push_back(ev.ts.nanos);
        }
    }

    FileHeader h{};
    std::memcpy(h.magic, kMagic, 8);
    h.version = kVfdayVersion;
    h.date = session.date().yyyymmdd;
    h.instrument_count = static_cast<std::int32_t>(directory.size());
    h.total_rows = static_cast<std::int64_t>(session.total_observations());

    std::int64_t cursor_offset = static_cast<std::int64_t>(sizeof(FileHeader) + blob.size());

    h.table_offset = cursor_offset;
    h.table_bytes  = static_cast<std::uint32_t>(table.size() * sizeof(TableEntry));
    h.table_crc    = crc32(table.data(), h.table_bytes);
    cursor_offset += h.table_bytes;

    h.strings_offset = cursor_offset;
    h.strings_bytes  = static_cast<std::uint32_t>(strings.size());
    h.strings_crc    = crc32(strings.data(), h.strings_bytes);
    cursor_offset += h.strings_bytes;

    h.timeline_offset = cursor_offset;
    h.timeline_count  = static_cast<std::uint32_t>(timeline.size());
    h.timeline_crc    = crc32(timeline.data(), timeline.size() * sizeof(std::int64_t));
    cursor_offset += static_cast<std::int64_t>(timeline.size() * sizeof(std::int64_t));

    h.directory_offset = cursor_offset;
    h.directory_bytes  = static_cast<std::uint32_t>(directory.size() * sizeof(DirEntry));
    h.directory_crc    = crc32(directory.data(), h.directory_bytes);
    cursor_offset += h.directory_bytes;

    h.file_bytes = cursor_offset;
    h.header_crc = crc32(&h, offsetof(FileHeader, header_crc));

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) fail(path, "cannot open for writing");
        auto put = [&](const void* d, std::size_t n) {
            if (n != 0) out.write(static_cast<const char*>(d), static_cast<std::streamsize>(n));
        };
        put(&h, sizeof(h));
        put(blob.data(), blob.size());
        put(table.data(), h.table_bytes);
        put(strings.data(), h.strings_bytes);
        put(timeline.data(), timeline.size() * sizeof(std::int64_t));
        put(directory.data(), h.directory_bytes);
        out.flush();
        if (!out) fail(path, "write failed");
    }

    VfdayStats stats;
    stats.instruments = directory.size();
    stats.rows        = static_cast<std::size_t>(h.total_rows);
    stats.timestamps  = timeline.size();
    stats.bytes       = static_cast<std::size_t>(h.file_bytes);
    stats.bytes_per_row =
        stats.rows ? static_cast<double>(stats.bytes) / static_cast<double>(stats.rows) : 0.0;

    if (verify) {
        // Read the finished file back and compare every value against what went
        // in. A conversion that quietly mangled a column would otherwise not
        // surface until a backtest reported something odd, long after the
        // source was deleted.
        InstrumentRegistry check_registry;
        auto reread = VfdaySession::open(path, check_registry);

        if (reread->date() != session.date()) fail(path, "verify: date mismatch");
        if (reread->total_observations() != session.total_observations()) {
            fail(path, "verify: row count mismatch");
        }
        if (reread->instruments().size() != directory.size()) {
            fail(path, "verify: instrument count mismatch");
        }

        const auto original_timeline = session.timeline();
        if (!original_timeline.empty()) {
            if (reread->timeline().size() != original_timeline.size()) {
                fail(path, "verify: timeline length mismatch");
            }
            for (std::size_t i = 0; i < original_timeline.size(); ++i) {
                if (reread->timeline()[i] != original_timeline[i]) {
                    fail(path, "verify: timeline mismatch");
                }
            }
        }

        std::size_t checked = 0;
        for (const InstrumentId written : reread->instruments()) {
            const InstrumentSpec& spec = check_registry.spec(written);
            const std::string name(check_registry.underlying_name(spec.underlying));

            // Locate the same contract in the source by identity, never by id.
            InstrumentId original = InstrumentId::Invalid;
            for (const InstrumentId candidate : instruments) {
                const InstrumentSpec& s = session.registry().spec(candidate);
                if (s.kind == spec.kind && s.expiry == spec.expiry && s.strike == spec.strike &&
                    s.right == spec.right && s.lot_size == spec.lot_size &&
                    session.registry().underlying_name(s.underlying) == name) {
                    original = candidate;
                    break;
                }
            }
            if (!valid(original)) fail(path, "verify: instrument missing from source");
            if (!columns_equal(session.quotes(original), reread->quotes(written))) {
                fail(path, "verify: column data differs after round trip");
            }
            ++checked;
        }
        if (checked != directory.size()) fail(path, "verify: not every instrument was checked");
    }

    return stats;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

namespace {

std::vector<char> read_exact(std::ifstream& in, const std::string& path, std::int64_t offset,
                             std::size_t bytes, std::int64_t file_size, const char* what) {
    if (offset < 0 || bytes > static_cast<std::size_t>(file_size) ||
        offset + static_cast<std::int64_t>(bytes) > file_size) {
        fail(path, std::string(what) + ": region lies outside the file");
    }
    std::vector<char> buffer(bytes);
    in.seekg(offset);
    if (bytes != 0) in.read(buffer.data(), static_cast<std::streamsize>(bytes));
    if (!in) fail(path, std::string(what) + ": short read");
    return buffer;
}

}  // namespace

VfdaySession::~VfdaySession() {
    delete static_cast<std::ifstream*>(file_);
}

std::shared_ptr<VfdaySession> VfdaySession::open(const std::string& path,
                                                 InstrumentRegistry& registry) {
    namespace fs = std::filesystem;
    if (!fs::is_regular_file(path)) fail(path, "not a file");
    const auto file_size = static_cast<std::int64_t>(fs::file_size(path));

    std::ifstream in(path, std::ios::binary);
    if (!in) fail(path, "cannot open");

    if (file_size < static_cast<std::int64_t>(sizeof(FileHeader))) fail(path, "shorter than a header");

    FileHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in) fail(path, "short read on header");

    if (std::memcmp(h.magic, kMagic, 8) != 0) fail(path, "not a vfday file");
    if (h.version != kVfdayVersion) {
        fail(path, "version " + std::to_string(h.version) + ", expected " +
                       std::to_string(kVfdayVersion));
    }
    if (crc32(&h, offsetof(FileHeader, header_crc)) != h.header_crc) {
        fail(path, "header checksum mismatch");
    }
    if (h.file_bytes != file_size) {
        fail(path, "expected " + std::to_string(h.file_bytes) + " bytes, found " +
                       std::to_string(file_size) + " — truncated or appended to");
    }
    if (h.instrument_count < 0) fail(path, "negative instrument count");

    auto session = std::shared_ptr<VfdaySession>(new VfdaySession());
    session->path_ = path;
    session->date_ = Date{h.date};
    session->registry_ = &registry;
    session->total_rows_ = static_cast<std::size_t>(h.total_rows);

    const auto table_raw = read_exact(in, path, h.table_offset, h.table_bytes, file_size, "table");
    if (crc32(table_raw.data(), table_raw.size()) != h.table_crc) {
        fail(path, "instrument table checksum mismatch");
    }
    if (h.table_bytes % sizeof(TableEntry) != 0) fail(path, "instrument table is misaligned");

    const auto strings_raw =
        read_exact(in, path, h.strings_offset, h.strings_bytes, file_size, "strings");
    if (crc32(strings_raw.data(), strings_raw.size()) != h.strings_crc) {
        fail(path, "string table checksum mismatch");
    }

    const auto dir_raw =
        read_exact(in, path, h.directory_offset, h.directory_bytes, file_size, "directory");
    if (crc32(dir_raw.data(), dir_raw.size()) != h.directory_crc) {
        fail(path, "directory checksum mismatch");
    }
    if (h.directory_bytes % sizeof(DirEntry) != 0) fail(path, "directory is misaligned");

    const auto timeline_bytes = static_cast<std::size_t>(h.timeline_count) * sizeof(std::int64_t);
    const auto timeline_raw =
        read_exact(in, path, h.timeline_offset, timeline_bytes, file_size, "timeline");
    if (crc32(timeline_raw.data(), timeline_raw.size()) != h.timeline_crc) {
        fail(path, "timeline checksum mismatch");
    }

    const auto* table = reinterpret_cast<const TableEntry*>(table_raw.data());
    const std::size_t table_count = table_raw.size() / sizeof(TableEntry);
    const auto* dir = reinterpret_cast<const DirEntry*>(dir_raw.data());
    const std::size_t dir_count = dir_raw.size() / sizeof(DirEntry);

    if (dir_count != static_cast<std::size_t>(h.instrument_count)) {
        fail(path, "directory length disagrees with the header");
    }

    session->timeline_.reserve(h.timeline_count);
    {
        const auto* values = reinterpret_cast<const std::int64_t*>(timeline_raw.data());
        for (std::uint32_t i = 0; i < h.timeline_count; ++i) {
            if (i > 0 && values[i] <= values[i - 1]) fail(path, "timeline is not ascending");
            session->timeline_.push_back(Timestamp{values[i]});
        }
    }

    std::size_t rows_seen = 0;
    for (std::size_t i = 0; i < dir_count; ++i) {
        const DirEntry& e = dir[i];
        if (e.table_index < 0 || static_cast<std::size_t>(e.table_index) >= table_count) {
            fail(path, "directory references a missing instrument");
        }
        if (e.blob_offset < static_cast<std::int64_t>(sizeof(FileHeader)) ||
            e.blob_offset + static_cast<std::int64_t>(e.blob_bytes) > h.table_offset) {
            fail(path, "instrument data lies outside the payload");
        }
        if (e.last_ts < e.first_ts) fail(path, "instrument has a backwards time range");

        const TableEntry& t = table[static_cast<std::size_t>(e.table_index)];
        if (static_cast<std::size_t>(t.underlying_offset) + t.underlying_length >
            strings_raw.size()) {
            fail(path, "instrument name lies outside the string table");
        }

        InstrumentSpec spec;
        spec.underlying = registry.intern_underlying(
            std::string_view(strings_raw.data() + t.underlying_offset, t.underlying_length));
        spec.kind      = static_cast<InstrumentKind>(t.kind);
        spec.right     = static_cast<Right>(t.right);
        spec.expiry    = Date{t.expiry};
        spec.strike    = Price::from_minor(t.strike_minor);
        spec.lot_size  = t.lot_size;
        spec.tick_size = Price::from_minor(t.tick_minor);

        const InstrumentId id = registry.add(spec);
        if (session->entries_.count(index_of(id)) != 0) {
            fail(path, "the same contract appears twice");
        }
        session->entries_.emplace(index_of(id), Entry{e.table_index, e.row_count, e.blob_offset,
                                                      e.blob_bytes, e.blob_crc,
                                                      Timestamp{e.first_ts}, Timestamp{e.last_ts}});
        session->instruments_.push_back(id);
        rows_seen += e.row_count;
    }

    if (rows_seen != static_cast<std::size_t>(h.total_rows)) {
        fail(path, "directory row counts do not sum to the header total");
    }

    return session;
}

bool VfdaySession::printed_by(InstrumentId id, Timestamp t) const {
    const auto it = entries_.find(index_of(id));
    if (it == entries_.end()) return false;
    return it->second.row_count > 0 && it->second.first <= t;
}

const VfdaySession::Columns& VfdaySession::materialise(InstrumentId id) const {
    const auto cached = decoded_.find(index_of(id));
    if (cached != decoded_.end()) return cached->second;

    const auto it = entries_.find(index_of(id));
    if (it == entries_.end()) {
        static const Columns empty;
        return empty;
    }
    const Entry& e = it->second;

    if (file_ == nullptr) {
        auto* stream = new std::ifstream(path_, std::ios::binary);
        if (!*stream) { delete stream; fail(path_, "cannot reopen"); }
        file_ = stream;
    }
    auto& in = *static_cast<std::ifstream*>(file_);

    std::vector<std::uint8_t> raw(e.blob_bytes);
    in.seekg(e.blob_offset);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(e.blob_bytes));
    if (!in) fail(path_, "short read on instrument data");

    // Verified at the point of use, not merely at open. A block that rotted on
    // disk after the file was opened is still caught before anything reads it.
    if (crc32(raw.data(), raw.size()) != e.blob_crc) {
        fail(path_, "instrument data checksum mismatch — the file is corrupt");
    }

    Columns cols;
    std::vector<std::int64_t> values;
    const std::uint8_t* p = raw.data();
    std::size_t remaining = raw.size();

    auto next_column = [&]() -> const std::vector<std::int64_t>& {
        const std::size_t used = decode_column(p, remaining, values);
        if (used == 0) fail(path_, "instrument data could not be decoded");
        if (values.size() != e.row_count) fail(path_, "column length disagrees with the directory");
        p += used;
        remaining -= used;
        return values;
    };

    {
        const auto& v = next_column();
        cols.ts.reserve(v.size());
        for (const std::int64_t x : v) cols.ts.push_back(Timestamp{x});
    }
    for (std::vector<Price>* target : {&cols.last, &cols.bid, &cols.ask}) {
        const auto& v = next_column();
        target->reserve(v.size());
        for (const std::int64_t x : v) target->push_back(Price::from_minor(static_cast<std::int32_t>(x)));
    }
    for (std::vector<Qty>* target : {&cols.bid_qty, &cols.ask_qty, &cols.last_qty}) {
        const auto& v = next_column();
        target->reserve(v.size());
        for (const std::int64_t x : v) target->push_back(static_cast<Qty>(x));
    }
    {
        const auto& v = next_column();
        cols.open_interest.assign(v.begin(), v.end());
    }

    if (remaining != 0) fail(path_, "trailing bytes after the last column");
    if (!cols.ts.empty() && (cols.ts.front() != e.first || cols.ts.back() != e.last)) {
        fail(path_, "decoded time range disagrees with the directory");
    }

    return decoded_.emplace(index_of(id), std::move(cols)).first->second;
}

QuoteColumns VfdaySession::quotes(InstrumentId id) const {
    const Columns& c = materialise(id);
    if (c.ts.empty()) return QuoteColumns{};
    return QuoteColumns{c.ts, c.last, c.bid, c.ask,
                        c.bid_qty, c.ask_qty, c.last_qty, c.open_interest};
}

std::size_t VfdaySession::decoded_bytes() const {
    std::size_t total = 0;
    for (const auto& [id, c] : decoded_) {
        total += c.ts.size() * (sizeof(Timestamp) + 3 * sizeof(Price) + 3 * sizeof(Qty) +
                                sizeof(std::int64_t));
    }
    return total;
}

// ---------------------------------------------------------------------------
// Source
// ---------------------------------------------------------------------------

VfdaySource::VfdaySource(const std::string& directory, InstrumentRegistry& registry)
    : registry_(&registry) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(directory)) {
        problems_.push_back("not a directory: " + directory);
        return;
    }

    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vfday") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());

    for (const fs::path& p : paths) {
        // Opening validates the whole structure, so a damaged file is reported
        // here rather than part-way through a backtest.
        try {
            auto session = VfdaySession::open(p.string(), registry);
            const std::int32_t key = session->date().yyyymmdd;
            if (files_.count(key) != 0) {
                problems_.push_back("two files claim " + session->date().to_string() + ": " +
                                    files_[key] + " and " + p.string());
                continue;
            }
            files_.emplace(key, p.string());
            cache_.emplace(key, std::move(session));
        } catch (const std::exception& e) {
            problems_.emplace_back(e.what());
        }
    }
}

std::vector<Date> VfdaySource::sessions() const {
    std::vector<Date> out;
    out.reserve(files_.size());
    for (const auto& [key, path] : files_) out.push_back(Date{key});
    return out;
}

std::shared_ptr<SessionData> VfdaySource::load(Date d) {
    const auto cached = cache_.find(d.yyyymmdd);
    if (cached != cache_.end()) return cached->second;

    const auto it = files_.find(d.yyyymmdd);
    if (it == files_.end()) return nullptr;

    auto session = VfdaySession::open(it->second, *registry_);
    cache_.emplace(d.yyyymmdd, session);
    return session;
}

}  // namespace volforge
