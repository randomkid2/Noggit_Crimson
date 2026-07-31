// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_test_macros.hpp>

#include <noggit/database/ChangesetArchive.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <locale>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Noggit::Database;

namespace
{
  // A directory of its own per test case, removed on the way out.
  //
  // The name is randomised and claimed with create_directory rather than derived from the test
  // name, because tests/CMakeLists.txt registers this suite several times over -- once per tag
  // plus a whole-suite run -- and ctest may run those processes concurrently. Two processes
  // executing the same TEST_CASE against one shared directory is a flake that only appears
  // under -j, which is the worst kind to chase.
  class TempDirectory
  {
    public:
      TempDirectory()
      {
        std::random_device entropy;

        for (int attempt (0); attempt < 64; ++attempt)
        {
          std::filesystem::path candidate
            ( std::filesystem::temp_directory_path()
            / ("noggit-archive-test-" + std::to_string(entropy()) + "-"
               + std::to_string(entropy()))
            );

          std::error_code ec;

          // create_directory answers false when the name is already taken, which is exactly the
          // atomic claim wanted here -- exists() followed by create would race.
          if (std::filesystem::create_directory(candidate, ec))
          {
            _path = std::move(candidate);
            return;
          }
        }

        throw std::runtime_error("could not claim a unique temporary directory for the test");
      }

      ~TempDirectory()
      {
        // Error code, never an exception: a destructor running during a failed assertion must
        // not replace the failure with a terminate.
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
      }

      TempDirectory(TempDirectory const&) = delete;
      TempDirectory& operator=(TempDirectory const&) = delete;

      std::filesystem::path const& path() const { return _path; }

      // A path under the temp directory that does not exist yet.
      std::filesystem::path child(std::string const& name) const { return _path / name; }

    private:
      std::filesystem::path _path;
  };

  // 2026-08-01T16:00:00Z. Verified against DateTimeOffset.FromUnixTimeSeconds.
  constexpr std::uint64_t STAMP_2026 = 1785600000ull;

  // 9999-12-31T23:59:59Z, the last instant the fixed-width name can hold.
  constexpr std::uint64_t STAMP_MAX = 253402300799ull;

  bool contains(std::string const& haystack, std::string const& needle)
  {
    return haystack.find(needle) != std::string::npos;
  }

  // Written outside the archive's API, so a test can put something in the root that
  // ChangesetArchive did not create.
  void writeRaw(std::filesystem::path const& path, std::string const& content)
  {
    std::ofstream file (path, std::ios::out | std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(file));
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    REQUIRE(static_cast<bool>(file));
  }

  std::size_t countFilesIn(std::filesystem::path const& directory)
  {
    std::size_t count (0);

    for (std::filesystem::directory_entry const& entry
         : std::filesystem::directory_iterator(directory))
    {
      if (entry.is_regular_file())
      {
        ++count;
      }
    }

    return count;
  }

  // Every byte that a shell, a glob, a Windows API or a path parser would read as something
  // other than a letter of a file name. Asserted as one predicate rather than one check per
  // character so a failure names the offending file rather than one of its bytes.
  bool isFilesystemSafe(std::string const& name)
  {
    for (char const c : name)
    {
      unsigned char const byte (static_cast<unsigned char>(c));

      bool const unsafe
        (  byte == '/' || byte == '\\' || byte == ':'
        || byte == '*' || byte == '?' || byte == '<' || byte == '>' || byte == '|'
        || byte == '"' || byte == ' '
        || byte < 0x20 || byte >= 0x80
        );

      if (unsafe)
      {
        return false;
      }
    }

    return !name.empty();
  }

  // Installs a global locale for the duration of one test case and puts the old one back, even
  // if an assertion throws. Catch2 runs every case in one process, so a leaked global locale
  // would silently change the meaning of whichever cases happen to run afterwards.
  class GlobalLocale
  {
    public:
      explicit GlobalLocale(std::locale const& locale)
        : _previous (std::locale::global(locale)) {}

      ~GlobalLocale()
      {
        std::locale::global(_previous);
      }

      GlobalLocale(GlobalLocale const&) = delete;
      GlobalLocale& operator=(GlobalLocale const&) = delete;

    private:
      std::locale _previous;
  };

  std::vector<std::filesystem::path> fileNamesOf(std::vector<ArchiveEntry> const& entries)
  {
    std::vector<std::filesystem::path> names;

    for (ArchiveEntry const& entry : entries)
    {
      names.push_back(entry.file_name);
    }

    return names;
  }
}

TEST_CASE("a stored changeset reloads byte for byte", "[archive][recovery]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  // Every byte here is one a real changeset carries and a naive implementation mangles: bare
  // newlines that Windows text mode would expand to CRLF, an already-CRLF line that the same
  // mode would double, a UTF-8 comment, and a NUL. The last one is not something the builder
  // emits, but it is the cheapest proof that the payload is treated as bytes and not as a
  // C string.
  std::string const sql
    ( std::string("-- Noggit changeset\n")
    + "-- Ren\xC3\xA9 placed a spawn \xE2\x80\x94 tile (49,31)\n"
    + "SET @CGUID := 200000;\r\n"
    + "DELETE FROM `creature` WHERE `guid` = @CGUID;\n"
    + std::string("\0", 1)
    + "\n"
    );

  std::filesystem::path const written (archive.store(sql, "elwynn", STAMP_2026));

  REQUIRE(std::filesystem::exists(written));
  CHECK(written.parent_path() == temp.path());

  // The size on disk is the assertion that actually pins down binary mode. A text-mode write
  // followed by a text-mode read can still round-trip in memory on Windows while the file on
  // disk holds three extra bytes -- which is what a reviewer or a mysql client would then see.
  CHECK(std::filesystem::file_size(written) == static_cast<std::uintmax_t>(sql.size()));

  std::string const loaded (archive.load(written.filename().string()));

  CHECK(loaded.size() == sql.size());
  CHECK(loaded == sql);
}

