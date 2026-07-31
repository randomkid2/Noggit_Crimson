// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/ChangesetBuilder.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Noggit::Database;

namespace
{
  // Only names that do NOT vary between schema generations appear here as literals. Everything
  // that varies -- the wander distance column, the creature_addon pose columns -- is resolved
  // through the SchemaModel at emission time. A literal "wander_distance" in this file would
  // silently produce a broken changeset against an AzerothCore target, which is the single
  // failure this class exists to prevent.
  constexpr char const* TABLE_CREATURE = "creature";
  constexpr char const* TABLE_CREATURE_ADDON = "creature_addon";
  constexpr char const* TABLE_GAMEOBJECT = "gameobject";

  constexpr char const* COLUMN_GUID = "guid";
  constexpr char const* COLUMN_ID = "id";

  constexpr char const* VARIABLE_CREATURE_GUID = "@CGUID";
  constexpr char const* VARIABLE_GAMEOBJECT_GUID = "@OGUID";
  constexpr char const* VARIABLE_PATH = "@PATH";

  // Six decimals is comfortably inside FLOAT precision at world magnitudes: at ~9500 yards the
  // representable step of a FLOAT is roughly 1e-3, so the emitted text carries more digits than
  // the column can hold and nothing is lost on write.
  constexpr int COORDINATE_DECIMALS = 6;

  // Nine significant digits name any IEEE binary32 value uniquely (FLT_MAX_DIGITS10 is 9), so a
  // rotation component emitted at this precision reloads as the float it came from rather than
  // as a neighbour of it. Six DECIMALS would be only six significant digits where |v| <= 1,
  // which is eight float steps of slack -- enough to rewrite an untouched row.
  constexpr int ROTATION_SIGNIFICANT_DIGITS = 9;

  // Ceiling on the decimals the significant-digit rule may ask for. Nine significant digits of a
  // value as small as the smallest subnormal float needs 53 of them; nothing a FLOAT column can
  // hold needs more. It exists so no input can produce an unbounded literal.
  constexpr int ROTATION_MAX_DECIMALS = 53;

  constexpr std::size_t LINE_WIDTH = 92;
  constexpr std::size_t RULE_WIDTH = 90;

  constexpr std::uint64_t MAX_UINT32 = 0xFFFFFFFFull;

  // creature_addon pose defaults, for a creature that has NO addon row yet.
  //
  // CreatureSpawn carries no pose data on purpose: Noggit places spawns, it does not author
  // emotes or stand states. These are the TDB defaults, and SheathState 1 (melee) is what the
  // reference changeset in tools/dev-db/03_example_changeset.sql writes.
  //
  // They are never applied to a row that already exists -- the addon statement updates only
  // path_id on a key collision. Writing them over an existing row would silently unmount,
  // unsheathe and de-aura a creature whose position was the only thing the user changed.
  constexpr std::uint32_t DEFAULT_MOUNT = 0;
  constexpr std::uint32_t DEFAULT_MOUNT_CREATURE_ID = 0;
  constexpr std::uint32_t DEFAULT_STAND_STATE = 0;
  constexpr std::uint32_t DEFAULT_ANIM_TIER = 0;
  constexpr std::uint32_t DEFAULT_VIS_FLAGS = 0;
  constexpr std::uint32_t DEFAULT_SHEATH_STATE = 1;
  constexpr std::uint32_t DEFAULT_PVP_FLAGS = 0;
  constexpr std::uint32_t DEFAULT_EMOTE = 0;
  constexpr std::uint32_t DEFAULT_VISIBILITY_DISTANCE_TYPE = 0;

  ValidationIssue makeIssue(ValidationIssue::Severity severity, std::string message)
  {
    ValidationIssue issue;
    issue.severity = severity;
    issue.message = std::move(message);
    return issue;
  }

  char const* severityLabel(ValidationIssue::Severity severity)
  {
    return severity == ValidationIssue::Severity::BLOCKING ? "ERROR" : "WARNING";
  }

  // Copies `source` into `sink`, prefixing every message with the spawn it concerns. Without
  // the prefix a caller holding issues() sees "wander distance must be zero" with no way to
  // tell which of two hundred spawns produced it.
  void appendIssues
    ( std::vector<ValidationIssue>& sink
    , std::vector<ValidationIssue> const& source
    , std::string const& context
    )
  {
    for (ValidationIssue const& issue : source)
    {
      sink.push_back(makeIssue(issue.severity, context + ": " + issue.message));
    }
  }

