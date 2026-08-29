// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINMASKSTORE_HPP
#define NOGGIT_TERRAINMASKSTORE_HPP

#include <noggit/terrain/TerrainMask.hpp>
#include <noggit/terrain/TerrainMaskFilters.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Noggit
{
  // ONE NAMED MASK, in the three parts it is actually made of. The split is the load-bearing idea
  // in the whole persistence design, so it is worth stating before the members:
  //
  //   1. STACK -- the filter stack the user authored. Small, human-readable, and the only part
  //      that describes INTENT. Saved as JSON.
  //   2. PAINT -- the hand-adjustments made after deriving. Irreplaceable, because nothing can
  //      recompute a decision a human made with a brush. Saved as a run-length-encoded sidecar.
  //   3. COMPOSITED -- the stack baked over the terrain, with the paint folded on top. This is the
  //      big one, and it is NEVER saved, because it is a pure function of the other two plus the
  //      terrain. Rebaking it is cheap; storing it is not.
  //
  // That is why "the valley floor survives a restart" costs a few kilobytes on disk rather than the
  // hundreds of megabytes the composited field occupies in memory. It also means a mask follows the
  // terrain: reshape the valley, rebake, and the mask is right again. Rebaking is deliberately
  // EXPLICIT rather than automatic -- a mask that silently redefined itself under a stroke would be
  // impossible to reason about, and the one thing worse than a stale mask is a moving one.
  struct NamedTerrainMask
  {
    std::string name;

    MaskFilterStack stack;

    // Hand-painted adjustments. Sparse and typically tiny -- a few dozen chunks after a long
    // session -- because only chunks the user actually brushed are stored at all.
    TerrainMask paint;

    // How paint folds onto the derived result during a bake. Max by default, which makes painting
    // ADD to a derived mask without being able to erase it; switching to Min makes the brush carve
    // out of the derived mask instead. Those are the two things a user wants ninety per cent of the
    // time, and both are reachable without a second brush mode.
    MaskCombine paint_combine = MaskCombine::Max;

    // Not persisted. See the note above.
    TerrainMask composited;

    // Which tiles `composited` is valid over, as tile_x * 64 + tile_z. Not persisted.
    //
    // This set is what makes the fail-open rule in TerrainMaskQuery implementable: without it an
    // unbaked tile is indistinguishable from a tile the mask genuinely excludes, and the query
    // would have to choose between clipping everything on terrain it has never looked at, or
    // clipping nothing anywhere.
    std::set<std::uint32_t> baked_tiles;

    bool tileIsBaked(int tile_x, int tile_z) const;
    void markTileBaked(int tile_x, int tile_z);

    // Drops the composited field AND the baked flag for one tile. The paint layer is untouched --
    // it is the part that cannot be recomputed.
    void releaseTile(int tile_x, int tile_z);

    // Forgets every bake. The next query fails open until something rebakes.
    void invalidateBake();

    std::size_t compositedBytes() const;
    std::size_t paintBytes() const;
  };

  // The one home for every named mask, the active selection, and the project sidecar.
  //
  // PROCESS-WIDE, like TerrainRuleStore and for the same reason: a mask describes a world, and
  // hanging it off MapView would silently discard it every time the user returned to the menu and
  // reopened a map. Unlike TerrainRuleStore it is per-PROJECT rather than per-installation, because
  // "the valley floor" is a statement about one map's terrain and means nothing against another
  // project's.
  //
  // NOT a QObject and no change signal, matching TerrainRuleStore: there is one writer (the mask
  // dialog) and the readers ask at the moment they need an answer. A signal nobody connects would
  // buy a moc pass and an API that looks live and is not.
  class TerrainMaskStore
  {
    public:
      static TerrainMaskStore* instance();

      // --- The named set ---

      std::vector<std::string> names() const;

      NamedTerrainMask* find(std::string const& name);
      NamedTerrainMask const* find(std::string const& name) const;

      // Null when the name is empty, already taken, or contains nothing usable as a filename. The
      // name is the user's; the on-disk filename is derived from it and de-duplicated separately,
      // so two masks named "Valley" and "valley" are distinct here and do not collide on a
      // case-insensitive filesystem.
      NamedTerrainMask* create(std::string const& name);

      bool remove(std::string const& name);
      bool rename(std::string const& old_name, std::string const& new_name);

      // --- Selection ---

      // Empty when nothing is selected.
      std::string const& activeName() const;
      // Pass "" to select nothing. False when the name is unknown.
      bool setActive(std::string const& name);

      NamedTerrainMask* active();
      NamedTerrainMask const* active() const;

      // The master switch every brush is clipped by. OFF at every project load, deliberately:
      // clipping changes what a brush does without changing how it looks, and a clip that quietly
      // survives a restart is discovered halfway through a session by way of a brush that "does not
      // work on this side of the hill".
      bool clippingEnabled() const;
      void setClippingEnabled(bool enabled);

      // --- The query, behind TerrainMaskQuery::factorAt ---

      // [0, 1]. 1.0 whenever clipping is off, nothing is selected, the coordinate is not finite, or
      // the tile has not been baked. See TerrainMaskQuery.hpp for why every one of those fails open.
      float factorAt(float world_x, float world_z) const;

      // --- Residency ---

      // WIRE THIS TO TILE UNLOAD. Drops the composited field for one tile across EVERY mask, not
      // only the active one. It is what bounds the store to the resident tile set; see the memory
      // note on TerrainMask.
      void releaseTile(int tile_x, int tile_z);

      std::size_t compositedBytes() const;

      // Soft cap on composited bytes across all masks. Default 192 MiB.
      //
      // The number comes from the residency arithmetic rather than from taste: a mask dense over
      // every tile MapIndex keeps resident costs 81 MiB (81 tiles at 1.000 MiB, the integer disc of
      // radius `unload_dist`, default 5 -- map_index.cpp:82). 192 MiB leaves room for two masks
      // baked at once and for a user who has raised unload_dist, while still being a fraction of
      // what the editor already spends on textures.
      std::size_t budgetBytes() const;
      void setBudgetBytes(std::size_t bytes);

      // Drops composited fields when over budget, oldest-selected first, never touching the active
      // mask and never touching any paint layer. Returns the number of masks dropped. Call after a
      // bake.
      std::size_t enforceBudget();

      // --- Persistence ---
      //
      // The PROJECT DIRECTORY IS PASSED IN rather than fetched from CurrentProject, and that is not
      // an accident of layering. Project::CurrentProject::get() asserts on a null project
      // (CurrentProject.hpp:17), so a store that reached for it would abort a debug build any time
      // it was touched before a project was opened -- which is exactly when a persistence layer
      // gets called by a test, by a crash handler, or by an autosave timer that outlived a map
      // close. Taking the path as an argument means the caller, which already knows it has a
      // project, is the only thing that can be wrong about it. It also keeps this module free of
      // the project headers entirely.

      // Reads <project_path>/noggit_masks/. Replaces the in-memory set. Clipping is left OFF and
      // nothing is baked, so a freshly loaded project clips nothing until the user asks.
      bool load(std::string const& project_path);
      // Writes <project_path>/noggit_masks/. Stacks as JSON, paint layers as run-length-encoded
      // sidecars, composited fields not at all.
      bool save(std::string const& project_path) const;

      // One line describing why the last load or save returned false. Empty after a success.
      std::string const& lastError() const;

    private:
      TerrainMaskStore() = default;

      // Keeps TerrainMaskQuery::Detail::g_clipping_active in step with "clipping is on AND a mask
      // is selected". Called from every path that can change either.
      void refreshQueryFlag() const;

      std::vector<NamedTerrainMask> _masks;
      std::string _active_name;

      // Never persisted. See setClippingEnabled.
      bool _clipping_enabled = false;

      std::size_t _budget_bytes = 192u * 1024u * 1024u;

      mutable std::string _last_error;
  };
}

#endif // NOGGIT_TERRAINMASKSTORE_HPP
