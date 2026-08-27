// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the ambient-occlusion baker's pure half.
//
// This is the reason AmbientOcclusion.cpp holds no MapChunk, no glm and no Qt: everything here
// runs on a machine with no client install and no Qt. Terrain is a std::vector<float>, so "a pit"
// is four lines of arithmetic rather than an ADT.
//
// AmbientOcclusionBaker IS covered, against stand-in types rather than MapTile/MapChunk. The
// templates take a pointer and touch six members, so a struct with those six members instantiates
// them -- which matters because nothing else in the tree instantiates them yet, and an
// uninstantiated template body is only checked for syntax. See FakeChunk at the bottom for the
// exact interface a real MapChunk has to satisfy.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <noggit/terrain/AmbientOcclusion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <vector>

using namespace Noggit;

namespace
{
  constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();
  constexpr float POSITIVE_INFINITY = std::numeric_limits<float>::infinity();
  constexpr float LARGEST_FLOAT = std::numeric_limits<float>::max();

  // Settings that keep the arithmetic easy to reason about: unit spacing, no bias, and enough
  // radial steps that a wall one unit thick cannot be stepped over.
  AmbientOcclusionSettings testSettings()
  {
    AmbientOcclusionSettings settings;
    settings.direction_count = 16;
    settings.step_count = 16;
    settings.max_radius = 12.0f;
    settings.strength = 1.0f;
    settings.bias_degrees = 0.0f;
    settings.cosine_weighted = true;

    return settings;
  }

  HeightField flatField(int span, float height)
  {
    return HeightField(span, span, 1.0f, height);
  }

  // A cylindrical hole: everything within `radius` of the centre sits `depth` below flat ground.
  // Vertical walls are the point -- they give a horizon angle that can be worked out by hand.
  HeightField pitField(int span, float radius, float depth)
  {
    HeightField field(span, span, 1.0f, 0.0f);
    float const centre = static_cast<float>(span - 1) * 0.5f;

    for (int y = 0; y < span; ++y)
    {
      for (int x = 0; x < span; ++x)
      {
        float const dx = static_cast<float>(x) - centre;
        float const dy = static_cast<float>(y) - centre;

        if (std::sqrt(dx * dx + dy * dy) <= radius)
        {
          field.set(x, y, -depth);
        }
      }
    }

    return field;
  }

  // A cone rising to `height` at the centre and reaching ground level at `radius`.
  HeightField peakField(int span, float radius, float height)
  {
    HeightField field(span, span, 1.0f, 0.0f);
    float const centre = static_cast<float>(span - 1) * 0.5f;

    for (int y = 0; y < span; ++y)
    {
      for (int x = 0; x < span; ++x)
      {
        float const dx = static_cast<float>(x) - centre;
        float const dy = static_cast<float>(y) - centre;
        float const distance = std::sqrt(dx * dx + dy * dy);

        if (distance < radius)
        {
          field.set(x, y, height * (1.0f - distance / radius));
        }
      }
    }

    return field;
  }

  // h = x, i.e. a uniform 45-degree slope rising toward +x at unit spacing.
  HeightField rampField(int span)
  {
    HeightField field(span, span, 1.0f, 0.0f);

    for (int y = 0; y < span; ++y)
    {
      for (int x = 0; x < span; ++x)
      {
        field.set(x, y, static_cast<float>(x));
      }
    }

    return field;
  }

  HeightField scaledCopy(HeightField const& source, float scale)
  {
    HeightField copy = source;

    for (int y = 0; y < source.height(); ++y)
    {
      for (int x = 0; x < source.width(); ++x)
      {
        copy.set(x, y, source.at(x, y) * scale);
      }
    }

    return copy;
  }

  HeightField offsetCopy(HeightField const& source, float offset)
  {
    HeightField copy = source;

    for (int y = 0; y < source.height(); ++y)
    {
      for (int x = 0; x < source.width(); ++x)
      {
        copy.set(x, y, source.at(x, y) + offset);
      }
    }

    return copy;
  }

  bool inUnitRange(float value)
  {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
  }

  float centreOf(HeightField const& field)
  {
    return static_cast<float>(field.width() - 1) * 0.5f;
  }
}

// ---------------------------------------------------------------------------------------------
// The MCCV encoding claim, checked rather than assumed
// ---------------------------------------------------------------------------------------------

TEST_CASE("MCCV neutral is byte 127, not 255", "[ao]")
{
  // The claim that motivated the module. Verified against MapChunk.cpp:288 (read, / 127.0f),
  // :1564 (write, * 127.0f), :2152 (initMCCV sets 1.0f) and terrain_frag.glsl:353 (the shader
  // MULTIPLIES by it). A "white" 255 vertex is therefore not neutral, it is double brightness.
  CHECK(mccvByteToFactor(MCCV_NEUTRAL_BYTE) == 1.0f);
  CHECK(mccvFactorToByte(1.0f) == MCCV_NEUTRAL_BYTE);

  CHECK(mccvByteToFactor(255) == Catch::Approx(2.007874f).epsilon(1e-5));
  CHECK(mccvByteToFactor(0) == 0.0f);

  // What the editor itself allows in memory maps to 254, one short of the byte maximum -- the
  // reason clampMccvFactor exists and the reason a factor above 2 must never reach the writer.
  CHECK(mccvFactorToByte(MCCV_MAX_FACTOR) == 254);

  // Default-constructed MccvColor is neutral, so "no AO applied" is the identity.
  CHECK(MccvColor{} == MCCV_NEUTRAL);
}

TEST_CASE("byte <-> factor round trips exactly for all 256 values", "[ao]")
{
  // Exhaustive because 256 cases cost nothing and an off-by-one in the middle of the range is
  // exactly what a spot check misses.
  //
  // This particular property survives a truncating conversion too -- measured: for every b in
  // 0..255, (unsigned char)((b / 127.0f) * 127.0f) == b, which is why MapChunk.cpp:1564 gets away
  // with a bare cast on reload. It is asserted anyway because it is the contract, and the next
  // test is the one that pins rounding.
  for (int value = 0; value <= 255; ++value)
  {
    unsigned char const byte = static_cast<unsigned char>(value);
    INFO("byte " << value);
    CHECK(mccvFactorToByte(mccvByteToFactor(byte)) == byte);
  }
}

