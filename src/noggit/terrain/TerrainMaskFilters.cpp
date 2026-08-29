// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainMaskFilters.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
{
  constexpr float RADIANS_TO_DEGREES = 57.2957795130823209f;

  int clampInt(int value, int low, int high)
  {
    return value < low ? low : (value > high ? high : value);
  }

  float clampFloat(float value, float low, float high)
  {
    // NaN leaves through the low branch; see the identical helper in TerrainMask.cpp for why that
    // direction matters.
    if (!(value > low))
    {
      return low;
    }

    return value > high ? high : value;
  }

  bool isNan(float value)
  {
    return value != value;
  }

  // Integer lattice hash for the value noise. Deterministic on every platform because it is pure
  // 32-bit unsigned arithmetic -- no floating point, so no dependence on rounding mode, x87
  // intermediate precision, or the compiler's freedom to contract a multiply-add. A mask baked on
  // one machine and rebaked on another produces the same field, which is what makes the stack
  // rather than the texels the thing worth saving.
  std::uint32_t latticeHash(int x, int z, std::uint32_t seed)
  {
    std::uint32_t h = seed;
    h ^= static_cast<std::uint32_t>(x) * 0x8DA6B343u;
    h ^= static_cast<std::uint32_t>(z) * 0xD8163841u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
  }

  float latticeValue(int x, int z, std::uint32_t seed)
  {
    // The top 24 bits, so the result does not depend on the low bits the final xorshift has mixed
    // least. Divided by 2^24 rather than by 2^24 - 1, giving [0, 1).
    return static_cast<float>(latticeHash(x, z, seed) >> 8) / 16777216.0f;
  }

  float smoothstep(float t)
  {
    return t * t * (3.0f - 2.0f * t);
  }

  float valueNoise2D(float x, float z, std::uint32_t seed)
  {
    float const fx = std::floor(x);
    float const fz = std::floor(z);

    // The lattice coordinate is taken modulo 2^31 by the cast, which is exactly what is wanted: a
    // world position 34'133 yards from the origin at a 4-yard wavelength is only 8'533 lattice
    // steps out, nowhere near the wrap.
    int const ix = static_cast<int>(fx);
    int const iz = static_cast<int>(fz);

    float const tx = smoothstep(x - fx);
    float const tz = smoothstep(z - fz);

    float const v00 = latticeValue(ix, iz, seed);
    float const v10 = latticeValue(ix + 1, iz, seed);
    float const v01 = latticeValue(ix, iz + 1, seed);
    float const v11 = latticeValue(ix + 1, iz + 1, seed);

    float const top = v00 + (v10 - v00) * tx;
    float const bottom = v01 + (v11 - v01) * tx;

    return top + (bottom - top) * tz;
  }
}

namespace Noggit
{
  // --- MaskTileHeightField ---

  MaskTileHeightField::MaskTileHeightField()
    : _heights(static_cast<std::size_t>(MASK_TILE_VERTEX_COUNT), 0.0f)
  {
  }

  void MaskTileHeightField::setHeight(int vertex_x, int vertex_z, float height)
  {
    if ( vertex_x < 0 || vertex_x >= MASK_TILE_VERTEX_SIDE
      || vertex_z < 0 || vertex_z >= MASK_TILE_VERTEX_SIDE
       )
    {
      return;
    }

    _heights[static_cast<std::size_t>(vertex_z) * MASK_TILE_VERTEX_SIDE + static_cast<std::size_t>(vertex_x)]
      = height;

    _filled = true;

    invalidateDerived();
  }

  void MaskTileHeightField::invalidateDerived()
  {
    _slopes_valid = false;
    _curvature_step = 0;
    // The buffers themselves are kept. A bake fills the field vertex by vertex and then asks for
    // slope once; freeing and reallocating 65 KiB on each of 16'641 setHeight calls would dominate
    // the fill.
  }

