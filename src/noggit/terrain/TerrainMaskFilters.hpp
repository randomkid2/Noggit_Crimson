// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINMASKFILTERS_HPP
#define NOGGIT_TERRAINMASKFILTERS_HPP

#include <noggit/terrain/TerrainMask.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The PROCEDURAL half of the mask feature: a stack of filters that derive a mask from the terrain,
// and the arithmetic that folds them together.
//
// Pure STL, for the reason given at the top of TerrainMask.hpp -- the standalone test target links
// this file and nothing else from the editor. The world walk that feeds it lives at the bottom of
// this header as templates, so MapChunk.h and MapTile.h stay out.
namespace Noggit
{
  // --- The vertex grid a derived mask is computed on ---

  // 4.16667 yards, the spacing of the terrain's outer vertex grid. Restated from MapHeaders.h:60
  // (UNITSIZE = CHUNKSIZE / 8) for the no-glm reason.
  inline constexpr float MASK_UNIT_SIZE = MASK_CHUNK_SIZE / 8.0f;

  // 16 chunks of 8 units, plus the closing vertex: 129 vertices across one tile, edges shared
  // between adjacent chunks.
  inline constexpr int MASK_TILE_VERTEX_SIDE = MASK_TILE_CHUNK_SIDE * 8 + 1;
  inline constexpr int MASK_TILE_VERTEX_COUNT = MASK_TILE_VERTEX_SIDE * MASK_TILE_VERTEX_SIDE;

  // One tile's heights, resampled onto a regular 129x129 grid, plus the slope and curvature fields
  // derived from it.
  //
  // WHY A SEPARATE GRID RATHER THAN READING MapChunk::mVertices DIRECTLY. A chunk's vertex array
  // interleaves a 9x9 outer grid with an 8x8 inner grid (TerrainRules.hpp:341), and the inner
  // vertices sit at cell centres, half a unit off the outer lattice. A second difference taken
  // across that array would be sampling two different lattices and would report curvature where the
  // terrain is perfectly flat. Flattening to the outer grid first makes the stencil below correct
  // by construction.
  //
  // WHY CURVATURE IS COMPUTED HERE AND NOT ON THE MASK'S OWN 64x64 GRID. This is the one piece of
  // arithmetic in the feature that would be silently wrong if done the obvious way. The rendered
  // surface is piecewise linear between vertices 4.167 yards apart, so its true second derivative
  // is zero inside every triangle and undefined on the edges. Sampling curvature at the mask's
  // 0.52-yard texel spacing would therefore measure the triangulation, not the landform: the result
  // would be a grid of hairlines along the triangle edges and nothing in between. Curvature is a
  // property of the vertex lattice and has to be taken there, then resampled up.
  //
  // COST, computed. Three float grids of MASK_TILE_VERTEX_COUNT = 16'641 entries:
  //   heights    16'641 x 4 B = 66'564 B = 65.00 KiB
  //   slopes     the same
  //   curvature  the same
  //   total                     199'692 B = 195.01 KiB per tile, transient -- freed when the bake
  //                                         of that tile finishes.
  // Slope and curvature are built lazily, so a stack using neither costs only the height grid.
  class MaskTileHeightField
  {
  public:
    MaskTileHeightField();

    // Vertex indices are clamped into range rather than rejected. That is what makes the stencil in
    // curvatureAt legal at the tile boundary, and it has a consequence worth stating: replicating
    // the edge vertex makes the second difference across the tile seam zero, so a derived curvature
    // mask reads flat on the outermost 4.167-yard ring of every tile.
    //
    // The alternative -- pulling the neighbouring tile in to get real values -- would force-load
    // tiles the user has never visited, turning "bake this mask" into "load the continent". The
    // ring is a known, bounded artefact and it is documented in the dialog rather than hidden.
    void setHeight(int vertex_x, int vertex_z, float height);
    float height(int vertex_x, int vertex_z) const;

    // Marks the derived grids stale. Called by setHeight, so a caller that fills the field and then
    // asks for curvature always gets curvature of what it filled.
    void invalidateDerived();

    // Steepness from horizontal in degrees, [0, 90], from the central difference of the height
    // grid. Agrees with TerrainRules' slopeDegreesFromGradient, which the tests assert.
    float slopeDegrees(int vertex_x, int vertex_z) const;

