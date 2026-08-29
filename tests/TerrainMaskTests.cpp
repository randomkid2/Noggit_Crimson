// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the persistent-mask feature's pure half.
//
// This is why TerrainMask.cpp and TerrainMaskFilters.cpp hold no Qt, no glm and no MapChunk:
// everything here runs on a bare machine. A mask is a sparse byte field, terrain is a 129x129 grid
// of floats, and what gets exercised is the part that decides anything -- the sparse storage's
// collapse rule, the sign of curvature, the fail-open direction of every unanswerable query, and
// the memory arithmetic the whole design is justified by.
//
// The properties asserted are the ones that break silently. A mask that quietly materialises every
// chunk it touches still produces correct pictures, and only shows up as a four-gigabyte process an
// hour later. A curvature filter with the sign inverted produces a perfectly plausible mask that
// puts gravel on the ridges and lichen in the streambeds, and nobody notices until the map is
// textured.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <noggit/terrain/TerrainMask.hpp>
#include <noggit/terrain/TerrainMaskFilters.hpp>
#include <noggit/terrain/TerrainRules.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Noggit;

namespace
{
  MaskChunkAddress addressOf(int tile_x, int tile_z, int chunk_x, int chunk_z)
  {
    MaskChunkAddress address;
    address.tile_x = tile_x;
    address.tile_z = tile_z;
    address.chunk_x = chunk_x;
    address.chunk_z = chunk_z;
    return address;
  }

  // The overhead term TerrainMask::bytes() adds per stored chunk, recomputed here from the same
  // rule so the assertions below are checking the accounting rather than restating a magic number.
  // Block is private, so this reconstructs its layout: one byte plus one owning pointer.
  struct BlockLayoutProxy
  {
    std::uint8_t uniform;
    void* texels;
  };

  std::size_t entryOverhead()
  {
    return sizeof(std::pair<std::uint32_t const, BlockLayoutProxy>) + 3 * sizeof(void*);
  }

  // A smooth dome centred on one tile, used wherever a test needs terrain with a genuine second
  // derivative. Parabolic, so its curvature is analytic: for h = peak * (1 - (r/radius)^2) the
  // Laplacian is a constant -4 * peak / radius^2 everywhere inside the dome.
  void fillDome(MaskTileHeightField& field, float peak, float radius)
  {
    float const centre = 0.5f * static_cast<float>(MASK_TILE_VERTEX_SIDE - 1) * MASK_UNIT_SIZE;

    for (int z = 0; z < MASK_TILE_VERTEX_SIDE; ++z)
    {
      for (int x = 0; x < MASK_TILE_VERTEX_SIDE; ++x)
      {
        float const wx = static_cast<float>(x) * MASK_UNIT_SIZE - centre;
        float const wz = static_cast<float>(z) * MASK_UNIT_SIZE - centre;
        float const r2 = wx * wx + wz * wz;

        field.setHeight(x, z, peak * (1.0f - r2 / (radius * radius)));
      }
    }
  }

  void fillPlane(MaskTileHeightField& field, float height)
  {
    for (int z = 0; z < MASK_TILE_VERTEX_SIDE; ++z)
    {
      for (int x = 0; x < MASK_TILE_VERTEX_SIDE; ++x)
      {
        field.setHeight(x, z, height);
      }
    }
  }

  // A constant-gradient ramp. Its curvature is exactly zero everywhere, which is the property that
  // separates curvature from slope and the reason curvature is worth having as a separate filter.
  void fillRamp(MaskTileHeightField& field, float slope_per_yard)
  {
    for (int z = 0; z < MASK_TILE_VERTEX_SIDE; ++z)
    {
      for (int x = 0; x < MASK_TILE_VERTEX_SIDE; ++x)
      {
        field.setHeight(x, z, static_cast<float>(x) * MASK_UNIT_SIZE * slope_per_yard);
      }
    }
  }
}

