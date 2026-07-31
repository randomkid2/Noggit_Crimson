// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the missing-asset scanner's pure half.
//
// This is the reason AssetScan.cpp holds no World, no Model and no ClientData: everything here
// runs on a machine with no client install, no MPQ chain and no Qt. The existence probe is a
// std::function, so "the file is missing" is a table lookup rather than a filesystem state, and
// the aggregation -- dedup, per-kind counts, referrer capping, ordering, merging -- is what gets
// exercised.
//
// What is NOT covered here, by construction: AssetScanCollector, which walks the world. It is a
// set of templates instantiated against World/MapTile/Model/WMO and cannot be reached without a
// loaded map.

#include <catch2/catch_test_macros.hpp>

#include <noggit/AssetScan.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace Noggit;

namespace
{
  // Existence probe backed by a set of normalised keys, counting the calls it receives so a test
  // can assert that memoisation happened rather than assume it.
  struct FakeChain
  {
    std::set<std::string> present;
    std::vector<std::string> probed;

    AssetScanner::ExistsFn fn()
    {
      return [this] (std::string const& path) -> bool
      {
        probed.push_back(path);
        return present.count(path) != 0;
      };
    }
  };

  std::vector<std::string> keysOf(std::vector<MissingAsset> const& assets)
  {
    std::vector<std::string> keys;
    keys.reserve(assets.size());

    for (MissingAsset const& asset : assets)
    {
      keys.push_back(asset.key);
    }

    return keys;
  }

  bool reportContains(std::string const& report, std::string const& needle)
  {
    return report.find(needle) != std::string::npos;
  }
}

TEST_CASE("normalizeAssetKey lowercases, flips separators and collapses runs", "[assetscan]")
{
  CHECK(normalizeAssetKey("World\\Azeroth\\Foo.M2") == "world\\azeroth\\foo.m2");
  CHECK(normalizeAssetKey("World/Azeroth/Foo.m2") == "world\\azeroth\\foo.m2");
  CHECK(normalizeAssetKey("World\\\\Azeroth///Foo.m2") == "world\\azeroth\\foo.m2");

  // Leading separators are dropped: ClientData::getDiskPath builds `project_dir / filepath`, and
  // std::filesystem::operator/ discards the left operand when the right one is rooted, so a
  // leading separator would send the on-disk half of the probe to the wrong place entirely.
  CHECK(normalizeAssetKey("\\World\\Foo.m2") == "world\\foo.m2");
  CHECK(normalizeAssetKey("//World//Foo.m2") == "world\\foo.m2");

  // Trailing separator names a directory; the name in front of it is still usable.
  CHECK(normalizeAssetKey("World\\Foo.m2\\") == "world\\foo.m2");

  CHECK(normalizeAssetKey("  World\\Foo.m2\t\r\n") == "world\\foo.m2");
}

TEST_CASE("normalizeAssetKey rejects what cannot be a path", "[assetscan]")
{
  CHECK(normalizeAssetKey("").empty());
  CHECK(normalizeAssetKey("   ").empty());
  CHECK(normalizeAssetKey("\\").empty());
  CHECK(normalizeAssetKey("///\\\\").empty());
  CHECK(normalizeAssetKey("  \\ \\  ").empty() == false); // the space between separators IS a name
}

TEST_CASE("normalizeAssetKey leaves bytes above ASCII alone", "[assetscan]")
{
  // The point of the hand-rolled lowercase. ::tolower takes an int and is undefined for negative
  // values, which is every one of these bytes once a signed char is promoted -- and
  // ClientData::normalizeFilenameInternal (ClientData.cpp:669) calls exactly that on paths from
  // ADT texture lists and DBC string tables. A crash on a debug CRT is the good outcome; silent
  // locale-dependent mangling is the bad one.
  std::string const high_bytes ("World\\\xC3\x9F\xE4\xB8\xAD.BLP");
  std::string const key = normalizeAssetKey(high_bytes);

  CHECK(key == "world\\\xC3\x9F\xE4\xB8\xAD.blp");
}