    // Mean curvature of the heightfield in RECIPROCAL YARDS, i.e. one over the radius of curvature.
    // 0.02 is a bowl of 50-yard radius; 0 is a plane or a uniform slope.
    //
    // SIGN CONVENTION, stated loudly because it is precisely the thing that gets flipped and the
    // flip is invisible until someone has textured a continent backwards:
    //
    //     POSITIVE is CONCAVE -- a hollow, a drainage line, the inside of a valley floor.
    //     NEGATIVE is CONVEX  -- a ridge, a shoulder, the lip of a plateau.
    //
    // The five-point stencil is (h_left + h_right + h_up + h_down - 4 * h_centre) / spacing^2,
    // which is the discrete Laplacian and is positive when the neighbours sit above the centre.
    //
    // `step` is the stencil radius in vertex units and it is exposed because curvature is
    // SCALE-DEPENDENT: at step 1 the field picks up 8-yard gullies, at step 4 it picks up
    // 33-yard valleys and ignores the gullies entirely. A single fixed scale would make the filter
    // answer only one of the two questions a mapper actually asks. Clamped to at least 1.
    //
    // PRECISION, measured rather than assumed, because a second difference is a subtraction of
    // nearly equal numbers and that is where accuracy goes to die. The heights are float32, so each
    // carries up to half an ulp of error; the five-point stencil has coefficients summing to 8 in
    // absolute value, so the worst-case error in the numerator is 8 * 0.5 * eps * |h|, and the
    // division by spacing^2 scales it down quadratically with `step`. For terrain 300 yards above
    // zero:
    //
    //     step 1  (4.167 yd)   noise floor 8.24e-06 /yd
    //     step 2  (8.333 yd)   noise floor 2.06e-06 /yd
    //     step 4 (16.667 yd)   noise floor 5.15e-07 /yd
    //
    // Measured against an analytic dome -- h = 300 * (1 - r^2 / 400^2), whose exact Laplacian is
    // -0.0075 /yd -- the step-1 result came out at -0.00749531, an error of 4.69e-06, which sits
    // just under the predicted floor. TerrainMaskTests asserts both scales against these bounds.
    //
    // Two consequences worth knowing. Real landform curvature runs from about 0.006 /yd for a broad
    // hill to 0.25 /yd for a sharp gully, so even the worst case here is three orders of magnitude
    // below the signal. And raising `step` improves precision as well as widening the scale, which
    // is why the default is 2 rather than 1.
    float curvature(int vertex_x, int vertex_z, int step) const;

    // Bilinear samples by position INSIDE THE TILE, in yards from the tile's origin corner. This is
    // what the per-texel bake calls; the caller has already decomposed the world position.
    float sampleHeight(float in_tile_x, float in_tile_z) const;
    float sampleSlopeDegrees(float in_tile_x, float in_tile_z) const;
    float sampleCurvature(float in_tile_x, float in_tile_z, int step) const;

    // True once any height has been written. A stack baked against an unfilled field would derive
    // a mask from a plane at y = 0, so the baker refuses rather than producing a plausible-looking
    // wrong answer.
    bool filled() const;

  private:
    void ensureSlopes() const;
    void ensureCurvature(int step) const;

    std::vector<float> _heights;

    // mutable: the derived grids are a cache of a const-observable quantity, and the sample
    // functions have to stay const so the baker can hold the field by const reference.
    mutable std::vector<float> _slopes;
    mutable std::vector<float> _curvature;
    mutable int _curvature_step = 0;
    mutable bool _slopes_valid = false;

    bool _filled = false;
  };

  // --- Filters ---

  enum class MaskFilterKind
  {
    // Steepness in degrees, [0, 90].
    Slope,
    // World Y, in yards.
    Height,
    // Reciprocal yards, positive concave. See MaskTileHeightField::curvature.
    Curvature,
    // The alphamap of one named texture layer on the chunk, [0, 255] rescaled to [0, 1].
    LayerAlpha,
    // 1 where the chunk's area id is in the list, 0 elsewhere.
    AreaId,
    // Seeded fractal value noise, [0, 1].
    Noise,
    // A flat value. The layer that makes "start from fully masked in and subtract" expressible
    // without a filter that reads terrain at all.
    Constant
  };

