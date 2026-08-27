// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the alpha map integrity checker's pure half.
//
// Everything here runs on plain byte arrays, which is the reason AlphaIntegrity.cpp holds no
// MapChunk, no TextureSet, no Qt and no OpenGL: a defect is a value in an array rather than a
// state of a loaded map, so all three defect classes can be built by hand, combined, and repaired
// on a machine with no client install.
//
// What is NOT covered here, by construction: AlphaIntegrityCollector, which reads TextureSet and
// walks the world. It is a set of templates instantiated against World/MapTile/MapChunk/TextureSet
// and cannot be reached without a loaded map.

#include <catch2/catch_test_macros.hpp>

#include <noggit/terrain/AlphaIntegrity.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

using namespace Noggit;

namespace
{
  AlphaLayer filled(int value)
  {
    AlphaLayer layer;
    layer.fill(static_cast<std::uint8_t>(value));
    return layer;
  }

  // A stack of constant layers. Constant is the useful base case: it is trivially border-correct
  // (every texel equals every other), so any border mismatch a test sees is one it introduced.
  AlphaLayerStack stackOf(std::vector<int> const& values)
  {
    AlphaLayerStack stack;

    for (int value : values)
    {
      stack.addLayer(filled(value));
    }

    return stack;
  }

  constexpr std::size_t LAST = ALPHA_DIM - 1;
  constexpr std::size_t PENULTIMATE = ALPHA_DIM - 2;

  bool contains(std::string const& text, std::string const& needle)
  {
    return text.find(needle) != std::string::npos;
  }

  bool hasLayer(std::vector<std::size_t> const& layers, std::size_t index)
  {
    return std::find(layers.begin(), layers.end(), index) != layers.end();
  }
}

TEST_CASE("a stack of constant layers that fits in 255 is clean", "[alpha]")
{
  AlphaIntegrityReport const report = checkAlphaIntegrity(stackOf({100, 50, 25}));

  CHECK(report.layer_count == 3);
  CHECK(report.clean());
  CHECK_FALSE(report.has(AlphaDefect::OverOpaque));
  CHECK_FALSE(report.has(AlphaDefect::EmptyLayer));
  CHECK_FALSE(report.has(AlphaDefect::BorderMismatch));
  CHECK(contains(report.summary(), "clean"));
}

TEST_CASE("a chunk with no stored layers has nothing to be wrong with", "[alpha]")
{
  // One texture (or none) stores no alpha at all; TextureSet leaves the array empty.
  AlphaIntegrityReport const report = checkAlphaIntegrity(AlphaLayerStack{});

  CHECK(report.layer_count == 0);
  CHECK(report.clean());
}

TEST_CASE("combined weight past full opacity is reported, exactly at the boundary", "[alpha]")
{
  SECTION("255 is not over-opaque")
  {
    AlphaIntegrityReport const report = checkAlphaIntegrity(stackOf({200, 55}));
    CHECK(report.over_opaque_texels == 0);
    CHECK(report.max_excess == 0);
    CHECK(report.first_over_opaque_texel == ALPHA_TEXELS);
  }

  SECTION("256 is")
  {
    AlphaIntegrityReport const report = checkAlphaIntegrity(stackOf({200, 56}));
    CHECK(report.over_opaque_texels == ALPHA_TEXELS);
    CHECK(report.max_excess == 1);
    CHECK(report.first_over_opaque_texel == 0);
  }

  SECTION("a single offending texel is found and located")
  {
    AlphaLayerStack stack = stackOf({100, 50});
    stack.set(0, alphaTexel(9, 12), 220);

    AlphaIntegrityReport const report = checkAlphaIntegrity(stack);

    REQUIRE(report.over_opaque_texels == 1);
    CHECK(report.max_excess == 15);
    CHECK(report.first_over_opaque_texel == alphaTexel(9, 12));
    CHECK(alphaRow(report.first_over_opaque_texel) == 9);
    CHECK(alphaColumn(report.first_over_opaque_texel) == 12);
  }
}