TEST_CASE("an empty changeset is stored and reloads as empty", "[archive][recovery]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  // Zero bytes is a legitimate payload and the classic place a stream-based reader breaks:
  // `buffer << file.rdbuf()` sets failbit when it inserts nothing, which would turn an empty
  // changeset into a phantom I/O failure.
  std::filesystem::path const written (archive.store(std::string(), "nothing", STAMP_2026));

  CHECK(std::filesystem::file_size(written) == 0u);
  CHECK(archive.load(written.filename().string()).empty());

  std::vector<ArchiveEntry> const entries (archive.list());
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].size_bytes == 0u);
}

TEST_CASE("file names carry the stamp, the label and the sequence", "[archive]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  CHECK(archive.store("a", "elwynn", STAMP_2026).filename().string()
        == "20260801-160000-000-elwynn.sql");

  // The epoch and the last representable second, so the fixed-width fields are pinned at both
  // ends rather than only in the middle of the range.
  CHECK(archive.store("b", "epoch", 0).filename().string()
        == "19700101-000000-000-epoch.sql");
  CHECK(archive.store("c", "distant", STAMP_MAX).filename().string()
        == "99991231-235959-000-distant.sql");

  // A leap day, which is where a hand-rolled calendar conversion goes wrong first.
  CHECK(archive.store("d", "leap", 1709164800ull).filename().string()
        == "20240229-000000-000-leap.sql");
}

TEST_CASE("names sort chronologically as text", "[archive]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  // Deliberately stored out of order, and with labels whose alphabetical order is the reverse of
  // their chronological order, so a sort that leaned on the label would be caught.
  std::vector<std::uint64_t> const timestamps
    {1785600000ull, 0ull, 946684800ull, 1ull, 86400ull, 86399ull, STAMP_MAX, 1709164800ull};

  std::vector<std::pair<std::uint64_t, std::string>> stored;

  for (std::size_t i (0); i < timestamps.size(); ++i)
  {
    std::string const label (1, static_cast<char>('z' - static_cast<char>(i)));
    stored.emplace_back
      (timestamps[i], archive.store("payload", label, timestamps[i]).filename().string());
  }

  std::sort(stored.begin(), stored.end());

  // Sorted by timestamp above; the names must now be in ascending text order too. This is the
  // property the whole naming scheme exists to provide, and it is why the fields are zero
  // padded and fixed width.
  for (std::size_t i (1); i < stored.size(); ++i)
  {
    CHECK(stored[i - 1].second < stored[i].second);
  }
}

TEST_CASE("list reports newest first with the right sizes and timestamps", "[archive]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  archive.store("one", "oldest", 1000);
  archive.store("two-two", "middle", 2000);
  archive.store("three-three-three", "newest", 3000);

  std::vector<ArchiveEntry> const entries (archive.list());

  REQUIRE(entries.size() == 3u);

  CHECK(entries[0].label == "newest");
  CHECK(entries[0].timestamp == 3000u);
  CHECK(entries[0].size_bytes == 17u);

  CHECK(entries[1].label == "middle");
  CHECK(entries[1].timestamp == 2000u);
  CHECK(entries[1].size_bytes == 7u);

  CHECK(entries[2].label == "oldest");
  CHECK(entries[2].timestamp == 1000u);
  CHECK(entries[2].size_bytes == 3u);

  // The reported timestamp is decoded from the name, not read from the filesystem, so it must
  // be exactly what store() was given -- an mtime would drift the moment the file is copied.
  for (ArchiveEntry const& entry : entries)
  {
    CHECK(archive.load(entry.file_name).size() == entry.size_bytes);
  }
}

TEST_CASE("two stores in the same second do not collide", "[archive][recovery]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  std::filesystem::path const first (archive.store("first", "tile_49_31", STAMP_2026));
  std::filesystem::path const second (archive.store("second", "tile_49_31", STAMP_2026));

  CHECK(first != second);
  CHECK(first.filename().string() == "20260801-160000-000-tile_49_31.sql");
  CHECK(second.filename().string() == "20260801-160000-001-tile_49_31.sql");

  // Both payloads survive: the second store must never have overwritten the first, which is the
  // whole reason the sequence field exists.
  CHECK(archive.load(first.filename().string()) == "first");
  CHECK(archive.load(second.filename().string()) == "second");

  // A third store in the same second with a DIFFERENT label still takes the next sequence
  // number, so text order stays store order within the second rather than becoming label order.
  std::filesystem::path const third (archive.store("third", "aaa", STAMP_2026));
  CHECK(third.filename().string() == "20260801-160000-002-aaa.sql");
  CHECK(second.filename().string() < third.filename().string());

  std::vector<std::filesystem::path> const names (fileNamesOf(archive.list()));
  REQUIRE(names.size() == 3u);
  CHECK(names[0] == third.filename());
  CHECK(names[1] == second.filename());
  CHECK(names[2] == first.filename());
}

