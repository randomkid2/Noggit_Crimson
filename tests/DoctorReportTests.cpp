// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_test_macros.hpp>

#include <FixtureLoader.hpp>
#include <noggit/database/DoctorReport.hpp>
#include <noggit/database/SchemaModel.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace Noggit::Database;
using Noggit::Tests::fixturePath;
using Noggit::Tests::loadSchemaFixture;

namespace
{
  constexpr char const* REAL_FIXTURE = "schema-tdb335-25101.tsv";
  constexpr char const* DRIFTED_FIXTURE = "schema-alt-drifted.tsv";

  // The number of results checkSchema always produces. Asserted rather than assumed, because a
  // check silently disappearing is the one regression that makes every other assertion here
  // pass while covering less.
  constexpr std::size_t SCHEMA_CHECK_COUNT = 7;

  constexpr char const* DEV_SCHEMA = "noggit_dev_world";
  constexpr char const* LIVE_SCHEMA = "world";

  std::vector<ColumnInfo> fixtureColumns(char const* fixture)
  {
    return loadSchemaFixture(fixturePath(fixture));
  }

  bool sameName(std::string const& a, std::string const& b)
  {
    return std::equal
      ( a.begin(), a.end(), b.begin(), b.end()
      , [] (char x, char y)
        {
          return std::tolower(static_cast<unsigned char>(x))
              == std::tolower(static_cast<unsigned char>(y));
        }
      );
  }

  std::vector<ColumnInfo> withoutTable
    (std::vector<ColumnInfo> columns, std::string const& table)
  {
    columns.erase
      ( std::remove_if
          ( columns.begin(), columns.end()
          , [&table] (ColumnInfo const& c)
            {
              return sameName(c.table_name, table);
            }
          )
      , columns.end()
      );

    return columns;
  }

  std::vector<ColumnInfo> withoutColumn
    (std::vector<ColumnInfo> columns, std::string const& table, std::string const& column)
  {
    columns.erase
      ( std::remove_if
          ( columns.begin(), columns.end()
          , [&table, &column] (ColumnInfo const& c)
            {
              return sameName(c.table_name, table) && sameName(c.column_name, column);
            }
          )
      , columns.end()
      );

    return columns;
  }

  // Located by a distinctive fragment of the name rather than by index, so reordering the checks
  // does not silently repoint an assertion at a different one.
  //
  // Returns by value deliberately: a reference into a vector passed as a temporary dangles the
  // moment the full expression ends, and the resulting assertions read as passes.
  CheckResult check(std::vector<CheckResult> const& results, std::string const& keyword)
  {
    for (CheckResult const& r : results)
    {
      if (r.name.find(keyword) != std::string::npos)
      {
        return r;
      }
    }

    throw std::runtime_error("no check whose name contains '" + keyword + "'");
  }

  bool contains(std::string const& haystack, std::string const& needle)
  {
    return haystack.find(needle) != std::string::npos;
  }

  bool allHaveStatus(std::vector<CheckResult> const& results, CheckStatus status)
  {
    return std::all_of
      ( results.begin(), results.end()
      , [status] (CheckResult const& r)
        {
          return r.status == status;
        }
      );
  }

  // Every non-passing result has to be actionable. A FAIL with no remedy is the failure mode
  // this whole module exists to avoid: it tells the user something is wrong and nothing else.
  bool everyProblemHasARemedy(std::vector<CheckResult> const& results)
  {
    return std::all_of
      ( results.begin(), results.end()
      , [] (CheckResult const& r)
        {
          return r.status == CheckStatus::PASS || !r.remedy.empty();
        }
      );
  }

  // A real directory in the system temp area, removed on scope exit.
  class TempDir
  {
    public:
      TempDir()
      {
        static int sequence (0);

        std::filesystem::path const base (std::filesystem::temp_directory_path());
        long long const stamp
          ( static_cast<long long>
              (std::chrono::steady_clock::now().time_since_epoch().count())
          );

        _path = base / ("noggit-doctor-" + std::to_string(stamp) + "-"
                        + std::to_string(++sequence));

        std::filesystem::create_directories(_path);
      }