  float MaskTileHeightField::height(int vertex_x, int vertex_z) const
  {
    // CLAMPED, not rejected. This is what makes the stencils below legal on the tile boundary, and
    // it is the edge-replication artefact documented on the class.
    int const x = clampInt(vertex_x, 0, MASK_TILE_VERTEX_SIDE - 1);
    int const z = clampInt(vertex_z, 0, MASK_TILE_VERTEX_SIDE - 1);

    return _heights[static_cast<std::size_t>(z) * MASK_TILE_VERTEX_SIDE + static_cast<std::size_t>(x)];
  }

  void MaskTileHeightField::ensureSlopes() const
  {
    if (_slopes_valid)
    {
      return;
    }

    _slopes.assign(static_cast<std::size_t>(MASK_TILE_VERTEX_COUNT), 0.0f);

    for (int z = 0; z < MASK_TILE_VERTEX_SIDE; ++z)
    {
      for (int x = 0; x < MASK_TILE_VERTEX_SIDE; ++x)
      {
        // Central difference over two units. At the boundary the clamp in height() collapses this
        // to a one-sided difference over ONE unit but still divides by two, which would halve the
        // reported slope on the tile edge. Correcting it explicitly costs one comparison per axis
        // and removes a 4.167-yard ring of artificially gentle terrain from every tile seam -- the
        // exact ring where a mapper is most likely to be blending two tiles together.
        float const span_x = (x == 0 || x == MASK_TILE_VERTEX_SIDE - 1) ? 1.0f : 2.0f;
        float const span_z = (z == 0 || z == MASK_TILE_VERTEX_SIDE - 1) ? 1.0f : 2.0f;

        float const dh_dx = (height(x + 1, z) - height(x - 1, z)) / (span_x * MASK_UNIT_SIZE);
        float const dh_dz = (height(x, z + 1) - height(x, z - 1)) / (span_z * MASK_UNIT_SIZE);

        // atan of the gradient magnitude. Identical to TerrainRules' slopeDegreesFromGradient,
        // which TerrainMaskTests asserts against this.
        _slopes[static_cast<std::size_t>(z) * MASK_TILE_VERTEX_SIDE + static_cast<std::size_t>(x)]
          = std::atan(std::sqrt(dh_dx * dh_dx + dh_dz * dh_dz)) * RADIANS_TO_DEGREES;
      }
    }

    _slopes_valid = true;
  }

  void MaskTileHeightField::ensureCurvature(int step) const
  {
    int const radius = step < 1 ? 1 : step;

    if (_curvature_step == radius)
    {
      return;
    }

    _curvature.assign(static_cast<std::size_t>(MASK_TILE_VERTEX_COUNT), 0.0f);

    double const spacing = static_cast<double>(radius) * static_cast<double>(MASK_UNIT_SIZE);
    double const inverse_spacing_squared = 1.0 / (spacing * spacing);

    for (int z = 0; z < MASK_TILE_VERTEX_SIDE; ++z)
    {
      for (int x = 0; x < MASK_TILE_VERTEX_SIDE; ++x)
      {
        double const centre = static_cast<double>(height(x, z));

        // The five-point discrete Laplacian. Positive when the four neighbours sit above the
        // centre, i.e. positive in a hollow -- see the sign note on the declaration.
        //
        // ACCUMULATED IN DOUBLE, and it is worth being clear about what that does and does not buy.
        // This is a subtraction of nearly equal numbers: on terrain 300 yards up, the five heights
        // agree to four significant figures and their weighted sum is the small residue left over.
        // Double removes the rounding of the SUM, but it cannot recover precision the float32
        // heights never had -- the inputs are already rounded, and that rounding is what sets the
        // floor measured below. It is free here and there is no reason to leave the extra error in.
        double const laplacian = static_cast<double>(height(x - radius, z))
                               + static_cast<double>(height(x + radius, z))
                               + static_cast<double>(height(x, z - radius))
                               + static_cast<double>(height(x, z + radius))
                               - 4.0 * centre;

        _curvature[static_cast<std::size_t>(z) * MASK_TILE_VERTEX_SIDE + static_cast<std::size_t>(x)]
          = static_cast<float>(laplacian * inverse_spacing_squared);
      }
    }

    _curvature_step = radius;
  }

