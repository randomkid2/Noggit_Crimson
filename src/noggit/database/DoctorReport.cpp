// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/DoctorReport.hpp>

#include <noggit/database/SchemaModel.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace Noggit::Database;

namespace
{
  // Table and check names are collected here rather than spelled at each use site, because the
  // check name is what a bug report quotes back and a typo in one of two copies makes the same
  // check look like two different ones across versions.
  constexpr char const* TABLE_CREATURE = "creature";
  constexpr char const* TABLE_CREATURE_ADDON = "creature_addon";
  constexpr char const* TABLE_GAMEOBJECT = "gameobject";
  constexpr char const* TABLE_SMART_SCRIPTS = "smart_scripts";

  constexpr char const* CHECK_SPAWN_TABLES = "world spawn tables";
  constexpr char const* CHECK_CREATURE_COLUMNS = "creature spawn columns";
  constexpr char const* CHECK_WANDER_COLUMN = "creature wander-distance column";
  constexpr char const* CHECK_ADDON_POSE = "creature_addon pose encoding";
  constexpr char const* CHECK_VERSION_TABLE = "database version table";
  constexpr char const* CHECK_WAYPOINT_TABLE = "waypoint path table";
  constexpr char const* CHECK_SMART_SCRIPTS = "smart_scripts parameter counts";
  constexpr char const* CHECK_WRITABLE_SCHEMA = "writable schema";
  constexpr char const* CHECK_CLIENT_DATA = "client data path";

  // Columns every emitter and reader in this layer depends on by name. The wander-distance
  // column is deliberately NOT here: it varies between core generations and is resolved through
  // the SchemaModel, which is exactly what CHECK_WANDER_COLUMN verifies separately.
  constexpr char const* REQUIRED_CREATURE_COLUMNS[] =
    { "guid", "id", "map", "position_x", "position_y", "position_z", "orientation"
    , "spawntimesecs", "MovementType"
    };

  // smart_scripts grew a fifth event parameter and a fourth target parameter over time. The
  // floor is the OLDER shape, because a schema with the older counts is a different generation
  // rather than a broken database -- the emitter reads the real count from the schema. Anything
  // below the floor is not a recognised smart_scripts at all.
  constexpr std::size_t MIN_EVENT_PARAMS = 4;
  constexpr std::size_t MIN_TARGET_PARAMS = 3;

  // The stock 3.3.5a (client 3.3.5a.12340) MPQ chain, minus the optional patch-2..patch-5 and
  // the locale directory: those vary with locale and with how far the install was patched, so
  // requiring them would fail correct installs.
  constexpr char const* EXPECTED_MPQ_FILES[] =
    { "common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ", "patch.MPQ" };

  constexpr std::size_t EXPECTED_MPQ_COUNT = std::size(EXPECTED_MPQ_FILES);

  // Substrings that mark a schema name as deliberately disposable.
  //
  // The asymmetry is the point: anything WITHOUT one of these markers is assumed live. Guessing
  // "probably a scratch copy" about a schema named `world` is the one mistake in this project
  // that cannot be undone, so the unknown case has to land on the loud side.
  constexpr char const* DISPOSABLE_SCHEMA_MARKERS[] =
    { "dev", "test", "sandbox", "scratch", "staging", "tmp", "temp", "noggit" };

  // Rendering geometry. "SKIPPED" is the longest status label.
  constexpr int STATUS_LABEL_WIDTH = 7;
  constexpr std::size_t DETAIL_INDENT_WIDTH = 12;   // "  [" + 7 + "] "

  CheckResult makeResult
    (char const* name, CheckStatus status, std::string detail, std::string remedy)
  {
    CheckResult out;
    out.name = name;
    out.status = status;
    out.detail = std::move(detail);
    out.remedy = std::move(remedy);

    return out;
  }

  CheckResult passed(char const* name, std::string detail)
  {
    return makeResult(name, CheckStatus::PASS, std::move(detail), {});
  }

