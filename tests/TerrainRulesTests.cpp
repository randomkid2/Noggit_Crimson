// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the auto-texture rule evaluator's pure half.
//
// This is why TerrainRules.cpp holds no Qt, no glm and no MapChunk: everything here runs on a bare
// machine. Terrain is a pair of floats, a chunk is a stand-in struct with two arrays, and what
// gets exercised is the part that decides anything -- interval semantics, precedence between
// overlapping rules, slope from a normal, coverage aggregation.
//
// The properties asserted are chosen to be the ones that break silently in a rule editor: an
// inverted range that paints a continent instead of nothing, a precedence order that depends on
// the order rules were typed in, a slope that reads a cliff as flat because a normal was stored
// with its sign flipped.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <noggit/terrain/TerrainRules.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <vector>

using namespace Noggit;

namespace
{
  constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
  constexpr float INF_F = std::numeric_limits<float>::infinity();
  constexpr float DEGREES_TO_RADIANS = 0.0174532925199432958f;

  TerrainRule makeRule( std::string texture
                      , TerrainRange height
                      , TerrainRange slope
                      , int priority = 0
                      , std::uint8_t strength = TERRAIN_ALPHA_MAX
                      )
  {
    TerrainRule rule;
    rule.texture = std::move(texture);
    rule.height = height;
    rule.slope = slope;
    rule.priority = priority;
    rule.strength = strength;
    return rule;
  }

  TerrainSample sampleAt(float height, float slope_degrees)
  {
    TerrainSample sample;
    sample.height = height;
    sample.slope_degrees = slope_degrees;
    return sample;
  }

  // A normal tilted `degrees` away from +Y, in the YZ plane. Deliberately not unit-scaled by the
  // caller in some tests, to prove the evaluator normalises.
  TerrainNormal tiltedNormal(float degrees, float scale = 1.0f)
  {
    float const radians = degrees * DEGREES_TO_RADIANS;
    return TerrainNormal{0.0f, std::cos(radians) * scale, std::sin(radians) * scale};
  }

  // Minimal stand-in for MapChunk: the collector templates touch nothing but these two public
  // arrays (MapChunk.h:101-102), which is the whole reason they are templates.
  struct FakeVector
  {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
  };

  struct FakeChunk
  {
    FakeVector mVertices[TerrainRuleCollector::CHUNK_VERTEX_COUNT];
    FakeVector mNormals[TerrainRuleCollector::CHUNK_VERTEX_COUNT];

    FakeChunk()
    {
      for (int i = 0; i < TerrainRuleCollector::CHUNK_VERTEX_COUNT; ++i)
      {
        mNormals[i] = FakeVector{0.0f, 1.0f, 0.0f};
      }
    }
  };

  // The reference formulas, copied from MapChunk.cpp:351 and :356. The point of restating them
  // here rather than reusing the header's constants is that the test fails if the header's copy
  // drifts from the engine's.
  int referenceIndexLoD(int x, int y)
  {
    return (x + 1) * 9 + x * 8 + y;
  }

  int referenceIndexNoLoD(int x, int y)
  {
    return x * 8 + x * 9 + y;
  }
}

TEST_CASE("TerrainRange: unbounded, one-value and inverted intervals", "[terrainrules]")
{
  SECTION("an unbounded range constrains nothing")
  {
    TerrainRange const range = TerrainRange::any();

    CHECK(range.contains(0.0f));
    CHECK(range.contains(-1.0e30f));
    CHECK(range.contains(1.0e30f));
    CHECK_FALSE(range.bounded());
    CHECK(range.boundCount() == 0);
    CHECK_FALSE(range.empty());
  }

  SECTION("a default-constructed range is the unbounded one")
  {
    TerrainRange const range{};

    CHECK_FALSE(range.bounded());
    CHECK(range.contains(123.0f));
  }

  SECTION("min == max is a legal one-value interval, not an empty one")
  {
    TerrainRange const range = TerrainRange::between(10.0f, 10.0f);

    CHECK_FALSE(range.empty());
    CHECK(range.contains(10.0f));
    CHECK_FALSE(range.contains(std::nextafter(10.0f, 0.0f)));
    CHECK_FALSE(range.contains(std::nextafter(10.0f, 100.0f)));
    CHECK(range.boundCount() == 2);
  }

  SECTION("min > max is inverted and matches nothing -- endpoints are never silently swapped")
  {
    TerrainRange const range = TerrainRange::between(20.0f, 10.0f);

    CHECK(range.empty());
    CHECK_FALSE(range.contains(9.0f));
    CHECK_FALSE(range.contains(15.0f));
    CHECK_FALSE(range.contains(21.0f));
  }

  SECTION("half-bounded ranges are inclusive at their one endpoint")
  {
    TerrainRange const at_least = TerrainRange::atLeast(60.0f);
    CHECK(at_least.contains(60.0f));
    CHECK(at_least.contains(90.0f));
    CHECK_FALSE(at_least.contains(std::nextafter(60.0f, 0.0f)));
    CHECK(at_least.boundCount() == 1);
    CHECK(at_least.boundedBelow());
    CHECK_FALSE(at_least.boundedAbove());

    TerrainRange const at_most = TerrainRange::atMost(60.0f);
    CHECK(at_most.contains(60.0f));
    CHECK(at_most.contains(-100.0f));
    CHECK_FALSE(at_most.contains(std::nextafter(60.0f, 100.0f)));
    CHECK(at_most.boundCount() == 1);
    CHECK_FALSE(at_most.boundedBelow());
    CHECK(at_most.boundedAbove());
  }
}