      ~TempDir()
      {
        // No exception may escape here, so the error_code overload rather than the throwing one.
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
      }

      TempDir(TempDir const&) = delete;
      TempDir& operator=(TempDir const&) = delete;

      std::filesystem::path const& path() const { return _path; }
      std::string string() const { return _path.string(); }

      void touch(std::string const& name) const
      {
        std::ofstream file (_path / name);
        file << "not a real MPQ";
      }

      std::filesystem::path subdirectory(std::string const& name) const
      {
        std::filesystem::path const nested (_path / name);
        std::filesystem::create_directories(nested);

        return nested;
      }

    private:
      std::filesystem::path _path;
  };

  void touchIn(std::filesystem::path const& directory, std::string const& name)
  {
    std::ofstream file (directory / name);
    file << "not a real MPQ";
  }
}

TEST_CASE("the real TDB 335.25101 schema passes every schema check", "[doctor][schema]")
{
  std::vector<CheckResult> const results
    (Doctor::checkSchema(SchemaModel(fixtureColumns(REAL_FIXTURE))));

  REQUIRE(results.size() == SCHEMA_CHECK_COUNT);
  CHECK(allHaveStatus(results, CheckStatus::PASS));

  // Each check must report the answer it resolved, not just a verdict -- otherwise a check that
  // resolved the wrong variant still reads as a pass.
  CHECK(contains(check(results, "wander").detail, "wander_distance"));
  CHECK(contains(check(results, "pose").detail, "StandState"));
  CHECK(contains(check(results, "version table").detail, "version"));
  CHECK(contains(check(results, "waypoint").detail, "waypoint_data"));
  CHECK(contains(check(results, "smart_scripts").detail, "5 event params"));
  CHECK(contains(check(results, "smart_scripts").detail, "4 target params"));

  DoctorReport report;
  report.add(results);

  CHECK(report.worstStatus() == CheckStatus::PASS);
  CHECK(report.ok());
  CHECK(report.countWith(CheckStatus::PASS) == SCHEMA_CHECK_COUNT);
}

TEST_CASE("the drifted schema also passes: it is another generation, not a fault", "[doctor][drift]")
{
  // The point of the capability layer. Failing a schema that carries spawndist and bytes1 would
  // make the Doctor an assertion that the database matches one hardcoded generation, which is
  // precisely what SchemaModel exists to avoid.
  std::vector<CheckResult> const results
    (Doctor::checkSchema(SchemaModel(fixtureColumns(DRIFTED_FIXTURE))));

  REQUIRE(results.size() == SCHEMA_CHECK_COUNT);
  CHECK(allHaveStatus(results, CheckStatus::PASS));

  CHECK(contains(check(results, "wander").detail, "spawndist"));
  CHECK(contains(check(results, "pose").detail, "bytes1"));
  CHECK(contains(check(results, "version table").detail, "version_db_world"));
  CHECK(contains(check(results, "smart_scripts").detail, "4 event params"));
  CHECK(contains(check(results, "smart_scripts").detail, "3 target params"));

  // Same verdict, different resolved answers. Identical details would mean the checks report a
  // constant rather than what they actually resolved.
  std::vector<CheckResult> const real
    (Doctor::checkSchema(SchemaModel(fixtureColumns(REAL_FIXTURE))));

  CHECK(check(real, "wander").detail != check(results, "wander").detail);
  CHECK(check(real, "pose").detail != check(results, "pose").detail);
  CHECK(check(real, "smart_scripts").detail != check(results, "smart_scripts").detail);
}