  CheckResult warned(char const* name, std::string detail, std::string remedy)
  {
    return makeResult(name, CheckStatus::WARN, std::move(detail), std::move(remedy));
  }

  CheckResult failed(char const* name, std::string detail, std::string remedy)
  {
    return makeResult(name, CheckStatus::FAIL, std::move(detail), std::move(remedy));
  }

  // A skipped check must say what stopped it, so the reason is the detail and is not optional.
  CheckResult skipped(char const* name, std::string reason, std::string remedy)
  {
    return makeResult(name, CheckStatus::SKIPPED, std::move(reason), std::move(remedy));
  }

  // ASCII fold by hand, NOT std::tolower.
  //
  // std::tolower is locale-sensitive, and Qt calls setlocale(LC_ALL, "") at startup, so the fold
  // follows whatever locale the machine is configured with. Under a Turkish LC_CTYPE 'I' folds to
  // dotless 'i' rather than 'i', so a schema name containing I -- and the configured writable
  // schema is compared through this -- stops comparing equal to itself. The visible result is the
  // loud "you are connected to a live database" alarm firing against the CORRECT schema, which is
  // the one alarm that must never cry wolf.
  //
  // Schema and table names are ASCII, so an ASCII fold is complete for this purpose. Matches what
  // ChangesetArchive::sanitiseLabel already does for the same reason.
  std::string lowercased(std::string const& s)
  {
    std::string out;
    out.reserve(s.size());

    for (char c : s)
    {
      out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }

    return out;
  }

  // Display order, worst last. PASS ranks below SKIPPED so that a report which verified
  // everything it could still surfaces the unknowns instead of reading as a clean pass.
  int severityRank(CheckStatus status)
  {
    switch (status)
    {
      case CheckStatus::PASS:    return 0;
      case CheckStatus::SKIPPED: return 1;
      case CheckStatus::WARN:    return 2;
      case CheckStatus::FAIL:    return 3;
    }

    return 3;
  }

  std::string joined(std::vector<std::string> const& items)
  {
    std::string out;

    for (std::string const& item : items)
    {
      if (!out.empty())
      {
        out += ", ";
      }

      out += item;
    }

    return out;
  }

  // Keeps a multi-line detail aligned under its check name instead of wrapping back to column 0.
  std::string indentContinuations(std::string const& text, std::string const& indent)
  {
    std::string out;
    out.reserve(text.size());

    for (char c : text)
    {
      out.push_back(c);

      if (c == '\n')
      {
        out += indent;
      }
    }

    return out;
  }

  bool sameSchemaName(std::string const& a, std::string const& b)
  {
    // Case-insensitive because MySQL schema-name case behaviour depends on the platform and on
    // lower_case_table_names. A case-sensitive comparison here would report the configured dev
    // schema as a live one on a server that folds case, which is a false alarm loud enough to
    // train people to ignore the real one.
    return lowercased(a) == lowercased(b);
  }

  bool looksLive(std::string const& schema)
  {
    // Markers must match a WHOLE delimited token, not appear anywhere in the name.
    //
    // An unanchored substring search is a false negative in the one direction that matters:
    // "world_latest" contains "test" (la-TEST), "world_templates" contains "temp", and both
    // would be demoted from FAIL to a WARN reading "the name suggests a scratch copy, so this
    // is probably deliberate". A live production mirror would then leave ok() returning true,
    // and a caller checking `if (!report.ok())` would never surface it -- which is exactly the
    // "guessing probably a scratch copy" mistake this check exists to refuse to make.
    std::string const name (lowercased(schema));

    std::vector<std::string> tokens;
    std::string current;

    for (char c : name)
    {
      if (c == '_' || c == '-' || c == '.')
      {
        tokens.push_back(current);
        current.clear();
      }
      else
      {
        current.push_back(c);
      }
    }

    tokens.push_back(current);

    for (char const* marker : DISPOSABLE_SCHEMA_MARKERS)
    {
      if (std::find(tokens.begin(), tokens.end(), std::string(marker)) != tokens.end())
      {
        return false;
      }
    }

    return true;
  }

