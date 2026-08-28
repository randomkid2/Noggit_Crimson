// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TEXTURELAYEROPS_HPP
#define NOGGIT_TEXTURELAYEROPS_HPP

#include <noggit/texturing/TextureLayerPolicy.hpp>

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class MapView;

namespace Noggit
{
  // How much terrain one layer operation covers.
  //
  // Noggit Green's clearing tool offers "brush or whole ADT" and those two are the useful ends of
  // the range; the single chunk in between is here because the four-layer budget is a PER-CHUNK
  // budget, so "fix this one chunk" is the unit the problem is actually stated in.
  enum class LayerOpScope : int
  {
    Brush = 0,
    Chunk = 1,
    Tile = 2
  };

  struct LayerOpScopeRequest
  {
    LayerOpScope scope = LayerOpScope::Brush;
    glm::vec3 position{};
    float radius = 15.f;
  };

  // What one run moved. Every counter is in the unit its name says.
  struct LayerOpResult
  {
    std::size_t chunks_visited = 0;
    std::size_t chunks_changed = 0;
    std::size_t layers_removed = 0;
    std::size_t layers_replaced = 0;
    std::size_t layers_added = 0;

    // Chunks the operation could not carry out: a slot the chunk does not have, or a palette
    // texture that would not fit because the chunk is full and no eviction was permitted. Counted
    // rather than reported as an error, because on a tile-sized scope some chunks refusing is the
    // normal outcome and the number is the useful part.
    std::size_t chunks_refused = 0;

    // Set when the run could not start at all -- no world, no loaded tile, a texture that will not
    // load. Empty on success, including a successful run that changed nothing.
    std::string error;

    bool changedAnything() const;
  };

  // PREPARE AREA: make a stretch of terrain ready to be painted, before the painting starts.
  //
  // The three switches are the three things that go wrong when a rule pass or a long session has
  // filled every chunk's four slots, and they run in the order listed because that is the order in
  // which they help each other: clearing the overlays frees slots, and freeing slots is what makes
  // the palette fit without evicting anything.
  struct PrepareAreaRequest
  {
    // Drop layers 1..n-1 on every chunk in scope, leaving nothing but the base texture. This is
    // the "clear overlays back to base" pass.
    bool clear_overlays = false;

    // Unix-normalised texture paths, at most four, to guarantee a slot for on every chunk in
    // scope. A texture already on a chunk costs nothing; one that is missing is added if there is
    // room.
    std::vector<std::string> palette;

    // Pre-clear a slot when a palette texture will not otherwise fit: evict the least visible
    // layer to make room. Off by default, because with it off Prepare Area cannot destroy a layer
    // -- it can only add to chunks that had room.
    bool evict_to_fit = false;
  };

  // Every function here brackets its whole run in ONE ActionManager action, so one Ctrl+Z reverts
  // every chunk the run touched. See the note in the implementation for why that is not just a
  // convenience.
  //
  // The caller owns making the GL context current; adding or replacing a layer constructs a
  // scoped_blp_texture_reference, which loads and uploads a BLP.
  namespace TextureLayerOps
  {
    // TEXTURE DUPLICATES. Fold every layer whose texture an earlier layer already holds into that
    // earlier layer.
    LayerOpResult purgeDuplicates(MapView* map_view, LayerOpScopeRequest const& scope);

    // TEXTURES BELOW THRESHOLD. Remove every layer whose peak alpha anywhere on its chunk is at
    // most `threshold`, on the 0-255 scale the brush's own opacity slider uses.
    LayerOpResult purgeBelowThreshold( MapView* map_view
                                     , LayerOpScopeRequest const& scope
                                     , std::uint8_t threshold
                                     );

    // LAYER REPLACEMENT. Put one texture into slot `slot` on every chunk in scope, keeping or
    // clearing that slot's alpha. Chunks with fewer than slot+1 layers are counted as refused.
    LayerOpResult replaceLayer( MapView* map_view
                              , LayerOpScopeRequest const& scope
                              , std::size_t slot
                              , std::string const& texture_path
                              , LayerAlphaHandling alpha_handling
                              );

    LayerOpResult prepareArea( MapView* map_view
                             , LayerOpScopeRequest const& scope
                             , PrepareAreaRequest const& request
                             );
  }
}

#endif // NOGGIT_TEXTURELAYEROPS_HPP
