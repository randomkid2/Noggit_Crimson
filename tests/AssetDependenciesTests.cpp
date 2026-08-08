// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <catch2/catch_test_macros.hpp>

#include <noggit/AssetDependencies.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Covers the half of the dependency packer that has no client install behind it: the path
// arithmetic and the three chunk parsers, driven with hand-built buffers.
//
// The driver (PatchAssetPacker) is NOT covered here and cannot be: it resolves through
// BlizzardArchive::ClientData, writes MPQ files through StormLib, and needs a real client. The same
// boundary AssetScanTests.cpp draws, for the same reason.
//
// The parsers are the part worth pinning. Every name they build is one the client will hash for,
// and three of them are spelled differently by the loaders in this tree than they are on disk --
// which is exactly how a packed archive ends up looking correct and doing nothing.

namespace
{
  void appendUint32(std::vector<char>& buffer, std::uint32_t value)
  {
    char bytes[4];
    std::memcpy(bytes, &value, 4);
    buffer.insert(buffer.end(), bytes, bytes + 4);
  }

  void appendInt16(std::vector<char>& buffer, std::int16_t value)
  {
    char bytes[2];
    std::memcpy(bytes, &value, 2);
    buffer.insert(buffer.end(), bytes, bytes + 2);
  }

  // Chunk magics are stored REVERSED on disk, which is why every loader here compares a uint32
  // against a multi-character literal. The tests spell them forwards and reverse them, so a test
  // reads as the format documents it.
  void appendChunk(std::vector<char>& buffer, char const* magic, std::vector<char> const& payload)
  {
    for (int index = 3; index >= 0; --index)
    {
      buffer.push_back(magic[index]);
    }

    appendUint32(buffer, static_cast<std::uint32_t>(payload.size()));
    buffer.insert(buffer.end(), payload.begin(), payload.end());
  }

  std::vector<char> stringBlock(std::vector<std::string> const& names)
  {
    std::vector<char> block;

    for (auto const& name : names)
    {
      block.insert(block.end(), name.begin(), name.end());
      block.push_back('\0');
    }

    return block;
  }

  bool contains(std::vector<Noggit::AssetReference> const& references, std::string const& path)
  {
    return std::any_of( references.begin(), references.end()
                      , [&path] (Noggit::AssetReference const& reference) { return reference.path == path; }
                      );
  }

  Noggit::AssetReference const* find(std::vector<Noggit::AssetReference> const& references, std::string const& path)
  {
    auto const it = std::find_if( references.begin(), references.end()
                                , [&path] (Noggit::AssetReference const& reference) { return reference.path == path; }
                                );

    return it == references.end() ? nullptr : &*it;
  }
}

TEST_CASE("normalizeReferencePath folds case and separators", "[assetdeps]")
{
  CHECK(Noggit::normalizeReferencePath("World\\WMO\\Foo.WMO") == "world/wmo/foo.wmo");
  CHECK(Noggit::normalizeReferencePath("  World/WMO/Foo.wmo  ") == "world/wmo/foo.wmo");
  CHECK(Noggit::normalizeReferencePath("\\World\\\\WMO\\Foo.wmo") == "world/wmo/foo.wmo");
  CHECK(Noggit::normalizeReferencePath("") == "");
  CHECK(Noggit::normalizeReferencePath("\\\\") == "");
}

TEST_CASE("normalizeReferencePath rewrites .mdx and .mdl only at the end", "[assetdeps]")
{
  CHECK(Noggit::normalizeReferencePath("World\\Foo.MDX") == "world/foo.m2");
  CHECK(Noggit::normalizeReferencePath("World\\Foo.mdl") == "world/foo.m2");
  CHECK(Noggit::normalizeReferencePath("World\\Foo.m2") == "world/foo.m2");

  // The bug this exists to avoid. ClientData::normalizeFilenameInternal does the rewrite with
  // std::regex_replace(filename, std::regex(".mdx"), ".m2"), where '.' matches ANY character and
  // every match is replaced -- so this path comes out of it as "creature/.m2thing/foo.m2" and the
  // packer would store a name nothing will ever ask for.
  CHECK(Noggit::normalizeReferencePath("creature/amdxthing/foo.mdx") == "creature/amdxthing/foo.m2");
}