TEST_CASE("the combined weight is computed without the wraparound TextureSet::sum_alpha has", "[alpha]")
{
  // sum_alpha (texture_set.cpp:1621) accumulates into a uint8_t, so 350 reads back as 94 there and
  // `255 - sum_alpha(i)` hands layer 0 a weight of 161 instead of none. That silent wrap is why
  // this defect class needs a checker at all, so the widened sum gets its own case.
  AlphaLayerStack const stack = stackOf({200, 100, 50});

  CHECK(stack.sumAt(0) == 350);
  CHECK(stack.sumAt(0) != (350 % 256));
  CHECK(stack.baseWeightAt(0) == -95);
  CHECK(checkAlphaIntegrity(stack).max_excess == 95);
}

TEST_CASE("a layer that is zero everywhere is dead weight", "[alpha]")
{
  SECTION("all-zero layer is reported by index")
  {
    AlphaIntegrityReport const report = checkAlphaIntegrity(stackOf({100, 0, 40}));

    REQUIRE(report.empty_layers.size() == 1);
    CHECK(report.empty_layers.front() == 1);
    CHECK(report.has(AlphaDefect::EmptyLayer));
  }

  SECTION("one non-zero texel is enough to keep a layer alive")
  {
    AlphaLayerStack stack = stackOf({100, 0});
    stack.set(1, alphaTexel(31, 31), 1);

    CHECK(checkAlphaIntegrity(stack).empty_layers.empty());
  }

  SECTION("every layer can be empty at once")
  {
    AlphaIntegrityReport const report = checkAlphaIntegrity(stackOf({0, 0, 0}));

    CHECK(report.empty_layers.size() == 3);
    // Not over-opaque: a chunk painted entirely with layer 0 is legal and common.
    CHECK_FALSE(report.has(AlphaDefect::OverOpaque));
  }
}

TEST_CASE("the duplicated last row and column are checked against their neighbour", "[alpha]")
{
  SECTION("last column, last row and corner are counted separately")
  {
    AlphaLayerStack stack = stackOf({100, 50});
    stack.set(0, alphaTexel(10, LAST), 200);
    stack.set(0, alphaTexel(LAST, 20), 7);

    AlphaIntegrityReport const report = checkAlphaIntegrity(stack);

    REQUIRE(report.border_mismatches.size() == 1);
    AlphaBorderMismatch const& mismatch = report.border_mismatches.front();

    CHECK(mismatch.layer == 0);
    CHECK(mismatch.last_column_texels == 1);
    CHECK(mismatch.last_row_texels == 1);
    CHECK_FALSE(mismatch.corner);
    CHECK(mismatch.total() == 2);
    CHECK(mismatch.first_texel == alphaTexel(10, LAST));
    CHECK(mismatch.max_delta == 100);
    CHECK(report.borderMismatchTexels() == 2);
  }

  SECTION("the corner is taken from (62,62), not from either edge")
  {
    // Alphamap.cpp:130 assigns amap[63*64+63] = amap[62*64+62]. A corner equal to its own row
    // neighbour but not to (62,62) is still wrong.
    AlphaLayerStack stack = stackOf({80});
    stack.set(0, alphaTexel(PENULTIMATE, PENULTIMATE), 33);

    AlphaIntegrityReport const report = checkAlphaIntegrity(stack);

    REQUIRE(report.border_mismatches.size() == 1);
    CHECK(report.border_mismatches.front().corner);
    // Changing (62,62) also breaks its own row's last column and its own column's last row.
    CHECK(report.border_mismatches.front().last_column_texels == 1);
    CHECK(report.border_mismatches.front().last_row_texels == 1);
  }

  SECTION("each layer is duplicated independently and reported independently")
  {
    AlphaLayerStack stack = stackOf({100, 50, 20});
    stack.set(0, alphaTexel(3, LAST), 101);
    stack.set(2, alphaTexel(LAST, 4), 21);

    AlphaIntegrityReport const report = checkAlphaIntegrity(stack);

    REQUIRE(report.border_mismatches.size() == 2);
    CHECK(report.border_mismatches[0].layer == 0);
    CHECK(report.border_mismatches[1].layer == 2);
    CHECK(report.border_mismatches[1].max_delta == 1);
  }
}