  // Directory entry names, lowercased, so the comparison survives both a Windows install
  // written as `Common.MPQ` and a case-sensitive filesystem holding `common.mpq`.
  std::set<std::string> lowercasedEntryNames
    (std::filesystem::path const& directory, std::error_code& ec)
  {
    std::set<std::string> names;

    std::filesystem::directory_iterator it (directory, ec);

    if (ec)
    {
      return names;
    }

    std::filesystem::directory_iterator const end;

    while (it != end)
    {
      // Only non-empty regular files count as archives.
      //
      // Recording every entry regardless of type let a directory named `common.MPQ`, or a 0-byte
      // cloud-storage placeholder or Git-LFS pointer of that name, satisfy the check: the Doctor
      // reported "all 5 expected 3.3.5a archives found" and then every MPQ open failed. Promoting
      // an unverified condition to a PASS is worse than having no Doctor at all, which is the one
      // rule this whole file is built on.
      std::error_code entry_ec;

      if (it->is_regular_file(entry_ec) && !entry_ec)
      {
        std::uintmax_t const size (it->file_size(entry_ec));

        if (!entry_ec && size > 0)
        {
          names.insert(lowercased(it->path().filename().string()));
        }
      }

      it.increment(ec);

      if (ec)
      {
        break;
      }
    }

    return names;
  }

  std::size_t countExpectedMpqs(std::set<std::string> const& names)
  {
    std::size_t found (0);

    for (char const* expected : EXPECTED_MPQ_FILES)
    {
      if (names.find(lowercased(expected)) != names.end())
      {
        ++found;
      }
    }

    return found;
  }

  std::vector<std::string> missingMpqs(std::set<std::string> const& names)
  {
    std::vector<std::string> missing;

    for (char const* expected : EXPECTED_MPQ_FILES)
    {
      if (names.find(lowercased(expected)) == names.end())
      {
        missing.emplace_back(expected);
      }
    }

    return missing;
  }
}

void DoctorReport::add(CheckResult result)
{
  _results.push_back(std::move(result));
}

void DoctorReport::add(std::vector<CheckResult> const& results)
{
  _results.insert(_results.end(), results.begin(), results.end());
}

std::size_t DoctorReport::countWith(CheckStatus status) const
{
  return static_cast<std::size_t>
    (std::count_if
      ( _results.begin(), _results.end()
      , [status] (CheckResult const& r)
        {
          return r.status == status;
        }
      )
    );
}

CheckStatus DoctorReport::worstStatus() const
{
  if (_results.empty())
  {
    return CheckStatus::SKIPPED;
  }

  CheckStatus worst (CheckStatus::PASS);

  for (CheckResult const& r : _results)
  {
    if (severityRank(r.status) > severityRank(worst))
    {
      worst = r.status;
    }
  }

  return worst;
}

bool DoctorReport::ok() const
{
  return !_results.empty() && worstStatus() != CheckStatus::FAIL;
}

std::string DoctorReport::render() const
{
  // The classic locale is imbued because this text is written to log files and pasted into bug
  // reports. A machine whose LC_NUMERIC formats numbers with a comma decimal separator would
  // otherwise emit a report that does not compare cleanly against one from the next machine.
  std::ostringstream out;
  out.imbue(std::locale::classic());

  out << "Noggit environment check: " << Doctor::statusLabel(worstStatus()) << "\n";

  if (_results.empty())
  {
    out << "  no checks were run, so nothing about this environment has been verified\n";

    return out.str();
  }

  out << "  " << _results.size() << " checks: "
      << countWith(CheckStatus::PASS) << " passed, "
      << countWith(CheckStatus::WARN) << " warned, "
      << countWith(CheckStatus::FAIL) << " failed, "
      << countWith(CheckStatus::SKIPPED) << " skipped\n";

  std::string const indent (DETAIL_INDENT_WIDTH, ' ');
  std::string const remedy_indent (indent + "        ");

  for (CheckResult const& r : _results)
  {
    out << "  [" << std::left << std::setw(STATUS_LABEL_WIDTH) << Doctor::statusLabel(r.status)
        << "] " << r.name << "\n";

    if (!r.detail.empty())
    {
      out << indent << indentContinuations(r.detail, indent) << "\n";
    }

    // Rendered for every non-passing status, including SKIPPED: "what would let this run" is
    // the most useful thing the report can say about a check that did not.
    if (!r.remedy.empty())
    {
      out << indent << "remedy: " << indentContinuations(r.remedy, remedy_indent) << "\n";
    }
  }

  return out.str();
}