TEST_CASE("archiveStoredName produces the backslash name MPQ hashes", "[assetdeps]")
{
  // MPQ resolves a file by hashing its name, so a forward slash here is a file the client never
  // finds: valid archive, correct payload, no effect in game.
  CHECK(Noggit::archiveStoredName("World/Maps/Expansion01/Expansion01_29_32.adt")
        == "world\\maps\\expansion01\\expansion01_29_32.adt");
  CHECK(Noggit::archiveStoredName("World\\Foo.MDX") == "world\\foo.m2");
}

TEST_CASE("skin names match what the client ships", "[assetdeps]")
{
  // dist/listfile/listfile.csv carries creature/rabbit/rabbit00.skin: no dot before the index.
  CHECK(Noggit::skinFileName("creature/rabbit/rabbit.m2", 0) == "creature/rabbit/rabbit00.skin");
  CHECK(Noggit::skinFileName("creature/rabbit/rabbit.m2", 3) == "creature/rabbit/rabbit03.skin");
}

TEST_CASE("anim names are zero padded, unlike Model.cpp", "[assetdeps]")
{
  // Model.cpp:497 emits "...129-0.anim". A grep for "-[0-9]\.anim$" over the shipped listfile
  // finds nothing at all, while "-0[0-9]\.anim$" finds 38484 -- so the loader's own spelling
  // resolves to no file and external animation data is silently never loaded.
  CHECK(Noggit::animFileName("creature/draeneiconstruct/draeneiconstruct.m2", 129, 0)
        == "creature/draeneiconstruct/draeneiconstruct0129-00.anim");
  CHECK(Noggit::animFileName("creature/x/x.m2", 0, 0) == "creature/x/x0000-00.anim");
  CHECK(Noggit::animFileName("creature/x/x.m2", -1, 0).empty());
}

TEST_CASE("WMO group names strip the extension from the end", "[assetdeps]")
{
  CHECK(Noggit::wmoGroupFileName("world/wmo/foo.wmo", 0) == "world/wmo/foo_000.wmo");
  CHECK(Noggit::wmoGroupFileName("world/wmo/foo.wmo", 12) == "world/wmo/foo_012.wmo");

  // WMO.cpp:817 does fname.insert(fname.find(".wmo"), ...) -- the FIRST occurrence -- so a
  // directory component ending in ".wmo" splices the index into the middle of the path.
  CHECK(Noggit::wmoGroupFileName("world/my.wmoset/foo.wmo", 1) == "world/my.wmoset/foo_001.wmo");
}

TEST_CASE("group files are recognised so they are never opened as roots", "[assetdeps]")
{
  CHECK(Noggit::isWmoGroupFileName("world/wmo/foo_000.wmo"));
  CHECK(Noggit::isWmoGroupFileName("world/wmo/foo_1000.wmo"));
  CHECK_FALSE(Noggit::isWmoGroupFileName("world/wmo/foo.wmo"));
  CHECK_FALSE(Noggit::isWmoGroupFileName("world/wmo/foo_00.wmo"));
  CHECK_FALSE(Noggit::isWmoGroupFileName("world/wmo/foo_abc.wmo"));
}

TEST_CASE("tileset specular companion", "[assetdeps]")
{
  CHECK(Noggit::tilesetSpecularName("Tileset\\Elwynn\\ElwynnGrass01.blp") == "tileset/elwynn/elwynngrass01_s.blp");
  CHECK(Noggit::tilesetSpecularName("world/foo/bar.blp").empty());
  CHECK(Noggit::tilesetSpecularName("tileset/elwynn/grass.m2").empty());
}