TEST_CASE("a hostile label cannot escape the root", "[archive][safety]")
{
  TempDirectory const temp;

  // The archive is a subdirectory, so an escape by one level lands somewhere observable rather
  // than in the system temp directory where it could not be told apart from another test's mess.
  std::filesystem::path const root (temp.child("archive"));
  ChangesetArchive archive (root);

  std::vector<std::string> const hostile
    { "../../evil"
    , "..\\..\\evil"
    , ".."
    , "."
    , "/etc/passwd"
    , "C:\\Windows\\System32\\evil"
    , "C:evil"
    , "tile:49:31"
    , "*.sql"
    , "wild?card"
    , "<redirect>"
    , "pipe|it"
    , "quote\"it"
    , "a\tb   c\nd"
    , "\x01\x02\x03"
    , ""
    , "___"
    , "trailing."
    , "\xD0\x98\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD"     // UTF-8, non-ASCII throughout
    , std::string(300, 'a')
    };

  std::size_t stored (0);

  for (std::string const& label : hostile)
  {
    std::filesystem::path const written (archive.store("payload", label, STAMP_2026));
    ++stored;

    // Landed directly in the root, under a name that is a single component.
    CHECK(written.parent_path() == root);
    CHECK(std::filesystem::exists(written));

    std::string const name (written.filename().string());
    CAPTURE(label, name);

    CHECK(isFilesystemSafe(name));
    CHECK_FALSE(contains(name, ".."));

    // Round-trips through the API it just came out of, which is what makes the sanitised name
    // usable rather than merely safe.
    CHECK(archive.load(name) == "payload");
  }

  // Nothing landed anywhere but the root: the parent holds the archive directory and nothing
  // else, and no obvious escape target was created beside it.
  CHECK(countFilesIn(root) == stored);
  CHECK(countFilesIn(temp.path()) == 0u);
  CHECK_FALSE(std::filesystem::exists(temp.child("evil")));
  CHECK_FALSE(std::filesystem::exists(temp.child("evil.sql")));
  CHECK_FALSE(std::filesystem::exists(temp.child("passwd")));
}

TEST_CASE("label sanitisation is predictable", "[archive][safety]")
{
  // Asserted directly as well as through store(), because this function is half the security
  // boundary of the class and "the file landed inside the root" would still pass if it were
  // mapping every label to the same name.
  CHECK(ChangesetArchive::sanitiseLabel("elwynn") == "elwynn");
  CHECK(ChangesetArchive::sanitiseLabel("tile_49_31") == "tile_49_31");
  CHECK(ChangesetArchive::sanitiseLabel("v1.2-rc3") == "v1.2-rc3");

  // Traversal, separators and drive letters lose their meaning; the readable part survives.
  CHECK(ChangesetArchive::sanitiseLabel("../../evil") == "evil");
  CHECK(ChangesetArchive::sanitiseLabel("/etc/passwd") == "etc_passwd");
  CHECK(ChangesetArchive::sanitiseLabel("C:\\Windows\\evil") == "C_Windows_evil");
  CHECK(ChangesetArchive::sanitiseLabel("tile:49:31") == "tile_49_31");

  // Whitespace runs collapse to one underscore rather than to several.
  CHECK(ChangesetArchive::sanitiseLabel("tile  49 \t 31") == "tile_49_31");
  CHECK(ChangesetArchive::sanitiseLabel("  spaced out  ") == "spaced_out");

  // Nothing usable left, so a default rather than an exception -- and never an empty label,
  // which would produce a name the parser rejects and list() would then not report.
  CHECK(ChangesetArchive::sanitiseLabel("") == "changeset");
  CHECK(ChangesetArchive::sanitiseLabel("..") == "changeset");
  CHECK(ChangesetArchive::sanitiseLabel("///") == "changeset");
  CHECK(ChangesetArchive::sanitiseLabel("\xD0\x98\xD0\xB7") == "changeset");

  // Capped, and the cap is applied to the sanitised form.
  CHECK(ChangesetArchive::sanitiseLabel(std::string(300, 'a'))
        == std::string(ChangesetArchive::MAX_LABEL_LENGTH, 'a'));
  CHECK(ChangesetArchive::sanitiseLabel(std::string(300, '/')) == "changeset");

  // Truncation must not leave a separator dangling at the end, where Windows would strip it and
  // the name on disk would stop matching the one store() returned.
  std::string const long_with_separator
    (std::string(ChangesetArchive::MAX_LABEL_LENGTH - 1, 'a') + "_bbbbb");
  std::string const capped (ChangesetArchive::sanitiseLabel(long_with_separator));
  CHECK(capped == std::string(ChangesetArchive::MAX_LABEL_LENGTH - 1, 'a'));
  CHECK(capped.back() != '_');
}

TEST_CASE("load refuses a name that leaves the root", "[archive][safety]")
{
  TempDirectory const temp;

  std::filesystem::path const root (temp.child("archive"));
  ChangesetArchive archive (root);
  archive.store("inside", "kept", STAMP_2026);

  // A real file to reach for. Without it, a passing test would only prove that a nonexistent
  // path is nonexistent -- it would not prove the guard rejected the name.
  writeRaw(temp.child("secret.sql"), "-- not yours\n");
  REQUIRE(std::filesystem::exists(temp.child("secret.sql")));

  CHECK_THROWS_AS(archive.load("../secret.sql"), ArchiveError);
  CHECK_THROWS_AS(archive.load("..\\secret.sql"), ArchiveError);
  CHECK_THROWS_AS(archive.load("../../secret.sql"), ArchiveError);
  CHECK_THROWS_AS(archive.load(".."), ArchiveError);
  CHECK_THROWS_AS(archive.load("."), ArchiveError);
  CHECK_THROWS_AS(archive.load(""), ArchiveError);
  CHECK_THROWS_AS(archive.load("sub/inside.sql"), ArchiveError);
  CHECK_THROWS_AS(archive.load("sub\\inside.sql"), ArchiveError);
  CHECK_THROWS_AS(archive.load("C:\\Windows\\win.ini"), ArchiveError);
  CHECK_THROWS_AS(archive.load("C:secret.sql"), ArchiveError);
  CHECK_THROWS_AS(archive.load("/etc/passwd"), ArchiveError);
  CHECK_THROWS_AS(archive.load(std::string("name\0hidden.sql", 15)), ArchiveError);

  CHECK_THROWS_AS(archive.load("20260801-160000-000-absent.sql"), ArchiveError);
  CHECK_THROWS_AS(archive.load("absent.sql"), ArchiveError);

  // The refusals did not damage or move anything.
  CHECK(std::filesystem::exists(temp.child("secret.sql")));
  CHECK(archive.list().size() == 1u);
}