TEST_CASE("TerrainRange: NaN on either side fails closed", "[terrainrules]")
{
  // An unusable sample must not match a rule that constrains that axis...
  CHECK_FALSE(TerrainRange::atLeast(60.0f).contains(NAN_F));
  CHECK_FALSE(TerrainRange::atMost(60.0f).contains(NAN_F));
  CHECK_FALSE(TerrainRange::between(0.0f, 90.0f).contains(NAN_F));

  // ...but a rule that constrains nothing on that axis performs no test, so it still applies.
  // This is what lets a catch-all keep working over terrain with unusable normals.
  CHECK(TerrainRange::any().contains(NAN_F));

  // A NaN ENDPOINT counts as a constraint rather than as an absent one, so the rule matches
  // nothing instead of everything.
  TerrainRange const poisoned = TerrainRange::between(NAN_F, 100.0f);
  CHECK(poisoned.boundedBelow());
  CHECK_FALSE(poisoned.contains(50.0f));
  CHECK_FALSE(poisoned.contains(NAN_F));
}

TEST_CASE("slopeDegreesFromNormal: bounds, monotonicity and invariances", "[terrainrules]")
{
  SECTION("the two anchors")
  {
    CHECK(slopeDegreesFromNormal({0.0f, 1.0f, 0.0f}) == Catch::Approx(0.0f).margin(1e-4));
    CHECK(slopeDegreesFromNormal({1.0f, 0.0f, 0.0f}) == Catch::Approx(90.0f).margin(1e-4));
    CHECK(slopeDegreesFromNormal({0.0f, 0.0f, 1.0f}) == Catch::Approx(90.0f).margin(1e-4));
  }

  SECTION("a normal tilted by t reads back as t, strictly monotonically, and never leaves [0, 90]")
  {
    float previous = -1.0f;

    for (int degrees = 0; degrees <= 90; degrees += 3)
    {
      float const angle = slopeDegreesFromNormal(tiltedNormal(static_cast<float>(degrees)));

      CHECK(angle == Catch::Approx(static_cast<float>(degrees)).margin(1e-3));
      CHECK(angle >= TERRAIN_SLOPE_MIN_DEGREES);
      CHECK(angle <= TERRAIN_SLOPE_MAX_DEGREES);
      CHECK(angle > previous);

      previous = angle;
    }
  }

  SECTION("scaling the normal does not change the angle -- stored normals are quantised, not unit")
  {
    for (int degrees = 0; degrees <= 90; degrees += 15)
    {
      float const unit = slopeDegreesFromNormal(tiltedNormal(static_cast<float>(degrees), 1.0f));
      float const big = slopeDegreesFromNormal(tiltedNormal(static_cast<float>(degrees), 37.5f));
      float const tiny = slopeDegreesFromNormal(tiltedNormal(static_cast<float>(degrees), 1.0e-4f));

      CHECK(big == Catch::Approx(unit).margin(1e-3));
      CHECK(tiny == Catch::Approx(unit).margin(1e-3));
    }
  }

  SECTION("flipping the normal does not change the angle -- a heightfield cannot overhang")
  {
    for (int degrees = 0; degrees <= 90; degrees += 15)
    {
      TerrainNormal const up = tiltedNormal(static_cast<float>(degrees));
      TerrainNormal const down{-up.x, -up.y, -up.z};

      CHECK(slopeDegreesFromNormal(down) == Catch::Approx(slopeDegreesFromNormal(up)).margin(1e-3));
    }

    // The case the folding exists for: a flipped near-vertical normal must still read as a cliff,
    // not as 180 degrees of "flat".
    CHECK(slopeDegreesFromNormal({0.0f, -1.0f, 0.0f}) == Catch::Approx(0.0f).margin(1e-4));
    CHECK(slopeDegreesFromNormal({0.0f, -0.05f, 0.998f}) > 85.0f);
  }

  SECTION("a normal that is not one yields NaN rather than a fabricated flat")
  {
    CHECK(std::isnan(slopeDegreesFromNormal({0.0f, 0.0f, 0.0f})));
    CHECK(std::isnan(slopeDegreesFromNormal({NAN_F, 1.0f, 0.0f})));
    CHECK(std::isnan(slopeDegreesFromNormal({INF_F, 1.0f, 0.0f})));
  }

  SECTION("a near-vertical normal does not overshoot acos' domain")
  {
    // The clamp in slopeDegreesFromNormal exists for this: |y| / length can land a hair above 1.
    for (float y : {1.0f, 0.9999999f, 1.0000001f})
    {
      float const angle = slopeDegreesFromNormal({0.0f, y, 0.0f});
      CHECK_FALSE(std::isnan(angle));
      CHECK(angle == Catch::Approx(0.0f).margin(1e-3));
    }
  }
}

TEST_CASE("slopeDegreesFromGradient agrees with the normal route", "[terrainrules]")
{
  CHECK(slopeDegreesFromGradient(0.0f, 0.0f) == Catch::Approx(0.0f).margin(1e-4));
  CHECK(slopeDegreesFromGradient(1.0f, 0.0f) == Catch::Approx(45.0f).margin(1e-4));
  CHECK(slopeDegreesFromGradient(0.0f, 1.0f) == Catch::Approx(45.0f).margin(1e-4));
  CHECK(slopeDegreesFromGradient(INF_F, 0.0f) == Catch::Approx(90.0f).margin(1e-4));
  CHECK(std::isnan(slopeDegreesFromGradient(NAN_F, 0.0f)));

  // The plane h = a*x + b*z has normal (-a, 1, -b). The two entry points must not disagree,
  // because a caller with finite differences and a caller with a stored normal must classify the
  // same terrain the same way.
  for (float a : {-4.0f, -1.0f, -0.25f, 0.0f, 0.25f, 1.0f, 4.0f})
  {
    for (float b : {-4.0f, -1.0f, -0.25f, 0.0f, 0.25f, 1.0f, 4.0f})
    {
      CHECK( slopeDegreesFromNormal({-a, 1.0f, -b})
          == Catch::Approx(slopeDegreesFromGradient(a, b)).margin(1e-3)
           );
    }
  }
}

