// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// The session register of model files that failed to load, and of the placements that reference
// them.
//
// WHY THIS EXISTS. Before this file, a missing M2 or WMO produced exactly one line in log.txt
// (AsyncObject::error_on_loading) and nothing else. Every downstream consumer then skipped the
// object silently: ModelRender::draw returned at the top, ModelInstance::intersect returned at
// the top, recalcExtents collapsed the axis-aligned box to a single point, and
// World::select_objects_in_area skipped the whole bucket. The placement was therefore invisible
// AND unclickable, which is worse than invisible -- the mapper could not select the broken
// placement in order to delete it or repoint it. The only in-app signal was a modal at save time
// telling the user to go and read a text file.
//
// TWO GRANULARITIES, DELIBERATELY. A load failure is per-FILE: the loader tries "world/foo.m2"
// once, and every placement that references it shares that one failure. A mapper's problem is
// per-PLACEMENT: which object, where, in which ADT. One missing file routinely has hundreds of
// placements, so the two are stored separately and joined on the normalised path -- files carry
// the count, placements carry the coordinates.
//
// THREADING. recordFileFailure() is called from AsyncLoader's worker threads
// (AsyncLoader::process -> AsyncObject::error_on_loading). recordPlacement() and every reader
// are called from the GUI thread. All of it is guarded by one mutex, and NOTHING in this header
// touches Qt, glm, OpenGL or any noggit type: a worker thread must never reach a QObject, and
// the surest way to guarantee that is for the type it calls to have no way of doing so. The GUI
// side learns that something changed by polling generation(), a single relaxed atomic load, so
// there is no cross-thread signal whose target lifetime has to be reasoned about. See
// MissingObjectsPanel for why that choice was made rather than a queued signal.
//
// STL-only for the same reason UidCollisionLog.hpp is: it can be compiled into the standalone
// Catch2 target without dragging the scene graph in, and the tile is kept as plain indices
// rather than as a TileIndex.