TEST_CASE("restated geometry constants match their derivations", "[mask]")
{
  // TerrainMask.hpp restates TILESIZE, CHUNKSIZE and TEXDETAILSIZE rather than including
  // MapHeaders.h, which pulls in glm and would keep this file off a bare machine. That restatement
  // is only safe while something checks it, which is this.
  CHECK(MASK_TILE_SIZE == Catch::Approx(533.33333f));
  CHECK(MASK_CHUNK_SIZE == Catch::Approx(533.33333f / 16.0f));
  CHECK(MASK_TEXEL_SIZE == Catch::Approx(533.33333f / 16.0f / 64.0f));
  CHECK(MASK_UNIT_SIZE == Catch::Approx(533.33333f / 16.0f / 8.0f));

  CHECK(MASK_CHUNK_TEXELS == 4096);
  CHECK(MASK_TILE_VERTEX_SIDE == 129);

  // The mask texel is the alphamap texel. If this ever stops holding, the "the clip is exactly as
  // sharp as the paint" argument in TerrainMask.hpp stops holding with it.
  CHECK(MASK_CHUNK_SIDE == 64);

  // The vertex index formula is restated a SECOND time in TerrainMaskFilters.hpp, after
  // TerrainRules.hpp already restated it from MapChunk.cpp:356. Two copies of a formula is one too
  // many to leave unchecked.
  for (int row = 0; row < 9; ++row)
  {
    for (int col = 0; col < 9; ++col)
    {
      CHECK( MaskFieldCollector::chunkOuterVertexIndex(row, col)
          == TerrainRuleCollector::chunkOuterVertexIndex(row, col));
    }
  }
}

TEST_CASE("MEASURED memory cost of the mask field", "[mask][memory]")
{
  // These are the numbers the design is justified by, computed rather than estimated.
  constexpr std::size_t dense_chunk_payload = static_cast<std::size_t>(MASK_CHUNK_TEXELS);
  constexpr std::size_t chunks_per_tile
    = static_cast<std::size_t>(MASK_TILE_CHUNK_SIDE) * MASK_TILE_CHUNK_SIDE;
  constexpr std::size_t tiles_per_map
    = static_cast<std::size_t>(MASK_MAP_TILE_SIDE) * MASK_MAP_TILE_SIDE;

  CHECK(dense_chunk_payload == 4096u);                                   // 4.000 KiB
  CHECK(chunks_per_tile == 256u);
  CHECK(dense_chunk_payload * chunks_per_tile == 1048576u);              // 1.000 MiB per tile
  CHECK(dense_chunk_payload * chunks_per_tile * tiles_per_map == 4294967296u); // 4.000 GiB per map

  // A fresh mask costs nothing. Absence is zero, so "no mask painted yet" allocates no blocks.
  TerrainMask mask;
  CHECK(mask.empty());
  CHECK(mask.bytes() == 0u);

  // A uniform chunk costs the map entry and NO payload. This is the property that makes a derived
  // mask over a mostly-flat continent affordable.
  MaskChunkAddress const address = addressOf(10, 20, 3, 4);
  mask.fillChunk(address, 255);

  CHECK(mask.chunkCount() == 1u);
  CHECK(mask.uniformChunkCount() == 1u);
  CHECK(mask.denseChunkCount() == 0u);
  CHECK(mask.bytes() == entryOverhead());

  // Writing the value it already holds must NOT materialise it, or a stroke dragged back and forth
  // over saturated ground would turn every chunk it crosses into 4 KiB.
  mask.setTexel(address, 5, 5, 255);
  CHECK(mask.denseChunkCount() == 0u);
  CHECK(mask.bytes() == entryOverhead());

  // A single differing texel does materialise it.
  mask.setTexel(address, 5, 5, 128);
  CHECK(mask.denseChunkCount() == 1u);
  CHECK(mask.bytes() == entryOverhead() + dense_chunk_payload);

  // ...and writing it back collapses it again, because writeChunk collapses on write. Without the
  // collapse a mask could only ever grow.
  std::vector<std::uint8_t> block(static_cast<std::size_t>(MASK_CHUNK_TEXELS), 255u);
  mask.writeChunk(address, block.data());
  CHECK(mask.denseChunkCount() == 0u);
  CHECK(mask.bytes() == entryOverhead());
}

TEST_CASE("a derived slope mask is mostly uniform, which is what makes it affordable", "[mask][memory]")
{
  // The claim in TerrainMask.hpp is that derived masks are overwhelmingly uniform because only the
  // chunks straddling the filter's threshold carry mixed values. This measures it on a real dome
  // rather than asserting it.
  MaskTileHeightField field;
  fillDome(field, 300.0f, 400.0f);

  MaskFilterStack stack;
  MaskFilterLayer layer;
  layer.kind = MaskFilterKind::Slope;
  layer.range = MaskRange::atLeast(35.0f);
  stack.layers().push_back(layer);

  TerrainMask mask;
  std::vector<std::uint8_t> block(static_cast<std::size_t>(MASK_CHUNK_TEXELS), 0u);

  for (int chunk_z = 0; chunk_z < MASK_TILE_CHUNK_SIDE; ++chunk_z)
  {
    for (int chunk_x = 0; chunk_x < MASK_TILE_CHUNK_SIDE; ++chunk_x)
    {
      MaskChunkAddress const address = addressOf(0, 0, chunk_x, chunk_z);
      REQUIRE(bakeMaskChunk(stack, field, MaskChunkInputs{}, address, block.data()));
      mask.writeChunk(address, block.data());
    }
  }

  CHECK(mask.chunkCount() == 256u);

  // The interesting figure. A tile stored densely would be 1'048'576 bytes of payload; this asserts
  // the sparse form is a small fraction of it. The exact split depends on where the 35-degree
  // contour of the dome falls, so the bound is loose on purpose -- what matters is that most chunks
  // pay nothing, not the precise count.
  CHECK(mask.uniformChunkCount() > 128u);
  CHECK(mask.bytes() < 256u * 4096u / 2u);

  INFO("dense chunks: " << mask.denseChunkCount()
       << "  uniform chunks: " << mask.uniformChunkCount()
       << "  bytes: " << mask.bytes()
       << "  dense-equivalent: " << (256u * 4096u));
  CHECK(mask.denseChunkCount() < 128u);
}