TEST_CASE("base client archives are told apart from the user's own patches", "[assetdeps]")
{
  CHECK(Noggit::isBaseClientArchiveName("common.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("common-2.mpq"));
  CHECK(Noggit::isBaseClientArchiveName("lichking.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("expansion.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("patch.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("patch-2.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("patch-3.MPQ"));

  // The whole point: these are where the user's own content lives, and they are loaded through
  // exactly the same patch-{number}/patch-{character} templates as the stock ones.
  CHECK_FALSE(Noggit::isBaseClientArchiveName("patch-4.MPQ"));
  CHECK_FALSE(Noggit::isBaseClientArchiveName("patch-9.mpq"));
  CHECK_FALSE(Noggit::isBaseClientArchiveName("patch-A.MPQ"));
  CHECK_FALSE(Noggit::isBaseClientArchiveName("alternate.MPQ"));
  CHECK_FALSE(Noggit::isBaseClientArchiveName("development.MPQ"));

  CHECK(Noggit::isBaseClientArchiveName("locale-enUS.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("speech-enUS.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("expansion-locale-enUS.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("lichking-speech-frFR.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("patch-enUS.MPQ"));
  CHECK(Noggit::isBaseClientArchiveName("patch-enUS-3.MPQ"));
  CHECK_FALSE(Noggit::isBaseClientArchiveName("patch-enUS-4.MPQ"));

  // The caller passes BaseArchive::path(), which is a full disk path, and a directory patch has no
  // extension at all.
  CHECK(Noggit::isBaseClientArchiveName("D:/games/wow-3.3.5a/Data/common.MPQ"));
  CHECK_FALSE(Noggit::isBaseClientArchiveName("D:/games/wow-3.3.5a/Data/patch-9.MPQ"));
  CHECK_FALSE(Noggit::isBaseClientArchiveName("D:/games/wow-3.3.5a/Data/mypatchdir"));
}

TEST_CASE("an ADT yields its texture, model and world model name blocks", "[assetdeps]")
{
  std::vector<char> adt;
  appendChunk(adt, "MVER", std::vector<char>(4, '\0'));
  appendChunk(adt, "MTEX", stringBlock({"Tileset\\Elwynn\\ElwynnGrass01.blp", "Tileset\\Generic\\Black.blp"}));
  appendChunk(adt, "MMDX", stringBlock({"World\\Trees\\Oak.MDX"}));
  appendChunk(adt, "MWMO", stringBlock({"World\\wmo\\Custom\\Tower.WMO"}));

  std::vector<Noggit::AssetReference> references;
  REQUIRE(Noggit::collectAdtReferences(adt.data(), adt.size(), references).complete());

  CHECK(contains(references, "tileset/elwynn/elwynngrass01.blp"));
  CHECK(contains(references, "tileset/generic/black.blp"));
  CHECK(contains(references, "world/trees/oak.m2"));
  CHECK(contains(references, "world/wmo/custom/tower.wmo"));

  // Terrain specular companions come in alongside the base name, and only for tileset paths.
  auto const* specular = find(references, "tileset/elwynn/elwynngrass01_s.blp");
  REQUIRE(specular);
  CHECK(specular->necessity == Noggit::ReferenceNecessity::Optional);

  // The model and the world model are followed; a .blp is a leaf.
  REQUIRE(find(references, "world/trees/oak.m2"));
  CHECK(find(references, "world/trees/oak.m2")->traversable);
  CHECK_FALSE(find(references, "tileset/elwynn/elwynngrass01.blp")->traversable);
}

TEST_CASE("a buffer that is not an ADT is rejected rather than mined for strings", "[assetdeps]")
{
  std::vector<char> not_an_adt (64, 'x');
  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectAdtReferences(not_an_adt.data(), not_an_adt.size(), references));

  CHECK(parsed.outcome == Noggit::ParseOutcome::Unrecognised);
  CHECK_FALSE(parsed.recognised());
  CHECK_FALSE(parsed.reason.empty());
  CHECK(references.empty());
}

TEST_CASE("a chunk declaring more bytes than the file holds is REPORTED, not swallowed", "[assetdeps]")
{
  std::vector<char> adt;
  appendChunk(adt, "MVER", std::vector<char>(4, '\0'));

  // MTEX claims 4096 bytes and the file ends. Past that point every offset is guesswork.
  for (int index = 3; index >= 0; --index)
  {
    adt.push_back("MTEX"[index]);
  }

  appendUint32(adt, 4096);
  std::vector<char> const tail (stringBlock({"tileset/a.blp"}));
  adt.insert(adt.end(), tail.begin(), tail.end());

  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectAdtReferences(adt.data(), adt.size(), references));

  // The bug this pins. The walk stops -- that part was always right -- and it used to say so by
  // returning TRUE with an empty list, which is byte-for-byte what a legitimate ADT referencing
  // nothing returns. The packer recorded such a file as Present, counted it as a successfully
  // walked reference, and named it in no failure list, so the patch shipped without every texture
  // and model the file listed and the report claimed success.
  CHECK(parsed.outcome == Noggit::ParseOutcome::Truncated);
  CHECK_FALSE(parsed.complete());
  CHECK(parsed.recognised());
  CHECK(references.empty());

  // The reason has to name the chunk, or the user is told a file is broken and not where.
  CHECK(parsed.reason.find("MTEX") != std::string::npos);
  CHECK(parsed.reason.find("4096") != std::string::npos);
}

TEST_CASE("a truncated ADT that yielded some names is reported as partial, not complete", "[assetdeps]")
{
  std::vector<char> adt;
  appendChunk(adt, "MVER", std::vector<char>(4, '\0'));
  appendChunk(adt, "MTEX", stringBlock({"Tileset\\Elwynn\\ElwynnGrass01.blp"}));

  // MMDX is cut off after its header, so the models are gone and the textures are not. This is the
  // harder case: `out` is NOT empty, so nothing about the result looks wrong to a caller that only
  // asks whether anything came back.
  for (int index = 3; index >= 0; --index)
  {
    adt.push_back("MMDX"[index]);
  }

  appendUint32(adt, 512);

  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectAdtReferences(adt.data(), adt.size(), references));

  CHECK(parsed.outcome == Noggit::ParseOutcome::Truncated);
  CHECK(contains(references, "tileset/elwynn/elwynngrass01.blp"));
  CHECK(parsed.reason.find("MMDX") != std::string::npos);
}

TEST_CASE("a tail too short to be a chunk header is truncation, not a clean end", "[assetdeps]")
{
  std::vector<char> adt;
  appendChunk(adt, "MVER", std::vector<char>(4, '\0'));
  appendChunk(adt, "MTEX", stringBlock({"tileset/a.blp"}));

  // Three bytes: the shape a half-copied file has. Every chunk before it parsed, so the reference
  // list is a prefix of the truth and looks perfectly healthy on its own.
  adt.push_back('M');
  adt.push_back('W');
  adt.push_back('M');

  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectAdtReferences(adt.data(), adt.size(), references));

  CHECK(parsed.outcome == Noggit::ParseOutcome::Truncated);
  CHECK(contains(references, "tileset/a.blp"));
  CHECK(parsed.reason.find("trailing") != std::string::npos);
}

TEST_CASE("a well formed ADT reports Complete with no reason", "[assetdeps]")
{
  std::vector<char> adt;
  appendChunk(adt, "MVER", std::vector<char>(4, '\0'));
  appendChunk(adt, "MTEX", stringBlock({"tileset/a.blp"}));

  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectAdtReferences(adt.data(), adt.size(), references));

  CHECK(parsed.complete());
  CHECK(parsed.recognised());
  CHECK(parsed.reason.empty());
}

namespace
{
  // A minimal MD20 with room for the fields the parser reads. Everything not named is zero.
  std::vector<char> buildModel( std::uint32_t view_count
                              , std::vector<std::pair<std::uint32_t, std::string>> const& textures
                              , std::vector<std::pair<std::int16_t, std::int16_t>> const& animations
                              )
  {
    constexpr std::size_t HEADER_SIZE = 304;

    std::vector<char> model (HEADER_SIZE, '\0');
    std::memcpy(model.data(), "MD20", 4);

    std::uint32_t const texture_offset = static_cast<std::uint32_t>(model.size());

    // Reserve the texture definition table, then append the names after it so their offsets are
    // known.
    model.resize(model.size() + textures.size() * 16, '\0');

    std::vector<std::uint32_t> name_offsets;

    for (auto const& texture : textures)
    {
      name_offsets.push_back(static_cast<std::uint32_t>(model.size()));
      model.insert(model.end(), texture.second.begin(), texture.second.end());
      model.push_back('\0');
    }

    for (std::size_t index = 0; index < textures.size(); ++index)
    {
      char* entry = model.data() + texture_offset + index * 16;
      std::uint32_t const type = textures[index].first;
      std::uint32_t const length = static_cast<std::uint32_t>(textures[index].second.size() + 1);

      std::memcpy(entry, &type, 4);
      std::memcpy(entry + 8, &length, 4);
      std::memcpy(entry + 12, &name_offsets[index], 4);
    }

    std::uint32_t const animation_offset = static_cast<std::uint32_t>(model.size());

    for (auto const& animation : animations)
    {
      std::vector<char> entry;
      appendInt16(entry, animation.first);
      appendInt16(entry, animation.second);
      entry.resize(64, '\0');
      model.insert(model.end(), entry.begin(), entry.end());
    }

    std::uint32_t const texture_count = static_cast<std::uint32_t>(textures.size());
    std::uint32_t const animation_count = static_cast<std::uint32_t>(animations.size());

    std::memcpy(model.data() + 28, &animation_count, 4);
    std::memcpy(model.data() + 32, &animation_offset, 4);
    std::memcpy(model.data() + 68, &view_count, 4);
    std::memcpy(model.data() + 80, &texture_count, 4);
    std::memcpy(model.data() + 84, &texture_offset, 4);

    return model;
  }
}

TEST_CASE("an M2 yields its named textures, its skins and its anims", "[assetdeps]")
{
  std::vector<char> const model
    ( buildModel( 3
                , { {0, "Creature\\Rabbit\\RabbitSkin.blp"}
                  , {11, ""}                                   // replaceable: no name in the file
                  , {0, "Creature\\Rabbit\\Eye.BLP"}
                  }
                , { {std::int16_t(0), std::int16_t(0)}, {std::int16_t(129), std::int16_t(1)} }
                )
    );

  std::vector<Noggit::AssetReference> references;
  REQUIRE(Noggit::collectModelReferences("creature/rabbit/rabbit.m2", model.data(), model.size(), references).complete());

  CHECK(contains(references, "creature/rabbit/rabbitskin.blp"));
  CHECK(contains(references, "creature/rabbit/eye.blp"));

  // Noggit substitutes "tileset/generic/black.blp" for every replaceable slot. That is an editor
  // constant, not something the map references, and it must not be attributed to the map.
  CHECK_FALSE(contains(references, "tileset/generic/black.blp"));

  // Every view the header declares, not just the one Noggit loads.
  CHECK(contains(references, "creature/rabbit/rabbit00.skin"));
  CHECK(contains(references, "creature/rabbit/rabbit01.skin"));
  CHECK(contains(references, "creature/rabbit/rabbit02.skin"));
  CHECK_FALSE(contains(references, "creature/rabbit/rabbit03.skin"));

  REQUIRE(find(references, "creature/rabbit/rabbit00.skin"));
  CHECK(find(references, "creature/rabbit/rabbit00.skin")->necessity == Noggit::ReferenceNecessity::Required);
  CHECK(find(references, "creature/rabbit/rabbit01.skin")->necessity == Noggit::ReferenceNecessity::Optional);

  REQUIRE(find(references, "creature/rabbit/rabbit0129-01.anim"));
  CHECK(find(references, "creature/rabbit/rabbit0129-01.anim")->necessity == Noggit::ReferenceNecessity::Optional);
}

TEST_CASE("a buffer that is not an MD20 is rejected", "[assetdeps]")
{
  std::vector<char> not_a_model (512, 'x');
  std::vector<Noggit::AssetReference> references;

  CHECK(Noggit::collectModelReferences("x/y.m2", not_a_model.data(), not_a_model.size(), references).outcome
        == Noggit::ParseOutcome::Unrecognised);
}

TEST_CASE("an MD20 with no header behind it is truncated, not unrecognised", "[assetdeps]")
{
  // The distinction is what the report says to the user: "this is not a model" sends them looking
  // for the wrong problem when what they have is a model that was copied half way.
  std::vector<char> stub (32, '\0');
  std::memcpy(stub.data(), "MD20", 4);

  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectModelReferences("x/y.m2", stub.data(), stub.size(), references));

  CHECK(parsed.outcome == Noggit::ParseOutcome::Truncated);
  CHECK(parsed.recognised());
  CHECK(references.empty());
}