TEST_CASE("assetKindFromPath classifies by extension", "[assetscan]")
{
  CHECK(assetKindFromPath("world\\foo.m2") == AssetKind::Model);
  CHECK(assetKindFromPath("world\\foo.M2") == AssetKind::Model);

  // .mdx and .mdl classify as Model but are NOT rewritten: the scanner has to report the string
  // the map actually contains, or the reporter goes looking for a name that appears nowhere.
  CHECK(assetKindFromPath("world\\foo.mdx") == AssetKind::Model);
  CHECK(assetKindFromPath("world\\foo.MDL") == AssetKind::Model);

  CHECK(assetKindFromPath("world\\wmo\\foo.wmo") == AssetKind::WorldModel);
  CHECK(assetKindFromPath("tileset\\generic\\black.blp") == AssetKind::Texture);

  CHECK(assetKindFromPath("world\\foo.wav") == AssetKind::Other);
  CHECK(assetKindFromPath("world\\foo") == AssetKind::Other);
  CHECK(assetKindFromPath("") == AssetKind::Other);

  // The dot belongs to a directory, so there is no extension at all.
  CHECK(assetKindFromPath("world\\tex.set\\foo") == AssetKind::Other);
  CHECK(assetKindFromPath("world\\tex.set\\foo.blp") == AssetKind::Texture);

  // Trailing dot with nothing after it is not an extension.
  CHECK(assetKindFromPath("world\\foo.") == AssetKind::Other);
}

TEST_CASE("AssetScanResult deduplicates by normalised key", "[assetscan]")
{
  AssetScanResult result;

  CHECK(result.empty());

  REQUIRE(result.addReference(AssetKind::Texture, "Tileset\\Foo.blp", "ADT 32,48", AssetStatus::Missing));
  REQUIRE(result.addReference(AssetKind::Texture, "tileset/foo.BLP", "ADT 32,49", AssetStatus::Missing));

  CHECK(result.totalReferences() == 2);
  CHECK(result.distinctReferenced() == 1);
  CHECK(result.distinctMissing() == 1);
  CHECK(result.referenceCount("TILESET\\FOO.BLP") == 2);
  CHECK(result.wasReferenced("tileset\\foo.blp"));
  CHECK(!result.wasReferenced("tileset\\bar.blp"));

  // The first spelling is preserved, because it is the one written in the data being fixed.
  std::vector<MissingAsset> const failures = result.failures();
  REQUIRE(failures.size() == 1);
  CHECK(failures[0].display_path == "Tileset\\Foo.blp");
  CHECK(failures[0].key == "tileset\\foo.blp");
  CHECK(failures[0].reference_count == 2);
}

TEST_CASE("AssetScanResult rejects unusable paths without counting them as references", "[assetscan]")
{
  AssetScanResult result;

  CHECK(!result.addReference(AssetKind::Texture, "", "ADT 0,0", AssetStatus::Missing));
  CHECK(!result.addReference(AssetKind::Texture, "   ", "ADT 0,0", AssetStatus::Missing));
  CHECK(!result.addReference(AssetKind::Model, "\\\\\\", "ADT 0,0", AssetStatus::Missing));

  CHECK(result.rejectedCount() == 3);
  CHECK(result.totalReferences() == 0);
  CHECK(result.distinctReferenced() == 0);
  CHECK(result.empty());
  CHECK(!result.hasFailures());
}