  float MaskTileHeightField::slopeDegrees(int vertex_x, int vertex_z) const
  {
    ensureSlopes();

    int const x = clampInt(vertex_x, 0, MASK_TILE_VERTEX_SIDE - 1);
    int const z = clampInt(vertex_z, 0, MASK_TILE_VERTEX_SIDE - 1);

    return _slopes[static_cast<std::size_t>(z) * MASK_TILE_VERTEX_SIDE + static_cast<std::size_t>(x)];
  }

  float MaskTileHeightField::curvature(int vertex_x, int vertex_z, int step) const
  {
    ensureCurvature(step);

    int const x = clampInt(vertex_x, 0, MASK_TILE_VERTEX_SIDE - 1);
    int const z = clampInt(vertex_z, 0, MASK_TILE_VERTEX_SIDE - 1);

    return _curvature[static_cast<std::size_t>(z) * MASK_TILE_VERTEX_SIDE + static_cast<std::size_t>(x)];
  }

  namespace
  {
    // Bilinear lookup shared by the three sample functions. `at` reads one lattice point.
    template <typename AtFn>
    float sampleBilinear(float in_tile_x, float in_tile_z, AtFn&& at)
    {
      float const gx = clampFloat( in_tile_x / MASK_UNIT_SIZE
                                 , 0.0f
                                 , static_cast<float>(MASK_TILE_VERTEX_SIDE - 1)
                                 );
      float const gz = clampFloat( in_tile_z / MASK_UNIT_SIZE
                                 , 0.0f
                                 , static_cast<float>(MASK_TILE_VERTEX_SIDE - 1)
                                 );

      int const x0 = static_cast<int>(gx);
      int const z0 = static_cast<int>(gz);

      float const fx = gx - static_cast<float>(x0);
      float const fz = gz - static_cast<float>(z0);

      float const v00 = at(x0, z0);
      float const v10 = at(x0 + 1, z0);
      float const v01 = at(x0, z0 + 1);
      float const v11 = at(x0 + 1, z0 + 1);

      float const top = v00 + (v10 - v00) * fx;
      float const bottom = v01 + (v11 - v01) * fx;

      return top + (bottom - top) * fz;
    }
  }

  float MaskTileHeightField::sampleHeight(float in_tile_x, float in_tile_z) const
  {
    return sampleBilinear(in_tile_x, in_tile_z, [this] (int x, int z) { return height(x, z); });
  }

  float MaskTileHeightField::sampleSlopeDegrees(float in_tile_x, float in_tile_z) const
  {
    ensureSlopes();
    return sampleBilinear(in_tile_x, in_tile_z, [this] (int x, int z) { return slopeDegrees(x, z); });
  }

  float MaskTileHeightField::sampleCurvature(float in_tile_x, float in_tile_z, int step) const
  {
    // Built once here rather than four times inside the lambda: ensureCurvature is a no-op when the
    // step already matches, but the no-op is still a branch per lattice read on a path that runs
    // 4096 times per chunk.
    ensureCurvature(step);
    return sampleBilinear(in_tile_x, in_tile_z, [this, step] (int x, int z) { return curvature(x, z, step); });
  }

  bool MaskTileHeightField::filled() const
  {
    return _filled;
  }

  // --- MaskRange ---

  MaskRange MaskRange::any()
  {
    return MaskRange{};
  }

  MaskRange MaskRange::atLeast(float low_value, float feather_value)
  {
    MaskRange range;
    range.low = low_value;
    range.feather = feather_value;
    return range;
  }

  MaskRange MaskRange::atMost(float high_value, float feather_value)
  {
    MaskRange range;
    range.high = high_value;
    range.feather = feather_value;
    return range;
  }