TEST_CASE("an M2 whose texture table runs past the end is reported", "[assetdeps]")
{
  std::vector<char> model
    (buildModel(1, { {0, "creature/rabbit/skin.blp"} }, { {std::int16_t(0), std::int16_t(0)} }));

  // Claim ten textures where one was written. The parser stops at the first entry outside the
  // buffer and still emits the skins, so `out` comes back non-empty and plausible.
  std::uint32_t const lying_count = 10;
  std::memcpy(model.data() + 80, &lying_count, 4);

  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectModelReferences("creature/rabbit/rabbit.m2", model.data(), model.size(), references));

  CHECK(parsed.outcome == Noggit::ParseOutcome::Truncated);
  CHECK(contains(references, "creature/rabbit/rabbit00.skin"));
  CHECK(parsed.reason.find("texture definition") != std::string::npos);
}

TEST_CASE("a WMO root yields its group files, textures, doodads and skybox", "[assetdeps]")
{
  std::vector<char> header (0x40, '\0');
  std::uint32_t const group_count = 3;
  std::memcpy(header.data() + 4, &group_count, 4);

  std::vector<char> const textures (stringBlock({"World\\Custom\\Wall.BLP", "World\\Custom\\Roof.blp"}));

  // Two materials. The first names texture 0, the second names texture 1 through its SECOND
  // offset -- the slot Noggit only reads for shaders 3/5/6/21/23, and which the client always
  // reads.
  std::vector<char> materials;
  materials.resize(0x80, '\0');
  std::uint32_t const wall_offset = 0;
  std::uint32_t const roof_offset = static_cast<std::uint32_t>(std::strlen("World\\Custom\\Wall.BLP") + 1);
  std::memcpy(materials.data() + 12, &wall_offset, 4);
  std::memcpy(materials.data() + 0x40 + 24, &roof_offset, 4);

  std::vector<char> const doodad_names (stringBlock({"World\\Custom\\Barrel.MDX"}));

  std::vector<char> doodads (0x28, '\0');
  std::uint32_t const barrel_offset = 0;
  std::memcpy(doodads.data(), &barrel_offset, 4);

  std::vector<char> wmo;
  appendChunk(wmo, "MVER", std::vector<char>(4, '\0'));
  appendChunk(wmo, "MOHD", header);
  appendChunk(wmo, "MOTX", textures);
  appendChunk(wmo, "MOMT", materials);
  appendChunk(wmo, "MOSB", stringBlock({"Environments\\Stars\\Sky.MDX"}));
  appendChunk(wmo, "MODN", doodad_names);
  appendChunk(wmo, "MODD", doodads);

  std::vector<Noggit::AssetReference> references;
  REQUIRE(Noggit::collectWorldModelReferences("world/wmo/custom/tower.wmo", wmo.data(), wmo.size(), references).complete());

  // THE reference AssetScan cannot produce. A root packed without its groups loads in no client.
  CHECK(contains(references, "world/wmo/custom/tower_000.wmo"));
  CHECK(contains(references, "world/wmo/custom/tower_001.wmo"));
  CHECK(contains(references, "world/wmo/custom/tower_002.wmo"));
  CHECK_FALSE(contains(references, "world/wmo/custom/tower_003.wmo"));

  // Group files name nothing themselves -- their materials are indices into the ROOT's MOMT -- so
  // opening one as a root would report a parse failure for a perfectly good file.
  REQUIRE(find(references, "world/wmo/custom/tower_000.wmo"));
  CHECK_FALSE(find(references, "world/wmo/custom/tower_000.wmo")->traversable);

  CHECK(contains(references, "world/custom/wall.blp"));
  CHECK(contains(references, "world/custom/roof.blp"));
  CHECK(contains(references, "world/custom/barrel.m2"));

  // The skybox is an M2 with its own skins and textures and appears in neither WMO::textures nor
  // WMO::modelis, so it has to be followed.
  auto const* skybox = find(references, "environments/stars/sky.m2");
  REQUIRE(skybox);
  CHECK(skybox->traversable);
  CHECK(skybox->necessity == Noggit::ReferenceNecessity::Optional);
}