TEST_CASE("AssetScanResult keeps the first verdict and counts disagreements", "[assetscan]")
{
  AssetScanResult result;

  result.addReference(AssetKind::Model, "world\\foo.m2", "ADT 1,1", AssetStatus::Missing);
  CHECK(result.disagreementCount() == 0);

  result.addReference(AssetKind::Model, "world\\foo.m2", "ADT 1,2", AssetStatus::Present);

  // First wins: the per-kind counts already emitted are consistent with it, and adopting the
  // newer answer silently would make the totals disagree with the sections of the report.
  CHECK(result.statusOf("world\\foo.m2") == AssetStatus::Missing);
  CHECK(result.disagreementCount() == 1);
  CHECK(result.distinctMissing() == 1);
}

TEST_CASE("AssetScanResult caps and deduplicates referrers", "[assetscan]")
{
  AssetScanResult result;

  for (int i = 0; i < 20; ++i)
  {
    result.addReference( AssetKind::Texture
                       , "tileset\\foo.blp"
                       , "ADT 0," + std::to_string(i)
                       , AssetStatus::Missing
                       , 3
                       );
  }

  // The same referrer over and over must not eat the budget.
  for (int i = 0; i < 5; ++i)
  {
    result.addReference(AssetKind::Texture, "tileset\\foo.blp", "ADT 0,0", AssetStatus::Missing, 3);
  }

  std::vector<MissingAsset> const failures = result.failures();
  REQUIRE(failures.size() == 1);
  CHECK(failures[0].reference_count == 25);
  CHECK(failures[0].referrers.size() == 3);
  CHECK(failures[0].referrers[0] == "ADT 0,0");
  CHECK(failures[0].referrers[1] == "ADT 0,1");
  CHECK(failures[0].referrers[2] == "ADT 0,2");
}

TEST_CASE("AssetScanResult counts per kind independently", "[assetscan]")
{
  AssetScanResult result;

  result.addReference(AssetKind::Model, "world\\a.m2", "t", AssetStatus::Missing);
  result.addReference(AssetKind::Model, "world\\b.m2", "t", AssetStatus::Present);
  result.addReference(AssetKind::Texture, "tex\\a.blp", "t", AssetStatus::Missing);
  result.addReference(AssetKind::Texture, "tex\\b.blp", "t", AssetStatus::Unreadable);
  result.addReference(AssetKind::WorldModel, "wmo\\a.wmo", "t", AssetStatus::Present);

  AssetKindCounts const models = result.counts(AssetKind::Model);
  CHECK(models.referenced == 2);
  CHECK(models.missing == 1);
  CHECK(models.unreadable == 0);

  AssetKindCounts const textures = result.counts(AssetKind::Texture);
  CHECK(textures.referenced == 2);
  CHECK(textures.missing == 1);
  CHECK(textures.unreadable == 1);

  AssetKindCounts const world_models = result.counts(AssetKind::WorldModel);
  CHECK(world_models.referenced == 1);
  CHECK(world_models.missing == 0);

  CHECK(result.counts(AssetKind::Other).referenced == 0);

  CHECK(result.distinctReferenced() == 5);
  CHECK(result.distinctMissing() == 2);
  CHECK(result.distinctUnreadable() == 1);
  CHECK(result.hasFailures());

  CHECK(result.failures(AssetKind::Texture).size() == 2);
  CHECK(result.failures(AssetKind::WorldModel).empty());
}