TEST_CASE("prune keeps exactly the newest N", "[archive][recovery]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  for (std::uint64_t i (0); i < 5; ++i)
  {
    archive.store("payload " + std::to_string(i), "step" + std::to_string(i), 1000 + i);
  }

  std::vector<std::filesystem::path> const before (fileNamesOf(archive.list()));
  REQUIRE(before.size() == 5u);

  CHECK(archive.prune(2) == 3u);

  std::vector<std::filesystem::path> const after (fileNamesOf(archive.list()));
  REQUIRE(after.size() == 2u);

  // The two that survived are the two newest, in the same order list() reported them before.
  CHECK(after[0] == before[0]);
  CHECK(after[1] == before[1]);

  // And they are still readable -- pruning must not corrupt what it keeps.
  CHECK(archive.load(after[0]) == "payload 4");
  CHECK(archive.load(after[1]) == "payload 3");

  // Asking to keep more than exist removes nothing and is not an error.
  CHECK(archive.prune(2) == 0u);
  CHECK(archive.prune(10) == 0u);
  CHECK(archive.list().size() == 2u);

  // keep_newest == 0 empties the archive.
  CHECK(archive.prune(0) == 2u);
  CHECK(archive.list().empty());

  // Pruning an empty archive is a no-op, not a failure.
  CHECK(archive.prune(0) == 0u);
  CHECK(archive.prune(3) == 0u);
}

TEST_CASE("prune leaves files the archive did not write", "[archive][safety]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  archive.store("mine", "mine", STAMP_2026);

  // Everything here is in the archive root but is not something store() produced: a hand-written
  // changeset, a note, a name that looks like ours but holds an impossible date, and a
  // subdirectory with a perfectly-formed name inside it.
  writeRaw(temp.child("handmade.sql"), "-- by hand\n");
  writeRaw(temp.child("notes.txt"), "remember\n");
  writeRaw(temp.child("20260231-160000-000-nodate.sql"), "-- 31st of February\n");
  writeRaw(temp.child("20260801-240000-000-nohour.sql"), "-- 24:00\n");

  REQUIRE(std::filesystem::create_directory(temp.child("nested")));
  writeRaw(temp.child("nested") / "20260801-160000-000-nested.sql", "-- not in the root\n");

  // list() reports only its own, so the flat scan and the parser are both doing their job.
  std::vector<ArchiveEntry> const entries (archive.list());
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].label == "mine");

  CHECK(archive.prune(0) == 1u);

  CHECK(std::filesystem::exists(temp.child("handmade.sql")));
  CHECK(std::filesystem::exists(temp.child("notes.txt")));
  CHECK(std::filesystem::exists(temp.child("20260231-160000-000-nodate.sql")));
  CHECK(std::filesystem::exists(temp.child("20260801-240000-000-nohour.sql")));
  CHECK(std::filesystem::exists(temp.child("nested") / "20260801-160000-000-nested.sql"));

  // A file with an unrecognised name is still readable by name, so a backup restored by hand
  // into the archive is not locked out of load() just because list() ignores it.
  CHECK(archive.load("handmade.sql") == "-- by hand\n");
}

TEST_CASE("nothing overwrites an existing file", "[archive][recovery]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  // A changeset restored into the archive by hand, or copied in from another machine, sits on
  // exactly the name the next store() would pick. It parses, so the sequence scan sees it and
  // steps past it -- and the copy is still readable afterwards.
  writeRaw(temp.child("20260801-160000-000-taken.sql"), "-- placed by hand\n");

  std::filesystem::path const written (archive.store("mine", "taken", STAMP_2026));

  CHECK(written.filename().string() == "20260801-160000-001-taken.sql");
  CHECK(archive.load("20260801-160000-000-taken.sql") == "-- placed by hand\n");
  CHECK(archive.load(written.filename().string()) == "mine");
}

TEST_CASE("a name occupied by something that is not a file is stepped over", "[archive][safety]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  // A DIRECTORY on the name store() would otherwise choose. This is the case the sequence scan
  // cannot cover on its own: the scan only looks at regular files, so it does not know this name
  // is taken, and without the existence check store() would try to open a directory for writing
  // and report a failure to archive a changeset that had nothing wrong with it.
  std::filesystem::path const obstruction (temp.child("20260801-160000-000-blocked.sql"));
  REQUIRE(std::filesystem::create_directory(obstruction));

  std::filesystem::path const written (archive.store("mine", "blocked", STAMP_2026));

  CHECK(written.filename().string() == "20260801-160000-001-blocked.sql");
  CHECK(archive.load(written.filename().string()) == "mine");

  // The obstruction is untouched, and is not reported as a stored changeset either.
  CHECK(std::filesystem::is_directory(obstruction));

  std::vector<ArchiveEntry> const entries (archive.list());
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].file_name == written.filename());
}