  char const* maskFilterKindName(MaskFilterKind kind);
  MaskFilterKind maskFilterKindFromName(char const* name);

  // A soft interval: 1 inside [low, high], falling linearly to 0 over `feather` units outside it.
  //
  // Feather is in the units of the axis being tested -- degrees for slope, yards for height,
  // reciprocal yards for curvature -- and not a normalised fraction, because the range of those
  // axes differs by four orders of magnitude and a single normalised feather would be unusable on
  // at least two of them.
  //
  // The edge cases follow TerrainRange (TerrainRules.hpp:47-70), which the user has already met in
  // the auto-texture dialog: an inverted interval matches nothing rather than being silently
  // repaired, and a NaN sample matches nothing. A rule that cannot be evaluated must not win.
  struct MaskRange
  {
    float low = -1.0e30f;
    float high = 1.0e30f;
    float feather = 0.0f;

    static MaskRange any();
    static MaskRange atLeast(float low_value, float feather_value = 0.0f);
    static MaskRange atMost(float high_value, float feather_value = 0.0f);
    static MaskRange between(float low_value, float high_value, float feather_value = 0.0f);

    // [0, 1]. 0 for NaN, for an inverted interval, and outside the feathered shoulders.
    float weight(float value) const;

    bool inverted() const;

    // True when either endpoint actually constrains. This is what separates a TEST filter from a
    // FIELD filter: slope, height and curvature measure an axis and an unconstrained range means
    // "do not test this axis", weight 1. Layer alpha and noise produce a value that is ALREADY a
    // mask in [0, 1], and for those an unconstrained range means "pass it through" -- returning 1
    // would discard the only thing the layer was added to read. See evaluateMaskLayer.
    bool bounded() const;
  };

  // One layer of the stack: what to measure, how to shape it, and how to fold it into what the
  // layers below produced.
  struct MaskFilterLayer
  {
    MaskFilterKind kind = MaskFilterKind::Slope;

    // How this layer's output folds into the accumulator. The FIRST enabled layer is always applied
    // as Replace regardless of what this says, because there is nothing to fold into and "Multiply"
    // as the first layer would otherwise produce an empty mask -- the single commonest way to build
    // a stack that looks broken.
    MaskCombine combine = MaskCombine::Replace;

    // Scales this layer's output before folding. See TerrainMask::combineChunk for why it scales
    // the value rather than blending the result.
    float opacity = 1.0f;

    // Inverts this layer's own output, before opacity and before combining. Distinct from
    // TerrainMask::invert, which inverts the finished field including the parts it never stored.
    bool invert = false;

    // A disabled layer is skipped entirely but kept, so a user can A/B a filter without losing its
    // numbers. Same reasoning as TerrainRule::enabled.
    bool enabled = true;

    // Slope, Height, Curvature and LayerAlpha all reduce to "measure a scalar, shape it with a
    // range", so they share one range rather than each carrying a differently-named copy.
    MaskRange range = MaskRange::any();

    // Curvature only: the stencil radius in vertex units, i.e. the SCALE the curvature is measured
    // at. 1 unit is 4.167 yards.
    int curvature_step = 2;

    // Noise only. Wavelength in yards rather than frequency, because a mapper thinks in "patches
    // about 60 yards across", not in cycles per yard.
    float noise_wavelength = 64.0f;
    int noise_octaves = 3;
    // Amplitude ratio between successive octaves. 0.5 is the usual pink-ish falloff.
    float noise_gain = 0.5f;
    std::uint32_t noise_seed = 1337u;

    // LayerAlpha only: which texture's alpha to read. Matched the way the caller matches texture
    // names elsewhere; this module never opens a file and treats it as an opaque string, exactly as
    // TerrainRule::texture does.
    std::string texture;

    // AreaId only. Empty matches nothing, which is the fail-closed direction.
    std::vector<int> area_ids;

    // Constant only.
    float constant = 1.0f;
  };

  // Everything a filter can read about one point. Filled by the baker per texel.
  struct MaskFilterSample
  {
    float world_x = 0.0f;
    float world_z = 0.0f;
    float height = 0.0f;
    float slope_degrees = 0.0f;
    float curvature = 0.0f;
    // [0, 1]. The baker leaves this at 0 when the chunk has no such layer, which makes a LayerAlpha
    // filter contribute nothing there rather than contributing everything.
    float layer_alpha = 0.0f;
    // -1 when unknown, which matches nothing.
    int area_id = -1;
  };