TEST_CASE("normalFromTriangle: corner order is irrelevant, degeneracy is not hidden", "[terrainrules]")
{
  SECTION("a level triangle is flat whichever way it is wound")
  {
    std::vector<TerrainVertex> corners
      {TerrainVertex{0.0f, 5.0f, 0.0f}, TerrainVertex{10.0f, 5.0f, 0.0f}, TerrainVertex{0.0f, 5.0f, 10.0f}};

    std::sort(corners.begin(), corners.end(), [] (TerrainVertex const& l, TerrainVertex const& r)
      { return l.x < r.x || (l.x == r.x && l.z < r.z); });

    do
    {
      TerrainNormal const normal = normalFromTriangle(corners[0], corners[1], corners[2]);

      CHECK(normal.y == Catch::Approx(1.0f).margin(1e-5));
      CHECK(slopeDegreesFromTriangle(corners[0], corners[1], corners[2])
              == Catch::Approx(0.0f).margin(1e-4));
    }
    while (std::next_permutation( corners.begin(), corners.end()
                                , [] (TerrainVertex const& l, TerrainVertex const& r)
                                  { return l.x < r.x || (l.x == r.x && l.z < r.z); }
                                ));
  }

  SECTION("a 45 degree ramp reads as 45, in all six corner orders")
  {
    // Rises 10 over 10 along X.
    TerrainVertex const a{0.0f, 0.0f, 0.0f};
    TerrainVertex const b{10.0f, 10.0f, 0.0f};
    TerrainVertex const c{0.0f, 0.0f, 10.0f};

    CHECK(slopeDegreesFromTriangle(a, b, c) == Catch::Approx(45.0f).margin(1e-3));
    CHECK(slopeDegreesFromTriangle(a, c, b) == Catch::Approx(45.0f).margin(1e-3));
    CHECK(slopeDegreesFromTriangle(b, a, c) == Catch::Approx(45.0f).margin(1e-3));
    CHECK(slopeDegreesFromTriangle(b, c, a) == Catch::Approx(45.0f).margin(1e-3));
    CHECK(slopeDegreesFromTriangle(c, a, b) == Catch::Approx(45.0f).margin(1e-3));
    CHECK(slopeDegreesFromTriangle(c, b, a) == Catch::Approx(45.0f).margin(1e-3));
  }

  SECTION("a vertical face reads as 90")
  {
    TerrainVertex const a{0.0f, 0.0f, 0.0f};
    TerrainVertex const b{0.0f, 0.0f, 10.0f};
    TerrainVertex const c{0.0f, 10.0f, 0.0f};

    CHECK(slopeDegreesFromTriangle(a, b, c) == Catch::Approx(90.0f).margin(1e-3));
  }

  SECTION("zero-area triangles yield the zero normal, which becomes a NaN slope")
  {
    TerrainVertex const a{0.0f, 0.0f, 0.0f};
    TerrainVertex const collinear_b{5.0f, 5.0f, 5.0f};
    TerrainVertex const collinear_c{10.0f, 10.0f, 10.0f};

    TerrainNormal const collinear = normalFromTriangle(a, collinear_b, collinear_c);
    CHECK(collinear.x == 0.0f);
    CHECK(collinear.y == 0.0f);
    CHECK(collinear.z == 0.0f);
    CHECK(std::isnan(slopeDegreesFromTriangle(a, collinear_b, collinear_c)));

    CHECK(std::isnan(slopeDegreesFromTriangle(a, a, a)));
  }
}

TEST_CASE("A point matching no rule wins nothing", "[terrainrules]")
{
  SECTION("an empty rule set")
  {
    TerrainRuleSet const rules;

    TerrainRuleResult const result = rules.evaluate(sampleAt(100.0f, 30.0f));

    CHECK_FALSE(result.matched);
    CHECK(result.alpha == 0);
    CHECK(result.texture.empty());
    CHECK(rules.matchCount(sampleAt(100.0f, 30.0f)) == 0);
  }

  SECTION("rules that all miss")
  {
    TerrainRuleSet rules;
    rules.addRule(makeRule("snow", TerrainRange::atLeast(400.0f), TerrainRange::any()));
    rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f)));

    TerrainSample const sample = sampleAt(100.0f, 12.0f);
    TerrainRuleResult const result = rules.evaluate(sample);

    CHECK_FALSE(result.matched);
    CHECK(result.alpha == 0);
    CHECK(result.texture.empty());
    CHECK(rules.matchCount(sample) == 0);
  }

  SECTION("an inverted range makes a rule that would otherwise match win nothing")
  {
    TerrainRuleSet rules;
    // Typed backwards: "between 400 and 100".
    rules.addRule(makeRule("snow", TerrainRange::between(400.0f, 100.0f), TerrainRange::any()));

    CHECK_FALSE(rules.evaluate(sampleAt(250.0f, 0.0f)).matched);
  }
}

