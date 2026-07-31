// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the DBC/MDDF model-name to .m2 rewrite.
//
// This is the whole reason the rewrite was pulled out of the resolver: the resolver needs client
// data and a loaded DBC to say anything, and this does not. Every malformed string a DBC string
// table can hand back is exercised here, at build time, on a machine with no client install.
//
// Several cases are written specifically against the three broken copies of this logic that
// already exist in the tree (ClientData::normalizeFilenameInternal, WMO.cpp:180,
// ObjectEditor.cpp:941). Where a case exists to pin a difference from one of those, the comment
// says which.

#include <catch2/catch_test_macros.hpp>

#include <noggit/database/ModelPathFixup.hpp>

#include <string>

using namespace Noggit::Database;

namespace
{
  std::string fixed(std::string const& input)
  {
    return ModelPathFixup::toM2Path(input);
  }

  // A name at exactly the length cap, and one byte over it, built rather than typed so the
  // boundary cannot drift out of step with MAX_MODEL_PATH_LENGTH.
  std::string nameOfLength(std::size_t length)
  {
    std::string const prefix = "Creature\\";
    std::string const suffix = ".mdx";

    REQUIRE(length > prefix.size() + suffix.size());

    std::string name (prefix);
    name.append(length - prefix.size() - suffix.size(), 'a');
    name += suffix;

    REQUIRE(name.size() == length);

    return name;
  }
}

TEST_CASE("mdx and mdl are rewritten to m2", "[modelpath]")
{
  CHECK(fixed("Creature\\Rat\\Rat.mdx") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature\\Rat\\Rat.mdl") == "Creature\\Rat\\Rat.m2");

  // Only the extension is touched. The stem keeps its case, because folding it is the archive
  // layer's job and doing it here would mean ::tolower on possibly non-ASCII bytes.
  CHECK(fixed("Creature\\Rat\\Rat.mdx") != "creature\\rat\\rat.m2");
}

TEST_CASE("extension case is irrelevant", "[modelpath]")
{
  // ModelManager.cpp's rfind(".mdx") is case-SENSITIVE and would leave every one of these
  // untouched, producing a key the MPQ chain cannot open.
  CHECK(fixed("Creature\\Rat\\Rat.MDX") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature\\Rat\\Rat.MdX") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature\\Rat\\Rat.mDl") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature\\Rat\\Rat.MDL") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature\\Rat\\Rat.M2") == "Creature\\Rat\\Rat.m2");
}

TEST_CASE("an existing m2 extension survives", "[modelpath]")
{
  CHECK(fixed("Creature\\Rat\\Rat.m2") == "Creature\\Rat\\Rat.m2");

  // Idempotence, asserted directly. The resolver caches results and the tile layer may re-fix a
  // path it already fixed; a second pass must be a no-op.
  std::string const once (fixed("Creature\\Rat\\Rat.mdx"));
  CHECK(fixed(once) == once);
}

TEST_CASE("an extension inside a directory name is not an extension", "[modelpath]")
{
  // The case that breaks ClientData::normalizeFilenameInternal. Its
  // regex_replace(path, regex(".mdx"), ".m2") matches the unescaped '.' against the 'a' of
  // "amdx" and yields "World\\.m2\\foo.m2" -- a destroyed directory name. WMO.cpp:180 does the
  // same thing with a literal "mdx" search and no anchor at all.
  CHECK(fixed("World\\amdx\\foo.mdx") == "World\\amdx\\foo.m2");
  CHECK(fixed("World\\mdx\\foo.mdx") == "World\\mdx\\foo.m2");
  CHECK(fixed("World\\mdl\\mdx\\m2\\foo.mdl") == "World\\mdl\\mdx\\m2\\foo.m2");

  // A dot in a DIRECTORY name is not an extension either, so the final component decides. Here
  // it has no extension at all, so there is nothing to rewrite and no model to name.
  CHECK(fixed("World\\thing.mdx\\foo").empty());
  CHECK(fixed("World\\v1.2\\foo").empty());

  // ... but a valid final component is unaffected by the dotted directory above it.
  CHECK(fixed("World\\thing.mdx\\foo.mdx") == "World\\thing.mdx\\foo.m2");
  CHECK(fixed("World\\v1.2\\foo.m2") == "World\\v1.2\\foo.m2");
}