TEST_CASE("AssetScanResult failure order is total and insertion independent", "[assetscan]")
{
  // Same data, opposite insertion order. The result is backed by an unordered_map, so without a
  // total order the two would come out in whatever the bucket walk produced and a re-scan of an
  // unchanged map could show a different report.
  auto build = [] (bool reversed)
  {
    std::vector<std::pair<AssetKind, std::string>> const assets
      { {AssetKind::Texture, "tex\\z.blp"}
      , {AssetKind::Texture, "tex\\a.blp"}
      , {AssetKind::Model, "model\\b.m2"}
      , {AssetKind::WorldModel, "wmo\\c.wmo"}
      };

    AssetScanResult result;

    auto record = [&result] (std::pair<AssetKind, std::string> const& asset, std::size_t times)
    {
      for (std::size_t i = 0; i < times; ++i)
      {
        result.addReference(asset.first, asset.second, "t" + std::to_string(i), AssetStatus::Missing);
      }
    };

    if (reversed)
    {
      for (auto it = assets.rbegin(); it != assets.rend(); ++it)
      {
        // tex\a.blp gets two references, everything else one, so the descending-count tiebreak
        // is actually exercised rather than falling straight through to the key comparison.
        record(*it, it->second == "tex\\a.blp" ? 2 : 1);
      }
    }
    else
    {
      for (auto const& asset : assets)
      {
        record(asset, asset.second == "tex\\a.blp" ? 2 : 1);
      }
    }

    return keysOf(result.failures());
  };

  std::vector<std::string> const expected
    {"model\\b.m2", "wmo\\c.wmo", "tex\\a.blp", "tex\\z.blp"};

  CHECK(build(false) == expected);
  CHECK(build(true) == expected);
}

TEST_CASE("AssetScanResult::merge folds two partial scans", "[assetscan]")
{
  AssetScanResult first;
  first.addReference(AssetKind::Texture, "Tileset\\Foo.blp", "ADT 1,1", AssetStatus::Missing);
  first.addReference(AssetKind::Model, "world\\a.m2", "ADT 1,1", AssetStatus::Present);
  first.addReference(AssetKind::Model, "", "ADT 1,1", AssetStatus::Missing);

  AssetScanResult second;
  second.addReference(AssetKind::Texture, "tileset/foo.blp", "ADT 2,2", AssetStatus::Missing);
  second.addReference(AssetKind::Model, "world\\b.m2", "ADT 2,2", AssetStatus::Missing);

  first.merge(second);

  CHECK(first.distinctReferenced() == 3);
  CHECK(first.totalReferences() == 4);
  CHECK(first.rejectedCount() == 1);
  CHECK(first.referenceCount("tileset\\foo.blp") == 2);
  CHECK(first.distinctMissing() == 2);

  std::vector<MissingAsset> const failures = first.failures();
  REQUIRE(failures.size() == 2);

  // The merged texture kept the spelling the first scan saw and gained the second scan's referrer.
  std::vector<MissingAsset> const textures = first.failures(AssetKind::Texture);
  REQUIRE(textures.size() == 1);
  CHECK(textures[0].display_path == "Tileset\\Foo.blp");
  CHECK(textures[0].referrers == std::vector<std::string>{"ADT 1,1", "ADT 2,2"});
}

TEST_CASE("AssetScanResult::merge records a verdict conflict rather than resolving it", "[assetscan]")
{
  AssetScanResult first;
  first.addReference(AssetKind::Model, "world\\a.m2", "ADT 1,1", AssetStatus::Present);

  AssetScanResult second;
  second.addReference(AssetKind::Model, "world\\a.m2", "ADT 2,2", AssetStatus::Missing);

  first.merge(second);

  CHECK(first.statusOf("world\\a.m2") == AssetStatus::Present);
  CHECK(first.disagreementCount() == 1);
  CHECK(first.referenceCount("world\\a.m2") == 2);
}

TEST_CASE("AssetScanner probes once per distinct asset", "[assetscan]")
{
  FakeChain chain;
  chain.present = {"tileset\\present.blp"};

  AssetScanner scanner (chain.fn());

  // 256 chunks all painting the same two textures: the shape the real terrain walk produces, and
  // the reason the probe is memoised. Without the cache this is 512 archive walks.
  for (int chunk = 0; chunk < 256; ++chunk)
  {
    scanner.addReference(AssetKind::Texture, "Tileset\\Present.blp", "ADT 32,48");
    scanner.addReference(AssetKind::Texture, "Tileset\\Absent.blp", "ADT 32,48");
  }

  CHECK(scanner.probeCount() == 2);
  CHECK(chain.probed.size() == 2);

  // The probe receives the normalised key, not the raw spelling.
  CHECK(std::find(chain.probed.begin(), chain.probed.end(), "tileset\\present.blp") != chain.probed.end());
  CHECK(std::find(chain.probed.begin(), chain.probed.end(), "tileset\\absent.blp") != chain.probed.end());

  AssetScanResult const& result = scanner.result();
  CHECK(result.totalReferences() == 512);
  CHECK(result.distinctReferenced() == 2);
  CHECK(result.distinctMissing() == 1);
  CHECK(result.statusOf("tileset\\present.blp") == AssetStatus::Present);
  CHECK(result.statusOf("tileset\\absent.blp") == AssetStatus::Missing);
}