TEST_CASE("a schema stripped of its discriminating columns fails with remedies", "[doctor][safety]")
{
  std::vector<ColumnInfo> columns (fixtureColumns(REAL_FIXTURE));

  // Remove exactly the facts each capability question is answered from, leaving the tables in
  // place. This is the schema the SchemaModel refuses to guess about.
  columns = withoutColumn(columns, "creature", "wander_distance");
  columns = withoutColumn(columns, "creature", "spawndist");

  for (char const* pose :
       { "StandState", "AnimTier", "VisFlags", "SheathState", "PvPFlags", "MountCreatureID"
       , "bytes1", "bytes2" })
  {
    columns = withoutColumn(columns, "creature_addon", pose);
  }

  columns = withoutTable(columns, "version");
  columns = withoutTable(columns, "version_db_world");
  columns = withoutTable(columns, "waypoint_data");

  for (char const* param :
       { "event_param1", "event_param2", "event_param3", "event_param4", "event_param5"
       , "target_param1", "target_param2", "target_param3", "target_param4" })
  {
    columns = withoutColumn(columns, "smart_scripts", param);
  }

  std::vector<CheckResult> const results (Doctor::checkSchema(SchemaModel(columns)));

  REQUIRE(results.size() == SCHEMA_CHECK_COUNT);

  CHECK(check(results, "wander").status == CheckStatus::FAIL);
  CHECK(check(results, "pose").status == CheckStatus::FAIL);
  CHECK(check(results, "version table").status == CheckStatus::FAIL);
  CHECK(check(results, "waypoint").status == CheckStatus::FAIL);
  CHECK(check(results, "smart_scripts").status == CheckStatus::FAIL);

  // The tables are all still there, so the failures must be attributed to the columns rather
  // than reported as "this is not a world database".
  CHECK(check(results, "spawn tables").status == CheckStatus::PASS);
  CHECK(check(results, "spawn columns").status == CheckStatus::PASS);

  CHECK(everyProblemHasARemedy(results));

  // A remedy has to say what to do, so a placeholder cannot pass for one.
  CHECK(check(results, "wander").remedy.size() > 40);
  CHECK(contains(check(results, "wander").remedy, "spawndist"));
  CHECK(contains(check(results, "pose").remedy, "bytes1"));
  CHECK(contains(check(results, "version table").remedy, "version_db_world"));

  DoctorReport report;
  report.add(results);

  CHECK(report.worstStatus() == CheckStatus::FAIL);
  CHECK_FALSE(report.ok());
  CHECK(report.countWith(CheckStatus::FAIL) == 5);
}

TEST_CASE("a check that could not run reports SKIPPED, never PASS", "[doctor][safety]")
{
  SECTION("creature absent")
  {
    std::vector<CheckResult> const results
      ( Doctor::checkSchema
          (SchemaModel(withoutTable(fixtureColumns(REAL_FIXTURE), "creature")))
      );

    CHECK(check(results, "spawn tables").status == CheckStatus::FAIL);
    CHECK(contains(check(results, "spawn tables").detail, "creature"));

    // Both creature-dependent checks are unknowns now. Reporting them as failures would treble
    // the noise from one cause; reporting them as passes would be a lie.
    CHECK(check(results, "wander").status == CheckStatus::SKIPPED);
    CHECK(check(results, "spawn columns").status == CheckStatus::SKIPPED);
    CHECK(contains(check(results, "wander").detail, "creature"));

    // The rest of the schema is intact and must still be reported on.
    CHECK(check(results, "version table").status == CheckStatus::PASS);
    CHECK(check(results, "waypoint").status == CheckStatus::PASS);
  }

  SECTION("creature_addon absent")
  {
    std::vector<CheckResult> const results
      ( Doctor::checkSchema
          (SchemaModel(withoutTable(fixtureColumns(REAL_FIXTURE), "creature_addon")))
      );

    CHECK(check(results, "pose").status == CheckStatus::SKIPPED);
    CHECK_FALSE(check(results, "pose").detail.empty());
    CHECK(check(results, "spawn tables").status == CheckStatus::PASS);
  }

  SECTION("smart_scripts absent is survivable, not a failure")
  {
    std::vector<CheckResult> const results
      ( Doctor::checkSchema
          (SchemaModel(withoutTable(fixtureColumns(REAL_FIXTURE), "smart_scripts")))
      );

    CHECK(check(results, "smart_scripts").status == CheckStatus::SKIPPED);
    CHECK_FALSE(check(results, "smart_scripts").detail.empty());

    DoctorReport report;
    report.add(results);

    // An unknown surfaces in the worst status but does not fail the run: spawn editing works.
    CHECK(report.worstStatus() == CheckStatus::SKIPPED);
    CHECK(report.ok());
  }

  SECTION("an empty schema skips what it cannot see and fails the rest")
  {
    // An account with no SELECT grant on the target sees information_schema as empty, so this is
    // a real state and not a synthetic one. Nothing here may report PASS.
    std::vector<CheckResult> const results (Doctor::checkSchema(SchemaModel()));

    REQUIRE(results.size() == SCHEMA_CHECK_COUNT);

    DoctorReport report;
    report.add(results);

    CHECK(report.countWith(CheckStatus::PASS) == 0);
    CHECK(report.countWith(CheckStatus::FAIL) == 3);      // spawn tables, version, waypoints
    CHECK(report.countWith(CheckStatus::SKIPPED) == 4);   // creature x2, addon pose, smart_scripts
    CHECK_FALSE(report.ok());
    CHECK(everyProblemHasARemedy(results));

    // The grants explanation belongs here: an empty schema and a missing privilege are
    // indistinguishable from outside, and only one of them is fixed by changing the schema name.
    CHECK(contains(check(results, "spawn tables").remedy, "SELECT"));
  }
}