char const* Doctor::statusLabel(CheckStatus status)
{
  switch (status)
  {
    case CheckStatus::PASS:    return "PASS";
    case CheckStatus::WARN:    return "WARN";
    case CheckStatus::FAIL:    return "FAIL";
    case CheckStatus::SKIPPED: return "SKIPPED";
  }

  // Unreachable for a valid enumerator, but a cast from an out-of-range int is legal C++ and
  // must not fall off the end of the function.
  return "SKIPPED";
}

std::vector<CheckResult> Doctor::checkSchema(SchemaModel const& schema)
{
  std::vector<CheckResult> results;

  bool const has_creature (schema.hasTable(TABLE_CREATURE));
  bool const has_gameobject (schema.hasTable(TABLE_GAMEOBJECT));

  // Run first so that everything below can report SKIPPED against a named missing table rather
  // than repeating the same discovery once per check.
  if (has_creature && has_gameobject)
  {
    results.push_back
      ( passed
          ( CHECK_SPAWN_TABLES
          , "creature and gameobject are both present ("
            + std::to_string(schema.tableCount()) + " tables, "
            + std::to_string(schema.columnCount()) + " columns introspected)"
          )
      );
  }
  else
  {
    std::vector<std::string> absent;

    if (!has_creature)
    {
      absent.emplace_back(TABLE_CREATURE);
    }

    if (!has_gameobject)
    {
      absent.emplace_back(TABLE_GAMEOBJECT);
    }

    results.push_back
      ( failed
          ( CHECK_SPAWN_TABLES
          , "missing: " + joined(absent) + ". This does not look like a TrinityCore world"
            " database."
          , "Point the connection at a world database, not auth or characters. If the schema"
            " name is right, check that the connecting account has SELECT on it:"
            " information_schema reports nothing for a schema the user has no privilege on, so"
            " a grants problem looks exactly like an empty database."
          )
      );
  }

  if (!has_creature)
  {
    results.push_back
      ( skipped
          ( CHECK_CREATURE_COLUMNS
          , "the creature table is absent, so its columns could not be examined"
          , "Resolve the missing-table failure above first."
          )
      );
    results.push_back
      ( skipped
          ( CHECK_WANDER_COLUMN
          , "the creature table is absent, so the wander-distance column could not be resolved"
          , "Resolve the missing-table failure above first."
          )
      );
  }
  else
  {
    std::vector<std::string> missing;

    for (char const* column : REQUIRED_CREATURE_COLUMNS)
    {
      if (!schema.hasColumn(TABLE_CREATURE, column))
      {
        missing.emplace_back(column);
      }
    }

    if (missing.empty())
    {
      results.push_back
        ( passed
            ( CHECK_CREATURE_COLUMNS
            , "all " + std::to_string(std::size(REQUIRED_CREATURE_COLUMNS))
              + " columns this layer names literally are present"
            )
        );
    }
    else
    {
      results.push_back
        ( failed
            ( CHECK_CREATURE_COLUMNS
            , "creature is missing: " + joined(missing)
            , "Every one of these is emitted by name in a changeset INSERT, so the statement"
              " would die with ERROR 1054 after its DELETE had already committed. Re-run"
              " /schema-check against this target and update docs/schema-335.md if the target"
              " is legitimate."
            )
        );
    }

    try
    {
      results.push_back
        ( passed
            ( CHECK_WANDER_COLUMN
            , "resolved to creature." + schema.wanderDistanceColumn()
            )
        );
    }
    catch (SchemaCapabilityError const& e)
    {
      results.push_back
        ( failed
            ( CHECK_WANDER_COLUMN
            , e.what()
            , "creature carries neither wander_distance (TrinityCore 3.3.5) nor spawndist"
              " (AzerothCore and older cores). Emitting either name would produce a changeset"
              " that fails at apply time. Add the variant to SchemaModel::wanderDistanceColumn"
              " and record it in docs/schema-335.md, or point Noggit at a supported world"
              " database."
            )
        );
    }
  }

  if (!schema.hasTable(TABLE_CREATURE_ADDON))
  {
    results.push_back
      ( skipped
          ( CHECK_ADDON_POSE
          , "the creature_addon table is absent, so the pose encoding could not be determined"
          , "Without creature_addon there is no per-spawn path_id, so waypoint paths can only"
            " be bound through creature_template_addon. Confirm this is the intended target."
          )
      );
  }
  else
  {
    try
    {
      AddonPoseEncoding const encoding (schema.addonPoseEncoding());

      results.push_back
        ( passed
            ( CHECK_ADDON_POSE
            , encoding == AddonPoseEncoding::DISCRETE_COLUMNS
                ? std::string("discrete columns (StandState, AnimTier, VisFlags, SheathState,"
                              " PvPFlags, MountCreatureID)")
                : std::string("packed bytes1/bytes2")
            )
        );
    }
    catch (SchemaCapabilityError const& e)
    {
      results.push_back
        ( failed
            ( CHECK_ADDON_POSE
            , e.what()
            , "creature_addon has neither StandState nor bytes1, so there is no way to know how"
              " this schema stores stand state, mount and sheath. The published brief this"
              " project started from claims bytes1/bytes2 and is wrong for 3.3.5 -- see"
              " docs/schema-335.md correction 1. Re-run /schema-check against this target."
            )
        );
    }
  }

  try
  {
    results.push_back
      ( passed(CHECK_VERSION_TABLE, "found `" + schema.versionTable() + "`")
      );
  }
  catch (SchemaCapabilityError const& e)
  {
    results.push_back
      ( failed
          ( CHECK_VERSION_TABLE
          , e.what()
          , "Neither version_db_world nor version exists. Both must be probed -- the reference"
            " TDB 335.25101 database has only `version`, which is why checking one is not"
            " enough. A world database with no version marker is not one this tool should be"
            " editing; verify the schema name and the account's SELECT grants."
          )
      );
  }

  try
  {
    results.push_back
      ( passed(CHECK_WAYPOINT_TABLE, "found `" + schema.waypointPathTable() + "`")
      );
  }
  catch (SchemaCapabilityError const& e)
  {
    results.push_back
      ( failed
          ( CHECK_WAYPOINT_TABLE
          , e.what()
          , "waypoint_data is absent, so the waypoint editor has nowhere to read or write paths."
            " Note that `waypoints` and `script_waypoint` do NOT exist on TDB 335.25101 despite"
            " the brief claiming otherwise (docs/schema-335.md corrections 3 and 4), so neither"
            " is a fallback here."
          )
      );
  }

  if (!schema.hasTable(TABLE_SMART_SCRIPTS))
  {
    results.push_back
      ( skipped
          ( CHECK_SMART_SCRIPTS
          , "the smart_scripts table is absent, so its parameter counts could not be read"
          , "SmartAI editing is unavailable against this database. Spawn and waypoint editing"
            " are unaffected."
          )
      );
  }
  else
  {
    try
    {
      std::size_t const events (schema.smartScriptsEventParamCount());
      std::size_t const targets (schema.smartScriptsTargetParamCount());

      std::string const detail
        ( std::to_string(events) + " event params, " + std::to_string(targets)
          + " target params"
        );

      if (events >= MIN_EVENT_PARAMS && targets >= MIN_TARGET_PARAMS)
      {
        results.push_back(passed(CHECK_SMART_SCRIPTS, detail));
      }
      else
      {
        results.push_back
          ( failed
              ( CHECK_SMART_SCRIPTS
              , detail + ", below the minimum of " + std::to_string(MIN_EVENT_PARAMS) + " and "
                + std::to_string(MIN_TARGET_PARAMS)
              , "smart_scripts is narrower than any recognised core generation. Emitting a"
                " positional INSERT against it would misalign every column after the event"
                " parameters. Re-run /schema-check and record the shape in docs/schema-335.md."
              )
          );
      }
    }
    catch (SchemaCapabilityError const& e)
    {
      results.push_back
        ( failed
            ( CHECK_SMART_SCRIPTS
            , e.what()
            , "smart_scripts exists but exposes no event_param* or target_param* columns at all."
              " Confirm the account can see every column of the table: information_schema hides"
              " columns the user has no privilege on, which looks identical to a truncated table."
            )
        );
    }
  }

  return results;
}