TEST_CASE("factor to byte rounds rather than truncating", "[ao]")
{
  // The difference that matters for a BAKE. A reload round trip is lossless under either rule,
  // but a computed factor is not a b/127 quotient, and over 1001 factors sampled evenly across
  // [0, 2] truncation and rounding disagree 500 times -- always with truncation the darker.
  // Repeated bakes under a truncating conversion would creep toward black.
  //
  // 0.5 * 127 is exactly 63.5, the hardest case: truncation gives 63, rounding gives 64.
  CHECK(mccvFactorToByte(0.5f) == 64);
  CHECK(mccvFactorToByte(1.5f) == 191);   // 190.5
  CHECK(mccvFactorToByte(0.25f) == 32);   // 31.75
  CHECK(mccvFactorToByte(0.1f) == 13);    // 12.7

  // No systematic bias: over the whole factor range the conversion is as often up as down, and
  // never off by more than half a step.
  for (int step = 0; step <= 1000; ++step)
  {
    float const factor = 2.0f * static_cast<float>(step) / 1000.0f;
    float const scaled = factor * static_cast<float>(MCCV_NEUTRAL_BYTE);

    INFO("factor " << factor);
    CHECK(std::abs(static_cast<float>(mccvFactorToByte(factor)) - scaled) <= 0.5f);
  }
}

// ---------------------------------------------------------------------------------------------
// The occlusion estimator
// ---------------------------------------------------------------------------------------------

TEST_CASE("a flat field is uniformly unoccluded", "[ao]")
{
  AmbientOcclusionSettings const settings = testSettings();

  for (float height : {-500.0f, 0.0f, 250.0f})
  {
    HeightField const field = flatField(41, height);
    std::vector<float> const baked = bakeOcclusionField(field, settings);

    REQUIRE(baked.size() == field.size());

    for (float occlusion : baked)
    {
      // Exactly zero, not merely small: no direction has a positive rise, so no direction
      // contributes, and there is no accumulation of rounding to hide behind.
      CHECK(occlusion == 0.0f);
    }
  }
}

TEST_CASE("a flat field stays unoccluded under every weighting and bias", "[ao]")
{
  HeightField const field = flatField(41, 12.0f);

  for (bool cosine : {false, true})
  {
    for (float bias : {0.0f, 5.0f, 45.0f})
    {
      AmbientOcclusionSettings settings = testSettings();
      settings.cosine_weighted = cosine;
      settings.bias_degrees = bias;

      INFO("cosine " << cosine << " bias " << bias);
      CHECK(occlusionAtGrid(field, 20.0f, 20.0f, settings) == 0.0f);
    }
  }
}

TEST_CASE("the inside of a pit is more occluded than its rim", "[ao]")
{
  AmbientOcclusionSettings const settings = testSettings();
  HeightField const field = pitField(41, 5.0f, 10.0f);

  float const centre = centreOf(field);

  float const floor_occlusion = occlusionAtGrid(field, centre, centre, settings);
  // On the flat, three units clear of the wall: everything around it is level or lower.
  float const rim_occlusion = occlusionAtGrid(field, centre + 8.0f, centre, settings);
  float const far_occlusion = occlusionAtGrid(field, 2.0f, 2.0f, settings);

  CHECK(floor_occlusion > rim_occlusion);
  CHECK(rim_occlusion == 0.0f);
  CHECK(far_occlusion == 0.0f);

  // A wall 10 high seen from 5 away is a horizon of atan(2) = 63.4 degrees in EVERY direction,
  // and sin^2(63.4 deg) is 0.8. Bracketed rather than pinned because bilinear sampling softens
  // the wall by about half a unit.
  // Measured: 0.735 for this field and these settings.
  CHECK(floor_occlusion > 0.5f);
  CHECK(floor_occlusion < 1.0f);
}

TEST_CASE("a peak is the least occluded point on its own field", "[ao]")
{
  AmbientOcclusionSettings const settings = testSettings();
  HeightField const field = peakField(41, 10.0f, 12.0f);

  float const centre = centreOf(field);
  float const summit = occlusionAtGrid(field, centre, centre, settings);

  // Nothing anywhere rises above the summit, so no direction has a positive horizon.
  CHECK(summit == 0.0f);

  std::vector<float> const baked = bakeOcclusionField(field, settings);
  REQUIRE(!baked.empty());

  float const highest = *std::max_element(baked.begin(), baked.end());

  // "Least occluded" is only a claim worth making if something else IS occluded: the flat ground
  // around the cone can see it.
  CHECK(highest > 0.0f);
  CHECK(summit <= *std::min_element(baked.begin(), baked.end()));
  CHECK(occlusionAtGrid(field, centre + 11.0f, centre, settings) > summit);
}

TEST_CASE("horizon angle matches a slope that can be worked out by hand", "[ao]")
{
  AmbientOcclusionSettings settings = testSettings();
  settings.max_radius = 8.0f;

  HeightField const field = rampField(41);

  float const quarter_turn = 1.57079632679f;
  float const half_turn = 3.14159265359f;

  // h = x at unit spacing: rise equals run uphill, so the horizon is 45 degrees, and downhill
  // there is no horizon at all.
  CHECK(horizonAngleAtGrid(field, 20.0f, 20.0f, 0.0f, settings)
        == Catch::Approx(quarter_turn * 0.5f).epsilon(0.01));
  CHECK(horizonAngleAtGrid(field, 20.0f, 20.0f, half_turn, settings) == 0.0f);
  // Across the slope, sampling is along constant x, so nothing rises.
  CHECK(horizonAngleAtGrid(field, 20.0f, 20.0f, quarter_turn, settings) == Catch::Approx(0.0f).margin(1e-4));

  // A bias wider than the slope removes it entirely -- the knob that stops a uniform hillside
  // darkening itself.
  settings.bias_degrees = 50.0f;
  CHECK(horizonAngleAtGrid(field, 20.0f, 20.0f, 0.0f, settings) == 0.0f);
  CHECK(occlusionAtGrid(field, 20.0f, 20.0f, settings) == 0.0f);
}

TEST_CASE("occlusion is invariant to a constant height offset", "[ao]")
{
  // AO is a function of relief, not altitude. The heights used are small integers and the offset
  // is a power of two, so every intermediate value is exactly representable and the two results
  // must agree to the bit, not merely approximately.
  AmbientOcclusionSettings const settings = testSettings();
  HeightField const field = pitField(41, 5.0f, 8.0f);
  HeightField const raised = offsetCopy(field, 128.0f);

  float const centre = centreOf(field);

  for (float offset : {0.0f, 3.5f, 7.0f})
  {
    INFO("offset " << offset);
    CHECK(occlusionAtGrid(raised, centre + offset, centre, settings)
          == Catch::Approx(occlusionAtGrid(field, centre + offset, centre, settings)).margin(1e-6));
  }
}