  MaskRange MaskRange::between(float low_value, float high_value, float feather_value)
  {
    MaskRange range;
    range.low = low_value;
    range.high = high_value;
    range.feather = feather_value;
    return range;
  }

  bool MaskRange::inverted() const
  {
    return low > high;
  }

  bool MaskRange::bounded() const
  {
    return low > -1.0e29f || high < 1.0e29f;
  }

  float MaskRange::weight(float value) const
  {
    // NaN first. A sample that could not be computed must not match, exactly as TerrainRange
    // decides it (TerrainRules.hpp:66-70): failing closed means a filter whose input is broken
    // contributes nothing rather than contributing everything.
    if (isNan(value))
    {
      return 0.0f;
    }

    // An inverted interval matches nothing rather than being silently swapped. Swapping turns a
    // typo in two spin boxes into a mask covering half the continent, which costs far more to
    // notice than a mask covering nothing.
    if (inverted())
    {
      return 0.0f;
    }

    if (value >= low && value <= high)
    {
      return 1.0f;
    }

    if (!(feather > 0.0f))
    {
      return 0.0f;
    }

    float const distance = value < low ? (low - value) : (value - high);

    return distance >= feather ? 0.0f : (1.0f - distance / feather);
  }

  // --- Filter kinds ---

  char const* maskFilterKindName(MaskFilterKind kind)
  {
    switch (kind)
    {
      case MaskFilterKind::Slope:      return "slope";
      case MaskFilterKind::Height:     return "height";
      case MaskFilterKind::Curvature:  return "curvature";
      case MaskFilterKind::LayerAlpha: return "layer_alpha";
      case MaskFilterKind::AreaId:     return "area_id";
      case MaskFilterKind::Noise:      return "noise";
      case MaskFilterKind::Constant:   return "constant";
    }

    return "slope";
  }

  MaskFilterKind maskFilterKindFromName(char const* name)
  {
    if (!name)
    {
      return MaskFilterKind::Slope;
    }

    if (std::strcmp(name, "height") == 0)      { return MaskFilterKind::Height; }
    if (std::strcmp(name, "curvature") == 0)   { return MaskFilterKind::Curvature; }
    if (std::strcmp(name, "layer_alpha") == 0) { return MaskFilterKind::LayerAlpha; }
    if (std::strcmp(name, "area_id") == 0)     { return MaskFilterKind::AreaId; }
    if (std::strcmp(name, "noise") == 0)       { return MaskFilterKind::Noise; }
    if (std::strcmp(name, "constant") == 0)    { return MaskFilterKind::Constant; }

    return MaskFilterKind::Slope;
  }

  // --- Noise ---

  float maskValueNoise( float x
                      , float z
                      , float wavelength
                      , int octaves
                      , float gain
                      , std::uint32_t seed
                      )
  {
    if (!(wavelength > 0.0f))
    {
      return 0.0f;
    }

    // Capped at 8. Each octave doubles the frequency, so octave 8 has a wavelength of
    // wavelength / 128; at the default 64 yards that is already half a yard, which is finer than
    // one mask texel and can only alias. Letting the spin box go higher would spend time producing
    // noise the field cannot represent.
    int const count = clampInt(octaves, 1, 8);
    float const falloff = clampFloat(gain, 0.0f, 1.0f);

    float frequency = 1.0f / wavelength;
    float amplitude = 1.0f;
    float total = 0.0f;
    float normaliser = 0.0f;

    for (int octave = 0; octave < count; ++octave)
    {
      // The seed is offset per octave so the octaves are independent fields rather than the same
      // field at different scales, which would produce visible self-similar clumping along the
      // lattice diagonals.
      total += amplitude * valueNoise2D(x * frequency, z * frequency, seed + static_cast<std::uint32_t>(octave) * 0x9E3779B9u);
      normaliser += amplitude;

      amplitude *= falloff;
      frequency *= 2.0f;
    }

    // normaliser is at least 1 because the first octave's amplitude is 1, so this cannot divide by
    // zero even at gain 0.
    return total / normaliser;
  }

