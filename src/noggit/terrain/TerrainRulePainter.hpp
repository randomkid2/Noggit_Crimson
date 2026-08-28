// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINRULEPAINTER_HPP
#define NOGGIT_TERRAINRULEPAINTER_HPP

#include <noggit/terrain/TerrainRules.hpp>
#include <noggit/Alphamap.hpp>
#include <noggit/Brush.h>
#include <noggit/scoped_blp_texture_reference.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class MapChunk;
class MapTile;
class MapView;
class TextureSet;
class World;

namespace Noggit
{
  // What one run moved. Every counter is in the unit its name says, and the three "unchanged"
  // counters exist because "did the paint change anything" and "did the paint do anything" are
  // different questions -- see the note above ChunkTextureFingerprint in the implementation.
  struct TerrainPaintStats
  {
    // Chunks that came out different from how they went in.
    std::size_t chunks_painted = 0;
    // Of those, chunks every rule agreed on, which took one brush pass instead of 64.
    std::size_t chunks_uniform = 0;
    // Chunks the run brushed over and left byte-identical. An undo entry exists for these.
    std::size_t chunks_unchanged = 0;
    std::size_t units_painted = 0;
    // Units whose texture could not be added: the chunk holds four layers and all four are in use.
    std::size_t units_refused = 0;
    std::size_t units_unchanged = 0;

    void merge(TerrainPaintStats const& other);
  };

  // The one place a TerrainRuleSet is turned into paint on a chunk.
  //
  // WHY IT IS A CLASS RATHER THAN A FUNCTION. Three things have to be built once and reused across
  // every chunk of a run: the texture references (each construction is a lookup in the async object
  // map, and a tile-sized scope is tens of thousands of paint calls), the two brushes, and the pair
  // of fingerprint buffers (MAX_ALPHAMAPS is 3 and each plane is 64x64 bytes, so 12 KiB of alpha
  // per fingerprint and 24 KiB for the pair, against 256 chunks in a tile).
  //
  // WHY IT IS SHARED. Automatic Texturing's Apply button and Live Auto Texture paint the same rules
  // onto the same chunks and have to produce the same result. Two implementations of that would
  // drift the first time either was touched, and the drift would show up as "the live pass paints
  // something different from the preview I approved" -- which is unfalsifiable from the outside.
  //
  // WHAT THE CALLER OWNS, in order, because none of it can be done safely from in here:
  //
  //   1. Making the GL context current. Adding a layer constructs a scoped_blp_texture_reference,
  //      which loads and uploads a BLP.
  //   2. beginAction / endAction. The painter registers its per-chunk before-image on whatever
  //      action is running (NOGGIT_CUR_ACTION); it never opens or closes one. That is exactly what
  //      lets Live Auto Texture land inside the sculpting stroke's own undo step.
  //   3. Deciding WHICH chunks to visit.
  class TerrainRulePainter
  {
    public:
      // `rules` is COPIED, not referenced, and that is a correctness requirement rather than a
      // convenience. TerrainRuleResult::texture is a std::string_view borrowed from the winning
      // rule and documented as valid only until the set is modified (TerrainRules.hpp), and both
      // callers can have their set mutated underneath them: the dialog is modeless and its rule
      // list is edited keystroke by keystroke, and the live path reads a process-wide store that
      // that same dialog writes to. Owning the set for the length of a run makes every view the
      // evaluator hands back valid for as long as the paint needs it.
      TerrainRulePainter(MapView* map_view, TerrainRuleSet rules);

      // Loads one reference per distinct texture the rules name. Returns false and fills `error`
      // when one cannot be loaded, in which case paintChunk does nothing.
      //
      // Separate from the constructor because both callers have to report the failure their own way
      // -- the dialog with a message box, the live pass with a log line and a silent stand-down --
      // and neither wants an exception crossing a stroke-end hook.
      bool prepareTextures(std::string& error);

      // Evaluates the rules over one chunk's 8x8 unit grid and paints the result. Registers the
      // chunk on the running action and marks the tile changed only if it really touches it.
      //
      // Safe to call on a chunk this painter has already visited: registerChunkTextureChange
      // ignores a chunk it has seen (Action.cpp:701-705), so the before-image stays the one from
      // the first visit and undo still goes all the way back.
      void paintChunk(MapTile* tile, MapChunk* chunk);

      TerrainPaintStats const& stats() const;

      TerrainRuleSet const& rules() const;

    private:
      // Everything about a chunk's texturing that this tool can move: which layers exist, in which
      // order, and the committed alpha behind them.
      //
      // Taken before the first edit to a chunk and again after the last one, because "did this
      // paint change anything" cannot be answered any other way. TextureSet::paintTexture reports
      // true for a texel it rewrote with the value it already held (texture_set.cpp:955 sets
      // `changed` for every texel inside the radius, whether or not the alpha moved), and its other
      // early return reports true for a chunk it did not touch at all (texture_set.cpp:832,
      // `return nTextures == 1`). So the paint's own answer cannot be used to decide whether the
      // tile is now unsaved.
      struct ChunkTextureFingerprint
      {
        std::vector<std::string> layers;
        std::array<std::array<std::uint8_t, 64 * 64>, static_cast<std::size_t>(MAX_ALPHAMAPS)> alpha{};
        // An absent plane and an all-zero plane are different states of the chunk, and the
        // difference survives a save, so it is not safe to fold them together.
        std::array<bool, static_cast<std::size_t>(MAX_ALPHAMAPS)> alpha_present{};

        bool operator== (ChunkTextureFingerprint const& other) const;
      };

      static void captureFingerprint(TextureSet* texture_set, ChunkTextureFingerprint& out);

      MapView* _map_view;
      World* _world;
      TerrainRuleSet _rules;

      std::map<std::string, scoped_blp_texture_reference> _texture_refs;
      bool _textures_ready = false;

      Brush _unit_brush;
      Brush _chunk_brush;

      // Reused across chunks rather than declared inside the walk: 3 planes of 64x64 bytes is
      // 12 KiB of alpha each, 24 KiB for the pair, and a tile is 256 chunks.
      ChunkTextureFingerprint _before;
      ChunkTextureFingerprint _after;

      TerrainPaintStats _stats;
  };
}

#endif // NOGGIT_TERRAINRULEPAINTER_HPP