TEST_CASE("chunk addressing round-trips and rejects what it cannot represent", "[mask]")
{
  MaskChunkAddress const address = addressOf(63, 0, 15, 7);
  CHECK(MaskChunkAddress::fromPacked(address.packed()) == address);

  // packed() is tile-major, which is what makes storedChunks() deterministic and releaseTile's
  // key range contiguous.
  CHECK(addressOf(1, 0, 0, 0).packed() > addressOf(0, 63, 15, 15).packed());

  SECTION("world positions decompose correctly")
  {
    // The centre of tile 0, chunk 0.
    MaskChunkAddress const a = MaskChunkAddress::fromWorld(MASK_CHUNK_SIZE * 0.5f, MASK_CHUNK_SIZE * 0.5f);
    CHECK(a.valid());
    CHECK(a.tile_x == 0);
    CHECK(a.chunk_x == 0);

    // One chunk east.
    MaskChunkAddress const b = MaskChunkAddress::fromWorld(MASK_CHUNK_SIZE * 1.5f, MASK_CHUNK_SIZE * 0.5f);
    CHECK(b.chunk_x == 1);

    // Second tile.
    MaskChunkAddress const c = MaskChunkAddress::fromWorld(MASK_TILE_SIZE * 1.5f, MASK_TILE_SIZE * 0.5f);
    CHECK(c.tile_x == 1);
    CHECK(c.tile_z == 0);
  }

  SECTION("off the map and non-finite are rejected rather than folded")
  {
    // A truncating cast would fold this into tile 0 and produce a valid() address that clips a
    // brush against the wrong chunk. std::floor does not.
    CHECK_FALSE(MaskChunkAddress::fromWorld(-3.0f, 10.0f).valid());
    CHECK_FALSE(MaskChunkAddress::fromWorld(10.0f, -0.001f).valid());
    CHECK_FALSE(MaskChunkAddress::fromWorld(MASK_TILE_SIZE * 64.0f + 1.0f, 10.0f).valid());

    float const nan_value = std::nanf("");
    CHECK_FALSE(MaskChunkAddress::fromWorld(nan_value, 10.0f).valid());
    CHECK_FALSE(MaskChunkAddress::fromWorld(10.0f, nan_value).valid());
  }
}

TEST_CASE("combinators saturate instead of wrapping", "[mask]")
{
  // Wrapping would turn "subtract slightly too much" into "fully masked in", which is the silent
  // catastrophe this saturation exists to prevent.
  CHECK(maskCombine(MaskCombine::Add, 200, 100) == 255);
  CHECK(maskCombine(MaskCombine::Subtract, 50, 100) == 0);

  // Multiply by full strength is an EXACT identity. Truncating division would make it 253 for an
  // input of 254, so a stack of six full-strength Multiply layers would visibly fade a mask the
  // user believes is untouched.
  for (int v = 0; v <= 255; ++v)
  {
    CHECK(maskCombine(MaskCombine::Multiply, static_cast<std::uint8_t>(v), 255) == v);
  }

  CHECK(maskCombine(MaskCombine::Multiply, 255, 0) == 0);
  CHECK(maskCombine(MaskCombine::Min, 40, 200) == 40);
  CHECK(maskCombine(MaskCombine::Max, 40, 200) == 200);
  CHECK(maskCombine(MaskCombine::Replace, 40, 200) == 200);

  // Names round-trip, and an unknown name reads as Replace rather than as an empty mask.
  CHECK(maskCombineFromName(maskCombineName(MaskCombine::Multiply)) == MaskCombine::Multiply);
  CHECK(maskCombineFromName("nonsense") == MaskCombine::Replace);
}

