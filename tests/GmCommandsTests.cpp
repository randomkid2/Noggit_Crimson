// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <noggit/database/GmCommands.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <locale>
#include <optional>
#include <string>

using namespace Noggit::Database;
using Catch::Approx;

namespace
{
  constexpr double TWO_PI = 6.283185307179586476925286766559;

  // The anchor coordinate of the whole layer: creatures near x = -9500, y = 70 on map 0 are in
  // Elwynn Forest, tile (49, 31). Everything else in this file is text handling; this is the one
  // number that is ground truth.
  constexpr double ELWYNN_X = -9500.0;
  constexpr double ELWYNN_Y = 70.0;
  constexpr double ELWYNN_Z = 58.0;

  WorldPosition makePosition(double x, double y, double z)
  {
    WorldPosition position;
    position.x = x;
    position.y = y;
    position.z = z;

    return position;
  }

  TileIndex makeTile(int x, int y)
  {
    TileIndex tile;
    tile.x = x;
    tile.y = y;

    return tile;
  }

  WorldPosition elwynn()
  {
    return makePosition(ELWYNN_X, ELWYNN_Y, ELWYNN_Z);
  }

  // Restores the previous global locale on the way out, so a failing assertion inside the locale
  // test cannot leave the rest of the suite -- including Catch2's own reporting -- running under
  // a comma decimal separator.
  class ScopedGlobalLocale
  {
    public:
      explicit ScopedGlobalLocale(std::locale const& locale)
        : _previous (std::locale::global(locale))
      {
      }

      ~ScopedGlobalLocale()
      {
        std::locale::global(_previous);
      }

      ScopedGlobalLocale(ScopedGlobalLocale const&) = delete;
      ScopedGlobalLocale& operator=(ScopedGlobalLocale const&) = delete;

    private:
      std::locale _previous;
  };

  // A locale whose decimal separator is a comma, or nothing when this machine has none installed.
  //
  // The spelling differs between the Windows and POSIX runtimes and neither is guaranteed to be
  // present, so the test that uses this degrades to the weaker "contains '.', contains no ','"
  // assertion rather than failing on a machine with a minimal locale set.
  std::optional<std::locale> commaDecimalLocale()
  {
    char const* const names[] = {"de-DE", "German_Germany.1252", "de_DE.UTF-8", "de_DE"};

    for (char const* name : names)
    {
      try
      {
        std::locale const candidate (name);

        if (std::use_facet<std::numpunct<char>>(candidate).decimal_point() == ',')
        {
          return candidate;
        }
      }
      catch (std::exception const&)
      {
        // Not installed under that name here. Try the next spelling.
      }
    }

    return std::nullopt;
  }
}

TEST_CASE("the emitted command is the worldserver .go xyz form", "[gm][format]")
{
  // Exact text, not a pattern. This string is pasted into a chat box by a person, so its shape is
  // part of the contract: whole numbers carry no decimal point, and the map and the orientation
  // are always present even though the command treats both as optional.
  CHECK(teleportCommand(0, elwynn(), 4.71) == ".go xyz -9500 70 58 0 4.71");
  CHECK(teleportCommand(571, makePosition(5807.05, 640.5, 647.125), 0.0)
        == ".go xyz 5807.05 640.5 647.125 571 0");

  // Six decimals are kept and trailing zeros are not.
  CHECK(teleportCommand(0, makePosition(-9500.123456, 70.100000, 58.000001), 1.5)
        == ".go xyz -9500.123456 70.1 58.000001 0 1.5");

  // A `.go xyz x y z` without the map teleports within whatever map the GM is standing in, so
  // the map is never omitted -- not even map 0, which is the one a caller would be tempted to
  // treat as "no map".
  CHECK(teleportCommand(0, elwynn(), 0.0) == ".go xyz -9500 70 58 0 0");
}

TEST_CASE("emission normalises the orientation the way the core stores it", "[gm][format]")
{
  WorldPosition const position (elwynn());
  std::string const facing_north (".go xyz -9500 70 58 0 0");

  // The core folds an orientation into [0, 2*pi) on load. Emitting the raw value would produce a
  // command whose stored result differs from the number in the text, and a later round trip
  // through this module would then report a change nobody made.
  CHECK(teleportCommand(0, position, TWO_PI) == facing_north);
  CHECK(teleportCommand(0, position, -TWO_PI) == facing_north);
  CHECK(teleportCommand(0, position, 4.0 * TWO_PI) == facing_north);

  // -1.0 is three quarters of a turn the other way, which is 2*pi - 1 forwards.
  CHECK(teleportCommand(0, position, -1.0) == ".go xyz -9500 70 58 0 5.283185");
  CHECK(teleportCommand(0, position, TWO_PI + 1.0) == ".go xyz -9500 70 58 0 1");
}

