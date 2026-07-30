// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_GMCOMMANDS_HPP
#define NOGGIT_DATABASE_GMCOMMANDS_HPP

#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace Noggit::Database
{
  // The clipboard seam between the editor and a running worldserver. Two directions, both text:
  //
  //   * emission -- "put my character where the editor is looking", as TrinityCore's
  //     `.go xyz #x #y #z [#mapid [#orientation]]`.
  //   * parsing -- a coordinate copied out of a chat window, a bug report, a wiki page or a
  //     worldserver `.gps` dump, pasted straight into the editor.
  //
  // Pure text and arithmetic: no database, no filesystem, no Qt.
  //
  // The two directions are deliberately asymmetric. Emission produces exactly one shape;
  // parsing accepts many, because the input is whatever a human happened to have on the
  // clipboard. What parsing never does is invent a field. A missing or unreadable coordinate is
  // a rejection, not a default -- an assumed z of 0 buries a spawn under the terrain, and an
  // assumed map 0 moves it to Azeroth, and neither failure is visible until someone flies out
  // to look.

  class GmCommandError : public std::runtime_error
  {
    public:
      explicit GmCommandError(std::string const& message)
        : std::runtime_error(message) {}
  };

  // Decimals written per coordinate.
  //
  // Six is past what the destination can hold: `position_x/y/z` and `orientation` are FLOAT, and
  // the representable step of a float near the +-17000 edge of the map is roughly 2e-3 yards. So
  // the text carries more digits than the column does and the worst-case rounding error
  // introduced by formatting is 5e-7 yards.
  constexpr int GM_COORDINATE_DECIMALS = 6;

  // The round-trip guarantee: parseCoordinate(teleportCommand(map, position, orientation))
  // recovers `map` exactly, and the position and the NORMALISED orientation to within this.
  //
  // It is the 5e-7 formatting error above with an order of magnitude of headroom, and it is
  // still three orders tighter than the float step of the columns these numbers end up in.
  // Coordinates are never compared by exact equality -- see docs/schema-335.md, "Float
  // precision".
  constexpr double GM_ROUND_TRIP_TOLERANCE = 1.0e-6;

  // Largest map id ParsedCoordinate::map can carry, which is also the width of the `map` column:
  // smallint unsigned.
  constexpr std::uint32_t GM_MAX_MAP_ID = 0xFFFF;

  // A coordinate recovered from text.
  //
  // `map` and `orientation` mean nothing unless their has_* flag is set. Most pasted text is a
  // bare position, and a caller that reads either field unconditionally gets a plausible zero
  // rather than an error: map 0 is Azeroth and orientation 0 is facing north, both of which are
  // real answers to a question the text never asked.
  struct ParsedCoordinate
  {
    std::uint16_t map = 0;
    WorldPosition position;
    bool has_orientation = false;
    double orientation = 0.0;
    bool has_map = false;
  };

  // `.go xyz <x> <y> <z> <mapId> <orientation>`.
  //
  // The map and the orientation are always written even though the command treats both as
  // optional, because `.go xyz x y z` teleports within whatever map the GM is currently
  // standing in -- a command copied while inside an instance lands at the right numbers in the
  // wrong world.
  //
  // The orientation is normalised into [0, 2*pi) first, so the number pasted at the server is
  // the same one the `orientation` column would hold; the core normalises anyway, and a value
  // that changed on the way in reads as an edit nobody made.
  //
  // Coordinates are written with up to GM_COORDINATE_DECIMALS decimals, trailing zeros trimmed,
  // because a person has to read this and check it: "-9500 70 58" is verifiable at a glance and
  // "-9500.000000 70.000000 58.000000" is not. Trimming removes only digits the fixed form had
  // already rounded to zero.
  //
  // Throws GmCommandError on a non-finite coordinate. A NaN position means the value was
  // computed from bad input; "nan" is not a number the command parser accepts, and substituting
  // anything for it would silently teleport the user somewhere real.
  std::string teleportCommand
    (std::uint16_t map, WorldPosition const& position, double orientation);

  // The command that puts the GM at a spawn, ready to compare the editor against the game.
  std::string goCommandFor(CreatureSpawn const& spawn);
  std::string goCommandFor(GameObjectSpawn const& spawn);

  // Reads a coordinate out of arbitrary pasted text. Accepted shapes, all measured against what
  // people actually paste:
  //
  //   .go xyz -9500 70 58 0 4.71      the full command
  //   .go -9500 70 58                 the command without the xyz subcommand word
  //   -9500 70 58                     bare numbers
  //   -9500, 70, 58                   bare numbers, comma separated
  //   x: -9500 y: 70 z: 58            labelled, in any order, with ':' or '='
  //   a multi-line `.gps` block       labelled, buried in prose the parser has to ignore
  //
  // Returns nothing when the text cannot be read confidently: fewer than three numbers, more
  // than five, a non-numeric field, a map id that is not a map id, or an unrecognised leading
  // word. Partial input is a rejection and never a partial answer.
  //
  // Two numbers are a rejection even though `.go xyz #x #y` is a legal command in game: there the
  // server drops the character onto the terrain height it looks up itself, and this module has no
  // terrain. Reading such a paste would mean inventing a z, and a z of 0 is under the ground
  // almost everywhere in the world.
  //
  // The position is NOT checked against the world extent. Reading text and judging geography
  // are different questions, and a coordinate on a transport or just off the grid edge is still
  // a coordinate the user asked to be read -- TileCoordinates::isValidPosition is how a caller
  // asks the other question. A map id, by contrast, IS validated: it is an identifier rather
  // than a measurement, so a fractional or out-of-range value is a misread field, not a value
  // to be rounded.
  //
  // Neither reading nor writing consults the global locale. Both are immune to an LC_NUMERIC
  // that uses a comma decimal separator, which would otherwise turn one coordinate into two.
  std::optional<ParsedCoordinate> parseCoordinate(std::string const& text);
}

#endif