TEST_CASE("invert covers the chunks the mask never stored", "[mask]")
{
  // The property that makes invert() a flag rather than a pass over the data. Complementing "the
  // cliffs" has to yield "everything that is not the cliffs", including the thousands of chunks the
  // cliff mask never allocated. A pass over stored texels would leave those reading zero and the
  // inverted mask would clip everything away.
  TerrainMask mask;
  MaskChunkAddress const stored = addressOf(5, 5, 0, 0);
  MaskChunkAddress const never_stored = addressOf(9, 9, 3, 3);

  mask.fillChunk(stored, 255);

  CHECK(mask.texelAt(stored, 0, 0) == 255);
  CHECK(mask.texelAt(never_stored, 0, 0) == 0);

  mask.invert();

  CHECK(mask.inverted());
  CHECK(mask.texelAt(stored, 0, 0) == 0);
  CHECK(mask.texelAt(never_stored, 0, 0) == 255);

  // ...and it costs nothing.
  CHECK(mask.chunkCount() == 1u);
}

TEST_CASE("valueAt fails open on what it cannot answer", "[mask]")
{
  TerrainMask mask;

  // Non-finite returns 1.0 -- fully unmasked -- never 0.0. A query that cannot be answered must not
  // be the thing that silently disables a brush the user is holding down.
  CHECK(mask.valueAt(std::nanf(""), 100.0f) == Catch::Approx(1.0f));

  // Inside the map with nothing stored reads as absent, which is 0 for a non-inverted mask.
  CHECK(mask.valueAt(1000.0f, 1000.0f) == Catch::Approx(0.0f));

  // Off the map reads like an absent chunk, not like a hard zero, so an inverted mask does not grow
  // a seam at the map edge.
  mask.invert();
  CHECK(mask.valueAt(-5.0f, -5.0f) == Catch::Approx(1.0f));
}

TEST_CASE("copying a mask deep-copies its blocks", "[mask]")
{
  // The implicit copy constructor is ill-formed here because the payload is held by unique_ptr, and
  // the compiler reports that as a deleted function inside unordered_map rather than as anything to
  // do with this class. TerrainMaskStore keeps masks in a vector, so this has to work.
  TerrainMask original;
  MaskChunkAddress const address = addressOf(1, 1, 1, 1);

  original.fillChunk(address, 100);
  original.setTexel(address, 10, 10, 200);
  REQUIRE(original.chunkIsDense(address));

  TerrainMask copy (original);
  CHECK(copy.texelAt(address, 10, 10) == 200);
  CHECK(copy.texelAt(address, 0, 0) == 100);

  // Independent storage: writing through one must not be visible through the other.
  copy.setTexel(address, 10, 10, 5);
  CHECK(original.texelAt(address, 10, 10) == 200);
  CHECK(copy.texelAt(address, 10, 10) == 5);
}

TEST_CASE("releaseTile drops one tile and leaves the rest", "[mask]")
{
  // This is what bounds a composited mask to the resident tile set. Without it the field grows for
  // as long as the camera keeps moving.
  TerrainMask mask;
  mask.fillChunk(addressOf(3, 4, 0, 0), 255);
  mask.fillChunk(addressOf(3, 4, 15, 15), 255);
  mask.fillChunk(addressOf(3, 5, 0, 0), 255);

  REQUIRE(mask.chunkCount() == 3u);

  mask.releaseTile(3, 4);

  CHECK(mask.chunkCount() == 1u);
  CHECK(mask.texelAt(addressOf(3, 5, 0, 0), 0, 0) == 255);
}

TEST_CASE("painting feathers from the hard edge outward", "[mask]")
{
  TerrainMask mask;

  float const centre = MASK_TILE_SIZE * 0.5f;
  float const radius = 20.0f;

  std::size_t const touched = mask.paintCircle(centre, centre, radius, 0.5f, 1.0f, MaskCombine::Max);
  CHECK(touched > 0u);

  // Full strength at the centre, nothing outside the radius, something in between on the shoulder.
  CHECK(mask.valueAt(centre, centre) == Catch::Approx(1.0f).margin(0.02f));
  CHECK(mask.valueAt(centre + radius * 2.0f, centre) == Catch::Approx(0.0f));

  float const shoulder = mask.valueAt(centre + radius * 0.75f, centre);
  CHECK(shoulder > 0.05f);
  CHECK(shoulder < 0.95f);
}

