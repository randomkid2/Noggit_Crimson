// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the bookmark -> game_tele export.
//
// The conversion is the only part that can be silently wrong: a tele row with the wrong
// coordinates is obvious the first time you use it, but one with the wrong ORIENTATION lands you
// in the right place facing the wrong way, and that is the kind of thing nobody reports as a bug.
// So the angle gets the most attention here.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <noggit/database/GameTeleBuilder.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <string>
#include <vector>

using namespace Noggit::Database;
using Catch::Approx;

namespace
{
  constexpr double PI = 3.14159265358979323846;
  constexpr double TWO_PI = 2.0 * PI;

  bool contains(std::string const& haystack, std::string const& needle)
  {
    return haystack.find(needle) != std::string::npos;
  }

  GameTele::Entry entryAt(std::string name, double x, double y, double z, double orientation)
  {
    GameTele::Entry entry;
    entry.name = std::move(name);
    entry.map = 0;
    entry.position = WorldPosition{x, y, z};
    entry.orientation = orientation;
    return entry;
  }
}

TEST_CASE("camera yaw converts to the server orientation facing the same way", "[tele][frame]")
{
  // The two frames disagree about axis direction, so this is a 180 degree turn, not a units
  // change. Spot-checked at the cardinals rather than asserted as a formula, so a "simplification"
  // that drops the turn fails here rather than shipping teles that face backwards.
  CHECK(GameTele::orientationFromCameraYaw(0.0) == Approx(PI).margin(1e-9));
  CHECK(GameTele::orientationFromCameraYaw(180.0) == Approx(0.0).margin(1e-9));
  CHECK(GameTele::orientationFromCameraYaw(90.0) == Approx(3.0 * PI / 2.0).margin(1e-9));
  CHECK(GameTele::orientationFromCameraYaw(-90.0) == Approx(PI / 2.0).margin(1e-9));
}

TEST_CASE("converted orientations are always in the column's range", "[tele][frame]")
{
  // game_tele.orientation is a FLOAT the core normalises anyway; emitting something outside
  // [0, 2pi) would still work and would make every diff against a server-written row noisy.
  for (double yaw = -1080.0; yaw <= 1080.0; yaw += 7.5)
  {
    double const orientation (GameTele::orientationFromCameraYaw(yaw));

    CAPTURE(yaw, orientation);
    CHECK(orientation >= 0.0);
    CHECK(orientation < TWO_PI);
  }
}

TEST_CASE("a name that .tele cannot type is refused rather than written", "[tele][validation]")
{
  // A row whose name contains a space sits in the table looking correct and can never be used,
  // because `.tele` takes the name as one argument. Refusing it loudly beats emitting it.
  GameTele::Result const result
    ( GameTele::build
      ( { entryAt("Goldshire", -9457.0, 62.0, 56.0, 0.0)
        , entryAt("My Secret Base", 100.0, 200.0, 300.0, 0.0)
        , entryAt("", 1.0, 2.0, 3.0, 0.0)
        }
      )
    );

  CHECK(result.emitted == 1);
  REQUIRE(result.skipped.size() == 2);

  CHECK(contains(result.sql, "'Goldshire'"));
  CHECK_FALSE(contains(result.sql, "My Secret Base"));

  // The reason travels with the name, so the UI can say why rather than just dropping it.
  CHECK(contains(result.skipped[0], "My Secret Base"));
  CHECK(contains(result.skipped[0], "space"));
}

TEST_CASE("two bookmarks claiming one name are both refused", "[tele][validation]")
{
  // The defect: both were emitted, producing two rows `.tele` cannot tell apart -- the exact
  // ambiguity the DELETE-by-name exists to prevent, written by the code that prevents it.
  // Refused rather than deduplicated: choosing one sends the player to one of two places they
  // marked, and the file gives no clue which.
  GameTele::Result const result
    ( GameTele::build
      ( { entryAt("Camp", 1.0, 2.0, 3.0, 0.0)
        , entryAt("Goldshire", -9457.0, 62.0, 56.0, 0.0)
        , entryAt("Camp", 400.0, 500.0, 600.0, 0.0)
        }
      )
    );

  CHECK(result.emitted == 1);
  REQUIRE(result.skipped.size() == 2);

  CHECK(contains(result.sql, "'Goldshire'"));
  CHECK_FALSE(contains(result.sql, "'Camp'"));

  // Neither row is written, so neither destination may reach the file.
  CHECK_FALSE(contains(result.sql, "400.000000"));
  CHECK_FALSE(contains(result.sql, "1.000000, 2.000000"));

  // Both are reported, in entry order, so the UI can name what the user has to rename.
  CHECK(contains(result.skipped[0], "Camp"));
  CHECK(contains(result.skipped[0], "2 bookmarks"));
  CHECK(contains(result.skipped[1], "Camp"));
}