  // --- Layer evaluation ---

  float evaluateMaskLayer(MaskFilterLayer const& layer, MaskFilterSample const& sample)
  {
    float weight = 0.0f;

    switch (layer.kind)
    {
      case MaskFilterKind::Slope:
        weight = layer.range.weight(sample.slope_degrees);
        break;

      case MaskFilterKind::Height:
        weight = layer.range.weight(sample.height);
        break;

      case MaskFilterKind::Curvature:
        weight = layer.range.weight(sample.curvature);
        break;

      case MaskFilterKind::LayerAlpha:
        // A FIELD filter, not a TEST filter, and the distinction is why this is not simply
        // range.weight. An unbounded range on a slope filter means "do not constrain slope", which
        // is a weight of 1. An unbounded range on a filter whose input is ALREADY a mask in [0, 1]
        // means "use it as it is" -- returning 1 would throw away the only thing the layer was
        // added to read. Setting a range turns it back into a test, which is how a user thresholds
        // an existing layer into a hard mask.
        weight = layer.range.bounded() ? layer.range.weight(sample.layer_alpha)
                                       : clampFloat(sample.layer_alpha, 0.0f, 1.0f);
        break;

      case MaskFilterKind::AreaId:
      {
        // Fail-closed on an empty list: a filter the user has not finished configuring contributes
        // nothing rather than selecting the whole map.
        weight = 0.0f;

        for (int id : layer.area_ids)
        {
          if (id == sample.area_id)
          {
            weight = 1.0f;
            break;
          }
        }

        break;
      }

      case MaskFilterKind::Noise:
      {
        float const noise = maskValueNoise( sample.world_x
                                          , sample.world_z
                                          , layer.noise_wavelength
                                          , layer.noise_octaves
                                          , layer.noise_gain
                                          , layer.noise_seed
                                          );

        // A field filter, same as LayerAlpha; see the note there.
        weight = layer.range.bounded() ? layer.range.weight(noise) : noise;
        break;
      }

      case MaskFilterKind::Constant:
        weight = clampFloat(layer.constant, 0.0f, 1.0f);
        break;
    }

    // Per-layer invert, applied before opacity and before the combinator. This is the local
    // complement -- "not steep" -- and it is a different thing from TerrainMask::invert, which
    // complements the finished field including the chunks it never stored.
    return layer.invert ? (1.0f - weight) : weight;
  }

  // --- MaskFilterStack ---

  std::vector<MaskFilterLayer>& MaskFilterStack::layers()
  {
    return _layers;
  }

  std::vector<MaskFilterLayer> const& MaskFilterStack::layers() const
  {
    return _layers;
  }

  std::size_t MaskFilterStack::enabledCount() const
  {
    std::size_t count = 0;

    for (MaskFilterLayer const& layer : _layers)
    {
      count += layer.enabled ? 1u : 0u;
    }

    return count;
  }

  std::uint8_t MaskFilterStack::evaluate(MaskFilterSample const& sample) const
  {
    return evaluateWith(sample, [&sample] (int) { return sample.curvature; });
  }

  bool MaskFilterStack::needsHeightField() const
  {
    for (MaskFilterLayer const& layer : _layers)
    {
      if (!layer.enabled)
      {
        continue;
      }

      if ( layer.kind == MaskFilterKind::Slope
        || layer.kind == MaskFilterKind::Height
        || layer.kind == MaskFilterKind::Curvature
         )
      {
        return true;
      }
    }

    return false;
  }

  bool MaskFilterStack::needsSlope() const
  {
    for (MaskFilterLayer const& layer : _layers)
    {
      if (layer.enabled && layer.kind == MaskFilterKind::Slope)
      {
        return true;
      }
    }

    return false;
  }

  bool MaskFilterStack::needsCurvature() const
  {
    return maxCurvatureStep() > 0;
  }

