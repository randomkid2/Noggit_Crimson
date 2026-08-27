// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/AmbientOcclusion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// STL only, on purpose -- see the note at the top of AmbientOcclusion.hpp. Nothing in this file
// may include a Noggit, Qt, OpenGL or glm header, or tests/AmbientOcclusionTests.cpp stops
// linking on a machine with none of them installed.

namespace
{
  constexpr double PI = 3.14159265358979323846;
  constexpr double TWO_PI = 2.0 * PI;

  float quietNan()
  {
    return std::numeric_limits<float>::quiet_NaN();
  }

  // NaN-safe clamp into [0, 1], mapping NaN to 0.
  //
  // Written as negated comparisons rather than std::clamp because every comparison against NaN is
  // false: `!(value > 0)` is TRUE for NaN and returns the low end, whereas std::clamp(NaN, 0, 1)
  // returns NaN and would carry it straight into a vertex colour. This function is the single
  // place the module's "no input escapes [0, 1]" guarantee is enforced, so it is worth the
  // awkward spelling.
  float clamp01(float value)
  {
    if (!(value > 0.0f))
    {
      return 0.0f;
    }

    if (!(value < 1.0f))
    {
      return 1.0f;
    }

    return value;
  }

  float clampRange(float value, float low, float high)
  {
    if (!(value > low))
    {
      return low;
    }

    if (!(value < high))
    {
      return high;
    }

    return value;
  }

  bool isFinite(float value)
  {
    return std::isfinite(value);
  }

  float lerp(float from, float to, float t)
  {
    return from + (to - from) * t;
  }
}

namespace Noggit
{
  // --- HeightField -----------------------------------------------------------------------

  HeightField::HeightField() = default;