  // Identifiers are validated, never escaped. A name carrying anything a MySQL identifier
  // cannot hold did not come out of information_schema, so splicing it into a statement means
  // something upstream is already wrong -- refusing is the only safe answer.
  std::string quoteIdentifier(std::string const& name)
  {
    if (name.empty())
    {
      throw ChangesetError("Refusing to emit an empty SQL identifier.");
    }

    for (char c : name)
    {
      bool const acceptable
        (  (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '_'
        || c == '$'
        );

      if (!acceptable)
      {
        throw ChangesetError
          ("Refusing to emit the SQL identifier '" + name + "': it holds a character that cannot"
           " appear in an introspected table or column name. The schema this was resolved from"
           " is not trustworthy.");
      }
    }

    return "`" + name + "`";
  }

  std::string unsignedText(std::uint64_t value)
  {
    return std::to_string(value);
  }

  std::string signedText(std::int64_t value)
  {
    return std::to_string(value);
  }

  // Joins `pieces` with ", ", wrapping onto a new line indented by `indent` once a line would
  // pass LINE_WIDTH. `used` is how many columns the caller has already written on the first
  // line. Deterministic: the same changeset formats identically on every run, so a review diff
  // shows content changes and nothing else.
  std::string joinWrapped
    (std::vector<std::string> const& pieces, std::string const& indent, std::size_t used)
  {
    std::string out;
    std::size_t line = used;

    for (std::size_t i = 0; i < pieces.size(); ++i)
    {
      bool const last (i + 1 == pieces.size());
      std::string const piece (pieces[i] + (last ? "" : ","));

      if (i != 0)
      {
        if (line + 1 + piece.size() > LINE_WIDTH)
        {
          out += "\n" + indent;
          line = indent.size();
        }
        else
        {
          out += " ";
          ++line;
        }
      }

      out += piece;
      line += piece.size();
    }

    return out;
  }

  std::string sectionHeader(std::string const& title)
  {
    std::string out ("-- " + title + " ");

    while (out.size() < RULE_WIDTH)
    {
      out += "-";
    }

    return out + "\n";
  }

  // Renders `text` as SQL line comments. Splitting on newlines is not cosmetic: a description
  // holding a newline would otherwise end the comment and let the rest of the line execute.
  std::string commentBlock(std::string const& text)
  {
    std::string out;
    std::string line;

    for (char c : text)
    {
      if (c == '\n')
      {
        out += "-- " + line + "\n";
        line.clear();
      }
      else if (c != '\r')
      {
        line.push_back(c);
      }
    }

    if (!line.empty())
    {
      out += "-- " + line + "\n";
    }

    return out;
  }

  // "@CGUID" for the base value, "@CGUID+7" for anything above it. One SET line at the top
  // retargets every statement in the file, which is the TDB convention and the reason a
  // reviewer can move a whole changeset by editing the header.
  std::string variableExpression
    (char const* variable, std::uint64_t base, std::uint64_t value)
  {
    if (value == base)
    {
      return variable;
    }

    return std::string(variable) + "+" + unsignedText(value - base);
  }

  std::string deleteStatement
    ( std::string const& table
    , std::string const& key_column
    , std::vector<std::uint64_t> values
    , char const* variable
    , std::uint64_t base
    )
  {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());

    std::string out
      ("DELETE FROM " + quoteIdentifier(table) + " WHERE " + quoteIdentifier(key_column));

    if (values.size() == 1)
    {
      return out + " = " + variableExpression(variable, base, values.front()) + ";\n";
    }

    std::vector<std::string> expressions;
    expressions.reserve(values.size());

    for (std::uint64_t value : values)
    {
      expressions.push_back(variableExpression(variable, base, value));
    }

    out += " IN (";

    return out + joinWrapped(expressions, "   ", out.size()) + ");\n";
  }

  // `upsert_columns`, when non-empty, appends ON DUPLICATE KEY UPDATE for exactly those columns
  // and leaves every other column of an existing row untouched. Used where the emitter holds
  // only part of a row and must not overwrite the part it never read -- see the creature_addon
  // section in build().
  std::string insertStatement
    ( std::string const& table
    , std::vector<std::string> const& columns
    , std::vector<std::vector<std::string>> const& rows
    , std::vector<std::string> const& upsert_columns = {}
    )
  {
    std::vector<std::string> quoted;
    quoted.reserve(columns.size());

    for (std::string const& column : columns)
    {
      quoted.push_back(quoteIdentifier(column));
    }

    std::string out ("INSERT INTO " + quoteIdentifier(table) + "\n  (");
    out += joinWrapped(quoted, "   ", 3) + ") VALUES\n";

    for (std::size_t i = 0; i < rows.size(); ++i)
    {
      // A value list that has drifted out of step with the column list is the worst possible
      // outcome for this class: MySQL accepts nothing, or worse, accepts a row with every
      // field shifted by one. Catch it here rather than in a world database.
      if (rows[i].size() != columns.size())
      {
        throw ChangesetError
          ("Internal error building the " + table + " statement: row " + unsignedText(i)
           + " has " + unsignedText(rows[i].size()) + " values for " + unsignedText(columns.size())
           + " columns. Refusing to emit a misaligned INSERT.");
      }

      out += "  (" + joinWrapped(rows[i], "   ", 3) + ")";

      if (i + 1 != rows.size())
      {
        out += ",\n";
      }
    }

    if (!upsert_columns.empty())
    {
      std::vector<std::string> assignments;
      assignments.reserve(upsert_columns.size());

      for (std::string const& column : upsert_columns)
      {
        // VALUES(col) can only name a column the INSERT actually supplies. Catching it here
        // beats an ERROR 1054 part-way through applying a file whose DELETEs have committed.
        if (std::find(columns.begin(), columns.end(), column) == columns.end())
        {
          throw ChangesetError
            ("Internal error building the " + table + " statement: the update clause names '"
             + column + "', which is not among the columns being inserted.");
        }

        std::string const name (quoteIdentifier(column));

        // VALUES(col), not the row-alias form MySQL 8.0.20 introduced: the alias syntax does
        // not exist on MySQL 5.7 or on MariaDB, both ordinary hosts for a 3.3.5 world
        // database. VALUES() is accepted by every version, and it is the only way to reach a
        // per-row value from the update clause of a multi-row INSERT.
        assignments.push_back(name + " = VALUES(" + name + ")");
      }

      std::string const clause ("ON DUPLICATE KEY UPDATE ");

      out += "\n" + clause + joinWrapped(assignments, "  ", clause.size());
    }

    out += ";\n";

    return out;
  }