  bool MaskFilterStack::needsLayerAlpha() const
  {
    for (MaskFilterLayer const& layer : _layers)
    {
      if (layer.enabled && layer.kind == MaskFilterKind::LayerAlpha)
      {
        return true;
      }
    }

    return false;
  }

  bool MaskFilterStack::needsAreaId() const
  {
    for (MaskFilterLayer const& layer : _layers)
    {
      if (layer.enabled && layer.kind == MaskFilterKind::AreaId)
      {
        return true;
      }
    }

    return false;
  }

  int MaskFilterStack::maxCurvatureStep() const
  {
    int largest = 0;

    for (MaskFilterLayer const& layer : _layers)
    {
      if (layer.enabled && layer.kind == MaskFilterKind::Curvature)
      {
        int const step = layer.curvature_step < 1 ? 1 : layer.curvature_step;
        largest = step > largest ? step : largest;
      }
    }

    return largest;
  }

  std::vector<std::string> MaskFilterStack::requiredTextures() const
  {
    std::vector<std::string> textures;

    for (MaskFilterLayer const& layer : _layers)
    {
      if (layer.enabled && layer.kind == MaskFilterKind::LayerAlpha && !layer.texture.empty())
      {
        textures.push_back(layer.texture);
      }
    }

    std::sort(textures.begin(), textures.end());
    textures.erase(std::unique(textures.begin(), textures.end()), textures.end());

    return textures;
  }

  std::vector<std::string> MaskFilterStack::validate() const
  {
    std::vector<std::string> problems;

    if (enabledCount() == 0)
    {
      problems.push_back("no enabled layers: the mask would be empty everywhere");
    }

    int distinct_curvature_steps = 0;
    std::vector<int> seen_steps;

    for (std::size_t i = 0; i < _layers.size(); ++i)
    {
      MaskFilterLayer const& layer = _layers[i];

      if (!layer.enabled)
      {
        continue;
      }

      std::string const prefix = "layer " + std::to_string(i + 1) + ": ";

      if (layer.range.inverted())
      {
        problems.push_back(prefix + "range is inverted and matches nothing");
      }

      if (layer.range.feather < 0.0f)
      {
        problems.push_back(prefix + "feather is negative and is treated as zero");
      }

      switch (layer.kind)
      {
        case MaskFilterKind::Curvature:
          if (layer.curvature_step < 1)
          {
            problems.push_back(prefix + "curvature step below 1 is treated as 1");
          }

          if (std::find(seen_steps.begin(), seen_steps.end(), layer.curvature_step) == seen_steps.end())
          {
            seen_steps.push_back(layer.curvature_step);
            ++distinct_curvature_steps;
          }

          break;

        case MaskFilterKind::LayerAlpha:
          if (layer.texture.empty())
          {
            problems.push_back(prefix + "layer alpha filter names no texture and contributes nothing");
          }

          break;

        case MaskFilterKind::AreaId:
          if (layer.area_ids.empty())
          {
            problems.push_back(prefix + "area filter lists no area ids and matches nothing");
          }

          break;

        case MaskFilterKind::Noise:
          if (!(layer.noise_wavelength > 0.0f))
          {
            problems.push_back(prefix + "noise wavelength must be greater than zero");
          }

          break;

        default:
          break;
      }
    }

    // Advisory, not fatal, and it is here because the cost is invisible from the dialog. The height
    // field caches ONE curvature grid at a time (MaskTileHeightField::ensureCurvature), so a stack
    // that alternates between two scales rebuilds a 16'641-entry grid on every texel rather than
    // once per tile -- a bake that should take a fraction of a second instead takes minutes. Two
    // layers at the same scale cost nothing.
    if (distinct_curvature_steps > 1)
    {
      problems.push_back
        ( "curvature layers use "
        + std::to_string(distinct_curvature_steps)
        + " different scales: each extra scale rebuilds the curvature grid per texel and makes the bake very slow"
        );
    }

    // The first enabled layer always replaces, whatever its combinator says, so a stack that opens
    // with Multiply is not broken -- but the user who wrote it expected something else and should
    // be told which layer is being reinterpreted.
    for (std::size_t i = 0; i < _layers.size(); ++i)
    {
      if (!_layers[i].enabled)
      {
        continue;
      }

      if (_layers[i].combine != MaskCombine::Replace)
      {
        problems.push_back
          ( "layer " + std::to_string(i + 1)
          + " is the first enabled layer, so its combinator is ignored and it replaces"
          );
      }

      break;
    }

    return problems;
  }

