// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_ASSETDEPENDENCIES_HPP
#define NOGGIT_ASSETDEPENDENCIES_HPP

#include <noggit/AssetScan.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// What one file REFERENCES, read out of its bytes.
//
// This is deliberately not AssetScanCollector. That walk reads the loader's in-memory objects for
// LOADED tiles (AssetScan.hpp:522-533) and is unreachable from the project browser window, which
// owns Patch Client and has no World, no MapView and no GL context. It also never records the two
// links whose absence produces a model that loads in the editor and fails in game: a WMO's group
// files and an M2's .skin files. Neither name is stored anywhere -- WMOGroup::load composes
// "<root>_000.wmo" as a local and discards it (WMO.cpp:811-818), and Model::finishLoading does the
// same with "<base>00.skin" (Model.cpp:410-411).
//
// So the traversal is new and byte-level; the AGGREGATION half of AssetScan (normalizeAssetKey,
// AssetKind, AssetScanResult's dedup/ordering/reporting) is reused as-is.
//
// Everything here is pure: STL only, no Qt, no OpenGL, no BlizzardArchive. Callers hand it a
// buffer they obtained however they like. That is what makes it testable with synthetic bytes on a
// machine with no client install, the same split AssetScan.cpp uses.
namespace Noggit
{
  // Whether the client will break without this file.
  //
  // Optional exists because three of the names below are PROBED by the loaders rather than
  // required: external .anim files (Model.cpp:498 gates on exists()), a WMO's MOSB skybox
  // (WMO.cpp:433) and the "_s.blp" specular companion of a tileset texture
  // (TextureManager.cpp:491-501). Reporting every absent one as a broken reference would bury the
  // handful of names that really are missing under thousands that were never expected to be there.
  enum class ReferenceNecessity
  {
    Required,
    Optional
  };

  // One outgoing reference.
  struct AssetReference
  {
    // Canonical form: ASCII-lowercased, forward slashes, leading separators dropped, and .mdx/.mdl
    // rewritten to .m2 -- the same spelling BlizzardArchive::Listfile::FileKey stores, so it can be
    // handed straight to ClientFile.
    std::string path;
    AssetKind kind = AssetKind::Other;
    ReferenceNecessity necessity = ReferenceNecessity::Required;
    // False for files that reference nothing themselves, and -- the case that matters -- for WMO
    // GROUP files. A group file is a leaf: MOPY/MOVI/MOVT/MONR/MOTV/MOBA are raw arrays and its
    // materials are indices into the ROOT's MOMT, so it names no file at all. Opening one as a WMO
    // root would find no MOHD and report a parse failure for a perfectly good file.
    bool traversable = true;
  };

  // Canonical spelling of a reference as it appears in ADT/M2/WMO data.
  //
  // normalizeAssetKey does the lowercase and separator work (and avoids the ::tolower undefined
  // behaviour that ClientData::normalizeFilenameInternal has for bytes >= 0x80). This flips its
  // '\\' back to '/' to match FileKey, and applies the .mdx/.mdl -> .m2 rewrite ANCHORED AT THE
  // END. ClientData::normalizeFilenameInternal does that rewrite with
  // std::regex_replace(filename, std::regex(".mdx"), ".m2") where '.' matches ANY character and
  // every match is replaced, so "creature/amdxthing/foo.mdx" comes out as "creature/.m2thing/foo.m2".
  std::string normalizeReferencePath(std::string_view raw);

  // The same path as an MPQ STORED NAME: backslashes. MPQ resolves a file by hashing its name, so
  // what is stored has to be byte-for-byte what the client asks for.
  std::string archiveStoredName(std::string_view path);

  // Path with its final extension (and the dot) removed. Only the last component is considered, so
  // a directory containing a dot is safe.
  std::string stripExtension(std::string_view path);

  // "<base>NN.skin", NN zero padded to two digits. Model.cpp:410-411 builds index 00 this way and
  // throws the name away; the client needs every index below the header's nViews.
  std::string skinFileName(std::string_view model_path, unsigned view_index);

  // "<base>%04d-%02d.anim".
  //
  // NOT the spelling Model.cpp:497 emits. That one is unpadded ("...129-0.anim") and matches
  // nothing: grep for "-[0-9]\\.anim$" over dist/listfile/listfile.csv finds 0 entries while
  // "-0[0-9]\\.anim$" finds 38484, and creature/draeneiconstruct/draeneiconstruct0129-00.anim is a
  // real one. Returns an empty string for a negative id.
  std::string animFileName(std::string_view model_path, int animation_id, int sub_animation_id);

  // "<root>_NNN.wmo", NNN zero padded to three digits.
  //
  // The extension is stripped from the END, unlike WMO.cpp:817 which does
  // fname.insert(fname.find(".wmo"), ...) -- first occurrence, so a directory component containing
  // ".wmo" splices the index into the middle of the path.
  std::string wmoGroupFileName(std::string_view world_model_path, unsigned group_index);

  // True for a name shaped like a WMO group file, i.e. "_" plus at least three digits before
  // ".wmo". Used to keep group files out of the root parser.
  bool isWmoGroupFileName(std::string_view path);

  // "<base>_s.blp" for a path under "tileset/", empty otherwise. The 3.3.5 client loads a terrain
  // texture's specular companion, and MTEX lists only the base name (TextureManager.cpp:491-501).
  // The "_h.blp" height companion is deliberately not produced: TextureManager gates it on
  // modern_features, and a 3.3.5 client has no use for it.
  std::string tilesetSpecularName(std::string_view texture_path);