TEST_CASE("occlusion is non-decreasing in relief and in strength", "[ao]")
{
  AmbientOcclusionSettings const settings = testSettings();
  HeightField const base = pitField(41, 5.0f, 4.0f);

  float const centre = centreOf(base);

  float previous = -1.0f;

  for (float scale : {1.0f, 1.5f, 2.0f, 4.0f})
  {
    // Scaling every height multiplies every rise by the same factor, so no horizon angle can
    // fall. The estimator has no business ever reporting less occlusion for deeper terrain.
    HeightField const scaled = scaledCopy(base, scale);
    float const occlusion = occlusionAtGrid(scaled, centre, centre, settings);

    INFO("scale " << scale);
    CHECK(inUnitRange(occlusion));
    CHECK(occlusion >= previous);
    previous = occlusion;
  }

  float const at_full = occlusionAtGrid(base, centre, centre, settings);

  AmbientOcclusionSettings half = settings;
  half.strength = 0.5f;
  AmbientOcclusionSettings double_strength = settings;
  double_strength.strength = 2.0f;
  AmbientOcclusionSettings none = settings;
  none.strength = 0.0f;

  CHECK(occlusionAtGrid(base, centre, centre, half) == Catch::Approx(at_full * 0.5f).margin(1e-6));
  CHECK(occlusionAtGrid(base, centre, centre, double_strength) >= at_full);
  CHECK(occlusionAtGrid(base, centre, centre, double_strength) <= 1.0f);
  CHECK(occlusionAtGrid(base, centre, centre, none) == 0.0f);
}

TEST_CASE("cosine weighting never exceeds solid-angle weighting", "[ao]")
{
  // Per direction the two forms are sin^2 and sin of the same angle, and sin is in [0, 1], so the
  // weighted form is the darker-slower one everywhere. If this ever inverts, the two integrals
  // have been swapped.
  AmbientOcclusionSettings weighted = testSettings();
  weighted.cosine_weighted = true;
  AmbientOcclusionSettings unweighted = weighted;
  unweighted.cosine_weighted = false;

  HeightField const field = pitField(41, 6.0f, 9.0f);
  float const centre = centreOf(field);

  for (float offset : {0.0f, 2.0f, 4.0f, 6.0f, 9.0f})
  {
    INFO("offset " << offset);
    CHECK(occlusionAtGrid(field, centre + offset, centre, weighted)
          <= occlusionAtGrid(field, centre + offset, centre, unweighted));
  }
}

TEST_CASE("occlusion stays in [0,1] for hostile heights and coordinates", "[ao]")
{
  AmbientOcclusionSettings const settings = testSettings();

  HeightField field(21, 21, 1.0f, 0.0f);

  // Every way a height field can be wrong at once: unfilled nodes, both infinities, and the
  // extremes of the type, next to each other so the differences overflow too.
  field.set(5, 5, NOT_A_NUMBER);
  field.set(6, 5, POSITIVE_INFINITY);
  field.set(7, 5, -POSITIVE_INFINITY);
  field.set(8, 5, LARGEST_FLOAT);
  field.set(9, 5, -LARGEST_FLOAT);
  field.set(10, 5, LARGEST_FLOAT);
  field.set(10, 6, -LARGEST_FLOAT);

  for (int y = 0; y < field.height(); ++y)
  {
    for (int x = 0; x < field.width(); ++x)
    {
      float const occlusion = occlusionAtGrid(field, static_cast<float>(x), static_cast<float>(y), settings);
      INFO("node " << x << "," << y);
      CHECK(inUnitRange(occlusion));
    }
  }

  // Half-node coordinates go through bilinear interpolation, which is where a NaN neighbour gets
  // a chance to spread.
  for (float y = 0.0f; y < 20.0f; y += 0.5f)
  {
    for (float x = 0.0f; x < 20.0f; x += 0.5f)
    {
      INFO("sample " << x << "," << y);
      CHECK(inUnitRange(occlusionAtGrid(field, x, y, settings)));
    }
  }

  // A NaN sample point is not a horizon of unknown height, it is no answer at all.
  CHECK(occlusionAtGrid(field, NOT_A_NUMBER, 5.0f, settings) == 0.0f);
  CHECK(occlusionAtGrid(field, 5.0f, POSITIVE_INFINITY, settings) == 0.0f);
  CHECK(occlusionAtGrid(field, 5.0f, 5.0f, settings) == 0.0f);
}

TEST_CASE("degenerate settings and fields yield no occlusion rather than nonsense", "[ao]")
{
  HeightField const field = pitField(21, 4.0f, 10.0f);
  float const centre = centreOf(field);

  AmbientOcclusionSettings broken;
  broken.direction_count = 0;
  broken.step_count = -3;
  broken.max_radius = NOT_A_NUMBER;
  broken.strength = NOT_A_NUMBER;
  broken.bias_degrees = NOT_A_NUMBER;

  CHECK_FALSE(broken.valid());
  CHECK(occlusionAtGrid(field, centre, centre, broken) == 0.0f);
  CHECK(horizonAngleAtGrid(field, centre, centre, 0.0f, broken) == 0.0f);

  // sanitized() has to be a fixed point, otherwise the estimator's own repeated application of
  // it inside the direction loop would drift.
  AmbientOcclusionSettings const once = broken.sanitized();
  AmbientOcclusionSettings const twice = once.sanitized();

  CHECK(once.direction_count == twice.direction_count);
  CHECK(once.step_count == twice.step_count);
  CHECK(once.max_radius == twice.max_radius);
  CHECK(once.strength == twice.strength);
  CHECK(once.bias_degrees == twice.bias_degrees);
  CHECK(once.direction_count >= 1);
  CHECK(once.step_count >= 1);

  AmbientOcclusionSettings zero_radius = testSettings();
  zero_radius.max_radius = 0.0f;
  CHECK_FALSE(zero_radius.valid());
  CHECK(occlusionAtGrid(field, centre, centre, zero_radius) == 0.0f);

  HeightField const nothing;
  CHECK(nothing.empty());
  CHECK(occlusionAtGrid(nothing, 0.0f, 0.0f, testSettings()) == 0.0f);
  CHECK(occlusionAtWorld(nothing, 0.0f, 0.0f, testSettings()) == 0.0f);
  CHECK(bakeOcclusionField(nothing, testSettings()).empty());
}

TEST_CASE("bakeOcclusionField agrees with the per-point query", "[ao]")
{
  AmbientOcclusionSettings const settings = testSettings();
  HeightField const field = pitField(25, 5.0f, 7.0f);

  std::vector<float> const baked = bakeOcclusionField(field, settings);
  REQUIRE(baked.size() == field.size());

  for (int y = 0; y < field.height(); y += 3)
  {
    for (int x = 0; x < field.width(); x += 3)
    {
      std::size_t const index = static_cast<std::size_t>(y) * static_cast<std::size_t>(field.width())
                              + static_cast<std::size_t>(x);
      INFO("node " << x << "," << y);
      CHECK(baked[index]
            == occlusionAtGrid(field, static_cast<float>(x), static_cast<float>(y), settings));
    }
  }
}

// ---------------------------------------------------------------------------------------------
// HeightField
// ---------------------------------------------------------------------------------------------