TEST_CASE("a WMO with no MOHD is rejected", "[assetdeps]")
{
  std::vector<char> wmo;
  appendChunk(wmo, "MVER", std::vector<char>(4, '\0'));
  appendChunk(wmo, "MOTX", stringBlock({"a.blp"}));

  std::vector<Noggit::AssetReference> references;

  // The whole file parsed and there is no MOHD in it, so this really is "not a WMO root" -- which
  // is what a group file is, and why the packer keeps those out of this parser by name.
  CHECK(Noggit::collectWorldModelReferences("world/wmo/x.wmo", wmo.data(), wmo.size(), references).outcome
        == Noggit::ParseOutcome::Unrecognised);
}

TEST_CASE("a WMO cut short before MOHD is truncated, not 'not a WMO'", "[assetdeps]")
{
  std::vector<char> wmo;
  appendChunk(wmo, "MVER", std::vector<char>(4, '\0'));

  // MOTX claims 8192 bytes and the file stops. MOHD would have come later, so calling this
  // "contains no MOHD chunk" would be true and useless: it would send the reader looking for a
  // malformed root rather than a half-copied file.
  for (int index = 3; index >= 0; --index)
  {
    wmo.push_back("MOTX"[index]);
  }

  appendUint32(wmo, 8192);

  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectWorldModelReferences("world/wmo/x.wmo", wmo.data(), wmo.size(), references));

  CHECK(parsed.outcome == Noggit::ParseOutcome::Truncated);
  CHECK(parsed.recognised());
}