CheckResult Doctor::checkWritableSchema
  (std::string const& connected_schema, std::string const& writable_schema)
{
  if (connected_schema.empty())
  {
    return skipped
      ( CHECK_WRITABLE_SCHEMA
      , "no schema is selected on the connection, so there is nothing to compare against the"
        " configured writable schema"
      , "Set a schema in the connection settings. Until then every query has to name its schema"
        " explicitly and the write guard has nothing to check."
      );
  }

  if (writable_schema.empty())
  {
    // Not SKIPPED: the answer is known and it is bad. No writable schema configured means no
    // schema at all may be written, so this is reported against how dangerous the connected one
    // looks rather than deferred.
    std::string const detail
      ( "connected to `" + connected_schema + "` and no writable schema is configured, so"
        " nothing may be written"
      );

    std::string const remedy
      ( "Set writableSchema in tools/dev-db/db-policy.json to the disposable dev schema. Every"
        " other schema is read-only by policy; edits leave the editor as a reviewable .sql"
        " changeset."
      );

    return looksLive(connected_schema)
      ? failed(CHECK_WRITABLE_SCHEMA, detail + ", and `" + connected_schema
               + "` carries no dev/test marker so it must be assumed live", remedy)
      : warned(CHECK_WRITABLE_SCHEMA, detail, remedy);
  }

  if (sameSchemaName(connected_schema, writable_schema))
  {
    return passed
      ( CHECK_WRITABLE_SCHEMA
      , "connected to `" + connected_schema + "`, which is the configured writable schema"
      );
  }

  if (looksLive(connected_schema))
  {
    return failed
      ( CHECK_WRITABLE_SCHEMA
      , "connected to `" + connected_schema + "`, which is NOT the configured writable schema `"
        + writable_schema + "` and carries no dev/test marker. Treat it as a live world"
        " database."
      , "Reads are harmless and every write path refuses this schema, so nothing has been"
        " damaged. But an editing session pointed here produces changesets whose GUIDs and"
        " spawn ids belong to the live database, and applying one by hand would edit a"
        " production world. Repoint the connection at `" + writable_schema
        + "` before editing."
      );
  }

  return warned
    ( CHECK_WRITABLE_SCHEMA
    , "connected to `" + connected_schema + "` rather than the configured writable schema `"
      + writable_schema + "`. The name suggests a scratch copy, so this is probably"
      " deliberate."
    , "Writes are still refused: the connection only accepts DEV_WRITE against `"
      + writable_schema + "`. Repoint it there, or update writableSchema in"
      " tools/dev-db/db-policy.json if this copy is the intended target."
    );
}