  // rotation0..3 is a quaternion, not Euler angles. A caller that set only `orientation` gets
  // the quaternion derived from it; emitting the identity instead would leave every rotated
  // object facing north in game while looking correct in the editor.
  TileCoordinates::Quaternion resolvedRotation(GameObjectSpawn const& spawn)
  {
    if (TileCoordinates::isDefaultRotation(spawn.rotation))
    {
      return TileCoordinates::quaternionForOrientation(spawn.orientation);
    }

    return spawn.rotation;
  }

  // Empty unless an explicitly supplied quaternion disagrees with `orientation`. The two are
  // written to the same row and the core trusts the quaternion, so a disagreement means the
  // object faces one way in the editor and another in game.
  std::string rotationDisagreement(GameObjectSpawn const& spawn)
  {
    if (TileCoordinates::isDefaultRotation(spawn.rotation))
    {
      return {};
    }

    double const from_quaternion (TileCoordinates::orientationForQuaternion(spawn.rotation));
    double const from_orientation (TileCoordinates::normaliseOrientation(spawn.orientation));

    double const delta (TileCoordinates::yawSeparation(from_quaternion, from_orientation));

    if (delta <= TileCoordinates::ORIENTATION_AGREEMENT_TOLERANCE)
    {
      return {};
    }

    return "the supplied rotation quaternion yields orientation "
         + SqlFormat::coordinate(from_quaternion) + " but the orientation column says "
         + SqlFormat::coordinate(from_orientation) + ". The quaternion is emitted as given;"
           " the core uses it, not the orientation column.";
  }

  // Fixed-notation text with `decimals` places after the point. Both formatters go through this
  // one function, so neither can lose the classic locale or the "-0" rule while the other keeps
  // it.
  std::string fixedText(double value, int decimals)
  {
    // std::fixed rather than the default float format: the default switches to scientific
    // notation for small magnitudes, and 1e-05 is not accepted where MySQL expects a plain
    // number in every context this text is used. The classic locale is imbued because a
    // comma decimal separator would silently turn one value into two.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(decimals) << value;

    std::string text (out.str());

    // "-0.000000" is legal SQL but reads as a bug in review, and it is what every value between
    // -1e-7 and -0.0 formats to. Normalise the sign away when no significant digit survived.
    if (!text.empty() && text.front() == '-')
    {
      bool const all_zero
        (text.find_first_of("123456789") == std::string::npos);

      if (all_zero)
      {
        text.erase(text.begin());
      }
    }

    return text;
  }

  // Reads emitted text back the way the server will: parse as a double, then narrow to the FLOAT
  // the column actually holds. Parsing straight into a float would round twice, and could agree
  // with the value it came from where MySQL disagrees.
  float reparsedAsFloat(std::string const& text)
  {
    std::istringstream in (text);
    in.imbue(std::locale::classic());

    double value (0.0);
    in >> value;

    return static_cast<float>(value);
  }
}

std::string SqlFormat::coordinate(double value)
{
  // A non-finite coordinate would stream as "nan" or "inf", neither of which is a SQL number.
  // Emitting it produces a syntax error at apply time at best; substituting zero would move
  // the spawn to the map origin silently.
  if (!std::isfinite(value))
  {
    throw ChangesetError
      ("Refusing to format a non-finite coordinate for SQL. A NaN or infinite position means"
       " the value was computed from bad input; writing any substitute would move the spawn.");
  }

  return fixedText(value, COORDINATE_DECIMALS);
}

std::string SqlFormat::rotationComponent(double value)
{
  if (!std::isfinite(value))
  {
    throw ChangesetError
      ("Refusing to format a non-finite rotation component for SQL. A NaN or infinite quaternion"
       " component is not a rotation at all, and substituting the identity would silently turn"
       " the object to face a direction nobody chose.");
  }

  // Where the leading significant digit falls decides how many DECIMALS nine SIGNIFICANT digits
  // costs. A quaternion component lives in [-1, 1], so that is nine decimals for the components
  // carrying the rotation and more for the small ones: 0.0479425549 needs ten places to hold the
  // same nine digits, and cos(o/2) for o near pi lands more than a decade below that again.
  int decimals (ROTATION_SIGNIFICANT_DIGITS);

  if (value != 0.0)
  {
    int const leading_place (static_cast<int>(std::floor(std::log10(std::fabs(value)))));

    decimals = std::max(decimals, ROTATION_SIGNIFICANT_DIGITS - 1 - leading_place);
    decimals = std::min(decimals, ROTATION_MAX_DECIMALS);
  }

  // Narrowing a double from outside the FLOAT range is undefined behaviour, not merely lossy, so
  // the round-trip check below cannot be run on one. Such a value is not a quaternion component
  // either -- these live in [-1, 1] -- and there is nothing meaningful to round-trip against, so
  // the significant-digit text stands as written and validation reports the shape of the
  // quaternion separately.
  if (std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max()))
  {
    return fixedText(value, decimals);
  }

  float const target (static_cast<float>(value));

  // The digit count above is the guarantee; reading the text back is the proof of it. std::log10
  // is entitled to a rounding error of its own at a decade boundary, and one digit too few would
  // silently reinstate the very defect this function exists to remove. It normally agrees on the
  // first attempt, so the check costs one parse per component.
  for (int attempt (decimals); attempt <= ROTATION_MAX_DECIMALS; ++attempt)
  {
    std::string const text (fixedText(value, attempt));

    if (reparsedAsFloat(text) == target)
    {
      return text;
    }
  }

  return fixedText(value, ROTATION_MAX_DECIMALS);
}

