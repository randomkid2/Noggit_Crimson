// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_DOCTORREPORT_HPP
#define NOGGIT_DATABASE_DOCTORREPORT_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace Noggit::Database
{
  class SchemaModel;
  class WorldDatabaseConnection;

  // Outcome of a single environment check.
  //
  // SKIPPED exists so that "this could not be determined" is never reported as "this is fine".
  // A Doctor that promotes an unknown to a PASS is worse than no Doctor at all: it converts a
  // question the user could still have answered into confidence they should never have had.
  enum class CheckStatus
  {
    PASS,
    WARN,
    FAIL,
    SKIPPED
  };

  // One line of the health report.
  //
  // `detail` says what was observed, `remedy` says what to do about it. They are separate
  // fields because a dialog wants to show the remedy prominently and a log wants it inline,
  // and because a check with no actionable remedy is a check worth rewriting.
  struct CheckResult
  {
    std::string name;

    // Defaults to SKIPPED, not PASS: a result nobody filled in has verified nothing.
    CheckStatus status = CheckStatus::SKIPPED;

    std::string detail;
    std::string remedy;
  };

  // The collected results of a startup health check, and their rendering.
  //
  // Holds no logic about any individual check -- those live in namespace Doctor below, so the
  // ones needing no database stay in a translation unit that links no database client.
  class DoctorReport
  {
    public:
      void add(CheckResult result);
      void add(std::vector<CheckResult> const& results);

      std::vector<CheckResult> const& results() const { return _results; }
      bool empty() const { return _results.empty(); }
      std::size_t countWith(CheckStatus status) const;

      // The worst status present, ranked PASS < SKIPPED < WARN < FAIL.
      //
      // An empty report returns SKIPPED rather than PASS. Nothing was verified, and reporting
      // that as a pass is the one answer guaranteed to be wrong.
      CheckStatus worstStatus() const;

      // True only when at least one check ran and none of them failed.
      //
      // The `!empty()` half matters: a report that never received a result is not healthy, it
      // is unexamined, and a caller writing `if (!report.ok())` should see that.
      bool ok() const;

      // Plain text, suitable for a log file or a dialog. No markup, no colour, no Qt.
      std::string render() const;

    private:
      std::vector<CheckResult> _results;
  };

  // Builders for the individual checks.
  //
  // Split by dependency, not by subject: everything above checkConnection is pure and runs with
  // no database and no MySQL Connector/C++ present, which is what lets the schema assertions be
  // tested against captured fixtures. The two connection-taking functions are implemented in a
  // separate translation unit for the same reason -- linking them drags in the connector, and
  // the pure half must stay buildable without it.
  namespace Doctor
  {
    // "PASS", "WARN", "FAIL", "SKIPPED". Stable, for logs and for a UI that needs a label.
    char const* statusLabel(CheckStatus status);

    // The in-process form of tools/dev-db/schema-check.ps1: the assertions that decide whether
    // this database can be edited at all.
    //
    // Reports the *capability* rather than the 3.3.5 answer. A schema carrying `spawndist` and
    // `bytes1` is a different, legitimate core generation, not a broken database, and failing it
    // here would make the capability layer pointless. What fails is a schema that answers a
    // question in no recognised way, because that is where guessing corrupts a changeset.
    //
    // A check whose subject table is absent reports SKIPPED naming the table, so one missing
    // table produces one failure and a list of honest unknowns rather than a wall of red.
    std::vector<CheckResult> checkSchema(SchemaModel const& schema);

    // The loud one. Connecting to a live world database by mistake is the failure this whole
    // layer is built to prevent, so a live-looking schema that is not the configured writable
    // one is a FAIL even though reads are harmless.
    CheckResult checkWritableSchema
      (std::string const& connected_schema, std::string const& writable_schema);

    // PASS when the directory holds the 3.3.5a MPQ chain, WARN when it exists but the set is
    // incomplete, FAIL when it is not there at all.
    CheckResult checkClientDataPath(std::string const& path);

    // Every check that needs no database, in one call. This is the startup path when the
    // connector is absent or the connection failed.
    DoctorReport offlineReport
      ( SchemaModel const& schema
      , std::string const& connected_schema
      , std::string const& writable_schema
      , std::string const& client_data_path
      );

    // --- Checks that use the connection --------------------------------------------------
    // Implemented in DoctorConnectionChecks.cpp.

    CheckResult checkConnection(WorldDatabaseConnection const& connection);

    // Reads the applied database version through whichever version table exists.
    //
    // A version that does not name the 3.3.5 line is a WARN, not a FAIL: the tool will still
    // read such a database correctly, but nothing below this layer has been verified against it.
    CheckResult checkDatabaseVersion
      ( WorldDatabaseConnection const& connection
      , std::string const& schema
      , SchemaModel const& model
      );
  }
}

#endif
