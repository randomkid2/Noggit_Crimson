// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// A record of every uid collision that world_model_instances_storage repaired.
//
// Noggit renumbers colliding uids in memory as instances load, in
// world_model_instances_storage.cpp:119-125 (M2) and :186-189 (WMO). That repair works and must
// not change -- the renumbered uid is what the tile is then saved with, so altering it moves real
// object placements. The problem it leaves behind is diagnostic, not functional: the entire
// collision set is compressed into the single `_uid_duplicates_found` bool that MapView.cpp:5156
// turns into one modal warning naming nothing. A mapper is told that "a" uid was in use, and has
// no way to learn which objects were affected, in which tile, or how many. Duplicate uids arrive
// through uid.ini desync between mappers sharing a map, and the damage they do to object
// placement is what actually kills projects.
//
// WHAT THIS LOG DOES NOT COUNT, and the reason the count is worth trusting: one uid legitimately
// appears in more than one tile's MDDF/MODF whenever an object's bounding box crosses a tile
// border. Those repeats are not collisions and never were. Rows that agree on the transform are
// matched by SceneObject::isDuplicateOf and never reach the renumber. Rows that disagree because
// the user moved the object during this session are matched by its wasTransformedThisSession flag
// (world_model_instances_storage.cpp:101 and :168) and are likewise dropped rather than
// renumbered -- before that check existed, every such row was renumbered into a ghost copy of the
// object at its pre-move transform AND counted here, so the number this log reported was mostly
// self-inflicted. What is left is the real thing: a uid held by two different objects, or by an
// object nothing has touched. That is a uid.ini problem, which is what the report tells the user
// to go and fix.
//
// This header is deliberately free of noggit, Qt, glm and database includes -- STL only. That is
// what lets the recorder be compiled into the standalone Catch2 target, and it is why the tile is
// kept as plain indices rather than as a TileIndex.

#ifndef NOGGIT_UIDCOLLISIONLOG_HPP
#define NOGGIT_UIDCOLLISIONLOG_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Noggit
{
  enum class UidCollisionKind
  {
    M2,
    WMO
  };

  // Tile the colliding instance sat in, derived from its position.
  //
  // `known` is false when the position falls outside the 64x64 grid, which TileIndex reports as an
  // enormous std::size_t rather than a negative one (TileIndex.cpp:33). An out-of-bounds
  // placement is exactly the kind of thing worth surfacing, so it is recorded as "no tile" rather
  // than dropped or clamped to a tile it does not belong to.
  struct UidCollisionTile
  {
    std::uint32_t x = 0;
    std::uint32_t z = 0;
    bool known = false;
  };

  struct UidCollisionRecord
  {
    std::uint32_t previous_uid = 0;
    std::uint32_t assigned_uid = 0;
    UidCollisionKind kind = UidCollisionKind::M2;
    // FileKey path when the instance has one, else the fileDataID rendered as digits. Empty only
    // when the instance had no model attached at all.
    std::string model_name;
    UidCollisionTile tile;
  };

  class UidCollisionLog
  {
  public:
    // A uid.ini desync renumbers every object it touches, so a bad map can present collisions in
    // the tens of thousands. Keeping all of them would turn a diagnostic into a memory problem,
    // and no report is readable at that length anyway. The FIRST records are kept rather than the
    // last: the earliest collisions are the ones that identify where the desync came from, and
    // later ones are usually the same cause repeating.
    static constexpr std::size_t MAX_RECORDS = 4096;

    UidCollisionLog() = default;

    UidCollisionLog(UidCollisionLog const&) = delete;
    UidCollisionLog(UidCollisionLog&&) = delete;
    UidCollisionLog& operator= (UidCollisionLog const&) = delete;
    UidCollisionLog& operator= (UidCollisionLog&&) = delete;

    // Called from the loader threads, under the storage's own mutex. The log locks separately so
    // that a reader on the UI thread never has to take the storage lock, which instance loading
    // holds for the whole of an add.
    void record( std::uint32_t previous_uid
               , std::uint32_t assigned_uid
               , UidCollisionKind kind
               , std::string_view model_name
               , UidCollisionTile tile
               );

    // Copies, because handing out a reference to a container a loader thread is still appending
    // to cannot be made safe by the caller. Collisions are rare and reports are opened once, so
    // the copy is not on any hot path.
    [[nodiscard]]
    std::vector<UidCollisionRecord> snapshot() const;

    // How many records are held. Differs from totalCount() once MAX_RECORDS is passed.
    [[nodiscard]]
    std::size_t recordedCount() const;

    // How many collisions were repaired, including those past MAX_RECORDS. This is the number a
    // report should show the user; recordedCount() is only how many it can list.
    [[nodiscard]]
    std::size_t totalCount() const;

    [[nodiscard]]
    bool truncated() const;

    [[nodiscard]]
    bool empty() const;

    void clear();

  private:
    mutable std::mutex _mutex;
    std::vector<UidCollisionRecord> _records;
    std::size_t _total_count = 0;
  };

  // One line per record, for a log file or a report list. It lives next to the data rather than in
  // whichever widget renders it, so the wording is pinned by a test instead of by the UI.
  [[nodiscard]]
  std::string formatUidCollision(UidCollisionRecord const& record);
}

#endif // NOGGIT_UIDCOLLISIONLOG_HPP