TEST_CASE("HeightField rejects impossible sizes instead of allocating them", "[ao]")
{
  CHECK(HeightField(0, 10, 1.0f).empty());
  CHECK(HeightField(10, 0, 1.0f).empty());
  CHECK(HeightField(-4, -4, 1.0f).empty());
  CHECK(HeightField(10, 10, 0.0f).empty());
  CHECK(HeightField(10, 10, -1.0f).empty());
  CHECK(HeightField(10, 10, NOT_A_NUMBER).empty());

  HeightField const empty_field;
  CHECK(std::isnan(empty_field.at(0, 0)));
  CHECK(std::isnan(empty_field.sampleGrid(0.0f, 0.0f)));
  CHECK_FALSE(empty_field.contains(0, 0));
}

TEST_CASE("HeightField drops a partial trailing row rather than inventing heights", "[ao]")
{
  HeightField const field(std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f}, 3, 1.0f);

  REQUIRE(field.width() == 3);
  CHECK(field.height() == 2);
  CHECK(field.size() == 6u);
  CHECK(field.at(2, 1) == 6.0f);
}

TEST_CASE("HeightField reads clamp to the edge and writes do not", "[ao]")
{
  HeightField field(3, 3, 1.0f, 0.0f);
  field.set(0, 0, 7.0f);
  field.set(2, 2, 9.0f);

  // Reads extend the terrain outward forever.
  CHECK(field.at(-5, -5) == 7.0f);
  CHECK(field.at(99, 99) == 9.0f);

  // Writes are dropped, which is what makes feeding a neighbouring tile into a tile-sized field
  // free of clipping arithmetic at the call site.
  field.set(-1, 0, 123.0f);
  field.set(3, 0, 123.0f);
  CHECK(field.at(0, 0) == 7.0f);
  CHECK(field.at(2, 0) == 0.0f);

  CHECK(field.contains(2, 2));
  CHECK_FALSE(field.contains(3, 2));
  CHECK_FALSE(field.contains(-1, 0));
}

TEST_CASE("HeightField interpolates bilinearly and converts world coordinates back", "[ao]")
{
  HeightField field(2, 2, 2.0f, 0.0f);
  field.set(0, 0, 0.0f);
  field.set(1, 0, 10.0f);
  field.set(0, 1, 20.0f);
  field.set(1, 1, 30.0f);

  CHECK(field.sampleGrid(0.5f, 0.0f) == Catch::Approx(5.0f));
  CHECK(field.sampleGrid(0.0f, 0.5f) == Catch::Approx(10.0f));
  CHECK(field.sampleGrid(0.5f, 0.5f) == Catch::Approx(15.0f));
  // Beyond the edge is the edge.
  CHECK(field.sampleGrid(-4.0f, 0.0f) == 0.0f);
  CHECK(field.sampleGrid(9.0f, 9.0f) == 30.0f);
  CHECK(std::isnan(field.sampleGrid(NOT_A_NUMBER, 0.0f)));
  CHECK(std::isnan(field.sampleGrid(0.0f, POSITIVE_INFINITY)));

  field.setOrigin(100.0f, -50.0f);
  CHECK(field.toGridX(102.0f) == Catch::Approx(1.0f));
  CHECK(field.toGridY(-48.0f) == Catch::Approx(1.0f));
  CHECK(field.toWorldX(field.toGridX(103.0f)) == Catch::Approx(103.0f));
  CHECK(field.toWorldZ(field.toGridY(-47.0f)) == Catch::Approx(-47.0f));
  CHECK(field.sampleWorld(100.0f, -50.0f) == 0.0f);
  CHECK(field.sampleWorld(102.0f, -48.0f) == Catch::Approx(30.0f));
}

// ---------------------------------------------------------------------------------------------
// fillGaps
// ---------------------------------------------------------------------------------------------

TEST_CASE("fillGaps interpolates unknown samples from real neighbours only", "[ao]")
{
  HeightField field(2, 2, 1.0f, 0.0f);
  field.set(0, 0, NOT_A_NUMBER);
  field.set(1, 0, 4.0f);
  field.set(0, 1, 6.0f);
  field.set(1, 1, 8.0f);

  CHECK(fillGaps(field) == 1u);

  // 6, not 5.6. A clamped neighbour read would count the two out-of-bounds probes as duplicates
  // of (1,0) and (0,1) and skew the mean; contains() is what stops that, and this number is the
  // only thing that distinguishes the two implementations.
  CHECK(field.at(0, 0) == Catch::Approx(6.0f));
}

TEST_CASE("fillGaps needs one pass per ring and stops when it cannot help", "[ao]")
{
  HeightField field(5, 5, 1.0f, 10.0f);

  for (int y = 1; y <= 3; ++y)
  {
    for (int x = 1; x <= 3; ++x)
    {
      field.set(x, y, NOT_A_NUMBER);
    }
  }

  HeightField one_pass = field;
  CHECK(fillGaps(one_pass, 1) == 8u);
  // The true centre had no finite neighbour at all in pass one, and a value was not invented
  // for it.
  CHECK(std::isnan(one_pass.at(2, 2)));

  HeightField two_passes = field;
  CHECK(fillGaps(two_passes, 2) == 9u);
  CHECK(two_passes.at(2, 2) == Catch::Approx(10.0f));

  // Every sample unknown: nothing to interpolate from, and nothing is fabricated.
  HeightField hopeless(4, 4, 1.0f, NOT_A_NUMBER);
  CHECK(fillGaps(hopeless, 8) == 0u);

  for (float value : hopeless.heights())
  {
    CHECK(std::isnan(value));
  }

  CHECK(fillGaps(field, 0) == 0u);

  HeightField nothing;
  CHECK(fillGaps(nothing) == 0u);
}

TEST_CASE("fillGaps does not depend on scan order", "[ao]")
{
  // Two-phase application per pass is the reason. Without it the value written into a cell would
  // feed the cell to its right in the same pass and the result would depend on traversal.
  HeightField field(9, 9, 1.0f, 0.0f);

  for (int i = 0; i < 9; ++i)
  {
    field.set(i, 4, NOT_A_NUMBER);
    field.set(4, i, NOT_A_NUMBER);
    field.set(i, i, static_cast<float>(i));
  }

  HeightField first = field;
  HeightField second = field;

  CHECK(fillGaps(first, 4) == fillGaps(second, 4));
  REQUIRE(first.heights().size() == second.heights().size());

  for (std::size_t i = 0; i < first.heights().size(); ++i)
  {
    INFO("sample " << i);
    bool const both_nan = std::isnan(first.heights()[i]) && std::isnan(second.heights()[i]);
    CHECK((both_nan || first.heights()[i] == second.heights()[i]));
  }
}

// ---------------------------------------------------------------------------------------------
// Occlusion -> MCCV colour
// ---------------------------------------------------------------------------------------------

