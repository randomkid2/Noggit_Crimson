// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TEXTURELAYERPOLICY_HPP
#define NOGGIT_TEXTURELAYERPOLICY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Noggit
{
  // THE FOUR-LAYER CEILING, and what to do when a stroke hits it.
  //
  // An MCNK holds at most four MCLY entries. That is not a tunable, it is the width of the array in
  // the file format, and every path in this tree that adds a texture layer ends at
  // TextureSet::get_texture_index_or_add, which returns -1 when the chunk is full and no layer can
  // be evicted. The stroke then does nothing at all: no error, no cursor change, nothing on the
  // status bar. The paintability overlay (terrain_frag.glsl, driven from TexturingTool::onSelected)
  // already tints those chunks red, so the diagnosis exists. This is the remedy.
  //
  // The feature -- a mode selector on the texture tool, an "auto-replace the least visible layer"
  // rule, layer replacement and a prepare-area pass -- is taken from haloreach252's Noggit Red
  // fork, branch feature/texture_improvements (gitlab.com/haloreach252/noggit-red, GPL-3.0). That
  // branch was not available to read from this machine, so what follows is an independent
  // implementation of the described behaviour against this fork's own TextureSet, Action and
  // ActionManager, not a port of their code. The credit is for the design.
  enum class LayerFullPolicy : int
  {
    // Today's behaviour, and the default, so nobody who does not go looking for this is surprised
    // by a brush that silently deletes a layer.
    Skip = 0,
    // Evict the layer with the smallest total alpha contribution over the chunk. See
    // TextureSet::getLeastVisibleLayer for exactly how "least visible" is measured.
    ReplaceLeastVisible = 1,
    // Evict the layer holding one specific texture the user named. Does nothing on a chunk that
    // does not hold that texture -- the stroke is skipped there, as it is today.
    ReplaceNominated = 2
  };

  // What happens to a layer's alpha when its texture is swapped out from under it.
  enum class LayerAlphaHandling : int
  {
    // The incoming texture inherits the outgoing one's footprint exactly. This is a straight
    // substitution: the chunk looks the same except for which BLP is sampled.
    Keep = 0,
    // The slot's alpha is cleared, so the incoming texture starts invisible and the weight it gave
    // up returns to the base layer. This is "load the texture into the slot, I will paint it in".
    Reset = 1
  };

  // The process-wide answer to "a texture has to go onto a full chunk".
  //
  // WHY A PROCESS-WIDE VALUE RATHER THAN A PARAMETER. The decision has to be readable from inside
  // TextureSet::get_texture_index_or_add, which is reached through MapChunk::paintTexture and
  // World::paintTexture. Threading a policy argument down would change the signature of two
  // functions in World.h and MapChunk.h that a dozen call sites use, for a value that is a tool
  // setting rather than a per-call one. Noggit already solves this exact problem the same way for
  // the brush's own texture: Noggit::Ui::selected_texture is a process-wide holder that
  // MapChunk.cpp:488 reads directly. This follows that precedent.
  //
  // NOT THREAD-SAFE, and does not need to be: every writer is a Qt widget callback and every reader
  // runs under the GL context on the same thread.
  struct TextureLayerAdmission
  {
    LayerFullPolicy policy = LayerFullPolicy::Skip;

    // Unix-normalised (lowercase, forward slashes) so it can be compared against
    // TextureSet::filename without normalising on every texel. Only read when policy is
    // ReplaceNominated; empty means "no texture nominated", which makes ReplaceNominated behave
    // exactly like Skip.
    std::string nominated_texture;

    // A default-constructed value is the Skip policy, which is what every caller that wants
    // today's behaviour regardless of the user's setting should pass.
    static TextureLayerAdmission const& current();
    static void setCurrent(TextureLayerAdmission admission);
  };

  // Per-layer alpha measurements for one chunk, taken in a single pass.
  //
  // TWO DIFFERENT MEASURES, because the two features that need them ask different questions:
  //
  //   sum  -- the layer's TOTAL contribution over the chunk. This is what "least visible" means:
  //           a layer painted faintly everywhere and a layer painted solidly in one corner can
  //           both be worth evicting, and only a total sees that.
  //   peak -- the layer's LARGEST contribution at any one texel. This is what the threshold purge
  //           means: "this layer never gets above 12/255 anywhere, so it is invisible and it is
  //           still eating one of four slots".
  //
  // A single 4096-texel pass fills both, so nothing asks the chunk twice.
  struct LayerAlphaProfile
  {
    static constexpr std::size_t MAX_LAYERS = 4;
    static constexpr std::size_t TEXELS_PER_CHUNK = 64 * 64;

    // 255 * 4096. The largest sum a single layer can reach, which is a layer that is opaque over
    // the whole chunk. Used to turn a sum into the 0..1 coverage fraction the UI prints.
    static constexpr std::uint32_t MAX_LAYER_SUM = 255u * 4096u;

    std::size_t layers = 0;
    std::array<std::uint32_t, MAX_LAYERS> sum{};
    std::array<std::uint8_t, MAX_LAYERS> peak{};

    // sum[layer] / MAX_LAYER_SUM, in 0..1. Returns 0 for a layer this chunk does not have.
    float coverage(std::size_t layer) const;
  };
}

#endif // NOGGIT_TEXTURELAYERPOLICY_HPP