TEST_CASE("emission never produces a minus zero", "[gm][format]")
{
  // "-0" is legal and the server accepts it, but it reads as a bug to whoever is checking the
  // command and it does not match the "0" the same position produces once stored. Every value
  // between -1e-7 and -0.0 formats to "-0.000000" before the sign is dropped, and a negative
  // whole turn arrives at the formatter as -0.0.
  std::string const all_zero
    (teleportCommand(0, makePosition(-0.0, -1.0e-9, 0.0), -1.0e-18));

  CHECK(all_zero == ".go xyz 0 0 0 0 0");
  CHECK(all_zero.find('-') == std::string::npos);

  CHECK(teleportCommand(0, makePosition(-4.0e-7, -0.0, -0.0), -TWO_PI)
        == ".go xyz 0 0 0 0 0");

  // A value that survives rounding keeps its sign, so the rule above is not just "strip minus
  // signs from small numbers".
  CHECK(teleportCommand(0, makePosition(-1.0e-6, -0.5, -1.0), 0.0)
        == ".go xyz -0.000001 -0.5 -1 0 0");
}

TEST_CASE("a non-finite coordinate is refused rather than emitted", "[gm][format][safety]")
{
  double const not_a_number (std::numeric_limits<double>::quiet_NaN());
  double const infinity (std::numeric_limits<double>::infinity());

  // "nan" is not a number the command parser accepts, and substituting any finite value would
  // silently teleport the user somewhere real.
  CHECK_THROWS_AS(teleportCommand(0, makePosition(not_a_number, 70.0, 58.0), 0.0), GmCommandError);
  CHECK_THROWS_AS(teleportCommand(0, makePosition(-9500.0, infinity, 58.0), 0.0), GmCommandError);
  CHECK_THROWS_AS(teleportCommand(0, makePosition(-9500.0, 70.0, -infinity), 0.0), GmCommandError);

  // The orientation is different: normaliseOrientation folds a NaN or an infinity to zero, so it
  // can never reach the formatter non-finite. Facing north is a defensible answer to "no facing";
  // a position has no such fallback.
  CHECK_NOTHROW(teleportCommand(0, elwynn(), not_a_number));
  CHECK(teleportCommand(0, elwynn(), not_a_number) == ".go xyz -9500 70 58 0 0");
  CHECK(teleportCommand(0, elwynn(), infinity) == ".go xyz -9500 70 58 0 0");
}

TEST_CASE("goCommandFor(CreatureSpawn) uses the spawn's own map and facing", "[gm][spawn]")
{
  CreatureSpawn spawn;
  spawn.guid = 500000;
  spawn.id = 448;
  spawn.map = 571;
  spawn.position = makePosition(5807.05, 640.5, 647.125);
  spawn.orientation = 4.71;

  CHECK(goCommandFor(spawn) == ".go xyz 5807.05 640.5 647.125 571 4.71");
  CHECK(goCommandFor(spawn) == teleportCommand(spawn.map, spawn.position, spawn.orientation));

  // An un-normalised orientation on the struct still emits the stored facing.
  spawn.orientation = -1.0;
  CHECK(goCommandFor(spawn) == ".go xyz 5807.05 640.5 647.125 571 5.283185");
}