namespace
{
  // Scans assembled changeset text for `INSERT INTO `table` (`c1`, `c2`, ...)` and confirms
  // every named column exists in the schema. Reading the output back keeps this honest without
  // a manifest that could fall out of step with the emit sites.
  void requireEmittedColumnsExist
    (std::string const& sql, Noggit::Database::SchemaModel const& schema)
  {
    std::string const marker ("INSERT INTO `");
    std::vector<std::string> missing;

    for (std::size_t at = sql.find(marker); at != std::string::npos; at = sql.find(marker, at + 1))
    {
      std::size_t const table_begin (at + marker.size());
      std::size_t const table_end (sql.find('`', table_begin));

      if (table_end == std::string::npos)
      {
        continue;
      }

      std::string const table (sql.substr(table_begin, table_end - table_begin));

      std::size_t const list_begin (sql.find('(', table_end));
      std::size_t const list_end (sql.find(')', list_begin == std::string::npos ? 0 : list_begin));

      if (list_begin == std::string::npos || list_end == std::string::npos)
      {
        continue;
      }

      std::string const list (sql.substr(list_begin, list_end - list_begin));

      for (std::size_t open = list.find('`'); open != std::string::npos;
           open = list.find('`', open + 1))
      {
        std::size_t const close (list.find('`', open + 1));

        if (close == std::string::npos)
        {
          break;
        }

        std::string const column (list.substr(open + 1, close - open - 1));

        if (!column.empty() && !schema.hasColumn(table, column))
        {
          missing.push_back(table + "." + column);
        }

        open = close;
      }
    }

    if (!missing.empty())
    {
      std::string joined;

      for (std::string const& name : missing)
      {
        joined += (joined.empty() ? "" : ", ") + name;
      }

      throw Noggit::Database::ChangesetError
        ("Refusing to emit a changeset naming column(s) absent from the target schema: "
         + joined
         + ". Applying it would fail with ERROR 1054 after the DELETE statements had already"
           " committed. The target is a schema generation this emitter does not yet cover.");
    }
  }
}

std::string SqlFormat::quote(std::string const& value)
{
  // Returns the escaped BODY of a literal, without the surrounding quotes, so the result can
  // be embedded wherever a single-quoted literal is being assembled.
  //
  // A quote is escaped by DOUBLING it ('') rather than with a backslash. Backslash escaping is
  // what mysql_real_escape_string does, but it is only valid when the server is not running
  // with NO_BACKSLASH_ESCAPES -- which is part of ANSI sql_mode and common on hardened
  // installs. Changesets are applied with `mysql ... -e "source file.sql"`, so sql_mode comes
  // from the target server's configuration and not from any connection this tool controls.
  // Under NO_BACKSLASH_ESCAPES a name like O'Reilly emitted as 'O\'Reilly' terminates the
  // literal early and the remainder parses as SQL: a syntax error part-way through a file
  // whose earlier DELETEs have already committed. Doubling is correct under both sql_modes.
  //
  // Backslash itself is therefore left alone -- doubling it would be wrong under
  // NO_BACKSLASH_ESCAPES and is unnecessary otherwise, since a backslash cannot close a
  // literal. Control characters are rejected rather than escaped, because every escape
  // sequence for them is backslash-based and so not portable across sql_modes.
  for (char c : value)
  {
    if (c == '\n' || c == '\r' || c == '\0' || c == '\x1a')
    {
      throw ChangesetError
        ("Refusing to quote a value containing a control character: no portable escape for it"
         " exists, since every alternative is backslash-based and backslashes are literal under"
         " NO_BACKSLASH_ESCAPES.");
    }
  }

  std::string out;
  out.reserve(value.size() + 8);

  for (char c : value)
  {
    if (c == '\'')
    {
      out += "''";
    }
    else
    {
      out.push_back(c);
    }
  }

  return out;
}

ChangesetBuilder::ChangesetBuilder(SchemaModel schema, Options options)
  : _schema (std::move(schema))
  , _options (std::move(options))
{
}

void ChangesetBuilder::addCreature(CreatureSpawn const& spawn)
{
  std::string const context ("creature guid " + unsignedText(spawn.guid));

  for (CreatureSpawn const& existing : _creatures)
  {
    if (existing.guid == spawn.guid)
    {
      _issues.push_back
        ( makeIssue
            ( ValidationIssue::Severity::BLOCKING
            , context + ": added to this changeset twice. Two rows sharing a guid break"
                        " idempotency and collide on the primary key."
            )
        );
      break;
    }
  }

  // Everything below is applied to a copy, and the copy is what gets validated and emitted.
  // Validating the caller's struct instead would report errors for conditions this builder
  // then fixes, and would let a fix go out unvalidated.
  CreatureSpawn resolved (spawn);

  if (resolved.movement_type == MovementType::WAYPOINT)
  {
    // A waypoint creature is bound to its path through creature_addon.path_id and nowhere
    // else. Emitting the creature row without the addon row produces a spawn that stands
    // still, which reads to the user as "the editor lost my path" rather than as an error.
    resolved.has_addon = true;

    if (resolved.path_id == 0)
    {
      std::uint64_t const derived
        (static_cast<std::uint64_t>(resolved.guid) * _options.path_id_multiplier);

      if (derived == 0)
      {
        _issues.push_back
          ( makeIssue
              ( ValidationIssue::Severity::BLOCKING
              , context + ": follows a waypoint path but no path id was given and the"
                          " configured multiplier derives 0, which the core reads as"
                          " \"no path\"."
              )
          );
      }
      else if (derived > MAX_UINT32)
      {
        _issues.push_back
          ( makeIssue
              ( ValidationIssue::Severity::BLOCKING
              , context + ": the conventional path id guid * "
                        + unsignedText(_options.path_id_multiplier) + " is "
                        + unsignedText(derived) + ", which does not fit the unsigned 32-bit"
                          " path_id column. Assign a path id explicitly."
              )
          );
      }
      else
      {
        resolved.path_id = static_cast<std::uint32_t>(derived);
      }
    }
  }

  appendIssues(_issues, SpawnValidation::validate(resolved), context);

  _creatures.push_back(resolved);
}