  // --- Baking ---

  bool bakeMaskChunk( MaskFilterStack const& stack
                    , MaskTileHeightField const& field
                    , MaskChunkInputs const& inputs
                    , MaskChunkAddress const& address
                    , std::uint8_t* out
                    )
  {
    if (!address.valid() || !out)
    {
      return false;
    }

    // Refuses rather than deriving a mask from a plane at y = 0. An all-zero height field produces
    // slope 0 and curvature 0 everywhere, so a "slope above 40" stack would bake a confidently
    // empty mask and a "slope below 10" stack would bake a confidently full one. Both look like
    // answers.
    if (stack.needsHeightField() && !field.filled())
    {
      return false;
    }

    bool const wants_slope = stack.needsSlope();
    bool const wants_curvature = stack.needsCurvature();
    bool const wants_height = stack.needsHeightField();

    float const chunk_origin_in_tile_x = static_cast<float>(address.chunk_x) * MASK_CHUNK_SIZE;
    float const chunk_origin_in_tile_z = static_cast<float>(address.chunk_z) * MASK_CHUNK_SIZE;

    float const tile_origin_x = static_cast<float>(address.tile_x) * MASK_TILE_SIZE;
    float const tile_origin_z = static_cast<float>(address.tile_z) * MASK_TILE_SIZE;

    for (int row = 0; row < MASK_CHUNK_SIDE; ++row)
    {
      // Texel CENTRES, matching the convention TerrainRules::evaluateGrid states: an alphamap texel
      // is an area, not a point, and sampling its corner biases every derived mask half a texel
      // north-west of the paint it will clip.
      float const in_tile_z = chunk_origin_in_tile_z
                            + (static_cast<float>(row) + 0.5f) * MASK_TEXEL_SIZE;

      for (int col = 0; col < MASK_CHUNK_SIDE; ++col)
      {
        float const in_tile_x = chunk_origin_in_tile_x
                              + (static_cast<float>(col) + 0.5f) * MASK_TEXEL_SIZE;

        MaskFilterSample sample;
        sample.world_x = tile_origin_x + in_tile_x;
        sample.world_z = tile_origin_z + in_tile_z;
        sample.area_id = inputs.area_id;

        std::size_t const offset
          = static_cast<std::size_t>(col) + MASK_CHUNK_SIDE * static_cast<std::size_t>(row);

        if (wants_height)
        {
          sample.height = field.sampleHeight(in_tile_x, in_tile_z);
        }

        if (wants_slope)
        {
          sample.slope_degrees = field.sampleSlopeDegrees(in_tile_x, in_tile_z);
        }

        if (inputs.layer_alpha)
        {
          sample.layer_alpha = static_cast<float>(inputs.layer_alpha[offset]) / 255.0f;
        }

        // Curvature is resolved per LAYER rather than once per texel, because each Curvature layer
        // may measure at its own scale. The lambda is what evaluateWith calls, and it only fires
        // for layers that actually need it -- a stack with no curvature layer never touches the
        // curvature grid, which is why wants_curvature only guards the fast path below.
        out[offset] = stack.evaluateWith
          ( sample
          , [&field, &in_tile_x, &in_tile_z, wants_curvature] (int step) -> float
            {
              return wants_curvature ? field.sampleCurvature(in_tile_x, in_tile_z, step) : 0.0f;
            }
          );
      }
    }

    return true;
  }
}