TEST_CASE("the creature spawn column check names every column it missed", "[doctor][schema]")
{
  std::vector<ColumnInfo> columns (fixtureColumns(REAL_FIXTURE));
  columns = withoutColumn(columns, "creature", "position_z");
  columns = withoutColumn(columns, "creature", "orientation");

  std::vector<CheckResult> const results (Doctor::checkSchema(SchemaModel(columns)));
  CheckResult const result (check(results, "spawn columns"));

  CHECK(result.status == CheckStatus::FAIL);
  CHECK(contains(result.detail, "position_z"));
  CHECK(contains(result.detail, "orientation"));

  // Naming a column that is present would send whoever reads this looking in the wrong place.
  CHECK_FALSE(contains(result.detail, "position_x"));
  CHECK_FALSE(result.remedy.empty());
}

TEST_CASE("checkWritableSchema is loud about a live-looking schema", "[doctor][safety]")
{
  SECTION("a live-looking mismatch fails")
  {
    CheckResult const r (Doctor::checkWritableSchema(LIVE_SCHEMA, DEV_SCHEMA));

    CHECK(r.status == CheckStatus::FAIL);
    CHECK(contains(r.detail, LIVE_SCHEMA));
    CHECK(contains(r.detail, DEV_SCHEMA));
    CHECK(contains(r.remedy, DEV_SCHEMA));

    // Reads are harmless, and the remedy must say so -- otherwise the honest reaction to this
    // failure is to assume damage was already done.
    CHECK(contains(r.remedy, "Reads are harmless"));
  }

  SECTION("any schema with no dev marker is treated as live")
  {
    for (char const* live : { "world", "acore_world", "legacy_world", "wotlk" })
    {
      CHECK(Doctor::checkWritableSchema(live, DEV_SCHEMA).status == CheckStatus::FAIL);
    }
  }

  SECTION("the configured writable schema passes")
  {
    CheckResult const r (Doctor::checkWritableSchema(DEV_SCHEMA, DEV_SCHEMA));

    CHECK(r.status == CheckStatus::PASS);
    CHECK(r.remedy.empty());
  }

  SECTION("names are compared without regard to case")
  {
    // MySQL identifier case behaviour varies with platform and lower_case_table_names, so a
    // case-sensitive comparison here would raise the loud warning against the correct schema.
    CHECK(Doctor::checkWritableSchema("NOGGIT_DEV_WORLD", DEV_SCHEMA).status
          == CheckStatus::PASS);
    CHECK(Doctor::checkWritableSchema("noggit_dev_world", "NoggIT_Dev_World").status
          == CheckStatus::PASS);
  }

  SECTION("a disposable-looking mismatch warns rather than failing")
  {
    for (char const* scratch : { "world_test_copy", "sandbox_world", "world_scratch" })
    {
      CheckResult const r (Doctor::checkWritableSchema(scratch, DEV_SCHEMA));

      CHECK(r.status == CheckStatus::WARN);
      CHECK_FALSE(r.remedy.empty());
    }
  }

  SECTION("no connected schema is an unknown, not a pass")
  {
    CheckResult const r (Doctor::checkWritableSchema("", DEV_SCHEMA));

    CHECK(r.status == CheckStatus::SKIPPED);
    CHECK_FALSE(r.detail.empty());
  }

  SECTION("no writable schema configured is reported against the connected one")
  {
    // Nothing may be written at all in this state. It is still worth distinguishing: connected
    // to `world` with no policy is the dangerous case, connected to a scratch copy is not.
    CHECK(Doctor::checkWritableSchema(LIVE_SCHEMA, "").status == CheckStatus::FAIL);
    CHECK(Doctor::checkWritableSchema("scratch_world", "").status == CheckStatus::WARN);
    CHECK(Doctor::checkWritableSchema("", "").status == CheckStatus::SKIPPED);
  }
}