TEST_CASE("timestamps round-trip through the file name", "[archive][recovery]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  std::vector<std::uint64_t> const timestamps
    { 0ull                 // the epoch
    , 1ull
    , 86399ull             // last second of the first day
    , 86400ull             // first second of the second day
    , 946684800ull         // 2000-01-01, the century boundary
    , 1709164800ull        // 2024-02-29, a leap day
    , 1785600000ull
    , STAMP_MAX
    };

  for (std::uint64_t const timestamp : timestamps)
  {
    archive.store("payload", "round", timestamp);
  }

  std::vector<ArchiveEntry> const entries (archive.list());
  REQUIRE(entries.size() == timestamps.size());

  // Newest first, so the decoded timestamps must be the input list reversed -- exactly, not
  // approximately: a calendar conversion that is a day out somewhere still produces plausible
  // names and would pass any weaker check.
  for (std::size_t i (0); i < entries.size(); ++i)
  {
    CHECK(entries[i].timestamp == timestamps[timestamps.size() - 1 - i]);
    CHECK(entries[i].label == "round");
  }
}

TEST_CASE("a timestamp that is not seconds is refused", "[archive][safety]")
{
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  // Milliseconds where seconds were expected is the realistic mistake, and it must not be
  // absorbed silently: the file would sort after every real changeset for the next eight
  // thousand years.
  CHECK_THROWS_AS(archive.store("payload", "millis", 1785600000000ull), ArchiveError);
  CHECK_THROWS_AS(archive.store("payload", "overflow", STAMP_MAX + 1), ArchiveError);
  CHECK_THROWS_AS
    (archive.store("payload", "huge", 18446744073709551615ull), ArchiveError);

  // The boundary itself is accepted, so the check is a limit and not an off-by-one.
  CHECK_NOTHROW(archive.store("payload", "boundary", STAMP_MAX));

  // A refused store wrote nothing at all.
  CHECK(archive.list().size() == 1u);
}

TEST_CASE("a missing root throws rather than silently doing nothing", "[archive][safety]")
{
  TempDirectory const temp;

  std::filesystem::path const absent (temp.child("no-such-directory"));
  REQUIRE_FALSE(std::filesystem::exists(absent));

  ChangesetArchive archive (absent);

  // Reading, listing and pruning an archive that is not there is a misconfiguration, and
  // answering "no backups" would hide every backup the real archive holds.
  CHECK_THROWS_AS(archive.list(), ArchiveError);
  CHECK_THROWS_AS(archive.prune(0), ArchiveError);
  CHECK_THROWS_AS(archive.prune(5), ArchiveError);
  CHECK_THROWS_AS(archive.load("20260801-160000-000-any.sql"), ArchiveError);

  // The message has to say what is wrong and name the directory. This one surfaces to a user who
  // then has to go and fix a configured path, and a bare "filesystem error" does not tell them
  // which path or why -- the throw is only half the requirement.
  try
  {
    archive.list();
    FAIL("list() on a missing root did not throw");
  }
  catch (ArchiveError const& e)
  {
    std::string const message (e.what());
    CHECK(contains(message, absent.string()));
    CHECK(contains(message, "not a directory"));
  }

  // store() is the deliberate exception: it creates the root. Refusing to write because a
  // directory does not exist yet would lose exactly the file the class exists to keep.
  std::filesystem::path const written (archive.store("payload", "first", STAMP_2026));
  CHECK(std::filesystem::is_directory(absent));
  CHECK(written.parent_path() == absent);
  CHECK(archive.load(written.filename().string()) == "payload");

  // Several levels at once, since the configured root may be nested under a project directory
  // that does not exist either.
  ChangesetArchive deep (temp.child("a") / "b" / "c");
  CHECK_THROWS_AS(deep.list(), ArchiveError);
  CHECK_NOTHROW(deep.store("payload", "nested", STAMP_2026));
  CHECK(deep.list().size() == 1u);
}

TEST_CASE("a root that is not a directory throws", "[archive][safety]")
{
  TempDirectory const temp;

  std::filesystem::path const file_root (temp.child("not-a-directory"));
  writeRaw(file_root, "I am a file\n");

  ChangesetArchive archive (file_root);

  CHECK_THROWS_AS(archive.list(), ArchiveError);
  CHECK_THROWS_AS(archive.prune(0), ArchiveError);
  CHECK_THROWS_AS(archive.load("20260801-160000-000-any.sql"), ArchiveError);

  // store() cannot create a directory over an existing file, and must say so rather than
  // reporting a success that did not happen.
  CHECK_THROWS_AS(archive.store("payload", "doomed", STAMP_2026), ArchiveError);

  // The file that was in the way is untouched.
  CHECK(std::filesystem::is_regular_file(file_root));
  CHECK(std::filesystem::file_size(file_root) == 12u);
}

TEST_CASE("an archive with no root is refused at construction", "[archive][safety]")
{
  // An empty path resolves against the process working directory, so backups would land
  // wherever Noggit was started from. Refused where the mistake is, not at first store().
  CHECK_THROWS_AS(ChangesetArchive(std::filesystem::path()), ArchiveError);
  CHECK_THROWS_AS(ChangesetArchive(std::filesystem::path("")), ArchiveError);
}