TEST_CASE("the border check is only valid for the format that duplicates", "[alpha]")
{
  // Big alpha and compressed MCAL deliver 4096 real bytes (Alphamap.cpp:54 and :93); row 63 there
  // is data, not a copy. Running the check on them would report legitimate maps as broken.
  CHECK(AlphaIntegrityOptions::forChunk(false, false).check_border);
  CHECK_FALSE(AlphaIntegrityOptions::forChunk(true, false).check_border);
  CHECK_FALSE(AlphaIntegrityOptions::forChunk(false, true).check_border);
  CHECK_FALSE(AlphaIntegrityOptions::forChunk(true, true).check_border);

  AlphaLayerStack stack = stackOf({100});
  stack.set(0, alphaTexel(5, LAST), 200);

  CHECK(checkAlphaIntegrity(stack, AlphaIntegrityOptions::forChunk(false, false)).has(AlphaDefect::BorderMismatch));
  CHECK_FALSE(checkAlphaIntegrity(stack, AlphaIntegrityOptions::forChunk(true, false)).has(AlphaDefect::BorderMismatch));

  // The other two checks are unaffected by the format.
  AlphaIntegrityOptions options = AlphaIntegrityOptions::forChunk(true, true);
  options.check_overflow = false;
  CHECK_FALSE(checkAlphaIntegrity(stackOf({200, 200}), options).has(AlphaDefect::OverOpaque));
  CHECK(checkAlphaIntegrity(stackOf({200, 200}), AlphaIntegrityOptions::forChunk(true, true))
          .has(AlphaDefect::OverOpaque));
}

TEST_CASE("the three defect classes are detected together and counted independently", "[alpha]")
{
  AlphaLayerStack stack = stackOf({200, 0, 100});   // over-opaque everywhere, layer 1 empty
  stack.set(0, alphaTexel(7, LAST), 199);           // and a border texel that was never duplicated

  AlphaIntegrityReport const report = checkAlphaIntegrity(stack);

  CHECK(report.has(AlphaDefect::OverOpaque));
  CHECK(report.has(AlphaDefect::EmptyLayer));
  CHECK(report.has(AlphaDefect::BorderMismatch));
  CHECK_FALSE(report.clean());

  CHECK(report.over_opaque_texels == ALPHA_TEXELS);
  CHECK(report.max_excess == 45);
  REQUIRE(report.empty_layers.size() == 1);
  CHECK(report.empty_layers.front() == 1);
  CHECK(report.borderMismatchTexels() == 1);

  std::string const text = report.toReport();
  CHECK(contains(text, "over-opaque"));
  CHECK(contains(text, "layer 2 is empty"));   // reported in chunk layer numbering, not stack index
  CHECK(contains(text, "border"));
}

TEST_CASE("repairing a clean stack changes nothing", "[alpha]")
{
  AlphaLayerStack const clean = stackOf({100, 50, 25});
  AlphaRepairResult const result = repairAlphaIntegrity(clean);

  CHECK_FALSE(result.changed());
  CHECK(result.over_opaque_texels_repaired == 0);
  CHECK(result.border_texels_repaired == 0);
  CHECK(result.layers == clean);
  CHECK(result.layers_to_drop.empty());
  CHECK(contains(result.summary(), "no repair needed"));
}

TEST_CASE("clipping takes the excess from the highest layers and leaves the rest alone", "[alpha]")
{
  SECTION("one layer absorbs it")
  {
    AlphaRepairResult const result = repairAlphaIntegrity(stackOf({100, 100, 100}));

    CHECK(result.over_opaque_texels_repaired == ALPHA_TEXELS);
    CHECK(result.layers.at(0, 0) == 100);
    CHECK(result.layers.at(1, 0) == 100);
    CHECK(result.layers.at(2, 0) == 55);
    CHECK(result.layers.sumAt(0) == ALPHA_OPAQUE);
  }

  SECTION("the excess cascades down when the top layer cannot cover it")
  {
    AlphaRepairResult const result = repairAlphaIntegrity(stackOf({250, 250, 250}));

    CHECK(result.layers.at(0, 0) == 250);
    CHECK(result.layers.at(1, 0) == 5);
    CHECK(result.layers.at(2, 0) == 0);
    CHECK(result.layers.sumAt(0) == ALPHA_OPAQUE);
    // The layer clipping emptied is offered for removal.
    CHECK(hasLayer(result.layers_to_drop, 2));
  }

  SECTION("a layer that was already at zero stays at zero")
  {
    AlphaRepairResult const result = repairAlphaIntegrity(stackOf({255, 100, 0}));

    CHECK(result.layers.at(0, 0) == 255);
    CHECK(result.layers.at(1, 0) == 0);
    CHECK(result.layers.at(2, 0) == 0);
    CHECK(result.layers_to_drop.size() == 2);
  }
}