TEST_CASE("erasing bites at the centre and leaves the rim alone", "[mask]")
{
  // REGRESSION. The obvious way to write an erase stroke -- paint the falloff and fold it with Min
  // -- does the opposite of what it looks like. The rim's falloff tends to 0, so Min drives the rim
  // to 0 and the brush erases hardest exactly where it should touch least, producing a hard-edged
  // hole slightly wider than the cursor. paintCircle's `complement` flag is the fix, and this is
  // what keeps it fixed.
  TerrainMask mask;

  float const centre = MASK_TILE_SIZE * 0.5f;
  float const radius = 30.0f;

  // Start fully masked in across the chunks the stroke will cross.
  for (int chunk_z = 0; chunk_z < MASK_TILE_CHUNK_SIDE; ++chunk_z)
  {
    for (int chunk_x = 0; chunk_x < MASK_TILE_CHUNK_SIDE; ++chunk_x)
    {
      mask.fillChunk(addressOf(0, 0, chunk_x, chunk_z), 255);
    }
  }

  REQUIRE(mask.valueAt(centre, centre) == Catch::Approx(1.0f));

  // Hardness 0.5, so the inner half of the radius is fully weighted. At hardness 0 the falloff is
  // linear all the way to the centre, and valueAt's bilinear filter reads four texel CENTRES that
  // sit up to half a texel off the stroke centre -- which leaves a residue of exactly 3/255 there.
  // That is the filter working correctly, not the stroke failing to erase, but it makes for a test
  // whose margin looks arbitrary. A hardness the user would actually pick avoids the question.
  mask.paintCircle(centre, centre, radius, 0.5f, 1.0f, MaskCombine::Min, true);

  // Fully erased at the centre.
  CHECK(mask.valueAt(centre, centre) == Catch::Approx(0.0f).margin(0.005f));

  // Barely touched just inside the rim -- this is the assertion that fails without `complement`.
  CHECK(mask.valueAt(centre + radius * 0.95f, centre) > 0.8f);

  // Completely untouched outside the radius.
  CHECK(mask.valueAt(centre + radius * 1.5f, centre) == Catch::Approx(1.0f));
}

TEST_CASE("range weights fail closed", "[mask][filters]")
{
  MaskRange const range = MaskRange::between(10.0f, 20.0f, 5.0f);

  CHECK(range.weight(15.0f) == Catch::Approx(1.0f));
  CHECK(range.weight(10.0f) == Catch::Approx(1.0f));
  CHECK(range.weight(7.5f) == Catch::Approx(0.5f));
  CHECK(range.weight(4.0f) == Catch::Approx(0.0f));
  CHECK(range.weight(22.5f) == Catch::Approx(0.5f));

  // A NaN sample matches nothing. Failing closed is the right direction: a filter whose input could
  // not be computed must contribute nothing rather than everything.
  CHECK(range.weight(std::nanf("")) == Catch::Approx(0.0f));

  // An inverted interval matches nothing rather than being silently repaired. Swapping the
  // endpoints would turn a typo in two spin boxes into a mask over half the continent.
  MaskRange inverted;
  inverted.low = 50.0f;
  inverted.high = 10.0f;
  CHECK(inverted.inverted());
  CHECK(inverted.weight(30.0f) == Catch::Approx(0.0f));

  // Unbounded is unbounded in both directions.
  CHECK_FALSE(MaskRange::any().bounded());
  CHECK(MaskRange::atLeast(1.0f).bounded());
}