TEST_CASE("Overlapping rules: priority is the first precedence key", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any(), 0, 200));
  rules.addRule(makeRule("road", TerrainRange::between(0.0f, 1000.0f), TerrainRange::between(0.0f, 90.0f), -5, 255));

  TerrainSample const sample = sampleAt(100.0f, 10.0f);

  // Both match; the road rule is far more specific and stronger, and still loses -- priority
  // outranks every other key.
  REQUIRE(rules.matchCount(sample) == 2);

  TerrainRuleResult const result = rules.evaluate(sample);
  CHECK(result.matched);
  CHECK(result.texture == "grass");
  CHECK(result.alpha == 200);
  CHECK(result.rule_index == 0);
}

TEST_CASE("Overlapping rules: specificity breaks a priority tie", "[terrainrules]")
{
  // The rule set every user writes first: no priorities anywhere.
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));
  rules.addRule(makeRule("snow", TerrainRange::atLeast(400.0f), TerrainRange::any()));
  rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f)));
  rules.addRule(makeRule("cliffsnow", TerrainRange::atLeast(400.0f), TerrainRange::atLeast(60.0f)));

  CHECK(rules.evaluate(sampleAt(100.0f, 10.0f)).texture == "grass");
  CHECK(rules.evaluate(sampleAt(500.0f, 10.0f)).texture == "snow");
  CHECK(rules.evaluate(sampleAt(100.0f, 70.0f)).texture == "rock");
  // Two bounded endpoints beats one.
  CHECK(rules.evaluate(sampleAt(500.0f, 70.0f)).texture == "cliffsnow");

  CHECK(rules.rules()[3].specificity() == 2);
  CHECK(rules.rules()[1].specificity() == 1);
  CHECK(rules.rules()[0].specificity() == 0);
}

TEST_CASE("Overlapping rules: strength then texture break the remaining ties", "[terrainrules]")
{
  SECTION("equal priority and specificity: the stronger rule wins")
  {
    TerrainRuleSet rules;
    rules.addRule(makeRule("faint", TerrainRange::atLeast(0.0f), TerrainRange::any(), 0, 60));
    rules.addRule(makeRule("solid", TerrainRange::atLeast(0.0f), TerrainRange::any(), 0, 240));

    TerrainRuleResult const result = rules.evaluate(sampleAt(10.0f, 10.0f));
    CHECK(result.texture == "solid");
    CHECK(result.alpha == 240);
  }

  SECTION("equal on everything but the name: the lexicographically smaller name wins")
  {
    TerrainRuleSet rules;
    rules.addRule(makeRule("zebra", TerrainRange::atLeast(0.0f), TerrainRange::any(), 0, 128));
    rules.addRule(makeRule("aardvark", TerrainRange::atLeast(0.0f), TerrainRange::any(), 0, 128));

    CHECK(rules.evaluate(sampleAt(10.0f, 10.0f)).texture == "aardvark");
    CHECK(rules.evaluate(sampleAt(10.0f, 10.0f)).rule_index == 1);
  }

  SECTION("rules identical in content produce the same answer, reported against the lower index")
  {
    TerrainRuleSet rules;
    rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any(), 0, 100));
    rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any(), 0, 100));

    TerrainRuleResult const result = rules.evaluate(sampleAt(10.0f, 10.0f));
    CHECK(result.texture == "grass");
    CHECK(result.alpha == 100);
    CHECK(result.rule_index == 0);
  }
}

TEST_CASE("Boundary values at exactly the slope and height limits", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));
  rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f)));
  rules.addRule(makeRule("snow", TerrainRange::atLeast(400.0f), TerrainRange::any()));

  // Exactly on the limit is inside: "rock above 60 degrees" includes 60.
  CHECK(rules.evaluate(sampleAt(0.0f, 60.0f)).texture == "rock");
  CHECK(rules.evaluate(sampleAt(0.0f, std::nextafter(60.0f, 0.0f))).texture == "grass");
  CHECK(rules.evaluate(sampleAt(400.0f, 0.0f)).texture == "snow");
  CHECK(rules.evaluate(sampleAt(std::nextafter(400.0f, 0.0f), 0.0f)).texture == "grass");

  // The physical extremes of the slope domain.
  CHECK(rules.evaluate(sampleAt(0.0f, TERRAIN_SLOPE_MIN_DEGREES)).texture == "grass");
  CHECK(rules.evaluate(sampleAt(0.0f, TERRAIN_SLOPE_MAX_DEGREES)).texture == "rock");

  SECTION("a one-value height band matches only that height")
  {
    TerrainRuleSet exact;
    exact.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));
    exact.addRule(makeRule("waterline", TerrainRange::between(0.0f, 0.0f), TerrainRange::any()));

    CHECK(exact.evaluate(sampleAt(0.0f, 0.0f)).texture == "waterline");
    CHECK(exact.evaluate(sampleAt(std::nextafter(0.0f, 1.0f), 0.0f)).texture == "grass");
    CHECK(exact.evaluate(sampleAt(std::nextafter(0.0f, -1.0f), 0.0f)).texture == "grass");
  }
}

