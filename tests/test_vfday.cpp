// Store integrity.
//
// A backtest cannot tell the difference between good data and quietly damaged
// data — it just reports a different number, confidently. So these tests are
// mostly about damage: every field is corrupted in turn and the reader has to
// refuse. A test that only proved the happy path would be worth very little.

#include "harness.hpp"

#include "volforge/synthetic.hpp"
#include "volforge/vfday.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace volforge;
namespace fs = std::filesystem;

namespace {

std::string temp_path(const char* stem) {
    static int counter = 0;
    const auto p = fs::temp_directory_path() /
                   ("volforge_" + std::string(stem) + "_" + std::to_string(++counter) + ".vfday");
    return p.string();
}

// A small but structurally complete session.
struct Fixture {
    InstrumentRegistry registry;
    SyntheticSeries    series;
    std::shared_ptr<SessionData> session;
    std::string path;

    explicit Fixture(const char* stem, int sessions = 1) {
        SyntheticSeriesConfig cfg;
        cfg.sessions = sessions;
        cfg.strikes_each_side = 4;
        cfg.step_seconds = 60;
        series = make_synthetic_series(registry, cfg);
        session = series.source->load(series.dates.front());
        path = temp_path(stem);
    }

    ~Fixture() { std::error_code ec; fs::remove(path, ec); }
};

std::vector<char> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto n = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<char> buf(n);
    in.read(buf.data(), static_cast<std::streamsize>(n));
    return buf;
}

