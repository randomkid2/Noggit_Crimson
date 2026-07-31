// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/GameTeleBuilder.hpp>

#include <noggit/database/ChangesetBuilder.hpp>

#include <cctype>
#include <sstream>

using namespace Noggit::Database;

namespace
{
  constexpr double DEGREES_TO_RADIANS = 3.14159265358979323846 / 180.0;

  // SqlFormat::quote escapes the BODY of a literal and deliberately omits the surrounding quotes,
  // so that callers can assemble one. Wrapping it here keeps that decision in one place rather
  // than at each of the three sites below.
  std::string quoted(std::string const& value)
  {
    return "'" + SqlFormat::quote(value) + "'";
  }

  // `.tele <name>` takes the name as a single argument, so a name the command cannot express is
  // worse than no row: it sits in the table looking correct and can never be used.
  bool isUsableTeleName(std::string const& name)
  {
    if (name.empty())
    {
      return false;
    }

    for (unsigned char c : name)
    {
      if (std::isspace(c))
      {
        return false;
      }
    }

    return true;
  }
}

double GameTele::orientationFromCameraYaw(double camera_yaw_degrees)
{
  // The 180 degree turn is derived in the header. Normalised through the same helper the
  // changeset emitter uses, so a tele and a spawn written from the same camera agree.
  return TileCoordinates::normaliseOrientation((camera_yaw_degrees + 180.0) * DEGREES_TO_RADIANS);
}

GameTele::Result GameTele::build(std::vector<Entry> const& entries)
{
  Result result;

  std::ostringstream out;

  out << "-- Noggit game_tele export. Review before applying.\n"
      << "--\n"
      << "-- Each destination is DELETEd by name before insert, so re-exporting a bookmark moves\n"
      << "-- it rather than leaving a second row that `.tele` would match ambiguously.\n"
      << "--\n"
      << "-- Ids are allocated from MAX(id) at apply time. game_tele.id has no reserved range for\n"
      << "-- custom rows, so a literal id chosen when this file was written would collide with\n"
      << "-- whatever the target database already holds.\n"
      << "--\n"
      << "--   Apply:  mysql <world> -e \"source <this file>\"\n"
      << "\n";

  std::vector<Entry> usable;

  for (auto const& entry : entries)
  {
    if (!isUsableTeleName(entry.name))
    {
      result.skipped.push_back
        ( entry.name.empty()
            ? std::string("(unnamed bookmark) - a game_tele name cannot be empty")
            : entry.name + " - a game_tele name cannot contain spaces, `.tele` takes one argument"
        );
      continue;
    }

    usable.push_back(entry);
  }

  if (usable.empty())
  {
    out << "-- Nothing to export.\n";
    result.sql = out.str();
    return result;
  }

  for (auto const& entry : usable)
  {
    out << "DELETE FROM `game_tele` WHERE `name` = " << quoted(entry.name) << ";\n";
  }

  out << "\nSET @TELE := (SELECT IFNULL(MAX(`id`), 0) FROM `game_tele`);\n\n"
      << "INSERT INTO `game_tele`\n"
      << "  (`id`, `position_x`, `position_y`, `position_z`, `orientation`, `map`, `name`) VALUES\n";

  for (std::size_t i = 0; i < usable.size(); ++i)
  {
    Entry const& entry = usable[i];

    out << "  (@TELE + " << (i + 1) << ", "
        << SqlFormat::coordinate(entry.position.x) << ", "
        << SqlFormat::coordinate(entry.position.y) << ", "
        << SqlFormat::coordinate(entry.position.z) << ", "
        << SqlFormat::coordinate(TileCoordinates::normaliseOrientation(entry.orientation)) << ", "
        << entry.map << ", "
        << quoted(entry.name) << ")"
        << (i + 1 == usable.size() ? ";\n" : ",\n");
  }

  result.sql = out.str();
  result.emitted = usable.size();

  return result;
}