TEST_CASE("checkClientDataPath distinguishes absent, incomplete and complete", "[doctor][client]")
{
  std::vector<std::string> const EXPECTED
    { "common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ", "patch.MPQ" };

  SECTION("no path configured is an unknown")
  {
    CheckResult const r (Doctor::checkClientDataPath(""));

    CHECK(r.status == CheckStatus::SKIPPED);
    CHECK_FALSE(r.detail.empty());
    CHECK_FALSE(r.remedy.empty());
  }

  SECTION("an absent directory fails")
  {
    TempDir const temp;
    std::string const missing ((temp.path() / "no-such-client").string());

    CheckResult const r (Doctor::checkClientDataPath(missing));

    CHECK(r.status == CheckStatus::FAIL);
    CHECK(contains(r.detail, "no-such-client"));
    CHECK_FALSE(r.remedy.empty());
  }

  SECTION("a file rather than a directory fails")
  {
    TempDir const temp;
    temp.touch("common.MPQ");

    CheckResult const r (Doctor::checkClientDataPath((temp.path() / "common.MPQ").string()));

    CHECK(r.status == CheckStatus::FAIL);
    CHECK(contains(r.detail, "not a directory"));
  }

  SECTION("an empty directory warns and names everything it wanted")
  {
    TempDir const temp;

    CheckResult const r (Doctor::checkClientDataPath(temp.string()));

    CHECK(r.status == CheckStatus::WARN);

    for (std::string const& name : EXPECTED)
    {
      CHECK(contains(r.detail, name));
    }

    CHECK_FALSE(r.remedy.empty());
  }

  SECTION("an incomplete set warns and names only what is missing")
  {
    TempDir const temp;
    temp.touch("common.MPQ");
    temp.touch("common-2.MPQ");
    temp.touch("expansion.MPQ");

    CheckResult const r (Doctor::checkClientDataPath(temp.string()));

    CHECK(r.status == CheckStatus::WARN);
    CHECK(contains(r.detail, "3 of 5"));
    CHECK(contains(r.detail, "lichking.MPQ"));
    CHECK(contains(r.detail, "patch.MPQ"));
    CHECK_FALSE(contains(r.detail, "expansion.MPQ"));
  }

  SECTION("the complete set passes")
  {
    TempDir const temp;

    for (std::string const& name : EXPECTED)
    {
      temp.touch(name);
    }

    // The optional tail of the chain must not be required: a client patched only to patch.MPQ
    // is still a usable 3.3.5a install.
    CheckResult const r (Doctor::checkClientDataPath(temp.string()));

    CHECK(r.status == CheckStatus::PASS);
    CHECK(r.remedy.empty());
    CHECK(contains(r.detail, temp.string()));
  }

  SECTION("archive names are matched without regard to case")
  {
    // Real installs are inconsistent about this and a case-sensitive filesystem makes it matter.
    TempDir const temp;
    temp.touch("COMMON.MPQ");
    temp.touch("common-2.mpq");
    temp.touch("Expansion.Mpq");
    temp.touch("lichking.MPQ");
    temp.touch("PATCH.mpq");

    CHECK(Doctor::checkClientDataPath(temp.string()).status == CheckStatus::PASS);
  }

  SECTION("the client root containing Data is accepted as well as Data itself")
  {
    // Noggit's own project path points at the client root, but the MPQ chain lives one level
    // down. Both are legitimate values for this setting.
    TempDir const temp;
    std::filesystem::path const data (temp.subdirectory("Data"));

    for (std::string const& name : EXPECTED)
    {
      touchIn(data, name);
    }

    CheckResult const r (Doctor::checkClientDataPath(temp.string()));

    CHECK(r.status == CheckStatus::PASS);

    // And it says which directory it actually looked in, so a wrong answer is diagnosable.
    CHECK(contains(r.detail, data.string()));
  }

  SECTION("a directory that is not a client at all warns rather than passing")
  {
    TempDir const temp;
    temp.touch("readme.txt");
    temp.subdirectory("Data");   // present but empty

    CheckResult const r (Doctor::checkClientDataPath(temp.string()));

    CHECK(r.status == CheckStatus::WARN);
    CHECK(contains(r.detail, "none of the expected"));
  }
}