TEST_CASE("AssetScanner separates missing from unreadable", "[assetscan]")
{
  FakeChain chain;
  chain.present = {"world\\corrupt.m2"};

  AssetScanner scanner (chain.fn());

  // In the chain but the loader rejected it: the file shipped, its contents are wrong.
  scanner.addReference("world\\corrupt.m2", "ADT 1,1", true);
  // Not in the chain, and the loader also failed. The loader failure adds nothing here.
  scanner.addReference("world\\absent.m2", "ADT 1,1", true);
  // In the chain and loaded.
  scanner.addReference(AssetKind::Model, "world\\corrupt.m2", "ADT 1,2", false);

  CHECK(scanner.result().statusOf("world\\corrupt.m2") == AssetStatus::Unreadable);
  CHECK(scanner.result().statusOf("world\\absent.m2") == AssetStatus::Missing);
  CHECK(scanner.result().distinctUnreadable() == 1);
  CHECK(scanner.result().distinctMissing() == 1);

  // The third call agrees with the memoised Present but not with the recorded Unreadable, which
  // is a verdict conflict by construction and must be visible rather than silent.
  CHECK(scanner.result().disagreementCount() == 1);
}

TEST_CASE("AssetScanner classifies by extension when the caller does not say", "[assetscan]")
{
  FakeChain chain;
  AssetScanner scanner (chain.fn());

  scanner.addReference("world\\a.m2", "t");
  scanner.addReference("world\\a.wmo", "t");
  scanner.addReference("tex\\a.blp", "t");
  scanner.addReference("sound\\a.wav", "t");

  CHECK(scanner.result().counts(AssetKind::Model).missing == 1);
  CHECK(scanner.result().counts(AssetKind::WorldModel).missing == 1);
  CHECK(scanner.result().counts(AssetKind::Texture).missing == 1);
  CHECK(scanner.result().counts(AssetKind::Other).missing == 1);
}

TEST_CASE("AssetScanner never probes an unusable path", "[assetscan]")
{
  FakeChain chain;
  AssetScanner scanner (chain.fn());

  scanner.addReference(AssetKind::Texture, "", "ADT 0,0");
  scanner.addReference(AssetKind::Texture, "  \\\\ ", "ADT 0,0");

  CHECK(scanner.probeCount() == 0);
  CHECK(chain.probed.empty());
  CHECK(scanner.result().rejectedCount() == 2);
  CHECK(scanner.result().empty());
}

TEST_CASE("AssetScanner with no probe reports everything missing", "[assetscan]")
{
  // The state a caller lands in when client data is not open yet. Reporting Present would be the
  // dangerous default: it clears every asset in the map on the strength of no evidence at all.
  AssetScanner scanner (AssetScanner::ExistsFn{});

  scanner.addReference("world\\a.m2", "t");

  CHECK(scanner.result().statusOf("world\\a.m2") == AssetStatus::Missing);
  CHECK(scanner.probeCount() == 1);
}