TEST_CASE("scaling preserves the blend and lands on exactly 255", "[alpha]")
{
  SECTION("an exact ratio stays exact")
  {
    AlphaRepairOptions repair;
    repair.over_opacity_strategy = OverOpacityRepair::ScaleProportionally;

    AlphaRepairResult const result = repairAlphaIntegrity(stackOf({200, 100}), {}, repair);

    CHECK(result.layers.at(0, 0) == 170);
    CHECK(result.layers.at(1, 0) == 85);
    CHECK(result.layers.sumAt(0) == ALPHA_OPAQUE);
  }

  SECTION("the rounding residual goes to the largest remainders, never to a zero layer")
  {
    AlphaRepairOptions repair;
    repair.over_opacity_strategy = OverOpacityRepair::ScaleProportionally;

    // 1 + 1 + 254 = 256. Floor division assigns 0 + 0 + 253; the two leftover units go to the
    // layers whose remainder is largest, which are the two 1s.
    AlphaRepairResult const result = repairAlphaIntegrity(stackOf({1, 1, 254}), {}, repair);

    CHECK(result.layers.at(0, 0) == 1);
    CHECK(result.layers.at(1, 0) == 1);
    CHECK(result.layers.at(2, 0) == 253);
    CHECK(result.layers.sumAt(0) == ALPHA_OPAQUE);
  }
}