void ChangesetBuilder::addGameObject(GameObjectSpawn const& spawn)
{
  std::string const context ("gameobject guid " + unsignedText(spawn.guid));

  for (GameObjectSpawn const& existing : _gameobjects)
  {
    if (existing.guid == spawn.guid)
    {
      _issues.push_back
        ( makeIssue
            ( ValidationIssue::Severity::BLOCKING
            , context + ": added to this changeset twice. Two rows sharing a guid break"
                        " idempotency and collide on the primary key."
            )
        );
      break;
    }
  }

  appendIssues(_issues, SpawnValidation::validate(spawn), context);

  std::string const disagreement (rotationDisagreement(spawn));

  if (!disagreement.empty())
  {
    _issues.push_back
      (makeIssue(ValidationIssue::Severity::WARNING, context + ": " + disagreement));
  }

  _gameobjects.push_back(spawn);
}

void ChangesetBuilder::addWaypointPath(WaypointPath const& path)
{
  std::string const context ("waypoint path " + unsignedText(path.id));

  for (WaypointPath const& existing : _paths)
  {
    if (existing.id == path.id)
    {
      _issues.push_back
        ( makeIssue
            ( ValidationIssue::Severity::BLOCKING
            , context + ": added to this changeset twice. The second set of nodes would be"
                        " appended to the first rather than replacing it."
            )
        );
      break;
    }
  }

  appendIssues(_issues, SpawnValidation::validate(path), context);

  _paths.push_back(path);
}

void ChangesetBuilder::removeCreature(std::uint32_t guid)
{
  _removed_creatures.push_back(guid);
}

void ChangesetBuilder::removeGameObject(std::uint32_t guid)
{
  _removed_gameobjects.push_back(guid);
}

void ChangesetBuilder::removeWaypointPath(std::uint32_t path_id)
{
  _removed_paths.push_back(path_id);
}

bool ChangesetBuilder::empty() const
{
  return _creatures.empty()
      && _gameobjects.empty()
      && _paths.empty()
      && _removed_creatures.empty()
      && _removed_gameobjects.empty()
      && _removed_paths.empty();
}