TEST_CASE("file names do not follow the global locale", "[archive][safety]")
{
  // A locale that groups thousands is enough to corrupt every name the archive writes: ostream
  // integer output consults the numpunct grouping facet, so an unimbued stream renders 20260801
  // as "20,260,801" -- a name holding a comma, sorting in the wrong place, and no longer
  // parseable by list(). The CRT, the MySQL connector and any UI toolkit can install a global
  // locale, so which names in an archive are affected would depend on what ran first.
  std::locale grouping;

  try
  {
    grouping = std::locale("en-US");
  }
  catch (std::exception const&)
  {
    SUCCEED("no grouping locale is installed on this machine, so there is nothing to prove");
    return;
  }

  // Confirmed rather than assumed: if this platform's en-US does not group integers, the test
  // below would pass without exercising anything and is better skipped than believed.
  std::ostringstream probe;
  probe.imbue(grouping);
  probe << 20260801;

  if (probe.str() == "20260801")
  {
    SUCCEED("this platform's en-US locale does not group integers");
    return;
  }

  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  GlobalLocale const installed (grouping);

  std::filesystem::path const written (archive.store("payload", "grouped", STAMP_2026));

  CHECK(written.filename().string() == "20260801-160000-000-grouped.sql");
  CHECK(isFilesystemSafe(written.filename().string()));

  // And the name still decodes, which is what a corrupted stamp would break.
  std::vector<ArchiveEntry> const entries (archive.list());
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].timestamp == STAMP_2026);
}

TEST_CASE("a deleted changeset is recoverable from its backup", "[archive][recovery]")
{
  // The milestone requirement, end to end: a changeset is emitted, archived, deleted from where
  // the user put it, and restored from the archive byte for byte.
  TempDirectory const temp;

  std::filesystem::path const root (temp.child("backups"));
  std::filesystem::path const emitted (temp.child("tile_49_31.sql"));

  std::string const sql
    ( "-- Noggit changeset: tile (49,31)\n"
      "SET @CGUID := 200000;\n"
      "DELETE FROM `creature` WHERE `guid` = @CGUID;\n"
      "INSERT INTO `creature` (`guid`, `id`) VALUES (@CGUID, 299);\n"
    );

  writeRaw(emitted, sql);

  ChangesetArchive archive (root);
  archive.store(sql, "tile 49 31", STAMP_2026);

  REQUIRE(std::filesystem::remove(emitted));
  REQUIRE_FALSE(std::filesystem::exists(emitted));

  std::vector<ArchiveEntry> const entries (archive.list());
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].label == "tile_49_31");
  CHECK(entries[0].timestamp == STAMP_2026);
  CHECK(entries[0].size_bytes == static_cast<std::uintmax_t>(sql.size()));

  std::string const recovered (archive.load(entries[0].file_name));
  CHECK(recovered == sql);

  writeRaw(emitted, recovered);
  CHECK(std::filesystem::file_size(emitted) == static_cast<std::uintmax_t>(sql.size()));
}

TEST_CASE("a reserved device name or a trailing space is refused, not resolved"
         , "[archive][safety][recovery]")
{
  // Regression test for two Win32 name-normalisation escapes that a purely lexical byte test
  // misses.
  //
  // load("NUL") passed every check -- non-empty, not "..", no separator, no colon, no control
  // byte -- and then resolved to the null DEVICE rather than a child of the root. file_size
  // succeeded with 0, the stream opened, and load() returned an empty string as a SUCCESSFUL
  // recovery of a backup that does not exist. Silently handing back nothing is the worst failure
  // this class has, since its entire purpose is that a lost changeset is recoverable.
  //
  // Separately, Windows strips trailing spaces and dots from the final component, so ".. " --
  // not equal to ".." and holding no separator -- resolved to the archive root's PARENT.
  std::filesystem::path const root
    (std::filesystem::temp_directory_path() / "noggit_archive_devicename_test");

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  REQUIRE_FALSE(ec);

  ChangesetArchive archive (root);
  archive.store("-- payload\n", "tile", 1785600000);

  for (char const* hostile : {"NUL", "nul", "CON", "con.sql", "COM1", "LPT1", "aux", "PRN"})
  {
    CAPTURE(hostile);
    CHECK_THROWS_AS(archive.load(hostile), ArchiveError);
  }

  for (char const* hostile : {".. ", "..", ". ", "backup.sql ", "backup.sql."})
  {
    CAPTURE(hostile);
    CHECK_THROWS_AS(archive.load(hostile), ArchiveError);
  }

  // The genuine entry still loads, so the new guards did not simply reject everything.
  std::vector<ArchiveEntry> const entries (archive.list());
  REQUIRE(entries.size() == 1);
  CHECK(archive.load(entries.front().file_name) == "-- payload\n");

  std::filesystem::remove_all(root, ec);
}

TEST_CASE("store refuses rather than truncating a name another writer already claimed"
         , "[archive][safety][recovery]")
{
  // store() used to pick a name with exists() and then open it with std::ios::trunc, so a
  // concurrent creator's changeset was silently destroyed in the window between the two. The
  // claim is now an exclusive create, which fails instead of truncating.
  std::filesystem::path const root
    (std::filesystem::temp_directory_path() / "noggit_archive_exclusive_test");

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  REQUIRE_FALSE(ec);

  ChangesetArchive archive (root);

  std::filesystem::path const first (archive.store("-- first\n", "tile", 1785600000));
  REQUIRE(std::filesystem::exists(first));

  // A second store at the same timestamp and label must land on a different name rather than
  // overwrite, and the original content must survive intact.
  std::filesystem::path const second (archive.store("-- second\n", "tile", 1785600000));
  CHECK(second != first);

  std::ifstream check (first, std::ios::in | std::ios::binary);
  std::string surviving ((std::istreambuf_iterator<char>(check))
                        , std::istreambuf_iterator<char>());
  CHECK(surviving == "-- first\n");

  std::filesystem::remove_all(root, ec);
}