TEST_CASE("both repair strategies hold their numeric invariants over every layer combination", "[alpha]")
{
  // One stack carries many independent cases: each texel gets its own (a, b, c) triple, so a
  // single repair pass exercises all of them and the invariants are checked per texel. Texels
  // past the combinations are left at zero, which is a legal state that must survive untouched.
  std::vector<int> const values {0, 1, 60, 128, 200, 255};
  std::vector<std::array<int, 3>> combinations;

  for (int a : values)
  {
    for (int b : values)
    {
      for (int c : values)
      {
        combinations.push_back({a, b, c});
      }
    }
  }

  REQUIRE(combinations.size() <= ALPHA_TEXELS);

  AlphaLayerStack original = stackOf({0, 0, 0});

  for (std::size_t texel = 0; texel < combinations.size(); ++texel)
  {
    for (std::size_t layer = 0; layer < 3; ++layer)
    {
      original.set(layer, texel, static_cast<std::uint8_t>(combinations[texel][layer]));
    }
  }

  AlphaIntegrityOptions options;
  // The border is deliberately not repaired here: this case is about the arithmetic, and a border
  // copy would overwrite the combinations that happen to land on the last row or column.
  options.check_border = false;

  SECTION("clipping")
  {
    AlphaRepairOptions repair;
    repair.over_opacity_strategy = OverOpacityRepair::ClipUpperLayers;

    AlphaRepairResult const result = repairAlphaIntegrity(original, options, repair);

    for (std::size_t texel = 0; texel < combinations.size(); ++texel)
    {
      int const before = original.sumAt(texel);
      int const after = result.layers.sumAt(texel);

      if (before <= ALPHA_OPAQUE)
      {
        // Untouched: repair must not "improve" a texel that was already legal.
        REQUIRE(after == before);

        for (std::size_t layer = 0; layer < 3; ++layer)
        {
          REQUIRE(result.layers.at(layer, texel) == original.at(layer, texel));
        }

        continue;
      }

      REQUIRE(after == ALPHA_OPAQUE);

      for (std::size_t layer = 0; layer < 3; ++layer)
      {
        // Never increases a weight.
        REQUIRE(result.layers.at(layer, texel) <= original.at(layer, texel));

        // Structural property of clipping downwards: the moment a layer is reduced, everything
        // above it has already been taken to zero.
        if (result.layers.at(layer, texel) < original.at(layer, texel))
        {
          for (std::size_t above = layer + 1; above < 3; ++above)
          {
            REQUIRE(result.layers.at(above, texel) == 0);
          }
        }
      }
    }
  }

  SECTION("scaling")
  {
    AlphaRepairOptions repair;
    repair.over_opacity_strategy = OverOpacityRepair::ScaleProportionally;

    AlphaRepairResult const result = repairAlphaIntegrity(original, options, repair);

    for (std::size_t texel = 0; texel < combinations.size(); ++texel)
    {
      int const before = original.sumAt(texel);

      if (before <= ALPHA_OPAQUE)
      {
        REQUIRE(result.layers.sumAt(texel) == before);
        continue;
      }

      REQUIRE(result.layers.sumAt(texel) == ALPHA_OPAQUE);

      for (std::size_t layer = 0; layer < 3; ++layer)
      {
        REQUIRE(result.layers.at(layer, texel) <= original.at(layer, texel));

        // A layer that painted nothing must not start painting.
        if (original.at(layer, texel) == 0)
        {
          REQUIRE(result.layers.at(layer, texel) == 0);
        }

        // Order preserved, which is what "preserves the blend" means concretely. Stated with a
        // strict >, because two layers that were EQUAL cannot both keep the leftover unit: the
        // apportionment has to give it to one of them, and it gives it to the lower index. The
        // weaker statement for that case -- they stay within one of each other -- is below.
        for (std::size_t other = 0; other < 3; ++other)
        {
          if (original.at(layer, texel) > original.at(other, texel))
          {
            REQUIRE(result.layers.at(layer, texel) >= result.layers.at(other, texel));
          }
          else if (original.at(layer, texel) == original.at(other, texel))
          {
            int const drift = static_cast<int>(result.layers.at(layer, texel))
                            - static_cast<int>(result.layers.at(other, texel));
            REQUIRE(std::abs(drift) <= 1);
          }
        }
      }
    }
  }
}

TEST_CASE("border repair restores the duplication the reader would have produced", "[alpha]")
{
  AlphaLayerStack stack = stackOf({100, 50});
  stack.set(0, alphaTexel(10, LAST), 200);
  stack.set(0, alphaTexel(LAST, 20), 7);
  stack.set(1, alphaTexel(LAST, LAST), 3);

  AlphaRepairResult const result = repairAlphaIntegrity(stack);

  CHECK(result.border_texels_repaired == 3);
  CHECK(result.layers.at(0, alphaTexel(10, LAST)) == 100);
  CHECK(result.layers.at(0, alphaTexel(LAST, 20)) == 100);
  CHECK(result.layers.at(1, alphaTexel(LAST, LAST)) == 50);
  CHECK_FALSE(checkAlphaIntegrity(result.layers).has(AlphaDefect::BorderMismatch));
}

TEST_CASE("border repair copies from (62,62) into the corner rather than along an edge", "[alpha]")
{
  AlphaLayerStack stack = stackOf({10});

  // A gradient along the last two rows and columns, so an implementation that copied the corner
  // from (63,62) or (62,63) would land on a different value than one copying from (62,62).
  stack.set(0, alphaTexel(PENULTIMATE, PENULTIMATE), 40);
  stack.set(0, alphaTexel(PENULTIMATE, LAST), 41);
  stack.set(0, alphaTexel(LAST, PENULTIMATE), 42);
  stack.set(0, alphaTexel(LAST, LAST), 43);

  AlphaRepairResult const result = repairAlphaIntegrity(stack);

  CHECK(result.layers.at(0, alphaTexel(PENULTIMATE, LAST)) == 40);
  CHECK(result.layers.at(0, alphaTexel(LAST, PENULTIMATE)) == 40);
  CHECK(result.layers.at(0, alphaTexel(LAST, LAST)) == 40);
}