TEST_CASE("worstStatus ranks by severity and ok() reflects only failures", "[doctor][report]")
{
  auto const one
    ( [] (CheckStatus status)
      {
        CheckResult r;
        r.name = "synthetic";
        r.status = status;
        r.detail = "detail";
        return r;
      }
    );

  SECTION("an empty report has verified nothing and says so")
  {
    DoctorReport const report;

    CHECK(report.empty());
    CHECK(report.worstStatus() == CheckStatus::SKIPPED);
    CHECK_FALSE(report.ok());
    CHECK(contains(report.render(), "no checks were run"));
    CHECK(contains(report.render(), "SKIPPED"));
    CHECK_FALSE(contains(report.render(), "PASS"));
  }

  SECTION("a default-constructed result claims nothing")
  {
    CheckResult const blank;

    CHECK(blank.status == CheckStatus::SKIPPED);
  }

  SECTION("PASS < SKIPPED < WARN < FAIL")
  {
    DoctorReport report;
    report.add(one(CheckStatus::PASS));
    CHECK(report.worstStatus() == CheckStatus::PASS);
    CHECK(report.ok());

    report.add(one(CheckStatus::SKIPPED));
    CHECK(report.worstStatus() == CheckStatus::SKIPPED);
    CHECK(report.ok());

    report.add(one(CheckStatus::WARN));
    CHECK(report.worstStatus() == CheckStatus::WARN);
    CHECK(report.ok());

    report.add(one(CheckStatus::FAIL));
    CHECK(report.worstStatus() == CheckStatus::FAIL);
    CHECK_FALSE(report.ok());

    // A later PASS must not soften an earlier failure.
    report.add(one(CheckStatus::PASS));
    CHECK(report.worstStatus() == CheckStatus::FAIL);
    CHECK_FALSE(report.ok());

    CHECK(report.results().size() == 5);
    CHECK(report.countWith(CheckStatus::PASS) == 2);
    CHECK(report.countWith(CheckStatus::WARN) == 1);
    CHECK(report.countWith(CheckStatus::FAIL) == 1);
    CHECK(report.countWith(CheckStatus::SKIPPED) == 1);
  }

  SECTION("a warning alone does not fail the run")
  {
    DoctorReport report;
    report.add(one(CheckStatus::WARN));

    CHECK(report.worstStatus() == CheckStatus::WARN);
    CHECK(report.ok());
  }
}