TEST_CASE("Evaluation is order-independent for equal priorities", "[terrainrules]")
{
  // Every rule here has priority 0, so nothing but content decides. Permuting the list must not
  // change any answer -- which is the property that makes a saved rule set reproducible after a
  // round trip through a UI list that reorders it.
  std::vector<TerrainRule> base
    { makeRule("grass", TerrainRange::any(), TerrainRange::any(), 0, 255)
    , makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(55.0f), 0, 255)
    , makeRule("snow", TerrainRange::atLeast(300.0f), TerrainRange::any(), 0, 200)
    , makeRule("sand", TerrainRange::atMost(20.0f), TerrainRange::atMost(10.0f), 0, 180)
    , makeRule("scree", TerrainRange::between(300.0f, 900.0f), TerrainRange::between(30.0f, 55.0f), 0, 128)
    };

  std::vector<TerrainSample> probes;
  for (int height = -50; height <= 950; height += 37)
  {
    for (int slope = 0; slope <= 90; slope += 7)
    {
      probes.push_back(sampleAt(static_cast<float>(height), static_cast<float>(slope)));
    }
  }

  std::sort(base.begin(), base.end(), [] (TerrainRule const& l, TerrainRule const& r)
    { return l.texture < r.texture; });

  TerrainRuleSet const reference(base);

  std::vector<std::string> expected_textures;
  std::vector<int> expected_alphas;
  expected_textures.reserve(probes.size());
  expected_alphas.reserve(probes.size());

  for (TerrainSample const& probe : probes)
  {
    TerrainRuleResult const result = reference.evaluate(probe);
    expected_textures.push_back(std::string(result.texture));
    expected_alphas.push_back(result.matched ? static_cast<int>(result.alpha) : -1);
  }

  std::size_t permutations = 0;

  do
  {
    TerrainRuleSet const permuted(base);
    ++permutations;

    for (std::size_t i = 0; i < probes.size(); ++i)
    {
      TerrainRuleResult const result = permuted.evaluate(probes[i]);

      REQUIRE(std::string(result.texture) == expected_textures[i]);
      REQUIRE((result.matched ? static_cast<int>(result.alpha) : -1) == expected_alphas[i]);
    }
  }
  while (std::next_permutation( base.begin(), base.end()
                              , [] (TerrainRule const& l, TerrainRule const& r)
                                { return l.texture < r.texture; }
                              ));

  // 5! -- proof the loop actually swept the permutations rather than exiting immediately.
  CHECK(permutations == 120);
}

TEST_CASE("Evaluation is deterministic and leaves the set untouched", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));
  rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f), 1));

  std::vector<std::size_t> const order_before = rules.precedenceOrder();
  TerrainSample const sample = sampleAt(120.0f, 61.0f);

  TerrainRuleResult const first = rules.evaluate(sample);

  for (int repeat = 0; repeat < 32; ++repeat)
  {
    TerrainRuleResult const again = rules.evaluate(sample);

    CHECK(again.matched == first.matched);
    CHECK(again.rule_index == first.rule_index);
    CHECK(again.texture == first.texture);
    CHECK(again.alpha == first.alpha);
  }

  CHECK(rules.precedenceOrder() == order_before);
  CHECK(rules.size() == 2);
}

TEST_CASE("Disabled rules neither match nor win", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));

  TerrainRule disabled = makeRule("rock", TerrainRange::any(), TerrainRange::any(), 100);
  disabled.enabled = false;
  rules.addRule(disabled);

  TerrainSample const sample = sampleAt(50.0f, 20.0f);

  CHECK(rules.matchCount(sample) == 1);
  CHECK(rules.evaluate(sample).texture == "grass");

  // Nor does it contribute a layer the caller would have to make room for.
  std::vector<std::string> const textures = rules.distinctTextures();
  REQUIRE(textures.size() == 1);
  CHECK(textures[0] == "grass");
}

TEST_CASE("evaluateRanked yields every match in precedence order and truncates", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any(), 0));
  rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f), 5));
  rules.addRule(makeRule("snow", TerrainRange::atLeast(400.0f), TerrainRange::any(), 3));
  rules.addRule(makeRule("sand", TerrainRange::atMost(10.0f), TerrainRange::any(), 9));

  TerrainSample const sample = sampleAt(500.0f, 70.0f);
  REQUIRE(rules.matchCount(sample) == 3);

  TerrainRuleResult ranked[8];
  std::size_t const written = rules.evaluateRanked(sample, ranked, 8);

  REQUIRE(written == 3);
  CHECK(ranked[0].texture == "rock");
  CHECK(ranked[1].texture == "snow");
  CHECK(ranked[2].texture == "grass");

  // The first ranked entry and evaluate() must never disagree.
  CHECK(rules.evaluate(sample).rule_index == ranked[0].rule_index);

  SECTION("a smaller buffer keeps the top of the order")
  {
    TerrainRuleResult two[2];
    std::size_t const truncated = rules.evaluateRanked(sample, two, 2);

    REQUIRE(truncated == 2);
    CHECK(two[0].texture == "rock");
    CHECK(two[1].texture == "snow");
  }

  SECTION("a zero-size or null buffer writes nothing")
  {
    TerrainRuleResult one[1];
    CHECK(rules.evaluateRanked(sample, one, 0) == 0);
    CHECK(rules.evaluateRanked(sample, nullptr, 4) == 0);
  }
}

TEST_CASE("An unusable slope falls through to the catch-all rather than reading as flat", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));
  rules.addRule(makeRule("flat_road", TerrainRange::any(), TerrainRange::atMost(5.0f), 0, 255));
  rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f), 0, 255));

  // A zero normal -- what a chunk with corrupt or never-computed normals produces.
  TerrainSample corrupt;
  corrupt.height = 100.0f;
  corrupt.slope_degrees = slopeDegreesFromNormal({0.0f, 0.0f, 0.0f});
  REQUIRE(std::isnan(corrupt.slope_degrees));

  TerrainRuleResult const result = rules.evaluate(corrupt);

  CHECK(result.matched);
  CHECK(result.texture == "grass");
  CHECK(rules.matchCount(corrupt) == 1);

  SECTION("with no catch-all, an unusable slope simply wins nothing")
  {
    TerrainRuleSet strict;
    strict.addRule(makeRule("flat_road", TerrainRange::any(), TerrainRange::atMost(5.0f)));
    strict.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f)));

    CHECK_FALSE(strict.evaluate(corrupt).matched);
  }
}