  // This layer's own output for one sample, in [0, 1], before opacity and before combining.
  float evaluateMaskLayer(MaskFilterLayer const& layer, MaskFilterSample const& sample);

  // The whole stack, in order, folded to a byte.
  class MaskFilterStack
  {
  public:
    MaskFilterStack() = default;

    std::vector<MaskFilterLayer>& layers();
    std::vector<MaskFilterLayer> const& layers() const;

    std::size_t enabledCount() const;

    // Folds every enabled layer in list order. The first enabled layer replaces; see
    // MaskFilterLayer::combine. Uses sample.curvature for every curvature layer, so it is the right
    // entry point only when the stack measures curvature at one scale -- which is why the baker
    // calls evaluateWith instead.
    std::uint8_t evaluate(MaskFilterSample const& sample) const;

    // The same fold, with curvature resolved per layer through `curvature_at(step)`.
    //
    // The callback exists because curvature is SCALE-DEPENDENT and two layers may legitimately
    // measure at two different scales -- "the broad valley floor" and "the gullies inside it" are
    // different filters over the same terrain. A single float on MaskFilterSample cannot carry
    // both, and duplicating the six-line fold in the baker to work around that is how the two
    // copies start disagreeing about what "the first enabled layer" means.
    //
    // Inline here rather than in the .cpp because it is a template; the file stays pure STL either
    // way.
    template <typename CurvatureFn>
    std::uint8_t evaluateWith(MaskFilterSample sample, CurvatureFn&& curvature_at) const
    {
      float accumulated = 0.0f;
      bool first = true;

      for (MaskFilterLayer const& layer : _layers)
      {
        if (!layer.enabled)
        {
          continue;
        }

        if (layer.kind == MaskFilterKind::Curvature)
        {
          sample.curvature = curvature_at(layer.curvature_step < 1 ? 1 : layer.curvature_step);
        }

        float value = evaluateMaskLayer(layer, sample);

        // Opacity scales the layer's own output before the fold, so Multiply at opacity 0 is the
        // identity rather than a collapse to black. See TerrainMask::combineChunk for the same
        // decision at chunk granularity.
        float const opacity = layer.opacity < 0.0f ? 0.0f : (layer.opacity > 1.0f ? 1.0f : layer.opacity);
        value *= opacity;

        std::uint8_t const byte_value
          = static_cast<std::uint8_t>((value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value)) * 255.0f + 0.5f);

        std::uint8_t const accumulated_byte
          = static_cast<std::uint8_t>((accumulated < 0.0f ? 0.0f : (accumulated > 1.0f ? 1.0f : accumulated)) * 255.0f + 0.5f);

        // The first enabled layer REPLACES whatever its combinator says. There is nothing beneath
        // it to fold into, and the common mistakes -- opening a stack with Multiply, which would
        // yield an empty mask, or with Add, which would work by accident -- both produce a stack
        // the user cannot debug from the result. validate() reports it.
        std::uint8_t const folded
          = maskCombine(first ? MaskCombine::Replace : layer.combine, accumulated_byte, byte_value);

        accumulated = static_cast<float>(folded) / 255.0f;
        first = false;
      }

      return static_cast<std::uint8_t>
        ((accumulated < 0.0f ? 0.0f : (accumulated > 1.0f ? 1.0f : accumulated)) * 255.0f + 0.5f);
    }

    // Which inputs the baker actually has to produce. A stack of Height layers needs no slope grid
    // and no curvature grid, and asking saves 130 KiB of scratch and a pass over 16'641 vertices
    // per tile.
    bool needsHeightField() const;
    bool needsSlope() const;
    bool needsCurvature() const;
    bool needsLayerAlpha() const;
    bool needsAreaId() const;

    // The largest curvature_step over enabled Curvature layers, or 0 when there are none. The
    // height field caches ONE curvature grid, so a stack mixing two scales rebuilds it per texel and
    // is worth warning about -- see validate().
    int maxCurvatureStep() const;

    // Distinct texture names over enabled LayerAlpha layers, sorted. What the baker has to look up
    // per chunk.
    std::vector<std::string> requiredTextures() const;