TEST_CASE("over-opacity is repaired before the border, so the border wins on shared texels", "[alpha]")
{
  // The last row is both over-opaque and undeduplicated. Clipping fixes the sum, then the border
  // pass overwrites it with the (already legal) neighbour, so the final state satisfies both.
  AlphaLayerStack stack = stackOf({100, 50});
  stack.set(0, alphaTexel(LAST, 20), 250);

  AlphaRepairResult const result = repairAlphaIntegrity(stack);

  CHECK(result.over_opaque_texels_repaired == 1);
  CHECK(result.layers.at(0, alphaTexel(LAST, 20)) == 100);
  CHECK(result.layers.at(1, alphaTexel(LAST, 20)) == 50);
  CHECK(checkAlphaIntegrity(result.layers).clean());
}

TEST_CASE("a repaired stack passes the check it was repaired against", "[alpha]")
{
  auto const roundTrip
  (
    [] (AlphaLayerStack const& stack, OverOpacityRepair strategy)
    {
      AlphaRepairOptions repair;
      repair.over_opacity_strategy = strategy;

      AlphaRepairResult const result = repairAlphaIntegrity(stack, {}, repair);
      AlphaIntegrityReport const report = checkAlphaIntegrity(result.layers);

      CHECK_FALSE(report.has(AlphaDefect::OverOpaque));
      CHECK_FALSE(report.has(AlphaDefect::BorderMismatch));

      // Idempotence: repairing the repaired stack is a no-op, byte for byte.
      AlphaRepairResult const again = repairAlphaIntegrity(result.layers, {}, repair);
      CHECK_FALSE(again.changed());
      CHECK(again.over_opaque_texels_repaired == 0);
      CHECK(again.border_texels_repaired == 0);
      CHECK(again.layers == result.layers);
      CHECK(again.layers_to_drop == result.layers_to_drop);
    }
  );

  // All three defect classes at once, plus texels that are fine, so the repair has to be
  // selective rather than uniform.
  AlphaLayerStack stack = stackOf({120, 0, 90});
  stack.set(0, alphaTexel(2, 2), 255);
  stack.set(2, alphaTexel(2, 2), 255);
  stack.set(0, alphaTexel(40, LAST), 3);
  stack.set(2, alphaTexel(LAST, 8), 250);
  stack.set(0, alphaTexel(LAST, LAST), 17);

  REQUIRE_FALSE(checkAlphaIntegrity(stack).clean());

  roundTrip(stack, OverOpacityRepair::ClipUpperLayers);
  roundTrip(stack, OverOpacityRepair::ScaleProportionally);
}

TEST_CASE("an empty layer survives repair and is reported for removal instead", "[alpha]")
{
  // Dropping a layer means dropping its MCLY entry and its texture reference too, which the pure
  // half cannot do without lying about which layer is which.
  AlphaRepairResult const result = repairAlphaIntegrity(stackOf({100, 0}));

  CHECK_FALSE(result.changed());
  CHECK(result.layers.size() == 2);
  REQUIRE(result.layers_to_drop.size() == 1);
  CHECK(result.layers_to_drop.front() == 1);
  CHECK(checkAlphaIntegrity(result.layers).has(AlphaDefect::EmptyLayer));
}

TEST_CASE("each repair can be declined, and the format overrides the border request", "[alpha]")
{
  AlphaLayerStack stack = stackOf({200, 100});
  stack.set(0, alphaTexel(5, LAST), 201);

  SECTION("over-opacity only")
  {
    AlphaRepairOptions repair;
    repair.repair_border = false;

    AlphaRepairResult const result = repairAlphaIntegrity(stack, {}, repair);

    CHECK(result.over_opaque_texels_repaired == ALPHA_TEXELS);
    CHECK(result.border_texels_repaired == 0);
    CHECK(checkAlphaIntegrity(result.layers).has(AlphaDefect::BorderMismatch));
  }

  SECTION("border only")
  {
    AlphaRepairOptions repair;
    repair.repair_over_opacity = false;

    AlphaRepairResult const result = repairAlphaIntegrity(stack, {}, repair);

    CHECK(result.over_opaque_texels_repaired == 0);
    CHECK(result.border_texels_repaired == 1);
    CHECK(checkAlphaIntegrity(result.layers).has(AlphaDefect::OverOpaque));
  }

  SECTION("a format whose border is real data is never touched, however the repair is asked")
  {
    AlphaRepairOptions repair;
    repair.repair_border = true;

    AlphaRepairResult const result
      = repairAlphaIntegrity(stack, AlphaIntegrityOptions::forChunk(true, false), repair);

    CHECK(result.border_texels_repaired == 0);
    // The over-opacity repair still ran, and it takes from the upper layer, so the border texel
    // keeps the value that a big-alpha chunk legitimately stores there.
    CHECK(result.layers.at(0, alphaTexel(5, LAST)) == 201);
    CHECK(result.layers.sumAt(alphaTexel(5, LAST)) == ALPHA_OPAQUE);
    CHECK(checkAlphaIntegrity(result.layers, AlphaIntegrityOptions::forChunk(false, false))
            .has(AlphaDefect::BorderMismatch));
  }
}

