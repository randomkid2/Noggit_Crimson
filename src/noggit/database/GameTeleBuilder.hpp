// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_GAMETELEBUILDER_HPP
#define NOGGIT_DATABASE_GAMETELEBUILDER_HPP

#include <noggit/database/TileCoordinates.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Noggit::Database
{
  // Turns editor bookmarks into `game_tele` rows, so a place you marked while building is a
  // `.tele <name>` away in game.
  //
  // Pure: no Qt, no database, no Noggit headers beyond this layer's own. The whole module is a
  // function of its inputs, which is what lets the conversion be tested without a client, a
  // server or a running editor -- and the conversion is the only part that can be silently wrong.
  //
  // `game_tele` columns, measured and recorded in docs/schema-335.md:
  //     id, position_x, position_y, position_z, orientation, map, name
  namespace GameTele
  {
    // One destination, already in server coordinates.
    struct Entry
    {
      std::string name;
      std::uint16_t map = 0;
      WorldPosition position;

      // Radians, server convention: 0 faces +x. Normalised on emit.
      double orientation = 0.0;
    };

    // Converts a Noggit camera yaw into the server orientation facing the same way.
    //
    // Not a units change -- the two frames disagree about which way the axes point, so the angle
    // itself moves. Derivation, and it is worth keeping because getting it wrong produces a tele
    // that lands correctly and faces backwards, which nobody notices until they use it:
    //
    //   Noggit's camera at pitch 0 looks along (sin(yaw), 0, cos(yaw)) -- Camera::direction
    //   rotates (0,0,1) by the yaw (camera.cpp:83-88).
    //   to_server maps a Noggit direction (dx, _, dz) to (-dz, -dx), the linear part of the
    //   inverse of to_client.
    //   A server orientation o faces (cos o, sin o).
    //
    //   So cos o = -cos(yaw) and sin o = -sin(yaw), giving o = yaw + 180 degrees.
    //
    // Returned normalised into [0, 2pi) so the value written is the one the column would hold.
    double orientationFromCameraYaw(double camera_yaw_degrees);

    // `DELETE` by name then `INSERT`, so re-exporting a bookmark updates its destination rather
    // than adding a second row the `.tele` command would then find ambiguously.
    //
    // Ids are allocated from `MAX(id)` at apply time rather than baked in, because game_tele.id
    // is a plain key with no reserved range for custom rows -- a literal id chosen here would
    // collide with whatever the target database already has. That makes the file safe to apply to
    // any world database, which is the point of a tele list.
    //
    // Names are validated, not escaped-and-hoped: `.tele` matches on the name, and a name with a
    // space in it cannot be typed as one argument. An entry whose name is empty or contains
    // whitespace is skipped and reported in `skipped`.
    struct Result
    {
      std::string sql;

      // Names that were not emitted, with the reason. Empty when everything converted.
      std::vector<std::string> skipped;

      std::size_t emitted = 0;
    };

    Result build(std::vector<Entry> const& entries);
  }
}

#endif