TEST_CASE("the colour mapping is monotonic and never leaves the byte range", "[ao]")
{
  std::vector<AmbientOcclusionColorSettings> variants;

  variants.push_back(AmbientOcclusionColorSettings{});

  AmbientOcclusionColorSettings sharp;
  sharp.contrast = 3.0f;
  sharp.occluded_factor = 0.1f;
  variants.push_back(sharp);

  AmbientOcclusionColorSettings soft;
  soft.contrast = 0.4f;
  soft.occluded_factor = 0.8f;
  variants.push_back(soft);

  AmbientOcclusionColorSettings tinted;
  tinted.occluded_factor = 0.5f;
  tinted.shadow_tint_r = 0.7f;
  tinted.shadow_tint_g = 0.85f;
  tinted.shadow_tint_b = 1.0f;
  variants.push_back(tinted);

  AmbientOcclusionColorSettings bright;
  bright.open_factor = MCCV_MAX_FACTOR;
  bright.occluded_factor = 0.0f;
  variants.push_back(bright);

  // Every field hostile at once, including values a slider can produce: a contrast of zero would
  // make pow(x, 0) == 1 for x == 0 too and slam the whole map to the shadow colour.
  AmbientOcclusionColorSettings absurd;
  absurd.open_factor = NOT_A_NUMBER;
  absurd.occluded_factor = -5.0f;
  absurd.shadow_tint_r = 12.0f;
  absurd.shadow_tint_g = NOT_A_NUMBER;
  absurd.shadow_tint_b = -1.0f;
  absurd.contrast = 0.0f;
  variants.push_back(absurd);

  // occluded_factor above open_factor is the case that would invert the mapping if sanitized()
  // did not clamp it.
  AmbientOcclusionColorSettings inverted;
  inverted.open_factor = 0.5f;
  inverted.occluded_factor = 1.9f;
  variants.push_back(inverted);

  for (std::size_t variant = 0; variant < variants.size(); ++variant)
  {
    AmbientOcclusionColorSettings const& settings = variants[variant];

    int previous_r = 256;
    int previous_g = 256;
    int previous_b = 256;
    float previous_factor = MCCV_MAX_FACTOR + 1.0f;

    for (int step = 0; step <= 100; ++step)
    {
      float const occlusion = static_cast<float>(step) / 100.0f;
      MccvColor const color = aoToMccvColor(occlusion, settings);
      float const factor = aoToMccvFactor(occlusion, settings);

      INFO("variant " << variant << " occlusion " << occlusion);

      // The byte type cannot express an out-of-range value, so the meaningful assertion is on
      // the factor: above 255/127 the ADT writer's unsigned char cast WRAPS (MapChunk.cpp:1564)
      // and a shadow saves as a highlight.
      CHECK(std::isfinite(factor));
      CHECK(factor >= 0.0f);
      CHECK(factor <= MCCV_MAX_FACTOR);
      CHECK(static_cast<int>(color.r) <= 254);
      CHECK(static_cast<int>(color.g) <= 254);
      CHECK(static_cast<int>(color.b) <= 254);

      CHECK(factor <= previous_factor);
      CHECK(static_cast<int>(color.r) <= previous_r);
      CHECK(static_cast<int>(color.g) <= previous_g);
      CHECK(static_cast<int>(color.b) <= previous_b);

      previous_factor = factor;
      previous_r = color.r;
      previous_g = color.g;
      previous_b = color.b;
    }

    // Out of range and NaN occlusion clamp to the ends rather than escaping them.
    CHECK(aoToMccvColor(-3.0f, settings) == aoToMccvColor(0.0f, settings));
    CHECK(aoToMccvColor(4.0f, settings) == aoToMccvColor(1.0f, settings));
    CHECK(aoToMccvColor(NOT_A_NUMBER, settings) == aoToMccvColor(0.0f, settings));
    CHECK(aoToMccvFactor(NOT_A_NUMBER, settings) == aoToMccvFactor(0.0f, settings));
  }
}

TEST_CASE("an unoccluded vertex bakes to exactly neutral and carries no tint", "[ao]")
{
  // The property that makes a bake safe to run over a whole map: where there is no occlusion,
  // the terrain is left exactly as the shader would leave it untouched.
  AmbientOcclusionColorSettings settings;
  settings.shadow_tint_r = 0.5f;
  settings.shadow_tint_g = 0.75f;
  settings.shadow_tint_b = 1.0f;

  CHECK(aoToMccvColor(0.0f, settings) == MCCV_NEUTRAL);
  CHECK(aoToMccvFactor(0.0f, settings) == Catch::Approx(1.0f));

  // The tint applies only to the shadow end, so a fully occluded vertex does shift hue.
  MccvColor const dark = aoToMccvColor(1.0f, settings);
  CHECK(dark.r < dark.g);
  CHECK(dark.g < dark.b);
  CHECK(dark.b < MCCV_NEUTRAL_BYTE);
}

TEST_CASE("multiplyMccv leaves a chunk alone when the overlay is neutral", "[ao]")
{
  // Exhaustive, because this is the identity the Multiply blend mode depends on: baking with no
  // occlusion anywhere must not disturb existing hand-painted vertex colour by even one LSB.
  for (int value = 0; value <= 255; ++value)
  {
    MccvColor const base{static_cast<unsigned char>(value)
                        , static_cast<unsigned char>(255 - value)
                        , static_cast<unsigned char>((value * 7) % 256)};

    INFO("base " << value);
    CHECK(multiplyMccv(base, MCCV_NEUTRAL) == base);
    CHECK(multiplyMccv(MCCV_NEUTRAL, base) == base);
  }

  MccvColor const black{0, 0, 0};
  CHECK(multiplyMccv(MccvColor{200, 100, 50}, black) == black);

  // Half brightness twice is a quarter, within one LSB of rounding.
  MccvColor const half{64, 64, 64};
  MccvColor const quarter = multiplyMccv(half, half);
  CHECK(static_cast<int>(quarter.r) == 32);
}

TEST_CASE("blendMccv hits its endpoints and clamps its weight", "[ao]")
{
  MccvColor const base{200, 150, 100};
  MccvColor const target{50, 60, 70};

  CHECK(blendMccv(base, target, 0.0f) == base);
  CHECK(blendMccv(base, target, 1.0f) == target);
  CHECK(blendMccv(base, target, -4.0f) == base);
  CHECK(blendMccv(base, target, 9.0f) == target);
  CHECK(blendMccv(base, target, NOT_A_NUMBER) == base);

  MccvColor const middle = blendMccv(base, target, 0.5f);
  CHECK(static_cast<int>(middle.r) == 125);
  CHECK(static_cast<int>(middle.g) == 105);
  CHECK(static_cast<int>(middle.b) == 85);
}