TEST_CASE("float weights are clamped and rounded the way a committed paint stroke is", "[alpha]")
{
  CHECK(alphaFromFloat(-1.f) == 0);
  CHECK(alphaFromFloat(0.f) == 0);
  CHECK(alphaFromFloat(0.4f) == 0);
  CHECK(alphaFromFloat(0.5f) == 1);
  CHECK(alphaFromFloat(127.5f) == 128);
  CHECK(alphaFromFloat(254.6f) == 255);
  CHECK(alphaFromFloat(255.f) == 255);
  CHECK(alphaFromFloat(1e9f) == 255);
  // NaN reaching the narrowing cast would be undefined behaviour, and a paint stroke that divides
  // by a zero total is one way to produce one.
  CHECK(alphaFromFloat(std::numeric_limits<float>::quiet_NaN()) == 0);
  CHECK(alphaFromFloat(-std::numeric_limits<float>::infinity()) == 0);
  CHECK(alphaFromFloat(std::numeric_limits<float>::infinity()) == 255);
}

TEST_CASE("the stack refuses more layers than the format has", "[alpha]")
{
  AlphaLayerStack stack;

  CHECK(stack.empty());
  CHECK(stack.addLayer(filled(1)));
  CHECK(stack.addZeroLayer());
  CHECK(stack.addLayer(filled(3)));
  CHECK(stack.size() == MAX_ALPHA_LAYERS);

  // Layer 0 of a chunk has no alpha map, so three stored layers is the whole of a 4-layer chunk.
  CHECK_FALSE(stack.addLayer(filled(4)));
  CHECK_FALSE(stack.addZeroLayer());
  CHECK(stack.size() == MAX_ALPHA_LAYERS);

  CHECK_FALSE(stack.addLayer(static_cast<std::uint8_t const*>(nullptr)));
  CHECK_FALSE(stack.addLayerFromFloats(nullptr));

  CHECK(stack.layerIsEmpty(1));
  CHECK_FALSE(stack.layerIsEmpty(0));
  CHECK_THROWS(stack.at(3, 0));
  CHECK_THROWS(stack.layer(3));
  CHECK_THROWS(stack.at(0, ALPHA_TEXELS));
}

TEST_CASE("a stack can be built from raw bytes or from in-flight float weights", "[alpha]")
{
  std::array<std::uint8_t, ALPHA_TEXELS> bytes{};
  std::array<float, ALPHA_TEXELS> floats{};

  for (std::size_t texel = 0; texel < ALPHA_TEXELS; ++texel)
  {
    bytes[texel] = static_cast<std::uint8_t>(texel % 256);
    floats[texel] = static_cast<float>(texel % 256) + 0.5f;
  }

  AlphaLayerStack stack;
  REQUIRE(stack.addLayer(bytes.data()));
  REQUIRE(stack.addLayerFromFloats(floats.data()));

  CHECK(stack.at(0, 0) == 0);
  CHECK(stack.at(0, 300) == 300 % 256);
  CHECK(stack.at(1, 0) == 1);            // 0.5 rounds up
  CHECK(stack.at(1, 255) == 255);        // 255.5 clamps rather than wrapping to 0
}