  HeightField::HeightField(int width, int height, float spacing, float fill_value)
  {
    if (width <= 0 || height <= 0 || !isFinite(spacing) || spacing <= 0.0f)
    {
      return;
    }

    _width = width;
    _height = height;
    _spacing = spacing;
    _heights.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill_value);
  }

  HeightField::HeightField(std::vector<float> heights, int width, float spacing)
  {
    if (width <= 0 || !isFinite(spacing) || spacing <= 0.0f || heights.empty())
    {
      return;
    }

    // A trailing partial row is dropped rather than padded. Padding would invent heights, and
    // inventing a height is the one thing this module must never do silently.
    int const rows = static_cast<int>(heights.size() / static_cast<std::size_t>(width));

    if (rows <= 0)
    {
      return;
    }

    heights.resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(width));

    _width = width;
    _height = rows;
    _spacing = spacing;
    _heights = std::move(heights);
  }

  int HeightField::width() const
  {
    return _width;
  }

  int HeightField::height() const
  {
    return _height;
  }

  float HeightField::spacing() const
  {
    return _spacing;
  }

  bool HeightField::empty() const
  {
    return _heights.empty();
  }

  std::size_t HeightField::size() const
  {
    return _heights.size();
  }

  void HeightField::setOrigin(float world_x, float world_z)
  {
    _origin_x = world_x;
    _origin_z = world_z;
  }

  float HeightField::originX() const
  {
    return _origin_x;
  }

  float HeightField::originZ() const
  {
    return _origin_z;
  }

  bool HeightField::contains(int x, int y) const
  {
    return !empty() && x >= 0 && y >= 0 && x < _width && y < _height;
  }

  float HeightField::at(int x, int y) const
  {
    if (empty())
    {
      return quietNan();
    }

    int const clamped_x = std::clamp(x, 0, _width - 1);
    int const clamped_y = std::clamp(y, 0, _height - 1);

    return _heights[static_cast<std::size_t>(clamped_y) * static_cast<std::size_t>(_width)
                    + static_cast<std::size_t>(clamped_x)];
  }

  void HeightField::set(int x, int y, float value)
  {
    if (!contains(x, y))
    {
      return;
    }

    _heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(_width)
             + static_cast<std::size_t>(x)] = value;
  }

  void HeightField::fill(float value)
  {
    std::fill(_heights.begin(), _heights.end(), value);
  }

  float HeightField::sampleGrid(float grid_x, float grid_y) const
  {
    if (empty() || !isFinite(grid_x) || !isFinite(grid_y))
    {
      return quietNan();
    }

    float const max_x = static_cast<float>(_width - 1);
    float const max_y = static_cast<float>(_height - 1);

    float const clamped_x = std::clamp(grid_x, 0.0f, max_x);
    float const clamped_y = std::clamp(grid_y, 0.0f, max_y);

    int const x0 = static_cast<int>(std::floor(clamped_x));
    int const y0 = static_cast<int>(std::floor(clamped_y));
    int const x1 = std::min(x0 + 1, _width - 1);
    int const y1 = std::min(y0 + 1, _height - 1);

    float const fx = clamped_x - static_cast<float>(x0);
    float const fy = clamped_y - static_cast<float>(y0);

    float const top = lerp(at(x0, y0), at(x1, y0), fx);
    float const bottom = lerp(at(x0, y1), at(x1, y1), fx);

    return lerp(top, bottom, fy);
  }

  float HeightField::sampleWorld(float world_x, float world_z) const
  {
    return sampleGrid(toGridX(world_x), toGridY(world_z));
  }

  float HeightField::toGridX(float world_x) const
  {
    return (world_x - _origin_x) / _spacing;
  }

  float HeightField::toGridY(float world_z) const
  {
    return (world_z - _origin_z) / _spacing;
  }

  float HeightField::toWorldX(float grid_x) const
  {
    return _origin_x + grid_x * _spacing;
  }

  float HeightField::toWorldZ(float grid_y) const
  {
    return _origin_z + grid_y * _spacing;
  }

  std::vector<float> const& HeightField::heights() const
  {
    return _heights;
  }

  std::size_t fillGaps(HeightField& field, int max_passes)
  {
    if (field.empty() || max_passes <= 0)
    {
      return 0;
    }

    struct Pending
    {
      int x;
      int y;
      float value;
    };

    std::vector<Pending> pending;
    std::size_t filled_total = 0;

    for (int pass = 0; pass < max_passes; ++pass)
    {
      pending.clear();

      for (int y = 0; y < field.height(); ++y)
      {
        for (int x = 0; x < field.width(); ++x)
        {
          if (isFinite(field.at(x, y)))
          {
            continue;
          }

          double sum = 0.0;
          int count = 0;

          for (int dy = -1; dy <= 1; ++dy)
          {
            for (int dx = -1; dx <= 1; ++dx)
            {
              if (dx == 0 && dy == 0)
              {
                continue;
              }

              // contains(), not at(): at() clamps, so a NaN node on the border would read
              // ITSELF as its own neighbour and never be recognised as unfillable.
              if (!field.contains(x + dx, y + dy))
              {
                continue;
              }

              float const neighbour = field.at(x + dx, y + dy);

              if (!isFinite(neighbour))
              {
                continue;
              }

              sum += static_cast<double>(neighbour);
              ++count;
            }
          }

          if (count == 0)
          {
            continue;
          }

          pending.push_back({x, y, static_cast<float>(sum / count)});
        }
      }

      if (pending.empty())
      {
        break;
      }

      // Applied only after the whole pass has been surveyed, so a fill never feeds the next fill
      // within the same pass and the result is independent of scan order.
      for (Pending const& entry : pending)
      {
        field.set(entry.x, entry.y, entry.value);
      }

      filled_total += pending.size();
    }

    return filled_total;
  }

  // --- settings --------------------------------------------------------------------------

  AmbientOcclusionSettings AmbientOcclusionSettings::sanitized() const
  {
    AmbientOcclusionSettings out = *this;

    // Upper bounds are cost ceilings, not correctness ones. 256 x 256 samples per vertex over a
    // tile is already ~4 billion reads; a caller that asks for more has a bug, not a plan.
    out.direction_count = std::clamp(direction_count, 1, 256);
    out.step_count = std::clamp(step_count, 1, 256);

    out.max_radius = (isFinite(max_radius) && max_radius > 0.0f) ? max_radius : 0.0f;
    out.strength = isFinite(strength) ? clampRange(strength, 0.0f, 8.0f) : 0.0f;
    // 89 rather than 90: a 90-degree bias would subtract the entire hemisphere and silently make
    // every bake a no-op, which is indistinguishable from the feature being broken.
    out.bias_degrees = isFinite(bias_degrees) ? clampRange(bias_degrees, 0.0f, 89.0f) : 0.0f;

    return out;
  }

  bool AmbientOcclusionSettings::valid() const
  {
    AmbientOcclusionSettings const clean = sanitized();

    return clean.max_radius > 0.0f && clean.strength > 0.0f;
  }

  AmbientOcclusionColorSettings AmbientOcclusionColorSettings::sanitized() const
  {
    AmbientOcclusionColorSettings out;

    out.open_factor = isFinite(open_factor) ? clampRange(open_factor, 0.0f, MCCV_MAX_FACTOR) : 1.0f;

    // Clamped to at most open_factor. This one line is what makes aoToMccvColor monotonically
    // non-increasing for any settings a caller can construct: the shadow end can never be
    // brighter than the open end, so the interpolation between them can never rise.
    float const requested_occluded = isFinite(occluded_factor) ? occluded_factor : 0.0f;
    out.occluded_factor = clampRange(requested_occluded, 0.0f, out.open_factor);

    out.shadow_tint_r = isFinite(shadow_tint_r) ? clamp01(shadow_tint_r) : 1.0f;
    out.shadow_tint_g = isFinite(shadow_tint_g) ? clamp01(shadow_tint_g) : 1.0f;
    out.shadow_tint_b = isFinite(shadow_tint_b) ? clamp01(shadow_tint_b) : 1.0f;

    // Lower bound above zero: contrast 0 makes pow(x, 0) == 1 for every x INCLUDING x == 0, which
    // would slam every vertex to the fully-occluded colour -- a black map from a value a slider
    // can reach.
    out.contrast = isFinite(contrast) ? clampRange(contrast, 0.05f, 20.0f) : 1.0f;

    return out;
  }

  // --- occlusion estimator ---------------------------------------------------------------

  float horizonAngleAtGrid( HeightField const& field
                          , float grid_x
                          , float grid_y
                          , float azimuth_radians
                          , AmbientOcclusionSettings const& settings
                          )
  {
    AmbientOcclusionSettings const clean = settings.sanitized();

    if (field.empty() || clean.max_radius <= 0.0f || !isFinite(azimuth_radians))
    {
      return 0.0f;
    }

    float const origin_height = field.sampleGrid(grid_x, grid_y);

    if (!isFinite(origin_height))
    {
      return 0.0f;
    }

    float const direction_x = std::cos(azimuth_radians);
    float const direction_y = std::sin(azimuth_radians);

    float max_tangent = 0.0f;

    // Radii are linear in the step index, starting at one step out rather than at zero: a sample
    // at radius zero is the origin itself and its tangent is 0/0.
    for (int step = 1; step <= clean.step_count; ++step)
    {
      float const radius = clean.max_radius * static_cast<float>(step)
                         / static_cast<float>(clean.step_count);
      float const grid_radius = radius / field.spacing();

      float const sample = field.sampleGrid( grid_x + direction_x * grid_radius
                                           , grid_y + direction_y * grid_radius
                                           );

      // Catches NaN (an unfilled node) and infinities alike. A skipped sample is not treated as
      // flat ground, it is simply not evidence either way.
      if (!isFinite(sample))
      {
        continue;
      }

      float const rise = sample - origin_height;

      // `!(rise > 0)` rather than `rise <= 0` so a NaN rise -- possible when origin_height and
      // sample are both huge and of opposite sign -- is rejected rather than compared.
      if (!(rise > 0.0f))
      {
        continue;
      }

      float const tangent = rise / radius;

      // rise can be up to ~3.4e38 and radius below 1, so the quotient can overflow to infinity
      // even though both operands are finite.
      if (isFinite(tangent) && tangent > max_tangent)
      {
        max_tangent = tangent;
      }
    }

    if (max_tangent <= 0.0f)
    {
      return 0.0f;
    }

    // atan, so the result is in [0, pi/2) for any finite non-negative tangent, with no separate
    // saturation case to get wrong.
    float const angle = std::atan(max_tangent);
    float const bias = static_cast<float>(static_cast<double>(clean.bias_degrees) * PI / 180.0);

    return std::max(0.0f, angle - bias);
  }

  float occlusionAtGrid( HeightField const& field
                       , float grid_x
                       , float grid_y
                       , AmbientOcclusionSettings const& settings
                       )
  {
    AmbientOcclusionSettings const clean = settings.sanitized();

    if (field.empty() || clean.max_radius <= 0.0f || clean.strength <= 0.0f)
    {
      return 0.0f;
    }

    if (!isFinite(grid_x) || !isFinite(grid_y))
    {
      return 0.0f;
    }

    if (!isFinite(field.sampleGrid(grid_x, grid_y)))
    {
      return 0.0f;
    }

    double occlusion_sum = 0.0;

    for (int direction = 0; direction < clean.direction_count; ++direction)
    {
      double const azimuth = TWO_PI * static_cast<double>(direction)
                           / static_cast<double>(clean.direction_count);

      float const angle = horizonAngleAtGrid(field, grid_x, grid_y, static_cast<float>(azimuth), clean);

      if (!(angle > 0.0f))
      {
        continue;
      }

      float const sin_angle = std::sin(angle);

      // Per azimuth slice, with the elevation measured from the horizontal:
      //
      //   solid angle only   visible = integral of cos(phi) dphi from angle to pi/2 = 1 - sin
      //   cosine weighted    visible = integral of sin(phi)cos(phi) dphi, normalised = cos^2
      //
      // so occluded is sin for the first and sin^2 = 1 - cos^2 for the second. Both are 0 at a
      // flat horizon, 1 at a vertical wall, and monotone in between -- which is what the
      // monotonicity tests rely on.
      float const occluded = clean.cosine_weighted ? (sin_angle * sin_angle) : sin_angle;

      occlusion_sum += static_cast<double>(clamp01(occluded));
    }

    double const mean = occlusion_sum / static_cast<double>(clean.direction_count);

    return clamp01(static_cast<float>(mean * static_cast<double>(clean.strength)));
  }

  float occlusionAtWorld( HeightField const& field
                        , float world_x
                        , float world_z
                        , AmbientOcclusionSettings const& settings
                        )
  {
    if (field.empty())
    {
      return 0.0f;
    }

    return occlusionAtGrid(field, field.toGridX(world_x), field.toGridY(world_z), settings);
  }

  std::vector<float> bakeOcclusionField(HeightField const& field, AmbientOcclusionSettings const& settings)
  {
    std::vector<float> result;

    if (field.empty())
    {
      return result;
    }

    result.resize(field.size(), 0.0f);

    for (int y = 0; y < field.height(); ++y)
    {
      for (int x = 0; x < field.width(); ++x)
      {
        result[static_cast<std::size_t>(y) * static_cast<std::size_t>(field.width())
               + static_cast<std::size_t>(x)]
          = occlusionAtGrid(field, static_cast<float>(x), static_cast<float>(y), settings);
      }
    }

    return result;
  }

  // --- occlusion -> MCCV -----------------------------------------------------------------

  bool operator==(MccvColor lhs, MccvColor rhs)
  {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
  }

  bool operator!=(MccvColor lhs, MccvColor rhs)
  {
    return !(lhs == rhs);
  }

  float mccvByteToFactor(unsigned char value)
  {
    // MapChunk.cpp:288, exactly. 127 and not 255 is the whole point: byte 127 gives 1.0f, the
    // neutral multiplier the terrain shader leaves alone (terrain_frag.glsl:353).
    return static_cast<float>(value) / static_cast<float>(MCCV_NEUTRAL_BYTE);
  }

  unsigned char mccvFactorToByte(float factor)
  {
    if (!(factor > 0.0f))
    {
      return 0;
    }

    float const scaled = factor * static_cast<float>(MCCV_NEUTRAL_BYTE);

    // Negated so infinity and NaN take this branch rather than reaching lround, whose result for
    // a value outside long's range is unspecified.
    if (!(scaled < static_cast<float>(MCCV_BYTE_MAX)))
    {
      return static_cast<unsigned char>(MCCV_BYTE_MAX);
    }

    long const rounded = std::lround(scaled);

    return static_cast<unsigned char>(std::clamp<long>(rounded, 0, MCCV_BYTE_MAX));
  }

  float clampMccvFactor(float factor)
  {
    return clampRange(factor, 0.0f, MCCV_MAX_FACTOR);
  }

  float aoToMccvFactor(float occlusion, AmbientOcclusionColorSettings const& settings)
  {
    AmbientOcclusionColorSettings const clean = settings.sanitized();

    float const shaped = std::pow(clamp01(occlusion), clean.contrast);

    return lerp(clean.open_factor, clean.occluded_factor, clamp01(shaped));
  }

  MccvColor aoToMccvColor(float occlusion, AmbientOcclusionColorSettings const& settings)
  {
    AmbientOcclusionColorSettings const clean = settings.sanitized();

    float const shaped = clamp01(std::pow(clamp01(occlusion), clean.contrast));

    // The tint multiplies the SHADOW end only, so occlusion 0 returns open_factor on all three
    // channels and an unoccluded vertex carries no hue shift at all.
    MccvColor out;
    out.r = mccvFactorToByte(lerp(clean.open_factor, clean.occluded_factor * clean.shadow_tint_r, shaped));
    out.g = mccvFactorToByte(lerp(clean.open_factor, clean.occluded_factor * clean.shadow_tint_g, shaped));
    out.b = mccvFactorToByte(lerp(clean.open_factor, clean.occluded_factor * clean.shadow_tint_b, shaped));

    return out;
  }

  MccvColor multiplyMccv(MccvColor base, MccvColor overlay)
  {
    MccvColor out;
    out.r = mccvFactorToByte(mccvByteToFactor(base.r) * mccvByteToFactor(overlay.r));
    out.g = mccvFactorToByte(mccvByteToFactor(base.g) * mccvByteToFactor(overlay.g));
    out.b = mccvFactorToByte(mccvByteToFactor(base.b) * mccvByteToFactor(overlay.b));

    return out;
  }

  MccvColor blendMccv(MccvColor base, MccvColor target, float weight)
  {
    float const t = clamp01(weight);

    MccvColor out;
    out.r = mccvFactorToByte(lerp(mccvByteToFactor(base.r), mccvByteToFactor(target.r), t));
    out.g = mccvFactorToByte(lerp(mccvByteToFactor(base.g), mccvByteToFactor(target.g), t));
    out.b = mccvFactorToByte(lerp(mccvByteToFactor(base.b), mccvByteToFactor(target.b), t));

    return out;
  }

  // --- chunk vertex lattice --------------------------------------------------------------

  ChunkVertexGridOffset chunkVertexGridOffset(int index)
  {
    if (index < 0 || index >= CHUNK_VERTEX_COUNT)
    {
      return ChunkVertexGridOffset();
    }

    int remaining = index;

    // The loader's own traversal, MapChunk.cpp:176-190: 17 rows, `(j % 2) ? 8 : 9` vertices each.
    for (int row = 0; row < 17; ++row)
    {
      bool const inner_row = (row % 2) != 0;
      int const count = inner_row ? 8 : 9;

      if (remaining < count)
      {
        ChunkVertexGridOffset offset;
        // xpos = i * UNITSIZE, plus half a unit on inner rows. At half-unit grid spacing that is
        // 2*i, plus 1.
        offset.column = inner_row ? (2 * remaining + 1) : (2 * remaining);
        // zpos = j * 0.5 * UNITSIZE, i.e. exactly one grid step per row.
        offset.row = row;

        return offset;
      }

      remaining -= count;
    }

    return ChunkVertexGridOffset();
  }

  int gridColumnForWorldX(HeightField const& field, float world_x)
  {
    float const grid_x = field.toGridX(world_x);

    if (!isFinite(grid_x))
    {
      return 0;
    }

    return static_cast<int>(std::lround(grid_x));
  }

  int gridRowForWorldZ(HeightField const& field, float world_z)
  {
    float const grid_y = field.toGridY(world_z);

    if (!isFinite(grid_y))
    {
      return 0;
    }

    return static_cast<int>(std::lround(grid_y));
  }
}