TEST_CASE("CURVATURE sign convention: positive is concave", "[mask][filters][curvature]")
{
  // The single most important assertion in this file. The sign is invisible until a continent has
  // been textured backwards, with gravel on the ridges and lichen in the streambeds.

  SECTION("a plane has zero curvature")
  {
    MaskTileHeightField field;
    fillPlane(field, 100.0f);
    CHECK(field.curvature(64, 64, 1) == Catch::Approx(0.0f).margin(1.0e-6f));
  }

  SECTION("a constant slope has zero curvature, which is what separates it from slope")
  {
    // A ramp is steep everywhere and curved nowhere. This is the whole reason curvature is a filter
    // the existing slope-and-height rule engine cannot express.
    MaskTileHeightField field;
    fillRamp(field, 0.5f);

    CHECK(field.curvature(64, 64, 1) == Catch::Approx(0.0f).margin(1.0e-5f));
    CHECK(field.slopeDegrees(64, 64) == Catch::Approx(26.565f).margin(0.01f));
  }

  SECTION("a dome is CONVEX, so its curvature is NEGATIVE")
  {
    // h = peak * (1 - r^2 / radius^2) has Laplacian -4 * peak / radius^2 everywhere.
    float const peak = 300.0f;
    float const radius = 400.0f;

    MaskTileHeightField field;
    fillDome(field, peak, radius);

    float const expected = -4.0f * peak / (radius * radius);

    // The margins are the COMPUTED float32 noise floor of a second difference, not a tolerance
    // picked until the test passed. A five-point stencil has coefficients summing to 8 in absolute
    // value and each height carries up to half an ulp, so the numerator's worst-case error is
    // 8 * 0.5 * eps * |h| and the division by spacing^2 shrinks it quadratically with the step:
    //
    //   step 1, spacing  4.167 yd -> 8.24e-06 /yd
    //   step 4, spacing 16.667 yd -> 5.15e-07 /yd
    //
    // The step-1 value measured here is -0.00749531 against an exact -0.0075, an error of
    // 4.69e-06 -- comfortably inside the bound and nowhere near the 0.006-to-0.25 /yd range that
    // real landforms occupy. See the precision note on MaskTileHeightField::curvature.
    float const floor_step_1 = 8.24e-6f;
    float const floor_step_4 = 5.15e-7f;

    CHECK(field.curvature(64, 64, 1) < 0.0f);
    CHECK(field.curvature(64, 64, 1) == Catch::Approx(expected).margin(floor_step_1));

    // Scale independence: for a quadratic the exact Laplacian is the same at every stencil radius,
    // which is a good check that the spacing division is right. The wider stencil is also the more
    // accurate one, hence the tighter margin.
    CHECK(field.curvature(64, 64, 4) == Catch::Approx(expected).margin(floor_step_4));
  }

  SECTION("a bowl is CONCAVE, so its curvature is POSITIVE -- the drainage-line case")
  {
    MaskTileHeightField field;
    fillDome(field, -300.0f, 400.0f);

    CHECK(field.curvature(64, 64, 1) > 0.0f);
    CHECK(field.curvature(64, 64, 1)
       == Catch::Approx(4.0f * 300.0f / (400.0f * 400.0f)).margin(8.24e-6f));
  }
}

TEST_CASE("slope from the height field agrees with the rule engine's slope", "[mask][filters]")
{
  // Two modules now compute slope. They have to agree, or a mask clipping an auto-texture rule
  // would disagree with the rule about where the cliff is.
  MaskTileHeightField field;
  float const gradient = 0.75f;
  fillRamp(field, gradient);

  float const from_field = field.slopeDegrees(64, 64);
  float const from_rules = slopeDegreesFromGradient(gradient, 0.0f);

  CHECK(from_field == Catch::Approx(from_rules).margin(0.001f));
}

TEST_CASE("noise is deterministic, bounded and seed-dependent", "[mask][filters]")
{
  float const a = maskValueNoise(123.0f, 456.0f, 64.0f, 3, 0.5f, 7u);
  float const b = maskValueNoise(123.0f, 456.0f, 64.0f, 3, 0.5f, 7u);

  // Determinism is what makes saving the STACK rather than the texels a viable persistence design:
  // a rebake on another machine has to reproduce the same field.
  CHECK(a == b);
  CHECK(a >= 0.0f);
  CHECK(a <= 1.0f);

  CHECK(maskValueNoise(123.0f, 456.0f, 64.0f, 3, 0.5f, 8u) != a);

  // A non-positive wavelength cannot be sampled and returns 0 rather than dividing by zero.
  CHECK(maskValueNoise(1.0f, 1.0f, 0.0f, 3, 0.5f, 1u) == Catch::Approx(0.0f));
}

TEST_CASE("field filters pass their value through, test filters do not", "[mask][filters]")
{
  MaskFilterSample sample;
  sample.slope_degrees = 45.0f;
  sample.layer_alpha = 0.25f;

  // A slope layer with no range does not constrain slope, so it weighs 1.
  MaskFilterLayer slope_layer;
  slope_layer.kind = MaskFilterKind::Slope;
  CHECK(evaluateMaskLayer(slope_layer, sample) == Catch::Approx(1.0f));

  // A layer-alpha layer with no range passes the alpha through. Returning 1 would discard the only
  // thing the layer was added to read.
  MaskFilterLayer alpha_layer;
  alpha_layer.kind = MaskFilterKind::LayerAlpha;
  CHECK(evaluateMaskLayer(alpha_layer, sample) == Catch::Approx(0.25f));

  // Giving it a range turns it back into a test.
  alpha_layer.range = MaskRange::atLeast(0.5f);
  CHECK(evaluateMaskLayer(alpha_layer, sample) == Catch::Approx(0.0f));

  // Per-layer invert is the local complement.
  slope_layer.invert = true;
  CHECK(evaluateMaskLayer(slope_layer, sample) == Catch::Approx(0.0f));
}