TEST_CASE("a backup whose name the narrow encoding cannot spell stays usable"
         , "[archive][safety][recovery]")
{
  // Regression test for the first M4-M5 review follow-up.
  //
  // collect() used to store entry.path().filename().string(). On Windows that conversion runs
  // through the active code page, and measured against the pre-fix code on MSVC 19.4x with code
  // page 1252 it does not substitute anything -- it THROWS ("No mapping for the Unicode
  // character exists in the target multi-byte code page"). So one backup copied in from a
  // machine with another encoding, an unremarkable file name there, took down list(), prune()
  // AND store(), which runs the same scan to choose its next sequence number. Noggit could not
  // archive a changeset at all while that file sat in the root.
  //
  // The name is built from a UTF-8 literal rather than a wide one so the test says the same
  // thing on both platforms, and the characters are CJK ideographs, which have no canonical
  // decomposition and so cannot be silently renormalised by the filesystem on the way in.
  // Spelled as escaped UTF-8 code units rather than as literal characters, because this file
  // carries no byte order mark: MSVC would read non-ASCII source bytes in the active code page
  // and silently change the very literal the test is about. The escapes are U+65E5 U+672C
  // U+8A9E.
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  std::filesystem::path const foreign
    (std::u8string (u8"20260801-160000-000-tile_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.sql"));

  writeRaw(temp.path() / foreign, "-- from elsewhere\n");
  REQUIRE(std::filesystem::exists(temp.path() / foreign));

  // One ordinary backup beside it, so a fix that simply refused to list anything foreign would
  // still have to explain why the archive is short.
  archive.store("-- mine\n", "mine", STAMP_2026 + 1);

  std::vector<ArchiveEntry> const entries (archive.list());
  REQUIRE(entries.size() == 2u);

  ArchiveEntry const* found (nullptr);

  for (ArchiveEntry const& entry : entries)
  {
    if (entry.file_name == foreign)
    {
      found = &entry;
    }
  }

  // The reported name is the name on disk, exactly. Before the fix this comparison could not be
  // made at all -- list() threw before returning anything.
  REQUIRE(found != nullptr);

  CHECK(found->timestamp == STAMP_2026);
  CHECK(found->size_bytes == 18u);

  // The name list() reports is the one that opens the file -- the property that makes an entry
  // a handle rather than a description.
  CHECK(archive.load(found->file_name) == "-- from elsewhere\n");

  // The label, by contrast, is explicitly a rendering: the readable part survives and every
  // non-ASCII code unit shows as '?'. How MANY question marks is an encoding detail (three
  // UTF-16 units on Windows, nine UTF-8 bytes elsewhere), so it is not asserted.
  CHECK(contains(found->label, "tile_"));
  CHECK(contains(found->label, "?"));

  // And that rendering is not a handle. Rebuilding a name from the label -- which is what a
  // caller does the moment it treats a displayed name as an address -- opens nothing: on
  // Windows because '?' is not a legal file name character at all, and anywhere else because no
  // such file exists. Asserted so the split between the two fields cannot quietly collapse back
  // into one string.
  CHECK_THROWS_AS
    (archive.load("20260801-160000-000-" + found->label + ".sql"), ArchiveError);

  // prune() must be able to delete it, and must take the ordinary backup with it. Before the fix
  // prune() never got as far as remove(): the listing it starts from threw first, so an archive
  // holding one foreign name could not be trimmed at all.
  CHECK(archive.prune(0) == 2u);
  CHECK_FALSE(std::filesystem::exists(temp.path() / foreign));
  CHECK(archive.list().empty());
}