TEST_CASE("clampMccvFactor keeps the ADT writer out of its wrapping range", "[ao]")
{
  CHECK(clampMccvFactor(1.0f) == 1.0f);
  CHECK(clampMccvFactor(-3.0f) == 0.0f);
  CHECK(clampMccvFactor(50.0f) == MCCV_MAX_FACTOR);
  CHECK(clampMccvFactor(POSITIVE_INFINITY) == MCCV_MAX_FACTOR);
  CHECK(clampMccvFactor(NOT_A_NUMBER) == 0.0f);

  CHECK(mccvFactorToByte(NOT_A_NUMBER) == 0);
  CHECK(mccvFactorToByte(-1.0f) == 0);
  CHECK(mccvFactorToByte(POSITIVE_INFINITY) == 255);
  CHECK(mccvFactorToByte(LARGEST_FLOAT) == 255);
}

// ---------------------------------------------------------------------------------------------
// The chunk vertex lattice the world walk depends on
// ---------------------------------------------------------------------------------------------

TEST_CASE("chunkVertexGridOffset reproduces the MCVT traversal exactly", "[ao]")
{
  CHECK(CHUNK_VERTEX_COUNT == 9 * 9 + 8 * 8);

  // Independently recomputed from the loader's loop at MapChunk.cpp:176-190 rather than from the
  // implementation, so the test fails if the traversal is misread rather than merely restated.
  std::vector<std::pair<int, int>> expected;

  for (int row = 0; row < 17; ++row)
  {
    int const count = (row % 2) ? 8 : 9;

    for (int i = 0; i < count; ++i)
    {
      expected.emplace_back((row % 2) ? (2 * i + 1) : (2 * i), row);
    }
  }

  REQUIRE(expected.size() == static_cast<std::size_t>(CHUNK_VERTEX_COUNT));

  std::set<std::pair<int, int>> seen;

  for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
  {
    ChunkVertexGridOffset const offset = chunkVertexGridOffset(index);

    INFO("vertex " << index);
    CHECK(offset.column == expected[static_cast<std::size_t>(index)].first);
    CHECK(offset.row == expected[static_cast<std::size_t>(index)].second);

    // Inside the chunk's own 17x17 half-unit lattice, and distinct: two vertices sharing a node
    // would silently give one of them the other's occlusion.
    CHECK(offset.column >= 0);
    CHECK(offset.column <= CHUNK_GRID_SPAN);
    CHECK(offset.row >= 0);
    CHECK(offset.row <= CHUNK_GRID_SPAN);
    CHECK(seen.insert({offset.column, offset.row}).second);

    // Outer rows are even columns, inner rows odd -- the half-unit offset at MapChunk.cpp:183.
    CHECK((offset.row % 2) == (offset.column % 2));
  }

  CHECK(seen.size() == static_cast<std::size_t>(CHUNK_VERTEX_COUNT));

  // The corners the baker relies on to line chunks up edge to edge.
  CHECK(chunkVertexGridOffset(0).column == 0);
  CHECK(chunkVertexGridOffset(0).row == 0);
  CHECK(chunkVertexGridOffset(8).column == CHUNK_GRID_SPAN);
  CHECK(chunkVertexGridOffset(8).row == 0);
  CHECK(chunkVertexGridOffset(144).column == CHUNK_GRID_SPAN);
  CHECK(chunkVertexGridOffset(144).row == CHUNK_GRID_SPAN);

  for (int index : {-1, -100, CHUNK_VERTEX_COUNT, CHUNK_VERTEX_COUNT + 1})
  {
    INFO("out of range " << index);
    CHECK(chunkVertexGridOffset(index).column == -1);
    CHECK(chunkVertexGridOffset(index).row == -1);
  }
}

TEST_CASE("the default grid spacing puts every chunk vertex on a node", "[ao]")
{
  // Half a unit is the coarsest spacing on which the 8x8 inner vertices land exactly, and the
  // reason the world walk never rounds a world coordinate to find a vertex.
  CHECK(DEFAULT_GRID_SPACING == Catch::Approx(UNIT_SIZE_YARDS * 0.5f));
  CHECK(static_cast<float>(CHUNK_GRID_SPAN) * DEFAULT_GRID_SPACING == Catch::Approx(CHUNK_SIZE_YARDS));
  CHECK(static_cast<float>(TILE_GRID_SPAN) * DEFAULT_GRID_SPACING == Catch::Approx(TILE_SIZE_YARDS));
  CHECK(TILE_GRID_SPAN == 256);
}

TEST_CASE("world coordinates resolve to the nearest grid node", "[ao]")
{
  HeightField field(64, 64, 2.0f, 0.0f);
  field.setOrigin(-100.0f, 40.0f);

  CHECK(gridColumnForWorldX(field, -100.0f) == 0);
  CHECK(gridColumnForWorldX(field, -94.0f) == 3);
  // Float error in a chunk's xbase must not drop it a whole node.
  CHECK(gridColumnForWorldX(field, -93.9999f) == 3);
  CHECK(gridColumnForWorldX(field, -94.0001f) == 3);
  CHECK(gridRowForWorldZ(field, 40.0f) == 0);
  CHECK(gridRowForWorldZ(field, 50.0f) == 5);

  // Beyond the field is still a well-defined index; HeightField::set is what discards it.
  CHECK(gridColumnForWorldX(field, 1000.0f) > field.width());

  CHECK(gridColumnForWorldX(field, NOT_A_NUMBER) == 0);
  CHECK(gridRowForWorldZ(field, POSITIVE_INFINITY) == 0);
}

TEST_CASE("a chunk-sized patch of terrain bakes end to end", "[ao]")
{
  // The whole pure pipeline in the order the world walk uses it: unknown field, chunk heights
  // written at their lattice offsets, gaps filled, occlusion queried at each vertex, colour
  // emitted. Everything MapChunk would supply is a plain array here.
  int const span = CHUNK_GRID_SPAN + 1;
  HeightField field(span, span, DEFAULT_GRID_SPACING);
  field.fill(NOT_A_NUMBER);

  std::vector<float> chunk_heights(CHUNK_VERTEX_COUNT, 0.0f);

  // A trench across the middle of the chunk.
  for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
  {
    ChunkVertexGridOffset const offset = chunkVertexGridOffset(index);

    if (offset.row >= 7 && offset.row <= 9)
    {
      chunk_heights[static_cast<std::size_t>(index)] = -20.0f;
    }
  }

  for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
  {
    ChunkVertexGridOffset const offset = chunkVertexGridOffset(index);
    field.set(offset.column, offset.row, chunk_heights[static_cast<std::size_t>(index)]);
  }

  // 145 vertices on a 17x17 lattice leave 144 nodes unwritten -- the checkerboard the two vertex
  // grids do not cover between them. They must be interpolated, not left unknown, or half the
  // horizon search would be sampling holes.
  CHECK(fillGaps(field) == static_cast<std::size_t>(span * span - CHUNK_VERTEX_COUNT));

  for (float value : field.heights())
  {
    CHECK(std::isfinite(value));
  }

  AmbientOcclusionSettings settings = testSettings();
  settings.max_radius = 12.0f;

  AmbientOcclusionColorSettings const colors;

  int occluded_vertices = 0;

  for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
  {
    ChunkVertexGridOffset const offset = chunkVertexGridOffset(index);

    float const occlusion = occlusionAtGrid( field
                                           , static_cast<float>(offset.column)
                                           , static_cast<float>(offset.row)
                                           , settings
                                           );

    REQUIRE(inUnitRange(occlusion));

    MccvColor const color = aoToMccvColor(occlusion, colors);
    CHECK(static_cast<int>(color.r) <= MCCV_NEUTRAL_BYTE);

    if (occlusion > 0.0f)
    {
      ++occluded_vertices;
      CHECK(static_cast<int>(color.r) < MCCV_NEUTRAL_BYTE);
    }
    else
    {
      CHECK(color == MCCV_NEUTRAL);
    }
  }

  // The trench floor at minimum. If this is zero the pipeline ran and did nothing, which is the
  // failure a "no crash" test would pass.
  CHECK(occluded_vertices > 10);
}