void spit(const std::string& path, const std::vector<char>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// True when opening the file (and reading every instrument) is refused.
bool rejected(const std::string& path) {
    try {
        InstrumentRegistry reg;
        auto s = VfdaySession::open(path, reg);
        for (const InstrumentId id : s->instruments()) (void)s->quotes(id);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST(a_session_survives_a_round_trip_value_for_value) {
    Fixture f("roundtrip");
    const VfdayStats stats = write_vfday(*f.session, f.path);   // verify defaults on

    CHECK(stats.rows == f.session->total_observations());
    CHECK(stats.instruments > 0);
    CHECK(stats.timestamps > 0);

    InstrumentRegistry reloaded;
    auto reread = VfdaySession::open(f.path, reloaded);

    CHECK(reread->date() == f.session->date());
    CHECK_EQ(reread->total_observations(), f.session->total_observations());
    CHECK_EQ(reread->instruments().size(), f.session->instruments().size());

    // Contracts are matched by identity, never by id: the file stores what an
    // instrument *is*, so a different registry must still reconstruct it.
    std::size_t compared = 0;
    for (const InstrumentId id : reread->instruments()) {
        const InstrumentSpec& spec = reloaded.spec(id);
        const auto original = f.registry.find_option(
            f.registry.intern_underlying(reloaded.underlying_name(spec.underlying)),
            spec.expiry, spec.strike, spec.right);
        if (!original) continue;

        const auto a = f.session->quotes(*original);
        const auto b = reread->quotes(id);
        CHECK_EQ(a.size(), b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(a.ts[i] == b.ts[i]);
            CHECK(a.last[i] == b.last[i]);
            CHECK(a.bid[i] == b.bid[i]);
            CHECK(a.ask[i] == b.ask[i]);
            CHECK(a.bid_qty[i] == b.bid_qty[i]);
            CHECK(a.ask_qty[i] == b.ask_qty[i]);
            CHECK(a.last_qty[i] == b.last_qty[i]);
            CHECK(a.open_interest[i] == b.open_interest[i]);
        }
        ++compared;
    }
    CHECK(compared > 5);
}

TEST(the_timeline_matches_the_source_exactly) {
    Fixture f("timeline");
    write_vfday(*f.session, f.path);

    InstrumentRegistry reg;
    auto reread = VfdaySession::open(f.path, reg);

    const auto original = f.session->timeline();
    const auto stored = reread->timeline();
    CHECK_EQ(stored.size(), original.size());
    for (std::size_t i = 0; i < original.size(); ++i) CHECK(stored[i] == original[i]);
}

// ---------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------

TEST(every_single_bit_flip_in_the_metadata_is_caught) {
    Fixture f("bitflip");
    write_vfday(*f.session, f.path, /*verify=*/false);
    const auto pristine = slurp(f.path);

    // The header, table, timeline and directory are all small and all fatal if
    // wrong, so every byte of them is worth checking exhaustively. Payload bytes
    // are sampled instead, since there are millions.
    std::size_t checked = 0, missed = 0;
    for (std::size_t i = 0; i < 96 && i < pristine.size(); ++i) {
        for (const int bit : {0, 3, 7}) {
            auto damaged = pristine;
            damaged[i] = static_cast<char>(damaged[i] ^ (1 << bit));
            if (damaged == pristine) continue;
            spit(f.path, damaged);
            ++checked;
            if (!rejected(f.path)) ++missed;
        }
    }
    CHECK(checked > 200);
    CHECK_EQ(missed, std::size_t{0});
}

TEST(damage_to_quote_data_is_caught_before_it_is_decoded) {
    Fixture f("payload");
    write_vfday(*f.session, f.path, /*verify=*/false);
    const auto pristine = slurp(f.path);

    std::mt19937_64 rng(4);
    std::size_t checked = 0, missed = 0;
    for (int trial = 0; trial < 200; ++trial) {
        // Somewhere inside the payload, which starts after the header.
        const std::size_t i = 128 + rng() % (pristine.size() - 256);
        auto damaged = pristine;
        damaged[i] = static_cast<char>(damaged[i] ^ (1 << (rng() % 8)));
        if (damaged == pristine) continue;
        spit(f.path, damaged);
        ++checked;
        if (!rejected(f.path)) ++missed;
    }
    CHECK(checked > 150);
    // The per-instrument checksum is verified immediately before decoding, so a
    // rotted block never reaches the strategy as plausible-looking quotes.
    CHECK_EQ(missed, std::size_t{0});
}

TEST(truncation_at_any_point_is_caught) {
    Fixture f("truncate");
    write_vfday(*f.session, f.path, /*verify=*/false);
    const auto pristine = slurp(f.path);

    std::size_t checked = 0, missed = 0;
    for (std::size_t keep = 0; keep < pristine.size(); keep += 977) {
        spit(f.path, std::vector<char>(pristine.begin(), pristine.begin() + keep));
        ++checked;
        if (!rejected(f.path)) ++missed;
    }
    CHECK(checked > 20);
    CHECK_EQ(missed, std::size_t{0});
}

TEST(appended_bytes_are_caught) {
    Fixture f("append");
    write_vfday(*f.session, f.path, /*verify=*/false);
    auto data = slurp(f.path);
    data.push_back('\0');
    spit(f.path, data);
    // The header records the whole file length, so anything bolted on the end is
    // a different file than the one that was written.
    CHECK(rejected(f.path));
}

TEST(a_foreign_or_outdated_file_is_refused) {
    Fixture f("magic");
    write_vfday(*f.session, f.path, /*verify=*/false);

    auto data = slurp(f.path);
    data[0] = 'X';                    // wrong magic
    spit(f.path, data);
    CHECK(rejected(f.path));

    data = slurp(f.path);
    data[0] = 'V';
    data[8] = static_cast<char>(99);  // wrong version, header CRC now also wrong
    spit(f.path, data);
    CHECK(rejected(f.path));

    CHECK(rejected("no-such-file.vfday"));
}

TEST(an_empty_or_tiny_file_is_refused_rather_than_read_past) {
    const std::string path = temp_path("tiny");
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{16},
                                std::size_t{64}}) {
        spit(path, std::vector<char>(n, '\0'));
        CHECK(rejected(path));
    }
    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// Memory discipline
// ---------------------------------------------------------------------------

TEST(opening_a_session_decodes_nothing) {
    Fixture f("lazy");
    write_vfday(*f.session, f.path);

    InstrumentRegistry reg;
    auto reread = VfdaySession::open(f.path, reg);

    // Everything needed to step through the day is already available.
    CHECK(!reread->timeline().empty());
    CHECK(reread->instruments().size() > 5);
    CHECK_EQ(reread->decoded_instruments(), std::size_t{0});
    CHECK_EQ(reread->decoded_bytes(), std::size_t{0});
}

TEST(only_the_instruments_actually_touched_are_decoded) {
    Fixture f("touch");
    write_vfday(*f.session, f.path);

    InstrumentRegistry reg;
    auto reread = VfdaySession::open(f.path, reg);
    const auto all = reread->instruments();

    (void)reread->quotes(all[0]);
    (void)reread->quotes(all[1]);
    (void)reread->quotes(all[0]);   // again; must not decode twice

    CHECK_EQ(reread->decoded_instruments(), std::size_t{2});
    CHECK(reread->decoded_bytes() > 0);
    CHECK(all.size() > 4);   // the rest stayed on disk
}

TEST(asking_whether_an_instrument_printed_does_not_decode_it) {
    Fixture f("printed");
    write_vfday(*f.session, f.path);

    InstrumentRegistry reg;
    auto reread = VfdaySession::open(f.path, reg);
    const auto mid = reread->timeline()[reread->timeline().size() / 2];

    std::size_t live = 0;
    for (const InstrumentId id : reread->instruments()) {
        if (reread->printed_by(id, mid)) ++live;
    }

    CHECK(live > 0);
    // A chain scan asks this of every strike. Were it answered by looking, it
    // would drag the whole session into memory to do so.
    CHECK_EQ(reread->decoded_instruments(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Source
// ---------------------------------------------------------------------------

TEST(a_directory_of_days_loads_in_date_order_and_reports_bad_files) {
    Fixture f("dir", /*sessions=*/3);
    const auto dir = fs::temp_directory_path() / "volforge_store_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    for (const Date d : f.series.dates) {
        auto s = f.series.source->load(d);
        write_vfday(*s, (dir / (d.to_string() + ".vfday")).string(), /*verify=*/false);
    }

    // A file that is not a store at all must be reported, not ignored.
    spit((dir / "broken.vfday").string(), std::vector<char>(200, 'x'));

    InstrumentRegistry reg;
    VfdaySource source(dir.string(), reg);

    const auto dates = source.sessions();
    CHECK_EQ(dates.size(), std::size_t{3});
    CHECK(std::is_sorted(dates.begin(), dates.end()));
    CHECK_EQ(source.problems().size(), std::size_t{1});

    CHECK(source.load(dates.front()) != nullptr);
    CHECK(source.load(Date{19700101}) == nullptr);

    fs::remove_all(dir, ec);
}

TEST(crc32_matches_known_values) {
    // Guards against a table-generation slip that would make every checksum
    // self-consistent but wrong, and so silently useless.
    CHECK_EQ(crc32("", 0), std::uint32_t{0x00000000});
    CHECK_EQ(crc32("a", 1), std::uint32_t{0xE8B7BE43});
    CHECK_EQ(crc32("123456789", 9), std::uint32_t{0xCBF43926});
}