std::string ChangesetBuilder::build() const
{
  if (_options.reject_invalid && SpawnValidation::hasErrors(_issues))
  {
    std::string detail;

    for (ValidationIssue const& issue : _issues)
    {
      if (issue.severity == ValidationIssue::Severity::BLOCKING)
      {
        detail += "\n  - " + issue.message;
      }
    }

    throw ChangesetError
      ("Refusing to build a changeset that failed validation. The server would reject or"
       " silently correct these rows at load, which is a worse outcome than not emitting"
       " them:" + detail);
  }

  std::ostringstream out;
  out.imbue(std::locale::classic());

  out << "-- Noggit changeset. Review before applying.\n";

  if (!_options.description.empty())
  {
    out << "--\n" << commentBlock(_options.description);
  }

  if (empty())
  {
    out << "--\n"
           "-- Nothing to do: this changeset is intentionally empty and applying it is a no-op.\n";
    return out.str();
  }

  out << "--\n"
         "--   Apply:  mysql <world> -e \"source <this file>\"\n"
         "--\n"
         "-- Idempotent: every table touched is cleared for the affected keys before it is\n"
         "-- rewritten, so applying this file twice leaves identical rows and raises no error.\n"
         "-- creature_addon is the exception and updates in place. The editor reads only path_id\n"
         "-- from that table, so clearing it would discard the mount, pose, emote and auras it\n"
         "-- never read: only path_id is written, and only for creatures this file edits. An\n"
         "-- addon row is deleted only when its creature is being deleted with it.\n"
         "-- Variable-driven: guids and path ids are declared once below, so a reviewer can\n"
         "-- retarget the whole changeset by editing the header.\n"
         "-- Column-explicit: never positional. Names that differ between core generations --\n"
         "-- the wander-distance column, the creature_addon pose columns, the waypoint table --\n"
         "-- were resolved from the target schema rather than assumed. Names that are stable\n"
         "-- across every supported generation appear literally, and every name emitted below\n"
         "-- was checked to exist in the target schema before this file was written.\n"
         "--\n"
         "-- The zone and area columns are omitted on purpose: ObjectMgr::LoadCreatures does not\n"
         "-- read them and the core does not write them either. The server derives both.\n"
         "-- Waypoint rows carry no core-managed guid column: authoring it corrupts the path.\n";

  if (!_issues.empty())
  {
    out << "--\n-- Validation notes:\n";

    for (ValidationIssue const& issue : _issues)
    {
      out << "--   " << severityLabel(issue.severity) << ": " << issue.message << "\n";
    }
  }

  out << "\n";

  // --- Variable header -------------------------------------------------------------------
  // Each base is the smallest key of its kind, so every offset is non-negative and the file
  // reads in ascending order.

  std::vector<std::uint64_t> creature_keys;
  std::vector<std::uint64_t> gameobject_keys;
  std::vector<std::uint64_t> path_keys;

  for (CreatureSpawn const& spawn : _creatures)
  {
    creature_keys.push_back(spawn.guid);

    if (spawn.has_addon && spawn.path_id != 0)
    {
      path_keys.push_back(spawn.path_id);
    }
  }

  for (std::uint32_t guid : _removed_creatures)
  {
    creature_keys.push_back(guid);
  }

  for (GameObjectSpawn const& spawn : _gameobjects)
  {
    gameobject_keys.push_back(spawn.guid);
  }

  for (std::uint32_t guid : _removed_gameobjects)
  {
    gameobject_keys.push_back(guid);
  }

  for (WaypointPath const& path : _paths)
  {
    path_keys.push_back(path.id);
  }

  for (std::uint32_t path_id : _removed_paths)
  {
    path_keys.push_back(path_id);
  }

  std::uint64_t const creature_base
    (creature_keys.empty()
       ? 0u
       : *std::min_element(creature_keys.begin(), creature_keys.end()));
  std::uint64_t const gameobject_base
    (gameobject_keys.empty()
       ? 0u
       : *std::min_element(gameobject_keys.begin(), gameobject_keys.end()));
  std::uint64_t const path_base
    (path_keys.empty() ? 0u : *std::min_element(path_keys.begin(), path_keys.end()));

  if (!creature_keys.empty())
  {
    out << "SET " << VARIABLE_CREATURE_GUID << "  := " << unsignedText(creature_base) << ";\n";
  }

  if (!gameobject_keys.empty())
  {
    out << "SET " << VARIABLE_GAMEOBJECT_GUID << "  := " << unsignedText(gameobject_base)
        << ";\n";
  }

  if (!path_keys.empty())
  {
    out << "SET " << VARIABLE_PATH << "   := " << unsignedText(path_base) << ";\n";
  }

  // --- creature --------------------------------------------------------------------------

  if (!creature_keys.empty())
  {
    out << "\n" << sectionHeader(TABLE_CREATURE);

    std::vector<std::uint64_t> delete_keys;

    for (CreatureSpawn const& spawn : _creatures)
    {
      delete_keys.push_back(spawn.guid);
    }

    for (std::uint32_t guid : _removed_creatures)
    {
      delete_keys.push_back(guid);
    }

    out << deleteStatement
             ( TABLE_CREATURE, COLUMN_GUID, delete_keys
             , VARIABLE_CREATURE_GUID, creature_base
             );

    if (!_creatures.empty())
    {
      // The one column whose name differs between cores. Resolved, never assumed.
      std::string const wander_column (_schema.wanderDistanceColumn());

      // The core's own WORLD_INS_CREATURE shape, which is exactly what the server writes when
      // a GM spawns a creature, and therefore the shape least likely to surprise it. No zone
      // or area column: the core derives both.
      std::vector<std::string> const columns
        { "guid", "id", "map", "spawnMask", "phaseMask", "modelid", "equipment_id"
        , "position_x", "position_y", "position_z", "orientation"
        , "spawntimesecs", wander_column, "currentwaypoint", "curhealth", "curmana"
        , "MovementType", "npcflag", "unit_flags", "dynamicflags"
        };

      std::vector<std::vector<std::string>> rows;
      std::string notes;

      for (CreatureSpawn const& spawn : _creatures)
      {
        for (ValidationIssue const& issue : SpawnValidation::validate(spawn))
        {
          notes += std::string("-- ") + severityLabel(issue.severity) + " (guid "
                 + unsignedText(spawn.guid) + "): " + issue.message + "\n";
        }

        rows.push_back
          ( { variableExpression(VARIABLE_CREATURE_GUID, creature_base, spawn.guid)
            , unsignedText(spawn.id)
            , unsignedText(spawn.map)
            , unsignedText(spawn.spawn_mask)
            , unsignedText(spawn.phase_mask)
            , unsignedText(spawn.model_id)
            , signedText(spawn.equipment_id)
            , SqlFormat::coordinate(spawn.position.x)
            , SqlFormat::coordinate(spawn.position.y)
            , SqlFormat::coordinate(spawn.position.z)
            , SqlFormat::coordinate(TileCoordinates::normaliseOrientation(spawn.orientation))
            , unsignedText(spawn.spawn_time_secs)
            , SqlFormat::coordinate(spawn.wander_distance)
            , unsignedText(spawn.current_waypoint)
            , unsignedText(spawn.cur_health)
            , unsignedText(spawn.cur_mana)
            , unsignedText(static_cast<std::uint32_t>(spawn.movement_type))
            , unsignedText(spawn.npc_flag)
            , unsignedText(spawn.unit_flags)
            , unsignedText(spawn.dynamic_flags)
            }
          );
      }

      out << notes << insertStatement(TABLE_CREATURE, columns, rows);
    }
  }

  // --- creature_addon --------------------------------------------------------------------
  // The one section that is not DELETE-then-INSERT, and the exception is deliberate.
  //
  // The read path selects exactly two things from this table: path_id, and a boolean saying
  // whether a row exists at all (SpawnQueryDetail.cpp:557-568). mount, MountCreatureID,
  // StandState, AnimTier, VisFlags, SheathState, PvPFlags, emote, visibilityDistanceType and
  // auras are never read, so CreatureSpawn cannot carry them and this emitter has nothing to
  // write back for them. A DELETE covering every creature the changeset touches, followed by an
  // INSERT of the hardcoded defaults below, therefore replaced whatever the target held with
  // zeroes -- a creature that was mounted, kneeling, sheathed, emoting or carrying auras lost
  // all of it the moment somebody nudged its position.
  //
  // Two ways out: read enough to round-trip the row, or stop writing columns that are never
  // read. The second is taken, and not merely because the first means widening both
  // CreatureSpawn and the row decoder. Round-tripping is a promise this emitter cannot keep --
  // any column a later core generation adds would be dropped by exactly the same mechanism --
  // whereas "write only what the editor authored" holds for columns nobody has invented yet.
  //
  // So an edited creature gets INSERT ... ON DUPLICATE KEY UPDATE `path_id`. Where no addon row
  // exists one is created from the defaults; where one exists only the path binding moves and
  // the pose survives. It stays idempotent, applying twice leaves identical rows, and it still
  // clears a path the editor removed: such a spawn arrives with path_id 0 and has_addon true,
  // so the update writes the 0.
  //
  // A creature the editor is deleting is the one case that still DELETEs here: its `creature`
  // row is going away, so what would be left is an orphan rather than anybody's data.
  //
  // Two measured facts this rests on, both from the reference structure dump. creature_addon is
  // keyed PRIMARY KEY (guid), so the upsert collides on exactly the row it means to -- the same
  // assumption the reader's LEFT JOIN on guid already makes, since without that key the join
  // would multiply spawns. And the world schema declares no foreign keys at all, so the
  // DELETE FROM `creature` above does not cascade here and take the addon row with it; if it
  // did, preserving the row in this section would be pointless.

  std::vector<CreatureSpawn> with_addon;

  for (CreatureSpawn const& spawn : _creatures)
  {
    if (spawn.has_addon)
    {
      with_addon.push_back(spawn);
    }
  }

  if (!with_addon.empty() || !_removed_creatures.empty())
  {
    out << "\n" << sectionHeader(TABLE_CREATURE_ADDON);

    if (!_removed_creatures.empty())
    {
      std::vector<std::uint64_t> delete_keys;

      for (std::uint32_t guid : _removed_creatures)
      {
        delete_keys.push_back(guid);
      }

      out << deleteStatement
               ( TABLE_CREATURE_ADDON, COLUMN_GUID, delete_keys
               , VARIABLE_CREATURE_GUID, creature_base
               );
    }

    if (!with_addon.empty())
    {
      AddonPoseEncoding const encoding (_schema.addonPoseEncoding());

      // Not every schema generation carries this one, and naming an absent column fails the
      // whole statement.
      bool const has_visibility_distance
        (_schema.hasColumn(TABLE_CREATURE_ADDON, "visibilityDistanceType"));

      std::vector<std::string> columns { "guid", "path_id", "mount" };

      if (encoding == AddonPoseEncoding::DISCRETE_COLUMNS)
      {
        columns.insert
          ( columns.end()
          , { "MountCreatureID", "StandState", "AnimTier", "VisFlags", "SheathState", "PvPFlags" }
          );
      }
      else
      {
        columns.insert(columns.end(), { "bytes1", "bytes2" });
      }

      columns.push_back("emote");

      if (has_visibility_distance)
      {
        columns.push_back("visibilityDistanceType");
      }

      columns.push_back("auras");

      std::vector<std::vector<std::string>> rows;

      for (CreatureSpawn const& spawn : with_addon)
      {
        std::vector<std::string> row
          { variableExpression(VARIABLE_CREATURE_GUID, creature_base, spawn.guid)
          , spawn.path_id == 0
              ? std::string("0")
              : variableExpression(VARIABLE_PATH, path_base, spawn.path_id)
          , unsignedText(DEFAULT_MOUNT)
          };

        if (encoding == AddonPoseEncoding::DISCRETE_COLUMNS)
        {
          row.push_back(unsignedText(DEFAULT_MOUNT_CREATURE_ID));
          row.push_back(unsignedText(DEFAULT_STAND_STATE));
          row.push_back(unsignedText(DEFAULT_ANIM_TIER));
          row.push_back(unsignedText(DEFAULT_VIS_FLAGS));
          row.push_back(unsignedText(DEFAULT_SHEATH_STATE));
          row.push_back(unsignedText(DEFAULT_PVP_FLAGS));
        }
        else
        {
          // The packed form is the same information in the older layout: bytes1 holds the
          // stand state in byte 0, vis flags in byte 2 and anim tier in byte 3; bytes2 holds
          // the sheath state in byte 0 and the pvp flags in byte 1. Derived from the same
          // constants so the two branches cannot drift apart.
          std::uint32_t const bytes1
            (DEFAULT_STAND_STATE | (DEFAULT_VIS_FLAGS << 16) | (DEFAULT_ANIM_TIER << 24));
          std::uint32_t const bytes2 (DEFAULT_SHEATH_STATE | (DEFAULT_PVP_FLAGS << 8));

          row.push_back(unsignedText(bytes1));
          row.push_back(unsignedText(bytes2));
        }

        row.push_back(unsignedText(DEFAULT_EMOTE));

        if (has_visibility_distance)
        {
          row.push_back(unsignedText(DEFAULT_VISIBILITY_DISTANCE_TYPE));
        }

        row.push_back("NULL");

        rows.push_back(row);
      }

      // path_id is the only column the editor authored, so it is the only one allowed to
      // overwrite an existing row. Everything else in the value tuple exists solely to give a
      // creature that has no addon row yet a complete one.
      out << insertStatement(TABLE_CREATURE_ADDON, columns, rows, {"path_id"});
    }
  }

  // --- gameobject ------------------------------------------------------------------------

  if (!gameobject_keys.empty())
  {
    out << "\n" << sectionHeader(TABLE_GAMEOBJECT);

    std::vector<std::uint64_t> delete_keys;

    for (GameObjectSpawn const& spawn : _gameobjects)
    {
      delete_keys.push_back(spawn.guid);
    }

    for (std::uint32_t guid : _removed_gameobjects)
    {
      delete_keys.push_back(guid);
    }

    out << deleteStatement
             ( TABLE_GAMEOBJECT, COLUMN_GUID, delete_keys
             , VARIABLE_GAMEOBJECT_GUID, gameobject_base
             );

    if (!_gameobjects.empty())
    {
      std::vector<std::string> const columns
        { "guid", "id", "map", "spawnMask", "phaseMask"
        , "position_x", "position_y", "position_z", "orientation"
        , "rotation0", "rotation1", "rotation2", "rotation3"
        , "spawntimesecs", "animprogress", "state"
        };

      std::vector<std::vector<std::string>> rows;
      std::string notes;

      for (GameObjectSpawn const& spawn : _gameobjects)
      {
        for (ValidationIssue const& issue : SpawnValidation::validate(spawn))
        {
          notes += std::string("-- ") + severityLabel(issue.severity) + " (guid "
                 + unsignedText(spawn.guid) + "): " + issue.message + "\n";
        }

        std::string const disagreement (rotationDisagreement(spawn));

        if (!disagreement.empty())
        {
          notes += "-- WARNING (guid " + unsignedText(spawn.guid) + "): " + disagreement + "\n";
        }

        TileCoordinates::Quaternion const rotation (resolvedRotation(spawn));

        rows.push_back
          ( { variableExpression(VARIABLE_GAMEOBJECT_GUID, gameobject_base, spawn.guid)
            , unsignedText(spawn.id)
            , unsignedText(spawn.map)
            , unsignedText(spawn.spawn_mask)
            , unsignedText(spawn.phase_mask)
            , SqlFormat::coordinate(spawn.position.x)
            , SqlFormat::coordinate(spawn.position.y)
            , SqlFormat::coordinate(spawn.position.z)
            , SqlFormat::coordinate(TileCoordinates::normaliseOrientation(spawn.orientation))
            // Not coordinate(): the quaternion components live where |v| <= 1, and six decimals
            // there is six significant digits -- enough slack to rewrite the stored bytes of a
            // gameobject this changeset never edited.
            , SqlFormat::rotationComponent(rotation.r0)
            , SqlFormat::rotationComponent(rotation.r1)
            , SqlFormat::rotationComponent(rotation.r2)
            , SqlFormat::rotationComponent(rotation.r3)
            , signedText(spawn.spawn_time_secs)
            , unsignedText(spawn.anim_progress)
            , unsignedText(spawn.state)
            }
          );
      }

      out << notes << insertStatement(TABLE_GAMEOBJECT, columns, rows);
    }
  }

  // --- waypoint path ---------------------------------------------------------------------
  // Only paths this changeset actually authors are cleared. A creature whose path id was
  // derived but whose nodes were not supplied keeps the path it already has.

  if (!_paths.empty() || !_removed_paths.empty())
  {
    std::string const waypoint_table (_schema.waypointPathTable());

    out << "\n" << sectionHeader(waypoint_table);

    std::vector<std::uint64_t> delete_keys;

    for (WaypointPath const& path : _paths)
    {
      delete_keys.push_back(path.id);
    }

    for (std::uint32_t path_id : _removed_paths)
    {
      delete_keys.push_back(path_id);
    }

    out << deleteStatement(waypoint_table, COLUMN_ID, delete_keys, VARIABLE_PATH, path_base);

    // `point` is 1-based and must stay contiguous: the core walks the nodes in order and a gap
    // truncates the path silently rather than erroring. The core-managed guid column is never
    // named here -- authoring it corrupts the path.
    std::vector<std::string> const columns
      { "id", "point", "position_x", "position_y", "position_z", "orientation"
      , "delay", "move_type", "action", "action_chance"
      };

    std::vector<std::vector<std::string>> rows;
    std::string notes;

    for (WaypointPath const& path : _paths)
    {
      for (ValidationIssue const& issue : SpawnValidation::validate(path))
      {
        notes += std::string("-- ") + severityLabel(issue.severity) + " (path "
               + unsignedText(path.id) + "): " + issue.message + "\n";
      }

      for (WaypointNode const& node : path.nodes)
      {
        rows.push_back
          ( { variableExpression(VARIABLE_PATH, path_base, path.id)
            , unsignedText(node.point)
            , SqlFormat::coordinate(node.position.x)
            , SqlFormat::coordinate(node.position.y)
            , SqlFormat::coordinate(node.position.z)
            , node.has_orientation
                ? SqlFormat::coordinate
                    (TileCoordinates::normaliseOrientation(node.orientation))
                : std::string("NULL")
            , unsignedText(node.delay_ms)
            , unsignedText(static_cast<std::uint32_t>(node.move_type))
            , unsignedText(node.action)
            , unsignedText(node.action_chance)
            }
          );
      }
    }

    if (!rows.empty())
    {
      out << notes << insertStatement(waypoint_table, columns, rows);
    }
    else
    {
      out << notes;
    }
  }

  std::string const sql (out.str());

  // Verify every column this file actually names exists on the target, by reading back the SQL
  // just produced rather than consulting a hand-maintained list. A manifest would drift from
  // the emit sites; the output cannot.
  //
  // This matters because the READ path is deliberately more tolerant than the write path:
  // SpawnQuery treats a dozen columns as optional and substitutes defaults when they are
  // absent, so a schema it loads happily can still be one this emitter names columns for. Left
  // unchecked that surfaces as ERROR 1054 part-way through applying the file, after the DELETE
  // statements at the top have already committed -- data removed and nothing written back.
  requireEmittedColumnsExist(sql, _schema);

  return sql;
}
