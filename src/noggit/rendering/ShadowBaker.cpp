// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/ShadowBaker.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace
{
  // Beyond this the light is close enough to vertical that (0, 1, 0) is useless as an up vector.
  //
  // 0.999 on the dot product is about 2.56 degrees off vertical. Chosen well clear of the point
  // where lookAt actually breaks rather than as close to it as float precision allows: the cross
  // product does not fail suddenly at exactly parallel, it loses precision progressively as the
  // vectors converge, so a basis built at 0.5 degrees off vertical is numerically poor long
  // before it is NaN. Switching early costs nothing -- the two choices of up vector give the same
  // shadows, only a different rotation of the depth texture within its own square.
  constexpr float UP_VECTOR_DEGENERACY_LIMIT = 0.999f;

  constexpr float PI_OVER_180 = 0.01745329251994329577f;

  bool isFinite(float value)
  {
    return std::isfinite(value);
  }

  bool isFinite(glm::vec3 const& v)
  {
    return isFinite(v.x) && isFinite(v.y) && isFinite(v.z);
  }
}

namespace Noggit::Rendering
{
  ShadowBakeSettings ShadowBakeSettings::sanitized() const
  {
    ShadowBakeSettings result (*this);

    // The lower bound on pitch is 1 degree, matching Green's control. It is not cosmetic: at 0
    // the light is horizontal, the fitted box has no thickness in the axis the projection needs
    // one in, and the matrix is singular.
    result.sun_pitch_degrees = isFinite(result.sun_pitch_degrees)
      ? std::clamp(result.sun_pitch_degrees, 1.0f, 90.0f)
      : 47.0f;

    // Wrapped rather than clamped -- an azimuth is periodic, and clamping 350 degrees to 180
    // would silently point the sun the opposite way.
    if (isFinite(result.sun_yaw_degrees))
    {
      result.sun_yaw_degrees = std::fmod(result.sun_yaw_degrees, 360.0f);

      if (result.sun_yaw_degrees < 0.0f)
      {
        result.sun_yaw_degrees += 360.0f;
      }
    }
    else
    {
      result.sun_yaw_degrees = 0.0f;
    }

    // 256 and not 255: on Green's scale 256 is the "shadow nothing" end, one past the largest
    // coverage a texel can report, and it has to stay reachable or the slider has no off position.
    result.threshold = std::clamp(result.threshold, 0, 256);

    // The floor is 1024 because that is the point at which the depth pass stops being finer than
    // the MCSH grid it feeds even with the sun directly overhead; below it the bake cannot
    // resolve what it is being asked to write. The ceiling is 8192, which is 256 MB of float
    // readback and already past what any GL 4.1 implementation guarantees for a renderbuffer.
    result.depth_resolution = std::clamp(result.depth_resolution, 1024, 8192);

    result.bias_yards = isFinite(result.bias_yards)
      ? std::clamp(result.bias_yards, 0.0f, 50.0f)
      : 1.0f;

    // 4 per axis is 16 samples per texel, 65536 per chunk, 16.7 million per tile. That is the
    // finishing-pass setting and it is seconds of CPU; more would not visibly improve a one-bit
    // output.
    result.supersample = std::clamp(result.supersample, 1, 4);

    result.caster_margin_yards = isFinite(result.caster_margin_yards)
      ? std::clamp(result.caster_margin_yards, 0.0f, 2000.0f)
      : 200.0f;

    return result;
  }