TEST_CASE("area filters fail closed on an empty list", "[mask][filters]")
{
  MaskFilterSample sample;
  sample.area_id = 12;

  MaskFilterLayer layer;
  layer.kind = MaskFilterKind::AreaId;

  // Unconfigured selects nothing, not everything.
  CHECK(evaluateMaskLayer(layer, sample) == Catch::Approx(0.0f));

  layer.area_ids.push_back(12);
  CHECK(evaluateMaskLayer(layer, sample) == Catch::Approx(1.0f));

  sample.area_id = 13;
  CHECK(evaluateMaskLayer(layer, sample) == Catch::Approx(0.0f));
}

TEST_CASE("the first enabled layer replaces whatever its combinator says", "[mask][filters]")
{
  // Opening a stack with Multiply would otherwise yield an empty mask -- the commonest way to build
  // a stack that looks broken and cannot be debugged from its output.
  MaskFilterStack stack;

  MaskFilterLayer layer;
  layer.kind = MaskFilterKind::Constant;
  layer.constant = 1.0f;
  layer.combine = MaskCombine::Multiply;
  stack.layers().push_back(layer);

  MaskFilterSample sample;
  CHECK(stack.evaluate(sample) == 255);

  // ...and validate() says so, rather than leaving the user to work it out.
  bool reported = false;

  for (std::string const& problem : stack.validate())
  {
    reported = reported || problem.find("first enabled layer") != std::string::npos;
  }

  CHECK(reported);

  SECTION("a disabled layer is skipped entirely but keeps its numbers")
  {
    stack.layers()[0].enabled = false;
    CHECK(stack.enabledCount() == 0u);
    CHECK(stack.layers()[0].constant == Catch::Approx(1.0f));
  }
}

TEST_CASE("the stack reports which inputs a bake has to produce", "[mask][filters]")
{
  MaskFilterStack stack;

  MaskFilterLayer height_layer;
  height_layer.kind = MaskFilterKind::Height;
  stack.layers().push_back(height_layer);

  // A stack of height layers needs no slope grid and no curvature grid; asking saves 130 KiB of
  // scratch and two passes over 16'641 vertices per tile.
  CHECK(stack.needsHeightField());
  CHECK_FALSE(stack.needsSlope());
  CHECK_FALSE(stack.needsCurvature());
  CHECK(stack.maxCurvatureStep() == 0);

  MaskFilterLayer curvature_layer;
  curvature_layer.kind = MaskFilterKind::Curvature;
  curvature_layer.curvature_step = 3;
  stack.layers().push_back(curvature_layer);

  CHECK(stack.needsCurvature());
  CHECK(stack.maxCurvatureStep() == 3);

  SECTION("mixed curvature scales are reported, because the cost is invisible from the dialog")
  {
    MaskFilterLayer second;
    second.kind = MaskFilterKind::Curvature;
    second.curvature_step = 1;
    stack.layers().push_back(second);

    bool reported = false;

    for (std::string const& problem : stack.validate())
    {
      reported = reported || problem.find("different scales") != std::string::npos;
    }

    CHECK(reported);
  }
}

TEST_CASE("a stack measuring two curvature scales sees two different values", "[mask][filters][curvature]")
{
  // evaluateWith resolves curvature PER LAYER rather than once per sample, because curvature is
  // scale-dependent and "the broad valley floor" and "the gullies inside it" are different filters
  // over the same terrain. This asserts the callback actually reaches each layer.
  MaskFilterStack stack;

  MaskFilterLayer broad;
  broad.kind = MaskFilterKind::Curvature;
  broad.curvature_step = 4;
  broad.range = MaskRange::atLeast(0.5f);
  stack.layers().push_back(broad);

  MaskFilterLayer fine;
  fine.kind = MaskFilterKind::Curvature;
  fine.curvature_step = 1;
  fine.range = MaskRange::atLeast(0.5f);
  fine.combine = MaskCombine::Min;
  stack.layers().push_back(fine);

  MaskFilterSample sample;

  // Curvature 1.0 at the coarse scale and 0.0 at the fine one: the first layer passes, the second
  // fails, and Min gives 0. If the callback were ignored both layers would see the same value.
  std::uint8_t const result = stack.evaluateWith
    (sample, [] (int step) -> float { return step >= 4 ? 1.0f : 0.0f; });

  CHECK(result == 0);

  // Reversing which scale is curved reverses the answer, proving both branches are reached.
  std::uint8_t const flipped = stack.evaluateWith
    (sample, [] (int step) -> float { return step >= 4 ? 0.0f : 1.0f; });

  CHECK(flipped == 0);

  // Both curved: both pass, Min keeps 255.
  std::uint8_t const both = stack.evaluateWith(sample, [] (int) -> float { return 1.0f; });
  CHECK(both == 255);
}