TEST_CASE("texel indexing agrees with the row-major layout the tree uses", "[alpha]")
{
  CHECK(alphaTexel(0, 0) == 0);
  CHECK(alphaTexel(1, 0) == ALPHA_DIM);
  CHECK(alphaTexel(0, 1) == 1);
  CHECK(alphaTexel(LAST, LAST) == ALPHA_TEXELS - 1);

  for (std::size_t texel = 0; texel < ALPHA_TEXELS; texel += 37)
  {
    CHECK(alphaTexel(alphaRow(texel), alphaColumn(texel)) == texel);
  }
}

TEST_CASE("defect names are stable enough to print", "[alpha]")
{
  CHECK(alphaDefectName(AlphaDefect::OverOpaque) == "over-opaque");
  CHECK(alphaDefectName(AlphaDefect::EmptyLayer) == "empty layer");
  CHECK(alphaDefectName(AlphaDefect::BorderMismatch) == "border mismatch");
}

TEST_CASE("the scan roll-up counts chunks and defects separately", "[alpha]")
{
  AlphaScanResult result;

  AlphaIntegrityReport clean;
  clean.layer_count = 2;

  AlphaIntegrityReport over;
  over.layer_count = 3;
  over.over_opaque_texels = 12;
  over.max_excess = 40;

  AlphaIntegrityReport both;
  both.layer_count = 3;
  both.over_opaque_texels = 5;
  both.max_excess = 9;
  both.empty_layers = {2};

  result.addChunk({1, 2, 0, 0}, clean);
  result.addChunk({1, 2, 0, 1}, over);
  result.addChunk({1, 2, 0, 2}, both);

  CHECK(result.chunksScanned() == 3);
  CHECK(result.chunksWithDefects() == 2);
  CHECK(result.chunksWith(AlphaDefect::OverOpaque) == 2);
  CHECK(result.chunksWith(AlphaDefect::EmptyLayer) == 1);
  CHECK(result.chunksWith(AlphaDefect::BorderMismatch) == 0);
  CHECK(result.overOpaqueTexels() == 17);
  CHECK(result.emptyLayerCount() == 1);
  CHECK(result.maxExcess() == 40);
  CHECK_FALSE(result.clean());

  REQUIRE(result.offenders().size() == 2);
  CHECK(result.offenders().front().label() == "ADT 1,2 chunk 0,1");
  CHECK(result.offendersOmitted() == 0);
  CHECK(contains(result.toReport(), "ADT 1,2 chunk 0,2"));
}

TEST_CASE("the offender list is capped and the overflow is counted, through merges too", "[alpha]")
{
  AlphaIntegrityReport broken;
  broken.layer_count = 1;
  broken.over_opaque_texels = 1;
  broken.max_excess = 1;

  AlphaScanResult first;
  AlphaScanResult second;

  for (int i = 0; i < 5; ++i)
  {
    first.addChunk({0, 0, i, 0}, broken, 3);
    second.addChunk({1, 1, i, 0}, broken, 3);
  }

  CHECK(first.offenders().size() == 3);
  CHECK(first.offendersOmitted() == 2);

  first.merge(second, 3);

  CHECK(first.chunksScanned() == 10);
  CHECK(first.chunksWithDefects() == 10);
  CHECK(first.overOpaqueTexels() == 10);
  // Cap already reached, so everything the other result carried is counted as omitted: its own 2
  // plus the 3 it had kept.
  CHECK(first.offenders().size() == 3);
  CHECK(first.offendersOmitted() == 7);
  CHECK(contains(first.toReport(), "and 7 more"));
}

TEST_CASE("a scan of clean chunks says so without listing anything", "[alpha]")
{
  AlphaScanResult result;
  AlphaIntegrityReport clean;
  clean.layer_count = 3;

  result.addChunk({0, 0, 0, 0}, clean);
  result.addChunk({0, 0, 0, 1}, clean);

  CHECK(result.clean());
  CHECK(result.chunksScanned() == 2);
  CHECK(result.offenders().empty());
  CHECK(contains(result.summary(), "no alpha map defects"));
  CHECK_FALSE(contains(result.toReport(), "ADT"));
}