  bool SunDepthMap::valid() const
  {
    return resolution > 0
      && depth.size() == static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution)
      && far_plane > near_plane;
  }

  glm::vec3 sunLightDirection(float pitch_degrees, float yaw_degrees)
  {
    float const pitch = pitch_degrees * PI_OVER_180;
    float const yaw = yaw_degrees * PI_OVER_180;

    float const cos_pitch = std::cos(pitch);

    // Negative y because this is the direction the light TRAVELS, downward from a sun above the
    // horizon. Getting this sign backwards produces a bake lit from underneath the terrain, which
    // renders as every downward-facing surface shadowed and every upward-facing one lit -- a
    // result that looks deliberate enough to survive review.
    glm::vec3 const direction ( cos_pitch * std::cos(yaw)
                              , -std::sin(pitch)
                              , cos_pitch * std::sin(yaw)
                              );

    float const length = glm::length(direction);

    if (!isFinite(direction) || length < 1e-6f)
    {
      return glm::vec3(0.0f, -1.0f, 0.0f);
    }

    return direction / length;
  }

  bool makeSunTransform( glm::vec3 const& min_bounds
                       , glm::vec3 const& max_bounds
                       , ShadowBakeSettings const& settings
                       , SunDepthMap& out
                       )
  {
    if (!isFinite(min_bounds) || !isFinite(max_bounds))
    {
      return false;
    }

    ShadowBakeSettings const clean (settings.sanitized());

    // Grown on every side before the fit, so a caster standing beside the tile is inside the
    // box's lateral extent as well as its depth. Growing the world box and then fitting is
    // simpler to be sure of than growing the fitted result, because the margin stays isotropic in
    // world space whatever direction the light is coming from.
    glm::vec3 const margin (clean.caster_margin_yards);
    glm::vec3 const grown_min (min_bounds - margin);
    glm::vec3 const grown_max (max_bounds + margin);

    if (grown_max.x <= grown_min.x || grown_max.y <= grown_min.y || grown_max.z <= grown_min.z)
    {
      return false;
    }

    glm::vec3 const light_direction (sunLightDirection(clean.sun_pitch_degrees, clean.sun_yaw_degrees));
    glm::vec3 const centre ((grown_min + grown_max) * 0.5f);

    // See the header: (0, 1, 0) is antiparallel to the light at a 90-degree pitch and lookAt
    // returns an all-NaN matrix, which would make every texel test as unshadowed and the bake
    // appear to run and do nothing.
    glm::vec3 up (0.0f, 1.0f, 0.0f);

    if (std::abs(glm::dot(light_direction, up)) > UP_VECTOR_DEGENERACY_LIMIT)
    {
      up = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    // First fit: eye at the centre of the box, purely to learn the box's extent in light space.
    glm::mat4 const probe_view (glm::lookAt(centre, centre + light_direction, up));

    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max();
    float max_y = std::numeric_limits<float>::lowest();
    float min_z = std::numeric_limits<float>::max();
    float max_z = std::numeric_limits<float>::lowest();

    // All eight corners, not just two. The box is axis-aligned in WORLD space and the view is
    // not, so the transformed corners are not axis-aligned in view space and no two of them
    // bound the rest. Taking only the min and max corners is the classic way to end up with a
    // box that clips its own geometry at every pitch except 90 degrees.
    for (int corner = 0; corner < 8; ++corner)
    {
      glm::vec3 const world ( (corner & 1) ? grown_max.x : grown_min.x
                            , (corner & 2) ? grown_max.y : grown_min.y
                            , (corner & 4) ? grown_max.z : grown_min.z
                            );

      glm::vec3 const view (probe_view * glm::vec4(world, 1.0f));

      min_x = std::min(min_x, view.x);
      max_x = std::max(max_x, view.x);
      min_y = std::min(min_y, view.y);
      max_y = std::max(max_y, view.y);
      min_z = std::min(min_z, view.z);
      max_z = std::max(max_z, view.z);
    }

    if (!isFinite(min_x) || !isFinite(max_x) || !isFinite(min_y)
        || !isFinite(max_y) || !isFinite(min_z) || !isFinite(max_z))
    {
      return false;
    }

    // View space looks down -Z, so a corner in front of the eye has a negative z and the most
    // negative corner is the farthest. Pushing the eye back by (max_z + margin) along -light
    // shifts every corner's z down by that amount, which places the nearest corner at exactly
    // -margin: the near plane comes out at +caster_margin_yards and the whole box, plus that much
    // room for casters standing between the box and the sun, is in front of the eye.
    float const pull_back = max_z + clean.caster_margin_yards;
    glm::vec3 const eye (centre - light_direction * pull_back);

    float const near_plane = clean.caster_margin_yards;
    float const far_plane = (max_z - min_z) + clean.caster_margin_yards;

    if (!(far_plane > near_plane))
    {
      return false;
    }

    out.light_view = glm::lookAt(eye, eye + light_direction, up);
    out.light_projection = glm::ortho(min_x, max_x, min_y, max_y, near_plane, far_plane);
    out.light_view_projection = out.light_projection * out.light_view;

    out.near_plane = near_plane;
    out.far_plane = far_plane;

    return true;
  }

  float shadowBiasInDepthUnits(SunDepthMap const& map, float bias_yards)
  {
    float const range = map.far_plane - map.near_plane;

    if (!(range > 0.0f) || !isFinite(bias_yards))
    {
      return 0.0f;
    }

    return bias_yards / range;
  }

  float sampleDepth(SunDepthMap const& map, float u, float v)
  {
    if (!map.valid() || u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
    {
      return -1.0f;
    }

    int const resolution = map.resolution;

    // Floor then clamp, so u or v of exactly 1.0 lands on the last texel instead of one past it.
    int const x = std::clamp(static_cast<int>(u * static_cast<float>(resolution)), 0, resolution - 1);
    int const y = std::clamp(static_cast<int>(v * static_cast<float>(resolution)), 0, resolution - 1);

    // Row y counted from the BOTTOM, matching glReadPixels' framebuffer origin. v is the same
    // normalised device coordinate the projection produced, and GL's y axis runs upward in both,
    // so no flip belongs here; adding one would mirror every shadow about the tile's centre line,
    // which at a low sun looks like a plausible shadow cast the wrong way.
    return map.depth[static_cast<std::size_t>(y) * static_cast<std::size_t>(resolution)
                     + static_cast<std::size_t>(x)];
  }

  bool isPointInShadow( SunDepthMap const& map
                      , glm::vec3 const& world_pos
                      , float bias_depth_units
                      )
  {
    if (!map.valid())
    {
      return false;
    }

    glm::vec4 const clip (map.light_view_projection * glm::vec4(world_pos, 1.0f));

    // No perspective divide: the projection is orthographic so w is exactly 1. Dividing anyway
    // would be harmless but would imply to a reader that it might not be.
    float const u = clip.x * 0.5f + 0.5f;
    float const v = clip.y * 0.5f + 0.5f;
    float const reference = clip.z * 0.5f + 0.5f;

    if (!isFinite(u) || !isFinite(v) || !isFinite(reference))
    {
      return false;
    }

    // Outside the depth pass's own depth range means the point is nearer than the near plane or
    // beyond the far plane, i.e. the fit did not cover it. Lit, not shadowed.
    if (reference < 0.0f || reference > 1.0f)
    {
      return false;
    }

    float const occluder = sampleDepth(map, u, v);

    // Negative is the "not covered" signal from sampleDepth. A depth of exactly 1.0 is the
    // cleared far plane -- nothing was drawn along that ray at all -- and is likewise lit.
    if (occluder < 0.0f || occluder >= 1.0f)
    {
      return false;
    }

    // The bias moves the SAMPLE toward the sun, which in an orthographic light space is the same
    // as lowering its reference depth by a constant. Written on the occluder's side of the
    // comparison for the same reason, one addition instead of one subtraction per sample.
    return occluder + bias_depth_units < reference;
  }

  float terrainHeightInChunk(glm::vec3 const* vertices, float local_x, float local_z)
  {
    if (!vertices)
    {
      return 0.0f;
    }

    if (!isFinite(local_x) || !isFinite(local_z))
    {
      return 0.0f;
    }

    float const clamped_x = std::clamp(local_x, 0.0f, CHUNK_SIZE_YARDS);
    float const clamped_z = std::clamp(local_z, 0.0f, CHUNK_SIZE_YARDS);

    float const cell_x = clamped_x / UNIT_SIZE_YARDS;
    float const cell_z = clamped_z / UNIT_SIZE_YARDS;

    // Clamped to 7 so a point exactly on the chunk's far edge belongs to the last cell with
    // u or v of 1 rather than to a ninth cell that has no vertices.
    int const column = std::clamp(static_cast<int>(cell_x), 0, 7);
    int const row = std::clamp(static_cast<int>(cell_z), 0, 7);

    float const u = std::clamp(cell_x - static_cast<float>(column), 0.0f, 1.0f);
    float const v = std::clamp(cell_z - static_cast<float>(row), 0.0f, 1.0f);

    // row*17 + column for the outer lattice and row*17 + 9 + column for the cell centre, which
    // is MapChunk::indexNoLoD and MapChunk::indexLoD written out (MapChunk.cpp:354-362). The
    // largest index reached is 7*17 + 9 + 7 = 135 for a centre and 8*17 + 8 = 144 for a corner,
    // both inside the 145 the caller supplies.
    int const base = row * 17;

    float const h00 = vertices[base + column].y;
    float const h10 = vertices[base + column + 1].y;
    float const h01 = vertices[base + 17 + column].y;
    float const h11 = vertices[base + 17 + column + 1].y;
    float const hc = vertices[base + 9 + column].y;

    // Which of the four triangles of the fan contains (u, v). The two diagonals of the cell are
    // v = u and v = 1 - u, so the sum and the difference pick the quadrant between them.
    //
    // Each branch is the barycentric expansion for that triangle against the cell centre, and
    // each is checkable by hand at its own three corners: the north form below gives h00 at
    // (0,0), h10 at (1,0) and hc at (0.5,0.5).
    if (u + v < 1.0f)
    {
      if (v < u)
      {
        // North: corners (0,0) and (1,0) with the centre.
        return h00 * (1.0f - u - v) + h10 * (u - v) + hc * (2.0f * v);
      }

      // West: corners (0,0) and (0,1) with the centre.
      return h00 * (1.0f - u - v) + h01 * (v - u) + hc * (2.0f * u);
    }

    if (v < u)
    {
      // East: corners (1,0) and (1,1) with the centre.
      return h10 * (u - v) + h11 * (u + v - 1.0f) + hc * (2.0f * (1.0f - u));
    }

    // South: corners (0,1) and (1,1) with the centre.
    return h01 * (v - u) + h11 * (u + v - 1.0f) + hc * (2.0f * (1.0f - v));
  }

  glm::vec3 mcshTexelCentre(float xbase, float zbase, int column, int row)
  {
    return glm::vec3( xbase + (static_cast<float>(column) + 0.5f) * MCSH_TEXEL_SIZE_YARDS
                    , 0.0f
                    , zbase + (static_cast<float>(row) + 0.5f) * MCSH_TEXEL_SIZE_YARDS
                    );
  }

  int bakeChunkShadowMap( SunDepthMap const& map
                        , glm::vec3 const* vertices
                        , float xbase
                        , float zbase
                        , ShadowBakeSettings const& settings
                        , std::uint8_t* out_shadow_map
                        )
  {
    if (!out_shadow_map)
    {
      return 0;
    }

    // Cleared unconditionally, before the validity checks below can bail. A bake that cannot run
    // must leave a defined map rather than whatever was in the caller's buffer, and the caller
    // treats "no texels set" as a result it can report rather than as an error.
    std::fill(out_shadow_map, out_shadow_map + MCSH_TEXEL_COUNT, static_cast<std::uint8_t>(0));

    if (!map.valid() || !vertices)
    {
      return 0;
    }

    ShadowBakeSettings const clean (settings.sanitized());

    // 256 is the "shadow nothing" end of the scale and no coverage can reach it, so the whole
    // sampling loop is skippable. Checked rather than left to fall out of the comparison because
    // at supersample 4 that loop is 65536 depth lookups per chunk.
    if (clean.threshold > 255)
    {
      return 0;
    }

    int const samples_per_axis = clean.supersample;
    float const sample_count = static_cast<float>(samples_per_axis * samples_per_axis);
    float const sample_step = MCSH_TEXEL_SIZE_YARDS / static_cast<float>(samples_per_axis);

    // Offsets of the sub-samples from the texel's low corner: the centres of an N x N grid over
    // the texel footprint. At N = 1 this is 0.5 * texel, the texel centre, so the supersampled
    // and single-sample paths agree about where the one sample goes.
    float const first_offset = 0.5f * sample_step;

    float const bias = shadowBiasInDepthUnits(map, clean.bias_yards);
    float const threshold = static_cast<float>(clean.threshold);

    int texels_set = 0;

    for (int row = 0; row < MCSH_RESOLUTION; ++row)
    {
      float const texel_z = zbase + static_cast<float>(row) * MCSH_TEXEL_SIZE_YARDS;

      for (int column = 0; column < MCSH_RESOLUTION; ++column)
      {
        float const texel_x = xbase + static_cast<float>(column) * MCSH_TEXEL_SIZE_YARDS;

        int shadowed = 0;

        for (int sample_z = 0; sample_z < samples_per_axis; ++sample_z)
        {
          float const world_z = texel_z + first_offset + static_cast<float>(sample_z) * sample_step;

          for (int sample_x = 0; sample_x < samples_per_axis; ++sample_x)
          {
            float const world_x = texel_x + first_offset + static_cast<float>(sample_x) * sample_step;

            // Height from the chunk's own lattice rather than from the depth buffer. Reading the
            // terrain's own depth back out of the render would be circular -- the surface would
            // always be at exactly its own recorded depth and nothing would ever self-shadow --
            // and it would also quantise the sample position to the depth texel grid, throwing
            // away the exact one-to-one MCSH alignment this whole approach is built on.
            float const height = terrainHeightInChunk( vertices
                                                     , world_x - xbase
                                                     , world_z - zbase
                                                     );

            if (isPointInShadow(map, glm::vec3(world_x, height, world_z), bias))
            {
              ++shadowed;
            }
          }
        }

        // Coverage on Green's 0..255 scale, compared against a 0..256 threshold. Full coverage
        // gives exactly 255, so a threshold of 256 shadows nothing and a threshold of 0 shadows
        // every texel the box covered -- both ends of the control mean what they say.
        float const coverage = (static_cast<float>(shadowed) / sample_count) * 255.0f;

        if (coverage >= threshold)
        {
          out_shadow_map[row * MCSH_RESOLUTION + column] = MCSH_SHADOW_VALUE;
          ++texels_set;
        }
      }
    }

    return texels_set;
  }
}