TEST_CASE("AssetScanner options are honoured and readable by the collector", "[assetscan]")
{
  AssetScanOptions options;
  options.max_referrers_per_asset = 2;
  options.scan_model_textures = false;

  FakeChain chain;
  AssetScanner scanner (chain.fn(), options);

  CHECK(scanner.options().max_referrers_per_asset == 2);
  CHECK(scanner.options().scan_model_textures == false);
  CHECK(scanner.options().scan_world_model_contents == true);

  scanner.addReference("world\\a.m2", "ADT 1,1");
  scanner.addReference("world\\a.m2", "ADT 1,2");
  scanner.addReference("world\\a.m2", "ADT 1,3");

  std::vector<MissingAsset> const failures = scanner.result().failures();
  REQUIRE(failures.size() == 1);
  CHECK(failures[0].referrers.size() == 2);
  CHECK(failures[0].reference_count == 3);
}

TEST_CASE("AssetScanner::takeResult hands over the result and resets", "[assetscan]")
{
  FakeChain chain;
  chain.present = {"world\\a.m2"};

  AssetScanner scanner (chain.fn());
  scanner.addReference("world\\a.m2", "t");
  scanner.addReference("world\\b.m2", "t");

  AssetScanResult const taken = scanner.takeResult();
  CHECK(taken.distinctReferenced() == 2);
  CHECK(taken.distinctMissing() == 1);

  // Reusing the scanner must not report counts that belong to a result someone else now owns,
  // and must not answer from a cache describing it.
  CHECK(scanner.result().empty());
  CHECK(scanner.probeCount() == 0);

  scanner.addReference("world\\a.m2", "t");
  CHECK(scanner.probeCount() == 1);
  CHECK(scanner.result().distinctReferenced() == 1);
}

TEST_CASE("summary reads correctly in both directions", "[assetscan]")
{
  AssetScanResult clean;
  clean.addReference(AssetKind::Model, "world\\a.m2", "t", AssetStatus::Present);
  CHECK(reportContains(clean.summary(), "no missing assets"));

  AssetScanResult dirty;
  dirty.addReference(AssetKind::Model, "world\\a.m2", "t", AssetStatus::Missing);
  dirty.addReference(AssetKind::Texture, "tex\\a.blp", "t", AssetStatus::Unreadable);
  CHECK(reportContains(dirty.summary(), "1 missing, 1 unreadable"));
}

TEST_CASE("toReport names the spelling the data actually contains", "[assetscan]")
{
  AssetScanResult result;
  result.addReference(AssetKind::Model, "World\\Azeroth\\Missing.MDX", "ADT 32,48", AssetStatus::Missing);
  result.addReference(AssetKind::Model, "world/azeroth/missing.mdx", "ADT 32,49", AssetStatus::Missing);
  result.addReference(AssetKind::Texture, "Tex\\Bad.blp", "ADT 32,48", AssetStatus::Unreadable);
  result.addReference(AssetKind::Texture, "Tex\\Good.blp", "ADT 32,48", AssetStatus::Present);
  result.addReference(AssetKind::Texture, "", "ADT 32,48", AssetStatus::Missing);

  std::string const report = result.toReport();

  CHECK(reportContains(report, "World\\Azeroth\\Missing.MDX"));
  CHECK(reportContains(report, "[missing]"));
  CHECK(reportContains(report, "[unreadable]"));
  CHECK(reportContains(report, "Tex\\Bad.blp"));
  CHECK(reportContains(report, "ADT 32,48, ADT 32,49"));
  CHECK(reportContains(report, "1 reference(s) rejected as unusable paths"));

  // A cleared asset must not appear in the failure list.
  CHECK(!reportContains(report, "Tex\\Good.blp"));

  // Identical input produces an identical report, which is what makes diffing two scans useful.
  CHECK(result.toReport() == report);
}

TEST_CASE("toReport says so when nothing is missing", "[assetscan]")
{
  AssetScanResult result;
  result.addReference(AssetKind::Model, "world\\a.m2", "t", AssetStatus::Present);

  std::string const report = result.toReport();

  CHECK(reportContains(report, "Nothing missing."));
  CHECK(!reportContains(report, "[missing]"));
}