#ifndef NOGGIT_MISSINGPLACEMENTLOG_HPP
#define NOGGIT_MISSINGPLACEMENTLOG_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Noggit
{
  // What the failed FILE is. Decided from the extension, because AsyncObject knows its file key
  // and nothing else -- it cannot see whether it is being loaded as a Model or a WMO without a
  // virtual on Model/WMO, and those headers are shared with other work.
  enum class MissingAssetKind
  {
    Model,       // .m2 / .mdx / .mdl
    WorldModel   // .wmo
  };

  // What the PLACEMENT is. WorldModelDoodad is the case the mapper cannot otherwise diagnose at
  // all: the .wmo loaded fine, but an M2 listed inside its MODD chunk did not, so the building
  // is there and one of its props is missing. Nothing in the renderer distinguished this before.
  enum class MissingPlacementKind
  {
    Model,
    WorldModel,
    WorldModelDoodad
  };

  // Missing means the probe says the file is not in the client data at all -- a wrong path, or a
  // patch that was never applied. Unreadable means the probe finds it and the loader still
  // rejected it -- a truncated or wrong-version file. They need opposite fixes, which is why
  // they are separate; the same split AssetScan.hpp already draws.
  //
  // Unknown is the honest state until a probe has run. The failure is recorded on a worker
  // thread, and ClientData::exists is not something a worker thread should be reaching into, so
  // the state is resolved later on the GUI thread by MissingObjectsPanel::refresh.
  enum class MissingAssetState
  {
    Unknown,
    Missing,
    Unreadable
  };

  // Tile the placement sits in, derived from its position.
  //
  // `known` is false when the position falls outside the 64x64 grid, which TileIndex reports as
  // an enormous std::size_t rather than a negative one (TileIndex.cpp:33, `x` and `z` are
  // std::size_t). An out-of-bounds placement is exactly the kind of thing worth surfacing, so it
  // is recorded as "no tile" rather than dropped or clamped to a tile it does not belong to.
  struct MissingPlacementTile
  {
    std::uint32_t x = 0;
    std::uint32_t z = 0;
    bool known = false;
  };

  struct MissingPlacementRecord
  {
    // As referenced, not normalised: this is the string the mapper has to go and find in the ADT
    // or in the model that referenced it, and the lowercased key appears nowhere in the data.
    std::string file_path;

    // The .wmo whose MODD chunk asked for this model. Empty for anything but WorldModelDoodad.
    std::string owner_path;

    MissingPlacementKind kind = MissingPlacementKind::Model;

    // The uid of the placement. For a WorldModelDoodad this is the uid of the OWNING WMO
    // placement, because a WMO's internal doodads have no uid of their own -- they are not
    // separate MDDF entries and cannot be selected or deleted individually.
    std::uint32_t owner_uid = 0;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    MissingPlacementTile tile;
  };

  struct MissingFileRecord
  {
    std::string file_path;
    MissingAssetKind kind = MissingAssetKind::Model;
    MissingAssetState state = MissingAssetState::Unknown;

    // Distinct placements seen referencing this file. Keeps counting past MAX_PLACEMENTS, so the
    // number the panel shows stays truthful even when the list it can show stops growing.
    std::size_t placement_count = 0;
  };

  class MissingPlacementLog
  {
  public:
    // A broken patch can make every model in a map fail at once, and neither a report nor a
    // table is readable at that length. The FIRST records are kept rather than the last, for the
    // same reason UidCollisionLog keeps the first: the earliest failures identify the cause, and
    // the rest are usually that cause repeating.
    //
    // 1024 files and 8192 placements: a fully populated 64x64 map holds on the order of a few
    // thousand distinct models, and 8192 rows is already far past what anyone will scroll.
    static constexpr std::size_t MAX_FILES = 1024;
    static constexpr std::size_t MAX_PLACEMENTS = 8192;

    // Process-wide, like AsyncLoader::instance, because the failure is recorded from
    // AsyncObject::error_on_loading, which has no route to a World, a MapView or anything else
    // that could own it. Construction is thread-safe under C++11 magic statics, and the object
    // has no thread affinity of any kind, so it does not matter whether a worker thread or the
    // GUI thread touches it first.
    static MissingPlacementLog& instance();

    MissingPlacementLog() = default;

    MissingPlacementLog(MissingPlacementLog const&) = delete;
    MissingPlacementLog(MissingPlacementLog&&) = delete;
    MissingPlacementLog& operator= (MissingPlacementLog const&) = delete;
    MissingPlacementLog& operator= (MissingPlacementLog&&) = delete;

    // CALLED FROM LOADER THREADS. Records one failed file, once.
    //
    // Returns false, and records nothing, for any path that is not a model: a failed .blp
    // already falls back to textures/shanecube.blp (TextureManager.cpp:520) and a failed .adt is
    // a different problem with its own reporting, so putting either in this list would only
    // bury the rows that need acting on.
    bool recordFileFailure(std::string_view file_path);

    // CALLED FROM THE GUI THREAD, by the placement walk. Deduplicated on
    // (normalised path, uid, quantised position), so re-running the walk after tiles stream in
    // and out adds new rows without duplicating the ones already there.
    //
    // Returns true when the record was new.
    bool recordPlacement(MissingPlacementRecord record);

    // CALLED FROM THE GUI THREAD, after probing the client data.
    void setFileState(std::string_view file_path, MissingAssetState state);

    // Bumped by every call that changes anything. The panel polls this instead of being pushed
    // to, so no worker thread ever holds a pointer to a widget. Relaxed: it is a change hint,
    // and every value it guards is read afterwards under the mutex anyway.
    [[nodiscard]]
    std::uint64_t generation() const;

    // Copies, because handing out a reference to a container a loader thread is still appending
    // to cannot be made safe by the caller. Both are read when a panel refreshes, which is at
    // most once every poll interval, so the copy is not on any hot path.
    [[nodiscard]]
    std::vector<MissingFileRecord> files() const;

    [[nodiscard]]
    std::vector<MissingPlacementRecord> placements() const;

    // How many placements are LISTED. totalPlacementCount() is how many were seen.
    [[nodiscard]]
    std::size_t recordedPlacementCount() const;

    [[nodiscard]]
    std::size_t totalPlacementCount() const;

    [[nodiscard]]
    std::size_t fileCount() const;

    [[nodiscard]]
    bool truncated() const;

    [[nodiscard]]
    bool empty() const;

    void clear();

    // Whether the viewport draws placeholder cubes for failed placements.
    //
    // This lives here, and not in Noggit::Rendering::WorldRenderParams beside
    // draw_models_with_box where it belongs, because WorldRender.hpp is being edited by other
    // work in this change set and adding a field to that struct would collide. It is an atomic
    // because it is written from the GUI thread (a menu toggle) and read from the render path.
    // See the report: moving it to WorldRenderParams is a one-line follow-up.
    [[nodiscard]]
    bool drawPlaceholders() const;

    void setDrawPlaceholders(bool draw);

  private:
    mutable std::mutex _mutex;

    // Keyed by normalised path so that "World\Foo.M2" and "world/foo.m2" are one file. Insertion
    // order is kept separately because a std::unordered_map has none, and the first failure to
    // arrive is the most useful one to show first.
    std::unordered_map<std::string, MissingFileRecord> _files;
    std::vector<std::string> _file_order;

    std::vector<MissingPlacementRecord> _placements;
    std::unordered_set<std::string> _placement_keys;
    std::size_t _total_placement_count = 0;

    std::atomic<std::uint64_t> _generation = {0};
    std::atomic<bool> _draw_placeholders = {true};
  };

  // Lowercased, backslashes turned into forward slashes. The listfile mixes both conventions and
  // the same model is referenced either way from different ADTs; without this the same missing
  // file appears as two rows.
  [[nodiscard]]
  std::string normaliseAssetPath(std::string_view path);

  // MissingAssetKind::Model for .m2/.mdx/.mdl, WorldModel for .wmo, and no value at all for
  // anything else -- which is how recordFileFailure decides what to ignore.
  [[nodiscard]]
  bool classifyMissingAsset(std::string_view path, MissingAssetKind& kind_out);

  [[nodiscard]]
  char const* missingPlacementKindLabel(MissingPlacementKind kind);

  [[nodiscard]]
  char const* missingAssetStateLabel(MissingAssetState state);

  // One line per record, for a log file or a report list. It lives next to the data rather than
  // in whichever widget renders it, so the wording is pinned by a test instead of by the UI.
  [[nodiscard]]
  std::string formatMissingPlacement(MissingPlacementRecord const& record);

  // === PLACEHOLDER GEOMETRY =================================================================
  //
  // The half-extent of the cube drawn, and picked, in place of a failed placement.
  //
  // A FIXED size, never the model's own bounding box. Model::bounding_box_min/max/radius are
  // uninitialised for a model that failed to load -- the constructor's memset is commented out
  // at Model.cpp:19-24 and GLM_FORCE_CTOR_INIT is not defined anywhere in this tree -- so
  // reading them produces indeterminate floats and, through recalcExtents, an indeterminate
  // frustum test. The same applies to WMO::extents.
  //
  // The three sizes are a design choice, scaled against the ADT chunk (33.333 yards) so that a
  // marker reads at the scale of the thing it stands in for: a doodad is roughly a prop, a WMO
  // is a building. They are not measured values and nothing depends on their exact magnitude
  // beyond being clickable and visible.
  namespace MissingPlacementGeometry
  {
    constexpr float MODEL_HALF_EXTENT = 2.0f;              // 4 yd cube, about a crate or a tree
    constexpr float WORLD_MODEL_HALF_EXTENT = 8.0f;        // 16 yd cube, about half a chunk
    constexpr float WORLD_MODEL_DOODAD_HALF_EXTENT = 1.0f; // 2 yd cube, interior clutter

    [[nodiscard]]
    constexpr float halfExtentFor(MissingPlacementKind kind)
    {
      switch (kind)
      {
        case MissingPlacementKind::WorldModel:
          return WORLD_MODEL_HALF_EXTENT;
        case MissingPlacementKind::WorldModelDoodad:
          return WORLD_MODEL_DOODAD_HALF_EXTENT;
        case MissingPlacementKind::Model:
        default:
          return MODEL_HALF_EXTENT;
      }
    }
  }
}

#endif // NOGGIT_MISSINGPLACEMENTLOG_HPP