TEST_CASE("a WMO truncated after MOHD still yields its group files, and says it was truncated", "[assetdeps]")
{
  std::vector<char> header (0x40, '\0');
  std::uint32_t const group_count = 2;
  std::memcpy(header.data() + 4, &group_count, 4);

  std::vector<char> wmo;
  appendChunk(wmo, "MVER", std::vector<char>(4, '\0'));
  appendChunk(wmo, "MOHD", header);

  // MOTX cut off after its header. The group files -- the references AssetScan cannot produce and
  // whose absence loads in no client -- are still known from nGroups and must still come back.
  for (int index = 3; index >= 0; --index)
  {
    wmo.push_back("MOTX"[index]);
  }

  appendUint32(wmo, 8192);

  std::vector<Noggit::AssetReference> references;

  Noggit::AssetParseResult const parsed
    (Noggit::collectWorldModelReferences("world/wmo/x.wmo", wmo.data(), wmo.size(), references));

  CHECK(parsed.outcome == Noggit::ParseOutcome::Truncated);
  CHECK(contains(references, "world/wmo/x_000.wmo"));
  CHECK(contains(references, "world/wmo/x_001.wmo"));
}

TEST_CASE("a MODD offset outside MODN is dropped, not dereferenced", "[assetdeps]")
{
  std::vector<char> header (0x40, '\0');
  std::vector<char> const doodad_names (stringBlock({"World\\Custom\\Barrel.MDX"}));

  std::vector<char> doodads (0x28, '\0');
  std::uint32_t const bad_offset = 0x00FFFFFF;
  std::memcpy(doodads.data(), &bad_offset, 4);

  std::vector<char> wmo;
  appendChunk(wmo, "MVER", std::vector<char>(4, '\0'));
  appendChunk(wmo, "MOHD", header);
  appendChunk(wmo, "MODN", doodad_names);
  appendChunk(wmo, "MODD", doodads);

  std::vector<Noggit::AssetReference> references;
  REQUIRE(Noggit::collectWorldModelReferences("world/wmo/x.wmo", wmo.data(), wmo.size(), references).complete());

  // The name is still found through the blob split, which is why both routes are walked: an
  // unusable offset costs nothing when the string itself was already collected.
  CHECK(contains(references, "world/custom/barrel.m2"));
  CHECK(references.size() == 1);
}