TEST_CASE("goCommandFor(GameObjectSpawn) trusts the quaternion over the column", "[gm][spawn]")
{
  GameObjectSpawn spawn;
  spawn.guid = 300000;
  spawn.id = 179697;
  spawn.map = 0;
  spawn.position = elwynn();
  spawn.orientation = 1.0;

  // A default-constructed rotation is the identity, which is indistinguishable from "nobody set
  // this", so the orientation column is what is used.
  CHECK(goCommandFor(spawn) == teleportCommand(0, spawn.position, 1.0));

  // With a rotation actually supplied, the quaternion wins: it is what the core faces the object
  // by, so it is what the GM should be looking along. Sending 1.0 here would point the camera a
  // quarter of a circle away from the object's real facing.
  spawn.rotation = TileCoordinates::quaternionForOrientation(3.0);

  std::optional<ParsedCoordinate> const from_quaternion (parseCoordinate(goCommandFor(spawn)));

  REQUIRE(from_quaternion.has_value());
  CHECK(from_quaternion->has_orientation);
  CHECK(from_quaternion->orientation == Approx(3.0).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(goCommandFor(spawn) != teleportCommand(0, spawn.position, 1.0));

  // q and -q are the same rotation, and rows written by different tools carry both signs. The
  // command must not change when only the sign does.
  std::string const positive (goCommandFor(spawn));

  spawn.rotation.r0 = -spawn.rotation.r0;
  spawn.rotation.r1 = -spawn.rotation.r1;
  spawn.rotation.r2 = -spawn.rotation.r2;
  spawn.rotation.r3 = -spawn.rotation.r3;

  CHECK(goCommandFor(spawn) == positive);
}

TEST_CASE("every shape a user realistically pastes is accepted", "[gm][parse]")
{
  struct Shape
  {
    char const* text;
    bool has_map;
    bool has_orientation;
  };

  Shape const shapes[] =
    { {".go xyz -9500 70 58 0 4.71", true, true}
    , {".go -9500 70 58", false, false}
    , {"-9500 70 58", false, false}
    , {"-9500, 70, 58", false, false}
    , {"x: -9500 y: 70 z: 58", false, false}
    , {"X: -9500 Y: 70 Z: 58 Map: 0", true, false}
    // Shapes past the required six, all of which arrive from real clipboards.
    , {"  .GO XYZ -9500 70 58 0 4.71  ", true, true}
    , {"go xyz -9500 70 58 0 4.71", true, true}
    , {"-9500\t70\t58\r\n", false, false}
    , {"-9500;70;58", false, false}
    , {".tele -9500 70 58 0", true, false}
    };

  for (Shape const& shape : shapes)
  {
    std::optional<ParsedCoordinate> const parsed (parseCoordinate(shape.text));

    CAPTURE(shape.text);
    REQUIRE(parsed.has_value());

    CHECK(parsed->position.x == Approx(ELWYNN_X).margin(GM_ROUND_TRIP_TOLERANCE));
    CHECK(parsed->position.y == Approx(ELWYNN_Y).margin(GM_ROUND_TRIP_TOLERANCE));
    CHECK(parsed->position.z == Approx(ELWYNN_Z).margin(GM_ROUND_TRIP_TOLERANCE));
    CHECK(parsed->has_map == shape.has_map);
    CHECK(parsed->has_orientation == shape.has_orientation);

    // Map 0 either way: read from the text, or left at its default. Which of the two happened is
    // exactly what has_map is for, and a caller that ignores it cannot tell Azeroth from
    // "unspecified".
    CHECK(parsed->map == 0);

    // The parse is only worth anything if the result lands where the coordinate really is.
    CHECK(TileCoordinates::tileForPosition(parsed->position) == makeTile(49, 31));
  }
}

TEST_CASE("a worldserver .gps block is read past its own decoys", "[gm][parse]")
{
  // Shaped after the real LANG_MAP_POSITION message. Three separate traps live in it:
  //
  //   * "Map:" appears TWICE. Once as the map id, and again at the end inside
  //     "Have height data (Map: 1 VMap: 1)", where it is a boolean and not a map at all.
  //   * "ZoneX:" and "ZoneY:" are zone-local coordinates, in a different frame entirely.
  //   * "GroundZ:" and "FloorZ:" are terrain heights near the position, not its z.
  //
  // A suffix or last-wins reading of any of the three answers with a plausible number from the
  // wrong field, which is worse than refusing.
  std::string const block
    ( "Map: 0 (Eastern Kingdoms) Zone: 12 (Elwynn Forest) Area: 9 (Northshire Valley) Phase: 1\n"
      "X: -9500 Y: 70 Z: 58 Orientation: 4.71\n"
      "grid[49:31]cell[4:4] InstanceID: 0\n"
      "ZoneX: 1234.5 ZoneY: 678.9\n"
      "GroundZ: 57.75 FloorZ: 57.9 Have height data (Map: 1 VMap: 1)\n"
    );

  std::optional<ParsedCoordinate> const parsed (parseCoordinate(block));

  REQUIRE(parsed.has_value());

  CHECK(parsed->has_map);
  CHECK(parsed->map == 0);

  CHECK(parsed->position.x == Approx(ELWYNN_X).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->position.y == Approx(ELWYNN_Y).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->position.z == Approx(ELWYNN_Z).margin(GM_ROUND_TRIP_TOLERANCE));

  CHECK(parsed->has_orientation);
  CHECK(parsed->orientation == Approx(4.71).margin(GM_ROUND_TRIP_TOLERANCE));

  CHECK(TileCoordinates::tileForPosition(parsed->position) == makeTile(49, 31));

  // Spelled out as rejections of the specific wrong answers, so a regression names itself.
  CHECK(parsed->map != 1);
  CHECK(parsed->position.y != Approx(678.9).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->position.z != Approx(57.75).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->position.z != Approx(57.9).margin(GM_ROUND_TRIP_TOLERANCE));
}

TEST_CASE("labels are read in any order, in any case, with ':' or '='", "[gm][parse]")
{
  std::optional<ParsedCoordinate> const parsed
    (parseCoordinate("Z=58 Y = 70 X=-9500 MAP_ID = 571 Ori=1.5"));

  REQUIRE(parsed.has_value());

  CHECK(parsed->position.x == Approx(ELWYNN_X).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->position.y == Approx(ELWYNN_Y).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->position.z == Approx(ELWYNN_Z).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->has_map);
  CHECK(parsed->map == 571);
  CHECK(parsed->has_orientation);
  CHECK(parsed->orientation == Approx(1.5).margin(GM_ROUND_TRIP_TOLERANCE));

  // A labelled orientation is normalised like an emitted one, so both directions agree about
  // what the core would store.
  std::optional<ParsedCoordinate> const wrapped
    (parseCoordinate("x: 0 y: 0 z: 0 orientation: -1.0"));

  REQUIRE(wrapped.has_value());
  CHECK(wrapped->has_orientation);
  CHECK(wrapped->orientation == Approx(TWO_PI - 1.0).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(wrapped->orientation >= 0.0);
  CHECK(wrapped->orientation < TWO_PI);
}

TEST_CASE("a positional reading follows the command's argument order", "[gm][parse]")
{
  // `.go xyz #x #y #z [#mapid [#orientation]]`. The FOURTH number is the map, not the
  // orientation: the two readings differ by a whole world, and the command's own signature is
  // the only evidence a four-number paste carries.
  std::optional<ParsedCoordinate> const three (parseCoordinate("-9500 70 58"));
  std::optional<ParsedCoordinate> const four (parseCoordinate("-9500 70 58 571"));
  std::optional<ParsedCoordinate> const five (parseCoordinate("-9500 70 58 571 4.71"));

  REQUIRE(three.has_value());
  REQUIRE(four.has_value());
  REQUIRE(five.has_value());

  CHECK_FALSE(three->has_map);
  CHECK_FALSE(three->has_orientation);

  CHECK(four->has_map);
  CHECK(four->map == 571);
  CHECK_FALSE(four->has_orientation);

  CHECK(five->has_map);
  CHECK(five->map == 571);
  CHECK(five->has_orientation);
  CHECK(five->orientation == Approx(4.71).margin(GM_ROUND_TRIP_TOLERANCE));

  // All three read the same position, so the extra fields are additive and never shift the
  // coordinates along.
  CHECK(three->position.x == Approx(four->position.x).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(four->position.z == Approx(five->position.z).margin(GM_ROUND_TRIP_TOLERANCE));
}

TEST_CASE("negative and zero coordinates survive parsing", "[gm][parse]")
{
  std::optional<ParsedCoordinate> const origin (parseCoordinate("0 0 0"));

  REQUIRE(origin.has_value());
  CHECK(origin->position.x == 0.0);
  CHECK(origin->position.y == 0.0);
  CHECK(origin->position.z == 0.0);
  CHECK(TileCoordinates::tileForPosition(origin->position) == makeTile(32, 32));

  std::optional<ParsedCoordinate> const all_negative
    (parseCoordinate("-9500.25, -70.5, -58.125, 0, 3.5"));

  REQUIRE(all_negative.has_value());
  CHECK(all_negative->position.x == Approx(-9500.25).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(all_negative->position.y == Approx(-70.5).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(all_negative->position.z == Approx(-58.125).margin(GM_ROUND_TRIP_TOLERANCE));

  // A pasted "-0" parses to a negative zero, which is arithmetically harmless but must not
  // survive back out into text as "-0".
  std::optional<ParsedCoordinate> const negative_zero (parseCoordinate("-0 -0 -0"));

  REQUIRE(negative_zero.has_value());
  CHECK(negative_zero->position.x == 0.0);
  CHECK(teleportCommand(0, negative_zero->position, 0.0) == ".go xyz 0 0 0 0 0");

  // Explicit exponents and a bare fraction, both of which a spreadsheet produces.
  std::optional<ParsedCoordinate> const exponents (parseCoordinate("-9.5005e3 7.0e1 .5"));

  REQUIRE(exponents.has_value());
  CHECK(exponents->position.x == Approx(-9500.5).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(exponents->position.y == Approx(70.0).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(exponents->position.z == Approx(0.5).margin(GM_ROUND_TRIP_TOLERANCE));
}

TEST_CASE("text that cannot be read confidently is refused", "[gm][parse][safety]")
{
  char const* const rejected[] =
    { ""
    , "   "
    , "\n\n"
    , "hello world"
    , "-9500"                             // one number
    , "-9500 70"                          // two: a missing z is not a zero
    , ".go xyz -9500 70"                  // legal in game, where the server finds the ground
    , "x: -9500 y: 70"                    // labelled, still missing z
    , "x: -9500 z: 58"
    , "Map: 0"                            // a map and no position
    , "Orientation: 4.71"
    , ".go xyz"                           // the command with no arguments
    , ".go xyz -9500 70 58 0 4.71 12"     // six numbers: one field too many to place
    , "-9500 70 abc"
    , "-9500 70 58abc"
    , "-9500 70 0x10"
    , "nan 70 58"
    , "inf 70 58"
    , "1e400 70 58"                       // reads as a number, is not a representable one
    , "-9500 70 58e"
    , "--9500 70 58"
    , "-9500,,70,58"                      // the empty field means something was lost in the copy
    , "-9500-70-58"
    , "-9500 70 58)"
    , ".summon Hogger"
    , ".go zonexy 49 31"
    , "ZoneX: 1 ZoneY: 2 GroundZ: 3"      // labels that only look like the ones that matter
    , "the body was at -9500 70 58"       // three numbers in a sentence is not a coordinate
    };

  for (char const* text : rejected)
  {
    CAPTURE(text);
    CHECK_FALSE(parseCoordinate(text).has_value());
  }
}

TEST_CASE("an unreadable map id is refused rather than dropped", "[gm][parse][safety]")
{
  // The map is the field a user is most explicit about. Answering with the position alone would
  // land them at the right numbers on whichever map the editor happened to have open, so a map
  // that cannot be read rejects the whole parse.
  char const* const rejected[] =
    { "-9500 70 58 -1"
    , "-9500 70 58 65536"
    , "-9500 70 58 70000 4.71"
    , "-9500 70 58 0.5"
    , "X: -9500 Y: 70 Z: 58 Map: -1"
    , "X: -9500 Y: 70 Z: 58 Map: 100000"
    , "X: -9500 Y: 70 Z: 58 Map: 1.5"
    };

  for (char const* text : rejected)
  {
    CAPTURE(text);
    CHECK_FALSE(parseCoordinate(text).has_value());
  }

  // Both ends of the representable range are fine, and a map written with a redundant decimal
  // point is still an integer.
  std::optional<ParsedCoordinate> const lowest (parseCoordinate("-9500 70 58 0"));
  std::optional<ParsedCoordinate> const highest (parseCoordinate("-9500 70 58 65535"));
  std::optional<ParsedCoordinate> const decimal_point (parseCoordinate("-9500 70 58 571.0"));

  REQUIRE(lowest.has_value());
  REQUIRE(highest.has_value());
  REQUIRE(decimal_point.has_value());

  CHECK(lowest->has_map);
  CHECK(lowest->map == 0);
  CHECK(highest->map == static_cast<std::uint16_t>(GM_MAX_MAP_ID));
  CHECK(decimal_point->map == 571);
}

TEST_CASE("a command round-trips through the parser", "[gm][roundtrip]")
{
  struct Sample
  {
    std::uint16_t map;
    double x;
    double y;
    double z;
    double orientation;
  };

  Sample const samples[] =
    { {0, ELWYNN_X, ELWYNN_Y, ELWYNN_Z, 4.71}
    , {0, -9528.7, 92.5, 56.187, 0.0}
    , {1, 1629.36, -4373.39, 31.2564, 3.14159}
    , {571, 5807.05, 640.5, 647.1, 6.2}
    , {0, 0.0, 0.0, 0.0, 0.0}
    , {0, MAP_HALF_EXTENT, -MAP_HALF_EXTENT, -500.25, 1.0}
    , {65535, -0.000001, 0.000001, -12345.678901, 2.0}
    , {0, ELWYNN_X, ELWYNN_Y, ELWYNN_Z, -1.0}          // normalised on the way out
    // Everything in this one folds to zero on the way out. The signs are negative on purpose:
    // tile ownership is (min, max] with the axis running BACKWARDS, so a coordinate 1e-9 on the
    // POSITIVE side of the origin belongs to tile 31 while 0 belongs to tile 32, and the tile
    // assertion below would fail for a reason that has nothing to do with this module. That case
    // has its own test case.
    , {0, -1.0e-9, -1.0e-9, -0.0, TWO_PI}
    };

  for (Sample const& sample : samples)
  {
    WorldPosition const position (makePosition(sample.x, sample.y, sample.z));
    std::string const command (teleportCommand(sample.map, position, sample.orientation));
    std::optional<ParsedCoordinate> const parsed (parseCoordinate(command));

    CAPTURE(command);
    REQUIRE(parsed.has_value());

    // The map is an integer and survives exactly. Nothing else does, and nothing else is
    // compared by equality -- see docs/schema-335.md on float precision.
    CHECK(parsed->has_map);
    CHECK(parsed->map == sample.map);

    CHECK(parsed->position.x == Approx(sample.x).margin(GM_ROUND_TRIP_TOLERANCE));
    CHECK(parsed->position.y == Approx(sample.y).margin(GM_ROUND_TRIP_TOLERANCE));
    CHECK(parsed->position.z == Approx(sample.z).margin(GM_ROUND_TRIP_TOLERANCE));

    CHECK(parsed->has_orientation);
    CHECK(parsed->orientation
          == Approx(TileCoordinates::normaliseOrientation(sample.orientation))
               .margin(GM_ROUND_TRIP_TOLERANCE));

    // The recovered position is in the same tile as the one that was emitted. Tolerance alone
    // would not catch a formatting bug that moved a coordinate across a tile edge.
    CHECK(TileCoordinates::tileForPosition(parsed->position)
          == TileCoordinates::tileForPosition(position));
  }
}

TEST_CASE("a coordinate a nanometre past a tile edge does not keep its tile", "[gm][roundtrip]")
{
  // Six decimals is past FLOAT resolution at world magnitudes, but it is not infinite, and the
  // origin is a tile edge. Tile ownership is the half-open interval (min, max] with the axis
  // running backwards, so ANY positive x is inside tile 31 while x = 0 itself belongs to tile 32.
  // A position 1e-9 on the positive side therefore formats to "0" and comes back one tile over.
  //
  // Asserted rather than avoided, because it is a property of a text command and not a defect:
  // the columns these numbers end up in are FLOAT, whose representable step is roughly 1e-3 at
  // world magnitudes, so no database on the far end could hold such a position either. Anyone
  // later tempted to treat a GM command as a tile-precise transport should trip over this.
  WorldPosition const just_past_the_edge (makePosition(1.0e-9, 1.0e-9, 0.0));

  CHECK(TileCoordinates::tileForPosition(just_past_the_edge) == makeTile(31, 31));

  std::string const command (teleportCommand(0, just_past_the_edge, 0.0));

  CHECK(command == ".go xyz 0 0 0 0 0");

  std::optional<ParsedCoordinate> const parsed (parseCoordinate(command));

  REQUIRE(parsed.has_value());

  // The position is still recovered to within the module's stated tolerance -- 1e-9 is three
  // orders inside it. It is the TILE that moves, because the tolerance straddles an edge.
  CHECK(parsed->position.x == Approx(1.0e-9).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(TileCoordinates::tileForPosition(parsed->position) == makeTile(32, 32));
  CHECK_FALSE(TileCoordinates::tileForPosition(parsed->position)
              == TileCoordinates::tileForPosition(just_past_the_edge));

  // One decimal place further from the edge than the format can lose, and the tile is stable
  // again. 0.001 is also about the float step of the destination column, so this is the real
  // limit of the whole path rather than of the text.
  WorldPosition const clear_of_the_edge (makePosition(0.001, 0.001, 0.0));
  std::optional<ParsedCoordinate> const clear
    (parseCoordinate(teleportCommand(0, clear_of_the_edge, 0.0)));

  REQUIRE(clear.has_value());
  CHECK(TileCoordinates::tileForPosition(clear->position)
        == TileCoordinates::tileForPosition(clear_of_the_edge));
}

TEST_CASE("the round trip holds across the whole world extent", "[gm][roundtrip]")
{
  // A sweep rather than samples, because the failure this guards against is magnitude-dependent:
  // fixed formatting loses precision as the integer part grows, and the world reaches +-17066.
  int position_failures = 0;
  int tile_failures = 0;
  int orientation_failures = 0;
  int parse_failures = 0;
  double first_failing_x = 0.0;

  for (int step = -170; step <= 170; ++step)
  {
    double const x (static_cast<double>(step) * 100.25);
    double const y (static_cast<double>(-step) * 99.75);
    double const z (static_cast<double>(step) * 0.125);
    double const orientation
      (TileCoordinates::normaliseOrientation(static_cast<double>(step) * 0.037));

    WorldPosition const position (makePosition(x, y, z));
    std::string const command
      (teleportCommand(static_cast<std::uint16_t>(step + 200), position, orientation));
    std::optional<ParsedCoordinate> const parsed (parseCoordinate(command));

    if (!parsed.has_value())
    {
      ++parse_failures;
      first_failing_x = x;
      continue;
    }

    bool const position_ok
      (  std::fabs(parsed->position.x - x) <= GM_ROUND_TRIP_TOLERANCE
      && std::fabs(parsed->position.y - y) <= GM_ROUND_TRIP_TOLERANCE
      && std::fabs(parsed->position.z - z) <= GM_ROUND_TRIP_TOLERANCE
      );

    if (!position_ok)
    {
      ++position_failures;
      first_failing_x = x;
    }

    if (TileCoordinates::tileForPosition(parsed->position)
        != TileCoordinates::tileForPosition(position))
    {
      ++tile_failures;
    }

    if (std::fabs(parsed->orientation - orientation) > GM_ROUND_TRIP_TOLERANCE
        || parsed->map != static_cast<std::uint16_t>(step + 200))
    {
      ++orientation_failures;
    }
  }

  CAPTURE(parse_failures, position_failures, tile_failures, orientation_failures, first_failing_x);
  CHECK(parse_failures == 0);
  CHECK(position_failures == 0);
  CHECK(tile_failures == 0);
  CHECK(orientation_failures == 0);
}

TEST_CASE("orientation round-trips at exactly zero and just below a full turn", "[gm][roundtrip]")
{
  WorldPosition const position (elwynn());

  std::optional<ParsedCoordinate> const at_zero
    (parseCoordinate(teleportCommand(0, position, 0.0)));

  REQUIRE(at_zero.has_value());
  CHECK(at_zero->has_orientation);
  CHECK(at_zero->orientation == 0.0);

  // Not merely equal to zero: a negative zero renders as "-0" on the next emission and reads as
  // a change against a stored 0.
  CHECK_FALSE(std::signbit(at_zero->orientation));

  // Just below a full turn has to stay just below it. Six decimals round 2*pi - 1e-9 DOWN to
  // 6.283185, which is still inside [0, 2*pi); rounding the other way would emit a full turn,
  // which the parser then folds to zero -- a spawn that faced almost all the way round would
  // come back facing north.
  double const near_full_turn (TWO_PI - 1.0e-9);
  std::optional<ParsedCoordinate> const near
    (parseCoordinate(teleportCommand(0, position, near_full_turn)));

  REQUIRE(near.has_value());
  CHECK(near->has_orientation);
  CHECK(near->orientation >= 0.0);
  CHECK(near->orientation < TWO_PI);
  CHECK(near->orientation == Approx(near_full_turn).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(near->orientation > 6.0);

  // The same thing one ulp below a full turn, which is the tightest the type allows.
  double const one_ulp_below (std::nextafter(TWO_PI, 0.0));
  std::optional<ParsedCoordinate> const tightest
    (parseCoordinate(teleportCommand(0, position, one_ulp_below)));

  REQUIRE(tightest.has_value());
  CHECK(tightest->orientation < TWO_PI);
  CHECK(tightest->orientation == Approx(TWO_PI).margin(1.0e-6));
}

TEST_CASE("emission and parsing ignore the global locale", "[gm][locale]")
{
  WorldPosition const position (makePosition(-9500.25, 70.5, 58.125));
  std::string const expected (".go xyz -9500.25 70.5 58.125 0 4.71");

  CHECK(teleportCommand(0, position, 4.71) == expected);

  // The contract, stated the way it can be checked without a locale installed: a decimal POINT,
  // and no comma anywhere. Under LC_NUMERIC=de_DE a naive formatter writes "-9500,25", which the
  // server does not reject -- it reads it as two arguments and teleports somewhere else.
  CHECK(expected.find('.') != std::string::npos);
  CHECK(expected.find(',') == std::string::npos);

  std::optional<std::locale> const comma (commaDecimalLocale());

  if (!comma.has_value())
  {
    WARN("no comma-decimal locale is installed on this machine, so the direct locale switch was"
         " not exercised; the '.'-present and ','-absent assertions above still ran");
    return;
  }

  // std::locale::global also moves the C locale, so this covers the iostreams path and anything
  // reaching for strtod underneath it.
  ScopedGlobalLocale const guard (*comma);

  std::string const under_comma_locale (teleportCommand(0, position, 4.71));

  CHECK(under_comma_locale == expected);
  CHECK(under_comma_locale.find(',') == std::string::npos);

  // Parsing is the direction std::stod would have broken: it would read "-9500.25" as -9500 and
  // stop at the point, losing the fraction without reporting anything.
  std::optional<ParsedCoordinate> const parsed (parseCoordinate(expected));

  REQUIRE(parsed.has_value());
  CHECK(parsed->position.x == Approx(-9500.25).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->position.y == Approx(70.5).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->position.z == Approx(58.125).margin(GM_ROUND_TRIP_TOLERANCE));
  CHECK(parsed->orientation == Approx(4.71).margin(GM_ROUND_TRIP_TOLERANCE));

  // A comma-decimal paste is NOT quietly reinterpreted either. "-9500,25 70,5 58,125" is six
  // numbers under this module's rules, not three, so it is refused rather than half-read.
  CHECK_FALSE(parseCoordinate("-9500,25 70,5 58,125").has_value());
}

TEST_CASE("a comma between two digits is refused rather than guessed", "[gm][parse][locale]")
{
  // Regression test. The scanner terminates a number at a comma, which is correct for the
  // "-9500, 70, 58" list this module must accept. But it made "-9500,25 70,5 58,125" -- what a
  // user on a comma-decimal locale copies out of a tool that respects LC_NUMERIC -- parse as SIX
  // integers rather than three reals, shifting every positional field along and inventing a map
  // id and an orientation out of fraction digits. The caller got a confident answer about a
  // position tens of yards from the one pasted.
  //
  // "-9500,70,58" cannot be distinguished from "-9500.70" then "58" by any rule, so declining is
  // the only honest response.
  CHECK_FALSE(parseCoordinate("-9500,25 70,5 58,125").has_value());
  CHECK_FALSE(parseCoordinate(".go xyz -9500,25 70,5 58,125 0").has_value());
  CHECK_FALSE(parseCoordinate("x: -9500,25 y: 70,5 z: 58,125").has_value());
  CHECK_FALSE(parseCoordinate("-9500,70,58").has_value());

  // A comma followed by whitespace is unambiguously a separator and must still work.
  CHECK(parseCoordinate("-9500, 70, 58").has_value());
  CHECK(parseCoordinate("-9500 , 70 , 58").has_value());

  // And the ordinary dot-decimal forms are untouched.
  CHECK(parseCoordinate("-9500.25 70.5 58.125").has_value());
  CHECK(parseCoordinate("-9500.25, 70.5, 58.125").has_value());
}