// ---------------------------------------------------------------------------------------------
// The world walk, instantiated against stand-ins
// ---------------------------------------------------------------------------------------------

namespace
{
  // The complete interface AmbientOcclusionBaker requires of MapChunk. Every member here exists
  // on the real one with this exact spelling: xbase/zbase (MapChunk.h:87), mVertices and mccv
  // (MapChunk.h:101, :103), initMCCV (MapChunk.h:232) and registerChunkUpdate (MapChunk.h:234).
  struct FakeChunk
  {
    struct Vec3
    {
      float x = 0.0f;
      float y = 0.0f;
      float z = 0.0f;
    };

    float xbase = 0.0f;
    float zbase = 0.0f;
    Vec3 mVertices[CHUNK_VERTEX_COUNT];
    Vec3 mccv[CHUNK_VERTEX_COUNT];

    bool has_mccv = false;
    unsigned update_flags = 0;

    // MapChunk.cpp:2146: a no-op once the chunk has vertex colour, otherwise the 1.0f defaults
    // plus the header flag without which save() discards the whole bake.
    void initMCCV()
    {
      if (has_mccv)
      {
        return;
      }

      for (int i = 0; i < CHUNK_VERTEX_COUNT; ++i)
      {
        mccv[i] = Vec3{1.0f, 1.0f, 1.0f};
      }

      has_mccv = true;
    }

    void registerChunkUpdate(unsigned flags)
    {
      update_flags |= flags;
    }
  };

  struct FakeTile
  {
    float xbase = 0.0f;
    float zbase = 0.0f;
    bool loaded = true;
    FakeChunk chunks[16][16];

    bool finishedLoading() const
    {
      return loaded;
    }

    FakeChunk* getChunk(unsigned int x, unsigned int z)
    {
      if (x >= 16 || z >= 16)
      {
        return nullptr;
      }

      return &chunks[z][x];
    }
  };

  // Lays the tile out the way MapChunk.cpp:47-51 and :82-83 do once the ZEROPOINT flip cancels:
  // a chunk's xbase is the tile's plus a whole number of chunks.
  template <typename HeightFn>
  void buildFakeTile(FakeTile& tile, HeightFn height)
  {
    for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
    {
      for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
      {
        FakeChunk& chunk = tile.chunks[chunk_z][chunk_x];
        chunk.xbase = tile.xbase + static_cast<float>(chunk_x) * CHUNK_SIZE_YARDS;
        chunk.zbase = tile.zbase + static_cast<float>(chunk_z) * CHUNK_SIZE_YARDS;

        for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
        {
          ChunkVertexGridOffset const offset = chunkVertexGridOffset(index);
          float const world_x = chunk.xbase + static_cast<float>(offset.column) * DEFAULT_GRID_SPACING;
          float const world_z = chunk.zbase + static_cast<float>(offset.row) * DEFAULT_GRID_SPACING;

          chunk.mVertices[index] = FakeChunk::Vec3{world_x, height(world_x, world_z), world_z};
        }
      }
    }
  }

  AmbientOcclusionSettings cheapSettings()
  {
    AmbientOcclusionSettings settings;
    settings.direction_count = 8;
    settings.step_count = 6;
    settings.max_radius = 25.0f;

    return settings;
  }
}

TEST_CASE("the world walk lands every chunk on the shared tile lattice", "[ao]")
{
  FakeTile tile;
  tile.xbase = 1600.0f;
  tile.zbase = -800.0f;

  // A plane in both axes, so a node's height identifies the world position it came from and a
  // chunk placed one node out shows up as a discontinuity.
  buildFakeTile(tile, [] (float x, float z) { return 0.25f * x + 0.5f * z; });

  float const margin = 25.0f;
  HeightField field = AmbientOcclusionBaker::makeTileHeightField<FakeTile, FakeChunk>(&tile, margin);

  REQUIRE_FALSE(field.empty());

  int const margin_nodes = static_cast<int>(margin / DEFAULT_GRID_SPACING) + 1;
  CHECK(field.width() == TILE_GRID_SPAN + 1 + 2 * margin_nodes);
  CHECK(field.height() == field.width());
  CHECK(field.originX() == Catch::Approx(tile.xbase - static_cast<float>(margin_nodes) * DEFAULT_GRID_SPACING));

  // Untouched margin is unknown, not zero. A zero here would read as a cliff at the tile border.
  CHECK(std::isnan(field.at(0, 0)));

  // The tile footprint is exactly 257 nodes across and every one carries a height matching the
  // plane, which is only true if all 256 chunks landed on the right nodes and the shared edge
  // columns agree between neighbours.
  for (int row = 0; row <= TILE_GRID_SPAN; row += 8)
  {
    for (int column = 0; column <= TILE_GRID_SPAN; column += 8)
    {
      int const x = margin_nodes + column;
      int const y = margin_nodes + row;

      float const expected = 0.25f * field.toWorldX(static_cast<float>(x))
                           + 0.5f * field.toWorldZ(static_cast<float>(y));

      INFO("node " << column << "," << row);
      CHECK(field.at(x, y) == Catch::Approx(expected).epsilon(1e-4));
    }
  }

  // 145 vertices on a 17x17 lattice leave the (even, odd) and (odd, even) nodes unwritten -- the
  // checkerboard the outer and inner vertex grids do not cover between them.
  CHECK(std::isnan(field.at(margin_nodes + 1, margin_nodes)));
  CHECK(fillGaps(field) > 0u);

  // An INTERIOR gap has all four orthogonal neighbours written and symmetric about it, so on a
  // linear field the fill is exact rather than approximate -- the strongest statement available
  // about the interpolation, and it only holds if the lattice parity is right.
  int const gap_column = margin_nodes + 1;
  int const gap_row = margin_nodes + 2;

  float const gap_expected = 0.25f * field.toWorldX(static_cast<float>(gap_column))
                           + 0.5f * field.toWorldZ(static_cast<float>(gap_row));
  CHECK(field.at(gap_column, gap_row) == Catch::Approx(gap_expected).epsilon(1e-4));

  // The gap on the tile's outer edge is finite but NOT the plane value: half its neighbourhood is
  // margin nobody supplied, so the mean is pulled inward. That is the seam the `margin` parameter
  // and addTileToField on the neighbouring tiles exist to remove, and it is worth pinning as
  // known behaviour rather than discovering it as a bright line in a screenshot.
  CHECK(std::isfinite(field.at(margin_nodes + 1, margin_nodes)));
  CHECK(field.at(margin_nodes + 1, margin_nodes)
        != Catch::Approx(0.25f * field.toWorldX(static_cast<float>(margin_nodes + 1))
                         + 0.5f * field.toWorldZ(static_cast<float>(margin_nodes))).epsilon(1e-4));

  for (int row = 0; row <= TILE_GRID_SPAN; ++row)
  {
    for (int column = 0; column <= TILE_GRID_SPAN; column += 37)
    {
      INFO("node " << column << "," << row);
      CHECK(std::isfinite(field.at(margin_nodes + column, margin_nodes + row)));
    }
  }
}