TEST_CASE("Coverage conserves samples and ranks deterministically", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f), 0, 255));
  rules.addRule(makeRule("snow", TerrainRange::atLeast(400.0f), TerrainRange::any(), 0, 128));

  TerrainRuleCoverage coverage;
  std::size_t total = 0;

  for (int height = 0; height <= 800; height += 100)
  {
    for (int slope = 0; slope <= 90; slope += 10)
    {
      coverage.addSample(rules.evaluate(sampleAt(static_cast<float>(height), static_cast<float>(slope))));
      ++total;
    }
  }

  REQUIRE(coverage.sampleCount() == total);

  // Conservation: nothing is counted twice and nothing is lost.
  std::size_t summed = coverage.unmatchedCount();
  for (TerrainCoverageEntry const& entry : coverage.entries())
  {
    summed += entry.sample_count;
  }
  CHECK(summed == coverage.sampleCount());
  CHECK(coverage.matchedCount() + coverage.unmatchedCount() == coverage.sampleCount());

  std::vector<TerrainCoverageEntry> const entries = coverage.entries();
  REQUIRE(entries.size() == 2);
  CHECK(coverage.distinctTextures() == 2);

  // Descending by count, and the alpha bookkeeping matches the rules that produced it.
  CHECK(entries[0].sample_count >= entries[1].sample_count);
  for (TerrainCoverageEntry const& entry : entries)
  {
    CHECK(entry.max_alpha == (entry.texture == "rock" ? 255 : 128));
    CHECK(entry.averageAlpha() == Catch::Approx(static_cast<float>(entry.max_alpha)).margin(1e-4));
    CHECK(entry.alpha_total == static_cast<std::uint64_t>(entry.max_alpha) * entry.sample_count);
  }

  CHECK(coverage.topEntries(1).size() == 1);
  CHECK(coverage.topEntries(1)[0].texture == entries[0].texture);
  CHECK(coverage.topEntries(99).size() == 2);

  SECTION("an empty coverage is empty, not degenerate")
  {
    TerrainRuleCoverage const nothing;
    CHECK(nothing.sampleCount() == 0);
    CHECK(nothing.matchedCount() == 0);
    CHECK(nothing.entries().empty());
    CHECK(nothing.topEntries(4).empty());
  }
}

TEST_CASE("Coverage merge matches a single accumulation", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any(), 0, 255));
  rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f), 0, 200));

  TerrainRuleCoverage whole;
  TerrainRuleCoverage first_half;
  TerrainRuleCoverage second_half;

  for (int slope = 0; slope <= 90; ++slope)
  {
    TerrainRuleResult const result = rules.evaluate(sampleAt(10.0f, static_cast<float>(slope)));
    whole.addSample(result);
    (slope < 45 ? first_half : second_half).addSample(result);
  }

  TerrainRuleCoverage forward = first_half;
  forward.merge(second_half);

  TerrainRuleCoverage backward = second_half;
  backward.merge(first_half);

  REQUIRE(forward.sampleCount() == whole.sampleCount());
  REQUIRE(backward.sampleCount() == whole.sampleCount());
  CHECK(forward.summary() == whole.summary());
  CHECK(backward.summary() == whole.summary());

  std::vector<TerrainCoverageEntry> const expected = whole.entries();
  std::vector<TerrainCoverageEntry> const merged_forward = forward.entries();
  std::vector<TerrainCoverageEntry> const merged_backward = backward.entries();

  REQUIRE(merged_forward.size() == expected.size());
  REQUIRE(merged_backward.size() == expected.size());

  for (std::size_t i = 0; i < expected.size(); ++i)
  {
    CHECK(merged_forward[i].texture == expected[i].texture);
    CHECK(merged_forward[i].sample_count == expected[i].sample_count);
    CHECK(merged_forward[i].alpha_total == expected[i].alpha_total);
    CHECK(merged_forward[i].max_alpha == expected[i].max_alpha);

    CHECK(merged_backward[i].texture == expected[i].texture);
    CHECK(merged_backward[i].sample_count == expected[i].sample_count);
  }
}