CheckResult Doctor::checkClientDataPath(std::string const& path)
{
  if (path.empty())
  {
    return skipped
      ( CHECK_CLIENT_DATA
      , "no client data path is configured, so nothing was inspected"
      , "Set the project's client data path to a 3.3.5a.12340 install. Without it there are no"
        " DBCs, so displayId cannot be resolved to a model and spawns render as placeholders."
      );
  }

  std::filesystem::path const root (path);
  std::error_code ec;

  bool const present (std::filesystem::exists(root, ec));

  // A filesystem error is not the same fact as absence -- a path on a disconnected network
  // share is neither present nor missing -- so it is reported as an unknown, not as a failure.
  if (ec)
  {
    return skipped
      ( CHECK_CLIENT_DATA
      , "could not examine '" + path + "': " + ec.message()
      , "Check that the drive or share is reachable and that the path is readable by this user."
      );
  }

  if (!present)
  {
    return failed
      ( CHECK_CLIENT_DATA
      , "'" + path + "' does not exist"
      , "Point the client data path at a stock 3.3.5a.12340 install (the directory holding the"
        " MPQ chain, or the client root containing Data). Prefer an unmodified client: a"
        " custom-content one resolves displayIds that the target server may not share."
      );
  }

  bool const is_directory (std::filesystem::is_directory(root, ec));

  if (ec)
  {
    return skipped
      ( CHECK_CLIENT_DATA
      , "could not examine '" + path + "': " + ec.message()
      , "Check that the path is readable by this user."
      );
  }

  if (!is_directory)
  {
    return failed
      ( CHECK_CLIENT_DATA
      , "'" + path + "' exists but is not a directory"
      , "The client data path must be a directory, not an archive or a file. Point it at the"
        " directory holding common.MPQ, or at the client root containing Data."
      );
  }

  std::filesystem::path inspected (root);
  std::set<std::string> names (lowercasedEntryNames(root, ec));

  if (ec)
  {
    return skipped
      ( CHECK_CLIENT_DATA
      , "could not list '" + path + "': " + ec.message()
      , "Check that this user has read access to the directory."
      );
  }

  std::size_t found (countExpectedMpqs(names));

  // A stock install keeps the MPQ chain in <client>/Data, but Noggit's own project path points
  // at the client root, so both values are legitimate here. Probing the subdirectory rather than
  // demanding one layout keeps a correct install from being reported as broken.
  if (found < EXPECTED_MPQ_COUNT)
  {
    std::filesystem::path const nested (root / "Data");
    std::error_code nested_ec;

    if (std::filesystem::is_directory(nested, nested_ec) && !nested_ec)
    {
      std::set<std::string> nested_names (lowercasedEntryNames(nested, nested_ec));
      std::size_t const nested_found (nested_ec ? 0 : countExpectedMpqs(nested_names));

      if (nested_found > found)
      {
        inspected = nested;
        names = std::move(nested_names);
        found = nested_found;
      }
    }
  }

  std::string const where (inspected.string());

  if (found == EXPECTED_MPQ_COUNT)
  {
    return passed
      ( CHECK_CLIENT_DATA
      , "all " + std::to_string(EXPECTED_MPQ_COUNT) + " expected 3.3.5a archives found in '"
        + where + "'"
      );
  }

  if (found == 0)
  {
    return warned
      ( CHECK_CLIENT_DATA
      , "'" + where + "' is a directory but holds none of the expected 3.3.5a archives ("
        + joined(missingMpqs(names)) + ")"
      , "This is probably not a WoW client directory. Point it at the Data directory of a"
        " 3.3.5a.12340 install, or at the client root containing it."
      );
  }

  return warned
    ( CHECK_CLIENT_DATA
    , "'" + where + "' holds " + std::to_string(found) + " of "
      + std::to_string(EXPECTED_MPQ_COUNT) + " expected archives; missing: "
      + joined(missingMpqs(names))
    , "An incomplete MPQ chain resolves some models and silently fails on others, which shows"
      " up as scattered missing spawns rather than as an error. Verify the install is a"
      " complete 3.3.5a.12340 client. Note that patch-2..patch-5 are not required here and are"
      " not counted."
    );
}

DoctorReport Doctor::offlineReport
  ( SchemaModel const& schema
  , std::string const& connected_schema
  , std::string const& writable_schema
  , std::string const& client_data_path
  )
{
  DoctorReport report;

  // Writable-schema first: it is the one result a user must read before doing anything, and a
  // report is read top-down.
  report.add(checkWritableSchema(connected_schema, writable_schema));
  report.add(checkSchema(schema));
  report.add(checkClientDataPath(client_data_path));

  return report;
}