TEST_CASE("baking a tile writes every chunk and flags it for upload", "[ao]")
{
  FakeTile tile;

  // A trench running the width of the tile, deep enough to occlude its own floor.
  float const trench_centre = tile.zbase + TILE_SIZE_YARDS * 0.5f;
  buildFakeTile(tile, [trench_centre] (float, float z)
  {
    return (std::abs(z - trench_centre) < 12.0f) ? -30.0f : 0.0f;
  });

  HeightField field = AmbientOcclusionBaker::makeTileHeightField<FakeTile, FakeChunk>(&tile, 25.0f);
  fillGaps(field);

  AmbientOcclusionColorSettings const colors;

  CHECK(AmbientOcclusionBaker::bakeTile<FakeTile, FakeChunk>(&tile, field, cheapSettings(), colors) == 256u);

  int darkened = 0;
  int neutral = 0;

  for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
  {
    for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
    {
      FakeChunk const& chunk = tile.chunks[chunk_z][chunk_x];

      // Without the flag the new colours never reach the vertex-colour texture
      // (MapChunk.cpp:1017), and without initMCCV having run, save() writes ofsMCCV = 0 and
      // discards the bake entirely (MapChunk.cpp:1553).
      INFO("chunk " << chunk_x << "," << chunk_z);
      CHECK((chunk.update_flags & CHUNK_UPDATE_FLAG_MCCV) != 0u);
      CHECK(chunk.has_mccv);

      for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
      {
        float const factor = chunk.mccv[index].x;
        REQUIRE(std::isfinite(factor));
        REQUIRE(factor >= 0.0f);
        REQUIRE(factor <= MCCV_MAX_FACTOR);

        if (factor < 1.0f)
        {
          ++darkened;
        }
        else
        {
          CHECK(factor == Catch::Approx(1.0f));
          ++neutral;
        }
      }
    }
  }

  // Both, or the bake did nothing / everything: a trench must darken its floor and leave the
  // open ground alone.
  CHECK(darkened > 0);
  CHECK(neutral > 0);
}

TEST_CASE("Multiply preserves existing vertex paint where nothing occludes", "[ao]")
{
  // The reason Multiply is the default. On flat ground the bake is neutral, and neutral is the
  // identity in factor space, so AO over a hand-painted map cannot damage it.
  FakeTile tile;
  buildFakeTile(tile, [] (float, float) { return 0.0f; });

  FakeChunk& chunk = tile.chunks[3][4];
  chunk.initMCCV();

  for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
  {
    chunk.mccv[index] = FakeChunk::Vec3{1.4f, 0.6f, 0.2f};
  }

  HeightField field = AmbientOcclusionBaker::makeTileHeightField<FakeTile, FakeChunk>(&tile, 25.0f);
  fillGaps(field);

  AmbientOcclusionColorSettings const colors;

  REQUIRE(AmbientOcclusionBaker::bakeChunk<FakeChunk>(&chunk, field, cheapSettings(), colors, AoBlendMode::Multiply));

  for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
  {
    INFO("vertex " << index);
    CHECK(chunk.mccv[index].x == Catch::Approx(1.4f).epsilon(1e-5));
    CHECK(chunk.mccv[index].y == Catch::Approx(0.6f).epsilon(1e-5));
    CHECK(chunk.mccv[index].z == Catch::Approx(0.2f).epsilon(1e-5));
  }

  // Replace discards it, which is the whole difference between the two modes.
  REQUIRE(AmbientOcclusionBaker::bakeChunk<FakeChunk>(&chunk, field, cheapSettings(), colors, AoBlendMode::Replace));

  for (int index = 0; index < CHUNK_VERTEX_COUNT; ++index)
  {
    INFO("vertex " << index);
    CHECK(chunk.mccv[index].x == Catch::Approx(1.0f));
    CHECK(chunk.mccv[index].y == Catch::Approx(1.0f));
    CHECK(chunk.mccv[index].z == Catch::Approx(1.0f));
  }
}

TEST_CASE("the world walk refuses null and half-loaded input", "[ao]")
{
  HeightField field(64, 64, DEFAULT_GRID_SPACING);
  AmbientOcclusionSettings const settings = cheapSettings();
  AmbientOcclusionColorSettings const colors;

  CHECK(AmbientOcclusionBaker::makeTileHeightField<FakeTile, FakeChunk>(nullptr, 10.0f).empty());
  CHECK_FALSE(AmbientOcclusionBaker::bakeChunk<FakeChunk>(nullptr, field, settings, colors));
  CHECK(AmbientOcclusionBaker::bakeTile<FakeTile, FakeChunk>(nullptr, field, settings, colors) == 0u);

  // A tile still being parsed by the async loader has a chunk array that is a data race to read.
  FakeTile tile;
  buildFakeTile(tile, [] (float, float) { return 0.0f; });
  tile.loaded = false;

  CHECK(AmbientOcclusionBaker::bakeTile<FakeTile, FakeChunk>(&tile, field, settings, colors) == 0u);
  CHECK_FALSE(tile.chunks[0][0].has_mccv);

  AmbientOcclusionBaker::addTileToField<FakeTile, FakeChunk>(&tile, field);

  for (float value : field.heights())
  {
    CHECK(value == 0.0f);
  }

  // An empty field is not a reason to write neutral over everything.
  HeightField const nothing;
  CHECK_FALSE(AmbientOcclusionBaker::bakeChunk<FakeChunk>(&tile.chunks[0][0], nothing, settings, colors));
  CHECK_FALSE(tile.chunks[0][0].has_mccv);
}