TEST_CASE("render is complete and readable", "[doctor][report]")
{
  DoctorReport report;

  CheckResult ok;
  ok.name = "something fine";
  ok.status = CheckStatus::PASS;
  ok.detail = "all good";
  report.add(ok);

  CheckResult bad;
  bad.name = "something broken";
  bad.status = CheckStatus::FAIL;
  bad.detail = "first line\nsecond line";
  bad.remedy = "do the thing";
  report.add(bad);

  CheckResult unknown;
  unknown.name = "something unknowable";
  unknown.status = CheckStatus::SKIPPED;
  unknown.detail = "the table was absent";
  unknown.remedy = "fix the failure above first";
  report.add(unknown);

  std::string const text (report.render());

  // The worst status has to be on the first line: a dialog may show only that.
  CHECK(text.substr(0, text.find('\n')) == "Noggit environment check: FAIL");

  CHECK(contains(text, "3 checks: 1 passed, 0 warned, 1 failed, 1 skipped"));

  for (CheckResult const& r : report.results())
  {
    CHECK(contains(text, r.name));
    CHECK(contains(text, r.detail.substr(0, r.detail.find('\n'))));
  }

  CHECK(contains(text, "[FAIL   ]"));
  CHECK(contains(text, "[SKIPPED]"));
  CHECK(contains(text, "remedy: do the thing"));

  // A skipped check must carry its remedy too: "what would let this run" is the most useful
  // thing the report can say about a check that did not.
  CHECK(contains(text, "remedy: fix the failure above first"));

  // A multi-line detail stays aligned under its check rather than wrapping to column 0.
  CHECK(contains(text, "first line\n            second line"));

  // A passing check with no remedy must not sprout an empty one.
  CHECK_FALSE(contains(text, "remedy: \n"));
}

TEST_CASE("offlineReport composes every check that needs no database", "[doctor][report]")
{
  TempDir const temp;

  for (char const* name :
       { "common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ", "patch.MPQ" })
  {
    temp.touch(name);
  }

  SchemaModel const schema (fixtureColumns(REAL_FIXTURE));

  SECTION("a healthy dev environment passes end to end")
  {
    DoctorReport const report
      (Doctor::offlineReport(schema, DEV_SCHEMA, DEV_SCHEMA, temp.string()));

    CHECK(report.results().size() == SCHEMA_CHECK_COUNT + 2);
    CHECK(report.worstStatus() == CheckStatus::PASS);
    CHECK(report.ok());
    CHECK(contains(report.render(), "Noggit environment check: PASS"));
  }

  SECTION("a live schema connected by mistake fails the report and nothing else")
  {
    DoctorReport const report
      (Doctor::offlineReport(schema, LIVE_SCHEMA, DEV_SCHEMA, temp.string()));

    CHECK_FALSE(report.ok());
    CHECK(report.worstStatus() == CheckStatus::FAIL);
    CHECK(report.countWith(CheckStatus::FAIL) == 1);
    CHECK(check(report.results(), "writable").status == CheckStatus::FAIL);

    // The writable-schema warning is what the user must read first, so it must be first.
    REQUIRE_FALSE(report.results().empty());
    CHECK(contains(report.results().front().name, "writable"));
  }

  SECTION("a missing client data path fails without disturbing the schema verdicts")
  {
    DoctorReport const report
      ( Doctor::offlineReport
          (schema, DEV_SCHEMA, DEV_SCHEMA, (temp.path() / "gone").string())
      );

    CHECK_FALSE(report.ok());
    CHECK(check(report.results(), "client data").status == CheckStatus::FAIL);
    CHECK(report.countWith(CheckStatus::PASS) == SCHEMA_CHECK_COUNT + 1);
  }
}

TEST_CASE("a live schema whose name merely contains a marker fragment still fails"
         , "[doctor][safety][report]")
{
  // Regression test. looksLive() used an unanchored substring search, so "world_latest" matched
  // the marker "test" (la-TEST) and a live production mirror was demoted from FAIL to a WARN
  // reading "the name suggests a scratch copy, so this is probably deliberate". ok() then
  // returned true and a caller checking `if (!report.ok())` never surfaced it -- the exact
  // guess this check exists to refuse to make.
  for (char const* live : {"world_latest", "world_greatest", "world_templates", "attempt_world"
                          , "contest_world", "world_hottest"})
  {
    CAPTURE(live);
    CheckResult const result (Doctor::checkWritableSchema(live, "noggit_dev_world"));
    CHECK(result.status == CheckStatus::FAIL);
    CHECK_FALSE(result.remedy.empty());
  }

  // A marker as a whole delimited token is still honoured, so the fix does not simply make
  // everything fail.
  for (char const* scratch : {"world_test", "test_world", "world-dev", "world.temp"
                             , "dev_world_copy"})
  {
    CAPTURE(scratch);
    CHECK(Doctor::checkWritableSchema(scratch, "noggit_dev_world").status != CheckStatus::FAIL);
  }
}