TEST_CASE("validate names the rules that can never do anything", "[terrainrules]")
{
  SECTION("a sound set with a catch-all reports nothing")
  {
    TerrainRuleSet rules;
    rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));
    rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f)));

    CHECK(rules.validate().empty());
  }

  SECTION("an empty set reports nothing -- there is nothing to be wrong about")
  {
    CHECK(TerrainRuleSet().validate().empty());
  }

  SECTION("each defect is named against its rule index")
  {
    TerrainRuleSet rules;
    rules.addRule(makeRule("", TerrainRange::any(), TerrainRange::any()));
    rules.addRule(makeRule("inverted", TerrainRange::between(400.0f, 100.0f), TerrainRange::between(80.0f, 10.0f)));
    rules.addRule(makeRule("radians", TerrainRange::any(), TerrainRange::atLeast(1.2f)));
    rules.addRule(makeRule("unit_error", TerrainRange::any(), TerrainRange::atLeast(1000.0f)));
    rules.addRule(makeRule("invisible", TerrainRange::any(), TerrainRange::atLeast(10.0f), 0, 0));
    rules.addRule(makeRule("poisoned", TerrainRange::between(NAN_F, 100.0f), TerrainRange::any()));

    std::vector<std::string> const problems = rules.validate();

    auto has = [&problems] (std::string const& needle)
    {
      return std::any_of(problems.begin(), problems.end(), [&needle] (std::string const& line)
        { return line.find(needle) != std::string::npos; });
    };

    CHECK(has("rule 0: empty texture identifier"));
    CHECK(has("rule 1: height range is inverted"));
    CHECK(has("rule 1: slope range is inverted"));
    CHECK(has("rule 3: slope range lies above 90 degrees"));
    CHECK(has("rule 4: blend strength is 0"));
    CHECK(has("rule 5: height range endpoint is not a number"));

    // Rule 0 is unbounded on both axes, so it is structurally a catch-all -- but it names no
    // texture, so it cannot be one. The missing-fallback warning must not be suppressed by it.
    CHECK(has("no catch-all rule"));

    // 1.2 degrees is a legal, if odd, threshold. It is not flagged: only a range that provably
    // cannot intersect [0, 90] is.
    CHECK_FALSE(has("rule 2"));
  }

  SECTION("the missing-fallback warning tracks whether a usable catch-all exists")
  {
    TerrainRuleSet no_fallback;
    no_fallback.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f)));

    std::vector<std::string> const problems = no_fallback.validate();
    REQUIRE(problems.size() == 1);
    CHECK(problems[0] == "no catch-all rule: points matching nothing are left untouched");

    // A rule bounded on ONE axis is not a fallback either -- it still leaves points uncovered.
    TerrainRuleSet half;
    half.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f)));
    half.addRule(makeRule("snow", TerrainRange::atLeast(400.0f), TerrainRange::any()));
    CHECK(half.validate().size() == 1);

    // A disabled catch-all is not in effect and does not count.
    TerrainRuleSet disabled_fallback;
    disabled_fallback.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f)));
    TerrainRule sleeping = makeRule("grass", TerrainRange::any(), TerrainRange::any());
    sleeping.enabled = false;
    disabled_fallback.addRule(sleeping);
    CHECK(disabled_fallback.validate().size() == 1);
  }

  SECTION("a disabled rule's defects are not reported")
  {
    TerrainRuleSet rules;
    rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));

    TerrainRule broken = makeRule("", TerrainRange::between(9.0f, 1.0f), TerrainRange::any());
    broken.enabled = false;
    rules.addRule(broken);

    CHECK(rules.validate().empty());
  }
}

TEST_CASE("evaluateGrid visits every cell centre exactly once", "[terrainrules]")
{
  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any()));

  constexpr int RESOLUTION = 8;
  constexpr float EXTENT = 33.3333f;
  constexpr float X_BASE = -100.0f;
  constexpr float Z_BASE = 250.0f;

  std::vector<int> visits(RESOLUTION * RESOLUTION, 0);
  std::vector<float> xs;
  std::vector<float> zs;

  evaluateGrid( rules, X_BASE, Z_BASE, EXTENT, RESOLUTION
              , [] (float x, float z) { return sampleAt(x + z, 0.0f); }
              , [&] (int row, int col, float x, float z, TerrainSample const& sample, TerrainRuleResult const& result)
                {
                  ++visits[static_cast<std::size_t>(row) * RESOLUTION + static_cast<std::size_t>(col)];
                  xs.push_back(x);
                  zs.push_back(z);

                  CHECK(sample.height == Catch::Approx(x + z).margin(1e-3));
                  CHECK(result.matched);
                }
              );

  CHECK(std::count(visits.begin(), visits.end(), 1) == RESOLUTION * RESOLUTION);
  REQUIRE(xs.size() == static_cast<std::size_t>(RESOLUTION) * RESOLUTION);

  // Cell centres: strictly inside the region, and the first one sits half a step in.
  float const step = EXTENT / RESOLUTION;
  CHECK(xs.front() == Catch::Approx(X_BASE + 0.5f * step).margin(1e-3));
  CHECK(zs.front() == Catch::Approx(Z_BASE + 0.5f * step).margin(1e-3));
  CHECK(xs.back() == Catch::Approx(X_BASE + EXTENT - 0.5f * step).margin(1e-3));

  for (float x : xs)
  {
    CHECK(x > X_BASE);
    CHECK(x < X_BASE + EXTENT);
  }

  SECTION("a non-positive resolution does nothing rather than dividing by zero")
  {
    int calls = 0;
    auto counting_sink = [&calls] (int, int, float, float, TerrainSample const&, TerrainRuleResult const&)
      { ++calls; };

    evaluateGrid(rules, 0.0f, 0.0f, 10.0f, 0, [] (float, float) { return TerrainSample{}; }, counting_sink);
    evaluateGrid(rules, 0.0f, 0.0f, 10.0f, -4, [] (float, float) { return TerrainSample{}; }, counting_sink);

    CHECK(calls == 0);
  }

  SECTION("resolution 1 is a single sample at the middle")
  {
    std::vector<float> centre;
    evaluateGrid( rules, 0.0f, 0.0f, 10.0f, 1
                , [] (float, float) { return TerrainSample{}; }
                , [&centre] (int, int, float x, float z, TerrainSample const&, TerrainRuleResult const&)
                  { centre.push_back(x); centre.push_back(z); }
                );

    REQUIRE(centre.size() == 2);
    CHECK(centre[0] == Catch::Approx(5.0f).margin(1e-4));
    CHECK(centre[1] == Catch::Approx(5.0f).margin(1e-4));
  }
}