TEST_CASE("forward slashes become backslashes", "[modelpath]")
{
  CHECK(fixed("Creature/Rat/Rat.mdx") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature/Rat\\Rat.mdx") == "Creature\\Rat\\Rat.m2");
}

TEST_CASE("duplicated separators collapse", "[modelpath]")
{
  CHECK(fixed("Creature\\\\Rat\\Rat.mdx") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature//Rat//Rat.mdx") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature/\\Rat\\///Rat.mdx") == "Creature\\Rat\\Rat.m2");

  // A leading separator run disappears entirely: MPQ keys are relative and a leading separator
  // makes every lookup miss.
  CHECK(fixed("\\Creature\\Rat\\Rat.mdx") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("//Creature/Rat/Rat.mdx") == "Creature\\Rat\\Rat.m2");
}

TEST_CASE("surrounding whitespace is trimmed", "[modelpath]")
{
  CHECK(fixed("  Creature\\Rat\\Rat.mdx") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("Creature\\Rat\\Rat.mdx  ") == "Creature\\Rat\\Rat.m2");
  CHECK(fixed("\t Creature\\Rat\\Rat.mdx \r\n") == "Creature\\Rat\\Rat.m2");

  // Interior whitespace is NOT touched. Real WoW model paths contain spaces
  // ("Creature\\Frost Wyrm\\...") and stripping them would break every one of them.
  CHECK(fixed("Creature\\Frost Wyrm\\FrostWyrm.mdx") == "Creature\\Frost Wyrm\\FrostWyrm.m2");
}

TEST_CASE("input that cannot be a model path yields an empty string", "[modelpath]")
{
  SECTION("empty and whitespace-only")
  {
    CHECK(fixed("").empty());
    CHECK(fixed("   ").empty());
    CHECK(fixed("\t\r\n").empty());
  }

  SECTION("separators only")
  {
    CHECK(fixed("\\").empty());
    CHECK(fixed("///").empty());
  }

  SECTION("no extension at all")
  {
    CHECK(fixed("Rat").empty());
    CHECK(fixed("Creature\\Rat\\Rat").empty());
    CHECK(fixed("m2").empty());
    CHECK(fixed("mdx").empty());
  }

  SECTION("a lone extension names nothing")
  {
    CHECK(fixed(".mdx").empty());
    CHECK(fixed(".mdl").empty());
    CHECK(fixed(".m2").empty());
    CHECK(fixed("Creature\\Rat\\.mdx").empty());
  }

  SECTION("a trailing separator is a directory")
  {
    CHECK(fixed("Creature\\Rat\\").empty());
    CHECK(fixed("Creature\\Rat.mdx\\").empty());
  }

  SECTION("an extension that is not a model")
  {
    // .wmo especially: it is a different loader, and guessing it into a .m2 key would produce a
    // missing-file error blamed on the model loader instead of on the caller.
    CHECK(fixed("World\\wmo\\Azeroth\\Buildings\\Human_Farm\\Farm.wmo").empty());
    CHECK(fixed("Creature\\Rat\\Rat.blp").empty());
    CHECK(fixed("Creature\\Rat\\Rat.m3").empty());
    CHECK(fixed("Creature\\Rat\\Rat.mdxx").empty());
    CHECK(fixed("Creature\\Rat\\Rat.md").empty());
  }

  SECTION("dot components, which escape the project directory once joined with fs::path")
  {
    CHECK(fixed("..\\..\\Windows\\System32\\evil.m2").empty());
    CHECK(fixed("Creature\\..\\Rat.mdx").empty());
    CHECK(fixed("Creature\\.\\Rat.mdx").empty());
    CHECK(fixed(".\\Rat.mdx").empty());

    // A component that merely STARTS with dots is fine -- only exactly "." and ".." are refused.
    CHECK(fixed("Creature\\..Rat\\Rat.mdx") == "Creature\\..Rat\\Rat.m2");
  }

  SECTION("bytes that cannot occur in a model path")
  {
    // A control byte means getString resolved a bogus offset and returned a pointer into record
    // data rather than into the string table. Refusing is how that becomes a named failure.
    CHECK(fixed(std::string("Creature\\Rat\\Rat.mdx\x01")).empty());
    CHECK(fixed(std::string("Creature\\R\x07t\\Rat.mdx")).empty());
    CHECK(fixed(std::string("Creature\\Rat\\Rat\x7F.mdx")).empty());

    // An embedded NUL, which is what a std::string built from an over-long unterminated read
    // looks like.
    CHECK(fixed(std::string("Creature\\Rat\\Rat.mdx\0junk", 25)).empty());

    // A colon makes the key absolute or names an alternate data stream. fs::path's operator/
    // discards the project directory when the right-hand side is absolute.
    CHECK(fixed("C:\\Users\\somebody\\Rat.mdx").empty());
    CHECK(fixed("Creature\\Rat\\Rat.mdx:stream").empty());
  }
}

TEST_CASE("length is capped at MAX_MODEL_PATH_LENGTH", "[modelpath]")
{
  std::string const at_cap (nameOfLength(ModelPathFixup::MAX_MODEL_PATH_LENGTH));
  std::string const over_cap (nameOfLength(ModelPathFixup::MAX_MODEL_PATH_LENGTH + 1));

  CHECK_FALSE(fixed(at_cap).empty());
  CHECK(fixed(over_cap).empty());

  // Padding must not be what pushes a name over the cap, since the length test happens after
  // trimming.
  CHECK(fixed("   " + at_cap + "   ") == fixed(at_cap));
}

TEST_CASE("looksLikeModelPath agrees with toM2Path on every input", "[modelpath]")
{
  // Asserted as an invariant over a list rather than case by case, because the failure mode that
  // matters is the two DISAGREEING -- a path that passes the check and then resolves to nothing.
  std::string const inputs[] =
    { "Creature\\Rat\\Rat.mdx"
    , "Creature\\Rat\\Rat.MDL"
    , "Creature\\Rat\\Rat.m2"
    , "creature/rat/rat.mdx"
    , "  \\\\Creature//Rat\\\\Rat.MDX  "
    , ""
    , "   "
    , "\\"
    , "Rat"
    , ".mdx"
    , "Creature\\Rat\\"
    , "Creature\\Rat\\Rat.wmo"
    , "..\\Rat.mdx"
    , "C:\\Rat.mdx"
    , "World\\thing.mdx\\foo"
    };

  for (std::string const& input : inputs)
  {
    CHECK(ModelPathFixup::looksLikeModelPath(input) == !fixed(input).empty());
  }

  // And a spot check that the list is not vacuous in either direction.
  CHECK(ModelPathFixup::looksLikeModelPath("Creature\\Rat\\Rat.mdx"));
  CHECK_FALSE(ModelPathFixup::looksLikeModelPath("Creature\\Rat\\Rat.wmo"));
}

TEST_CASE("real 3.3.5 CreatureModelData and GameObjectDisplayInfo names", "[modelpath]")
{
  // Shapes taken from actual WotLK DBC rows, since the synthetic cases above all look alike and
  // would not catch a rule that only breaks on a deeply nested or space-bearing real name.
  CHECK(fixed("Creature\\Chicken\\Chicken.mdx") == "Creature\\Chicken\\Chicken.m2");
  CHECK(fixed("Creature\\ArthasLichKing\\ArthasLichKing.mdx")
     == "Creature\\ArthasLichKing\\ArthasLichKing.m2");
  CHECK(fixed("World\\Generic\\PassiveDoodads\\Chairs\\HumanChair01.mdx")
     == "World\\Generic\\PassiveDoodads\\Chairs\\HumanChair01.m2");
  CHECK(fixed("World\\Azeroth\\Elwynn\\PassiveDoodads\\Signs\\ElwynnSign01.mdx")
     == "World\\Azeroth\\Elwynn\\PassiveDoodads\\Signs\\ElwynnSign01.m2");

  // GameObjectDisplayInfo.ModelName is the one field in this chain that legitimately holds .mdl
  // as well as .mdx.
  CHECK(fixed("World\\Generic\\Human\\Passive Doodads\\Chests\\HumanChest01.mdl")
     == "World\\Generic\\Human\\Passive Doodads\\Chests\\HumanChest01.m2");
}
