// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

enum class tile_mode
{
  // regular mode
  edit,
  // uid fix mode, ignore/change loading and saving to
  // reduce the uid fix all time
  uid_fix_all
};

enum class model_update
{
  add,
  remove,
  none
};

// Why a tile is being told that it needs saving.
//
// This exists because MapIndex::unloadTiles refuses to release any tile whose `changed` flag is
// set, so setting that flag for anything that is not a real edit pins the tile in memory for the
// rest of the session.
//
//   user_edit   -- the user changed something on this tile. It must not be unloaded and it must be
//                  offered for saving.
//   bookkeeping -- the tile was rewritten only in memory, by something the user did not ask for.
//                  The case this was added for is a uid being renumbered on load inside
//                  MapTile::finishLoading. Nothing about the file changed, so the tile has to stay
//                  releasable or a flight over a map with duplicate uids retains every tile it
//                  crosses.
//
// The third, "an operation in progress is holding raw pointers into this tile", is a lifetime and
// not an intent, so it is MapTile::pin()/unpin() instead.
//
// It lives in this header rather than map_index.hpp because World, the tile update queue and
// MapIndex all need it, and the queue's header cannot include map_index.hpp.
enum class tile_dirty_intent
{
  user_edit,
  bookkeeping
};