TEST_CASE("a changeset that grows while it is read is refused, not truncated"
         , "[archive][recovery]")
{
  // Regression test for the second M4-M5 review follow-up.
  //
  // load() measured the file and then read exactly that many bytes, so its length check could
  // only ever catch SHRINKAGE. A file that grew between the stat and the read -- an archive on
  // a share, a backup being rewritten by a second Noggit -- came back silently truncated, which
  // is the precise failure the check's own comment claimed to make impossible.
  //
  // Exercised through readExactly rather than through load() because it cannot be staged
  // against a real directory: making a file change length between two calls inside load() needs
  // a second writer and a won race, and a test that depends on winning a race is a test that
  // reports a fix that is not there. A stream that holds more or fewer bytes than it claims
  // asks the same question deterministically.

  // The measurement was right: the payload comes back whole, and the extra byte the read now
  // asks for is not left in the result.
  {
    std::istringstream exact ("0123456789");
    CHECK(ChangesetArchive::readExactly(exact, 10, "'exact'") == "0123456789");
  }

  // An empty changeset is legitimate and must not become an I/O error, even though reading past
  // its end sets both eofbit and failbit.
  {
    std::istringstream empty ("");
    CHECK(ChangesetArchive::readExactly(empty, 0, "'empty'").empty());
  }

  // Bytes, not text: a NUL, a CR and UTF-8 all survive the one-byte-longer read unchanged.
  {
    std::string const payload (std::string("-- a\0b\r\n", 8) + "\xC3\xA9\n");
    std::istringstream bytes (payload);
    CHECK(ChangesetArchive::readExactly(bytes, payload.size(), "'bytes'") == payload);
  }

  // Grew. Before the fix each of these returned the first `expected_bytes` and reported success.
  {
    std::istringstream grown ("0123456789abcdef");
    CHECK_THROWS_AS(ChangesetArchive::readExactly(grown, 10, "'grown'"), ArchiveError);
  }

  // Grew by exactly one byte, which is the boundary the extra byte of read exists to see.
  {
    std::istringstream grown_by_one ("0123456789a");
    CHECK_THROWS_AS(ChangesetArchive::readExactly(grown_by_one, 10, "'one'"), ArchiveError);
  }

  // Grew from nothing, the case the old code could not see at all because it skipped the read
  // entirely when the measured size was zero.
  {
    std::istringstream from_empty ("x");
    CHECK_THROWS_AS(ChangesetArchive::readExactly(from_empty, 0, "'from_empty'"), ArchiveError);
  }

  // Shrank -- the direction the old guard did catch. Kept so the fix cannot have traded one
  // failure for the other.
  {
    std::istringstream shrunk ("012");
    CHECK_THROWS_AS(ChangesetArchive::readExactly(shrunk, 10, "'shrunk'"), ArchiveError);
  }

  // The two failures have to be distinguishable in the message. "Short read" pointed at a
  // truncated file on disk and "grew" points at a file still being written; a user chasing a
  // failed recovery does very different things with those two, so a shared message would be
  // half the fix.
  try
  {
    std::istringstream grown ("0123456789abcdef");
    ChangesetArchive::readExactly(grown, 10, "'tile_49_31.sql'");
    FAIL("a stream holding more than it was measured at did not throw");
  }
  catch (ArchiveError const& e)
  {
    std::string const message (e.what());
    CHECK(contains(message, "tile_49_31.sql"));
    CHECK(contains(message, "grew"));
    CHECK(contains(message, "10"));
  }

  try
  {
    std::istringstream shrunk ("012");
    ChangesetArchive::readExactly(shrunk, 10, "'tile_49_31.sql'");
    FAIL("a stream holding less than it was measured at did not throw");
  }
  catch (ArchiveError const& e)
  {
    std::string const message (e.what());
    CHECK(contains(message, "tile_49_31.sql"));
    CHECK(contains(message, "Short read"));
    CHECK(contains(message, "3"));
    CHECK(contains(message, "10"));
  }

  // load() still reads a real file whole, so the seam did not change what the class does -- only
  // where the check lives.
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  std::string const sql ("-- Noggit changeset\nSET @CGUID := 200000;\n");
  std::filesystem::path const written (archive.store(sql, "grown", STAMP_2026));

  CHECK(archive.load(written.filename()) == sql);
}

TEST_CASE("a backup that vanishes mid-scan is skipped, not fatal", "[archive][safety]")
{
  // Regression test for the third M4-M5 review follow-up.
  //
  // collect() failed the whole archive when it could not size one directory entry. A second
  // Noggit pruning the same archive is enough to make a file disappear between the directory
  // scan and the stat, and because store() calls collect() to choose its next sequence number,
  // an unrelated backup vanishing made store() refuse to archive a changeset that had nothing
  // wrong with it. prune() already treats an already-gone file as a no-op; collect() now agrees.
  //
  // Tested as the predicate rather than through list(), because the race cannot be staged: the
  // deletion would have to land between two statements inside collect()'s loop, and on Windows
  // it could not be observed even then -- std::filesystem::directory_entry caches the size from
  // the directory enumeration, so the stat never reaches the disk. The part that can be wrong,
  // and the part a platform can disagree about, is the classification itself.
  CHECK(ChangesetArchive::isMissing
          (std::make_error_code(std::errc::no_such_file_or_directory)));
  CHECK(ChangesetArchive::isMissing(std::error_code(ENOENT, std::generic_category())));

#ifdef _WIN32
  // ERROR_FILE_NOT_FOUND (2) and ERROR_PATH_NOT_FOUND (3) arrive in std::system_category with
  // Win32 values, not in std::generic_category with POSIX ones. Comparing the code against an
  // errc routes through the category's own mapping, which is the only reason one predicate
  // answers this question on both platforms -- and it is exactly the kind of claim that is
  // asserted in a comment and turns out to be false.
  CHECK(ChangesetArchive::isMissing(std::error_code(2, std::system_category())));
  CHECK(ChangesetArchive::isMissing(std::error_code(3, std::system_category())));
#endif

  // A cleared code is not a missing file. Getting this wrong would skip every entry in the
  // archive and report an empty listing for a directory full of backups -- the failure this
  // class is least allowed to have.
  CHECK_FALSE(ChangesetArchive::isMissing(std::error_code()));

  // Everything else still fails the listing loudly. A backup that exists but cannot be read is
  // not the same as one that is gone, and quietly dropping it would hide it from the user at
  // the moment they went looking for it.
  CHECK_FALSE(ChangesetArchive::isMissing(std::make_error_code(std::errc::permission_denied)));
  CHECK_FALSE(ChangesetArchive::isMissing(std::make_error_code(std::errc::io_error)));
  CHECK_FALSE
    (ChangesetArchive::isMissing(std::make_error_code(std::errc::too_many_files_open)));
  CHECK_FALSE(ChangesetArchive::isMissing(std::make_error_code(std::errc::is_a_directory)));

  // The archive still works normally around the predicate: a directory holding only readable
  // backups lists all of them, which is what a fix that skipped too eagerly would break.
  TempDirectory const temp;
  ChangesetArchive archive (temp.path());

  archive.store("one", "first", 1000);
  archive.store("two", "second", 2000);

  CHECK(archive.list().size() == 2u);
}