  // True for an archive file name that ships with a stock 3.3.5a client, i.e. one whose contents a
  // player already has. Matched by name because that is the only thing distinguishing the user's
  // own patch from Blizzard's -- both are loaded through the same patch-{number}/patch-{character}
  // templates (ClientData.hpp:210-240).
  //
  // Stock: common, common-2, lichking, expansion, base, patch, patch-2, patch-3, and the locale
  // archives (locale-, speech-, expansion-locale-, lichking-locale-, expansion-speech-,
  // lichking-speech-, patch-<locale>, patch-<locale>-2, patch-<locale>-3).
  //
  // NOT stock, and therefore treated as the user's own content: patch-4 .. patch-9, patch-a ..
  // patch-z, patch-<locale>-4 .. -9, alternate.MPQ, development.MPQ, and any directory patch.
  bool isBaseClientArchiveName(std::string_view archive_file_name);

  // Why a walk over one file's bytes stopped.
  //
  // The three-way split is the point. A caller that cannot tell "this is not that format at all"
  // from "the format was recognised and the data ran out half way" cannot tell its user which of
  // the two happened -- and a caller that ignores the answer entirely ships a patch missing every
  // file the unparsed one named while reporting that everything was fine.
  enum class ParseOutcome
  {
    // Every byte is accounted for. `out` is everything the file names.
    Complete,
    // The format was recognised and the data ran out: a chunk header declaring more bytes than the
    // buffer holds, a trailing fragment too short to be a chunk header, or a table whose entries
    // run past the end. Whatever landed in `out` is a PREFIX of the truth, never the whole of it,
    // and it is routinely non-empty -- which is what makes this the harder case to notice.
    Truncated,
    // Not this format at all: no MVER, no MD20, no MOHD. Nothing was collected.
    Unrecognised
  };

  // [[nodiscard]] on the TYPE, not on each function. Every one of the three collectors returns this
  // and the whole point of the type existing is that the answer must not be dropped on the floor;
  // putting the attribute here makes that true for anything added later as well.
  struct [[nodiscard]] AssetParseResult
  {
    ParseOutcome outcome = ParseOutcome::Unrecognised;
    // One clause, lowercase, no trailing full stop, so it reads after a file name in a report:
    // "world/maps/foo.adt: chunk MTEX at offset 12 declares 4096 bytes and only 18 remain".
    // Empty when Complete.
    std::string reason;

    bool complete() const;
    // Anything but Unrecognised, i.e. `out` holds at least a prefix of what the file names.
    bool recognised() const;
  };

  // Every file an ADT (or a WDT -- same chunk shapes for the blocks read here) names.
  //
  // Reads MTEX, MMDX and MWMO by walking the chunk sequence from byte 0 rather than through MHDR's
  // offset table, so a file whose MHDR disagrees with its own layout still yields what it lists.
  //
  // The string BLOCKS are taken whole; MDDF/MODF placements are not consulted. That over-includes
  // an MMDX/MWMO entry with no placement, which costs the archive a few bytes and cannot omit
  // anything -- the opposite trade to AssetScanCollector, which walks placements and would miss a
  // reference the client resolves from the name table.
  //
  // Unrecognised if the buffer does not begin with an MVER chunk. Truncated if the chunk sequence
  // runs off the end of the buffer -- which yields whatever the chunks BEFORE that point named, so
  // a non-empty `out` is not evidence that the file was read whole.
  AssetParseResult collectAdtReferences(char const* data, std::size_t size, std::vector<AssetReference>& out);

  // Every file an M2 names: its type-0 textures, its nViews .skin files, and the .anim files its
  // sequences may live in.
  //
  // Replaceable slots (type != 0) are skipped. They carry no filename in the file at all -- the
  // texture is supplied at runtime from a creature display id -- and Noggit substitutes its own
  // constant "tileset/generic/black.blp" for them (Model.cpp:356-364), which is an editor
  // placeholder and not a dependency of the map.
  //
  // Unrecognised if the buffer is not an MD20. Truncated if the header is shorter than the fields
  // read here, or if the texture or animation table runs past the end of the buffer.
  AssetParseResult collectModelReferences( std::string_view model_path
                                         , char const* data
                                         , std::size_t size
                                         , std::vector<AssetReference>& out
                                         );

  // Every file a WMO ROOT names: its nGroups group files, its MOTX textures, its MODN doodads and
  // its MOSB skybox.
  //
  // MOTX and MODN are NUL separated blobs addressed by byte offset from MOMT and MODD. Both are
  // read BOTH ways -- every string in the blob, plus every string at an offset a material or doodad
  // entry points at -- and the union is returned. Splitting the blob alone cannot miss a whole
  // string but does miss an offset that legitimately points mid-string; following offsets alone
  // misses nothing the client asks for but needs both chunks to be well formed. For a packer,
  // over-inclusion costs bytes and omission costs the user an evening.
  //
  // Unrecognised if the chunk sequence read whole contains no MOHD. Truncated if the sequence runs
  // off the end of the buffer -- whether or not MOHD was reached first, because a WMO root cut short
  // is the same hole either way.
  AssetParseResult collectWorldModelReferences( std::string_view world_model_path
                                              , char const* data
                                              , std::size_t size
                                              , std::vector<AssetReference>& out
                                              );
}

#endif // NOGGIT_ASSETDEPENDENCIES_HPP