TEST_CASE("a file named by several parents counts once per parent", "[assetdeps]")
{
  // The rule PatchAssetPacker's breadth-first walk broke by using ONE set for two jobs.
  //
  // A path has to be queued once -- a WMO that names its own doodads is a cycle -- but the walk
  // also skipped RECORDING the reference for an already-queued target, so every asset came out
  // with exactly one reference however many files named it. A missing texture used by forty models
  // read as "(1 reference)", which is precisely the number that decides what to chase first.
  //
  // Both halves are exercised here: the count is taken from every edge, the queue from new targets
  // only.
  auto const buildAdt
    ( [] (std::string const& model)
      {
        std::vector<char> adt;
        appendChunk(adt, "MVER", std::vector<char>(4, '\0'));
        appendChunk(adt, "MTEX", stringBlock({"Tileset\\Shared\\Grass.blp"}));
        appendChunk(adt, "MMDX", stringBlock({model}));

        return adt;
      }
    );

  std::vector<char> const first (buildAdt("World\\Trees\\Oak.MDX"));
  std::vector<char> const second (buildAdt("World\\Trees\\Pine.MDX"));

  std::vector<std::pair<std::string, std::vector<char> const*>> const parents
    { {"world/maps/test/test_00_00.adt", &first}
    , {"world/maps/test/test_00_01.adt", &second}
    };

  Noggit::AssetScanResult scan;
  std::unordered_set<std::string> visited;
  std::size_t queued = 0;

  for (auto const& parent : parents)
  {
    std::vector<Noggit::AssetReference> references;

    REQUIRE(Noggit::collectAdtReferences(parent.second->data(), parent.second->size(), references).complete());

    for (auto const& reference : references)
    {
      scan.addReference(reference.kind, reference.path, parent.first, Noggit::AssetStatus::Missing);

      if (visited.insert(reference.path).second)
      {
        ++queued;
      }
    }
  }

  CHECK(scan.referenceCount("tileset/shared/grass.blp") == 2);
  CHECK(scan.referenceCount("tileset/shared/grass_s.blp") == 2);
  CHECK(scan.referenceCount("world/trees/oak.m2") == 1);
  CHECK(scan.referenceCount("world/trees/pine.m2") == 1);

  // Four distinct paths, walked once each. Counting an edge twice must not queue it twice.
  CHECK(queued == 4);
  CHECK(scan.distinctReferenced() == 4);
  CHECK(scan.totalReferences() == 6);

  // Both parents are named, which is the other half of what a real count buys: the report says
  // WHERE to go and fix it.
  std::vector<Noggit::MissingAsset> const failures (scan.failures(Noggit::AssetKind::Texture));

  auto const grass
    ( std::find_if( failures.begin(), failures.end()
                  , [] (Noggit::MissingAsset const& asset)
                    {
                      return asset.key == "tileset\\shared\\grass.blp";
                    }
                  )
    );

  REQUIRE(grass != failures.end());
  CHECK(grass->reference_count == 2);
  CHECK(grass->referrers.size() == 2);
}

TEST_CASE("one file naming the same texture twice is still one reference", "[assetdeps]")
{
  // The other side of the same rule, and the reason addReference deduplicates inside a single
  // parser: a WMO with 200 materials over 6 textures must not report 200 references from one file.
  std::vector<char> adt;
  appendChunk(adt, "MVER", std::vector<char>(4, '\0'));
  appendChunk(adt, "MTEX", stringBlock({"Tileset\\Shared\\Grass.blp", "TILESET\\shared\\grass.BLP"}));

  std::vector<Noggit::AssetReference> references;

  REQUIRE(Noggit::collectAdtReferences(adt.data(), adt.size(), references).complete());

  // grass.blp and its specular companion, once each, despite two spellings of the same name.
  CHECK(references.size() == 2);
}