TEST_CASE("names that collide only in case are treated as one name", "[tele][validation]")
{
  // game_tele.name is a utf8mb4_unicode_ci column with no unique key, so `DELETE WHERE name =
  // 'Camp'` already removes a stored 'camp'. Emitting both leaves two rows that no later export
  // of either one can clean up, and `.tele` resolves its argument case-insensitively too.
  GameTele::Result const result
    ( GameTele::build
      ( { entryAt("Camp", 1.0, 2.0, 3.0, 0.0)
        , entryAt("CAMP", 4.0, 5.0, 6.0, 0.0)
        }
      )
    );

  CHECK(result.emitted == 0);
  CHECK(result.skipped.size() == 2);
  CHECK_FALSE(contains(result.sql, "INSERT INTO"));
}

TEST_CASE("a repeated name does not cost the entries around it", "[tele][validation]")
{
  // Refusing a name must not turn into refusing the export. The @TELE offsets are also
  // recomputed over what is actually written, so a dropped entry cannot leave a gap that
  // collides with the next id the server hands out.
  GameTele::Result const result
    ( GameTele::build
      ( { entryAt("Alpha", 1.0, 2.0, 3.0, 0.0)
        , entryAt("Dup", 4.0, 5.0, 6.0, 0.0)
        , entryAt("Dup", 7.0, 8.0, 9.0, 0.0)
        , entryAt("Beta", 10.0, 11.0, 12.0, 0.0)
        }
      )
    );

  CHECK(result.emitted == 2);
  CHECK(result.skipped.size() == 2);

  CHECK(contains(result.sql, "@TELE + 1, 1.000000"));
  CHECK(contains(result.sql, "@TELE + 2, 10.000000"));
  CHECK_FALSE(contains(result.sql, "@TELE + 3"));
}

TEST_CASE("every input is accounted for", "[tele][validation]")
{
  // The property the two refusal rules have to share: nothing may vanish without a word. A
  // bookmark that is neither written nor reported is one the user believes was exported.
  std::vector<GameTele::Entry> const entries
    { entryAt("Alpha", 1.0, 2.0, 3.0, 0.0)
    , entryAt("has a space", 1.0, 2.0, 3.0, 0.0)
    , entryAt("", 1.0, 2.0, 3.0, 0.0)
    , entryAt("Dup", 1.0, 2.0, 3.0, 0.0)
    , entryAt("dup", 1.0, 2.0, 3.0, 0.0)
    , entryAt("Beta", 1.0, 2.0, 3.0, 0.0)
    };

  GameTele::Result const result (GameTele::build(entries));

  CHECK(result.emitted + result.skipped.size() == entries.size());
  CHECK(result.emitted == 2);
}

TEST_CASE("ids come from MAX(id) at apply time, never baked in", "[tele][sqlformat]")
{
  GameTele::Result const result
    ( GameTele::build
      ( { entryAt("Alpha", 1.0, 2.0, 3.0, 0.0)
        , entryAt("Beta", 4.0, 5.0, 6.0, 0.0)
        }
      )
    );

  // game_tele.id has no reserved custom range, so a literal id would collide with whatever the
  // target database already holds. The file has to be safe to apply anywhere.
  CHECK(contains(result.sql, "SET @TELE := (SELECT IFNULL(MAX(`id`), 0) FROM `game_tele`)"));
  CHECK(contains(result.sql, "@TELE + 1"));
  CHECK(contains(result.sql, "@TELE + 2"));
}

TEST_CASE("re-exporting a bookmark replaces its row instead of duplicating it", "[tele][sqlformat]")
{
  GameTele::Result const result (GameTele::build({entryAt("Camp", 10.0, 20.0, 30.0, 0.0)}));

  // Two rows with the same name make `.tele` ambiguous, which is a worse outcome than either
  // row alone.
  CHECK(contains(result.sql, "DELETE FROM `game_tele` WHERE `name` = 'Camp';"));

  std::size_t const insert_at (result.sql.find("INSERT INTO `game_tele`"));
  std::size_t const delete_at (result.sql.find("DELETE FROM `game_tele`"));

  REQUIRE(insert_at != std::string::npos);
  REQUIRE(delete_at != std::string::npos);
  CHECK(delete_at < insert_at);
}

TEST_CASE("an empty export is a valid file, not a broken one", "[tele][sqlformat]")
{
  GameTele::Result const result (GameTele::build({}));

  CHECK(result.emitted == 0);
  CHECK_FALSE(contains(result.sql, "INSERT INTO"));

  // Still a file a human can read and a server can source without erroring.
  CHECK(contains(result.sql, "Nothing to export"));
}