    // Problems that make a layer useless or a stack surprising, one line each; empty when sound.
    // Wording is stable enough to assert on, matching TerrainRuleSet::validate.
    std::vector<std::string> validate() const;

  private:
    std::vector<MaskFilterLayer> _layers;
  };

  // --- Noise ---

  // Seeded fractal value noise over world XZ, in [0, 1].
  //
  // Value noise rather than Perlin or simplex: it is a dozen lines, it has no gradient table to
  // seed consistently across a save and reload, and a mask does not need the directional isotropy
  // that makes gradient noise worth its complexity. Deterministic for a given seed on every
  // platform -- it is integer arithmetic and a smoothstep, with no floating-point hashing.
  float maskValueNoise(float x, float z, float wavelength, int octaves, float gain, std::uint32_t seed);

  // --- Baking ---

  // Per-chunk inputs the world has to supply and the pure half cannot compute.
  struct MaskChunkInputs
  {
    // MASK_CHUNK_TEXELS bytes of alpha for the texture the stack asked for, or null when the chunk
    // has no such layer. Layout matches MCAL, col + 64 * row.
    std::uint8_t const* layer_alpha = nullptr;
    int area_id = -1;
  };

  // Bakes one chunk of `stack` into `out`, which must hold MASK_CHUNK_TEXELS bytes.
  //
  // `field` supplies height, slope and curvature for the TILE the chunk belongs to; the chunk's
  // position inside that tile comes from `address`. Returns false without writing when the address
  // is invalid, `out` is null, or the field holds no heights and the stack needs it -- see
  // MaskTileHeightField::filled for why that last one refuses rather than proceeding.
  bool bakeMaskChunk( MaskFilterStack const& stack
                    , MaskTileHeightField const& field
                    , MaskChunkInputs const& inputs
                    , MaskChunkAddress const& address
                    , std::uint8_t* out
                    );

  // --- The world walk ---
  //
  // Templates only, so this header pulls in no Noggit headers; the same arrangement and the same
  // reason as TerrainRuleCollector at the bottom of TerrainRules.hpp.
  namespace MaskFieldCollector
  {
    // MapChunk::indexNoLoD (MapChunk.cpp:361), restated. TerrainRulesTests already asserts this
    // formula against the original; TerrainMaskTests asserts it against TerrainRules' copy so the
    // two restatements cannot drift.
    inline constexpr int chunkOuterVertexIndex(int row, int col)
    {
      return row * 8 + row * 9 + col;
    }

    // Fills `field` from one tile's chunks.
    //
    // ChunkT is MapChunk and TileT is MapTile; both are template parameters purely to keep their
    // headers out of here. Only the public mVertices array is touched (MapChunk.h:101).
    //
    // The 9x9 outer grids of adjacent chunks SHARE their edge vertices, so writing all 81 of every
    // chunk's outer vertices writes the shared edges twice with the same value. That is deliberate:
    // the alternative -- skipping the last row and column of each chunk and handling the tile's
    // final row separately -- is two more index expressions for no gain.
    template <typename TileT, typename ChunkT>
    bool fillTileHeightField(TileT* tile, MaskTileHeightField& field)
    {
      // finishedLoading() for the reason TerrainRuleCollector::collectTileUnits gives: reading a
      // half-parsed tile's chunk array is a data race.
      if (!tile || !tile->finishedLoading())
      {
        return false;
      }

      for (int chunk_z = 0; chunk_z < MASK_TILE_CHUNK_SIDE; ++chunk_z)
      {
        for (int chunk_x = 0; chunk_x < MASK_TILE_CHUNK_SIDE; ++chunk_x)
        {
          ChunkT* const chunk = tile->getChunk( static_cast<unsigned int>(chunk_x)
                                              , static_cast<unsigned int>(chunk_z)
                                              );

          if (!chunk)
          {
            continue;
          }

          for (int row = 0; row < 9; ++row)
          {
            for (int col = 0; col < 9; ++col)
            {
              field.setHeight( chunk_x * 8 + col
                             , chunk_z * 8 + row
                             , static_cast<float>(chunk->mVertices[chunkOuterVertexIndex(row, col)].y)
                             );
            }
          }
        }
      }

      return true;
    }
  }
}

#endif // NOGGIT_TERRAINMASKFILTERS_HPP