TEST_CASE("baking refuses rather than deriving a mask from a plane at zero", "[mask][filters]")
{
  // An unfilled field produces slope 0 and curvature 0 everywhere, so a "slope above 40" stack
  // would bake a confidently empty mask and a "slope below 10" stack a confidently full one. Both
  // look like answers, which is why this refuses instead.
  MaskFilterStack stack;

  MaskFilterLayer layer;
  layer.kind = MaskFilterKind::Slope;
  layer.range = MaskRange::atLeast(40.0f);
  stack.layers().push_back(layer);

  MaskTileHeightField empty_field;
  CHECK_FALSE(empty_field.filled());

  std::vector<std::uint8_t> block(static_cast<std::size_t>(MASK_CHUNK_TEXELS), 7u);

  CHECK_FALSE(bakeMaskChunk(stack, empty_field, MaskChunkInputs{}, addressOf(0, 0, 0, 0), block.data()));

  // Nothing was written, so the caller's buffer is untouched rather than half-filled.
  CHECK(block[0] == 7u);

  // An invalid address is refused too.
  MaskTileHeightField field;
  fillDome(field, 100.0f, 400.0f);
  CHECK_FALSE(bakeMaskChunk(stack, field, MaskChunkInputs{}, addressOf(-1, 0, 0, 0), block.data()));

  // A filled field bakes.
  CHECK(bakeMaskChunk(stack, field, MaskChunkInputs{}, addressOf(0, 0, 0, 0), block.data()));
}

TEST_CASE("a curvature mask separates a drainage line from the shoulder above it", "[mask][curvature]")
{
  // The end-to-end case the feature exists for, and the thing the slope-and-height rule engine
  // cannot express: a valley floor and the convex shoulder beside it can have the SAME slope and
  // the SAME height, and differ only in curvature.
  MaskTileHeightField field;

  // A trough running along Z: concave at the bottom, convex at the lips.
  float const centre_x = 0.5f * static_cast<float>(MASK_TILE_VERTEX_SIDE - 1) * MASK_UNIT_SIZE;

  for (int z = 0; z < MASK_TILE_VERTEX_SIDE; ++z)
  {
    for (int x = 0; x < MASK_TILE_VERTEX_SIDE; ++x)
    {
      float const wx = static_cast<float>(x) * MASK_UNIT_SIZE - centre_x;

      // A smooth trough: cosine-shaped, so the bottom is concave and the two lips are convex.
      field.setHeight(x, z, 30.0f * std::cos(wx * 0.02f));
    }
  }

  int const centre_vertex = (MASK_TILE_VERTEX_SIDE - 1) / 2;

  // The trough bottom sits at the cosine's maximum, which is a CREST in this parameterisation, so
  // it is convex -- negative curvature.
  CHECK(field.curvature(centre_vertex, centre_vertex, 2) < 0.0f);

  // A quarter-wavelength away the cosine is at its minimum, which is a hollow: concave, positive.
  // Half a wavelength east of the crest is the trough. 3.14159 / 0.02 = 157.08 yards, which is 37
  // vertices at 4.167 yards each, so vertex 64 + 37 = 101 -- inside the 129-vertex tile. Asserted
  // rather than guarded by an `if`, because a guard that is always true silently turns the rest of
  // this test into dead code the day the geometry changes.
  int const trough_offset = static_cast<int>((3.14159265f / 0.02f) / MASK_UNIT_SIZE);
  int const trough_vertex = centre_vertex + trough_offset;

  REQUIRE(trough_offset == 37);
  REQUIRE(trough_vertex < MASK_TILE_VERTEX_SIDE);

  CHECK(field.curvature(trough_vertex, centre_vertex, 2) > 0.0f);

  // ...and a concave-only filter separates them, which is the capability being added.
  MaskFilterLayer concave;
  concave.kind = MaskFilterKind::Curvature;
  concave.curvature_step = 2;
  concave.range = MaskRange::atLeast(0.0001f);

  MaskFilterSample crest;
  crest.curvature = field.curvature(centre_vertex, centre_vertex, 2);

  MaskFilterSample hollow;
  hollow.curvature = field.curvature(trough_vertex, centre_vertex, 2);

  CHECK(evaluateMaskLayer(concave, crest) == Catch::Approx(0.0f));
  CHECK(evaluateMaskLayer(concave, hollow) == Catch::Approx(1.0f));
}