TEST_CASE("Chunk vertex indexing agrees with MapChunk and covers every entry once", "[terrainrules]")
{
  using namespace Noggit::TerrainRuleCollector;

  CHECK(CHUNK_VERTEX_COUNT == 145);

  std::set<int> seen;

  for (int row = 0; row < CHUNK_OUTER_SIDE; ++row)
  {
    for (int col = 0; col < CHUNK_OUTER_SIDE; ++col)
    {
      int const index = chunkOuterVertexIndex(row, col);

      CHECK(index == referenceIndexNoLoD(row, col));
      CHECK(index >= 0);
      CHECK(index < CHUNK_VERTEX_COUNT);
      CHECK(seen.insert(index).second);
    }
  }

  for (int row = 0; row < CHUNK_INNER_SIDE; ++row)
  {
    for (int col = 0; col < CHUNK_INNER_SIDE; ++col)
    {
      int const index = chunkInnerVertexIndex(row, col);

      CHECK(index == referenceIndexLoD(row, col));
      CHECK(index >= 0);
      CHECK(index < CHUNK_VERTEX_COUNT);
      CHECK(seen.insert(index).second);
    }
  }

  // The interleaving is a bijection: 81 outer plus 64 inner exactly fill 0..144, with nothing
  // aliased and nothing skipped. An off-by-one in either formula breaks this and nothing else.
  CHECK(seen.size() == static_cast<std::size_t>(CHUNK_VERTEX_COUNT));
  CHECK(*seen.begin() == 0);
  CHECK(*seen.rbegin() == CHUNK_VERTEX_COUNT - 1);
}

TEST_CASE("The chunk collector reads heights and normals off a stand-in chunk", "[terrainrules]")
{
  using namespace Noggit::TerrainRuleCollector;

  TerrainRuleSet rules;
  rules.addRule(makeRule("grass", TerrainRange::any(), TerrainRange::any(), 0, 255));
  rules.addRule(makeRule("rock", TerrainRange::any(), TerrainRange::atLeast(60.0f), 0, 200));
  rules.addRule(makeRule("snow", TerrainRange::atLeast(400.0f), TerrainRange::any(), 0, 180));

  FakeChunk chunk;

  // Half the chunk is a high plateau, one unit is a cliff.
  for (int i = 0; i < CHUNK_VERTEX_COUNT; ++i)
  {
    chunk.mVertices[i].y = (i % 2 == 0) ? 500.0f : 100.0f;
  }

  int const cliff_index = chunkInnerVertexIndex(3, 4);
  chunk.mNormals[cliff_index] = FakeVector{0.0f, 0.2f, 0.98f};

  SECTION("null chunks are ignored")
  {
    int calls = 0;
    collectChunkVertices<FakeChunk>( nullptr, rules
                                   , [&calls] (FakeChunk*, int, TerrainSample const&, TerrainRuleResult const&)
                                     { ++calls; });
    collectChunkUnits<FakeChunk>( nullptr, rules
                                , [&calls] (FakeChunk*, int, int, TerrainSample const&, TerrainRuleResult const&)
                                  { ++calls; });
    CHECK(calls == 0);
  }

  SECTION("every vertex is visited once and classified from its own height and normal")
  {
    std::vector<int> visits(CHUNK_VERTEX_COUNT, 0);
    TerrainRuleCoverage coverage;

    collectChunkVertices( &chunk, rules
                        , [&] (FakeChunk* visited, int vertex_index, TerrainSample const& sample, TerrainRuleResult const& result)
                          {
                            REQUIRE(visited == &chunk);
                            ++visits[static_cast<std::size_t>(vertex_index)];
                            coverage.addSample(result);

                            CHECK(sample.height == chunk.mVertices[vertex_index].y);

                            if (vertex_index == cliff_index)
                            {
                              CHECK(sample.slope_degrees > 60.0f);
                              CHECK(result.texture == "rock");
                            }
                            else if (sample.height >= 400.0f)
                            {
                              CHECK(result.texture == "snow");
                            }
                            else
                            {
                              CHECK(result.texture == "grass");
                            }
                          }
                        );

    CHECK(std::count(visits.begin(), visits.end(), 1) == CHUNK_VERTEX_COUNT);
    CHECK(coverage.sampleCount() == static_cast<std::size_t>(CHUNK_VERTEX_COUNT));
    CHECK(coverage.unmatchedCount() == 0);
    CHECK(coverage.distinctTextures() == 3);
  }

  SECTION("the unit walk covers the 8x8 grid and lands on the inner vertices")
  {
    std::vector<int> visits(CHUNK_INNER_SIDE * CHUNK_INNER_SIDE, 0);
    int rock_units = 0;

    collectChunkUnits( &chunk, rules
                     , [&] (FakeChunk*, int unit_row, int unit_col, TerrainSample const& sample, TerrainRuleResult const& result)
                       {
                         ++visits[static_cast<std::size_t>(unit_row) * CHUNK_INNER_SIDE + static_cast<std::size_t>(unit_col)];

                         CHECK(sample.height == chunk.mVertices[chunkInnerVertexIndex(unit_row, unit_col)].y);

                         if (result.texture == "rock")
                         {
                           ++rock_units;
                           CHECK(unit_row == 3);
                           CHECK(unit_col == 4);
                         }
                       }
                     );

    CHECK(std::count(visits.begin(), visits.end(), 1) == CHUNK_INNER_SIDE * CHUNK_INNER_SIDE);
    CHECK(rock_units == 1);
  }

  SECTION("out-of-range indices yield a default sample rather than reading past the arrays")
  {
    CHECK(sampleChunkVertex(&chunk, -1).height == 0.0f);
    CHECK(sampleChunkVertex(&chunk, CHUNK_VERTEX_COUNT).height == 0.0f);
    CHECK(sampleChunkUnit(&chunk, -1, 0).height == 0.0f);
    CHECK(sampleChunkUnit(&chunk, 0, CHUNK_INNER_SIDE).height == 0.0f);
    CHECK(sampleChunkVertex<FakeChunk>(nullptr, 0).height == 0.0f);
  }
}
