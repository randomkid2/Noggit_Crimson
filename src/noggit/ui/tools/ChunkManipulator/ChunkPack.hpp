// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKPACK_HPP
#define NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKPACK_HPP

#include <noggit/ui/tools/ChunkManipulator/ChunkData.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// THE CHUNK PACK: a copied block of terrain as a self-contained file.
//
// WHY IT IS THE HEADLINE AND NOT A CONVENIENCE. NoggitWindow holds exactly one MapView
// (NoggitWindow.hpp:166) and `new MapView` appears once in the whole tree (NoggitWindow.cpp:364),
// so there is no second viewport to paste into inside one process. "Copy in one editor window,
// paste in another" therefore is not an in-memory clipboard with a file export bolted on -- the
// file IS the mechanism, and an in-memory clipboard is the degenerate same-process case of it.
//
// WHAT TRAVELS, AND WHAT DOES NOT. The full list of what a chunk carries is on ChunkCache in
// ChunkData.hpp, member by member with the reason each one is or is not there. The four decisions
// that matter most for a pack specifically:
//
//   * Textures travel as FILE NAMES. A texture id is an index into the source ADT's MTEX table,
//     and the destination ADT has its own; an index is meaningless one tile away, let alone one
//     map away. This is the same reason Action::undo rebuilds its texture references from strings
//     (Action.cpp:88-97) rather than keeping the ids.
//   * Models and WMOs travel as a FileKey plus a transform relative to the copy pivot. A uid is
//     per-World and cannot cross a process.
//   * Terrain vertices travel as 145 HEIGHTS. The X and Z of a vertex are a function of the
//     destination chunk's own base (MapChunk.cpp:180-185); carrying them would carry the source
//     map's coordinates into the destination.
//   * Nothing derived travels: normals (World::recalc_norms), liquid vertex format
//     (liquid_layer::changeLiquidID), MH2O fishable/fatigue attributes
//     (ChunkWater::update_attributes), and the detail-doodad layer mapping
//     (TextureSet::updateDoodadMapping) are all recomputed at the destination.
//
// FAILURE IS ALL-OR-NOTHING BY CONSTRUCTION. decodeChunkPack builds a complete ChunkPack in a
// local and returns it only on full success; the clipboard is never touched by a failing read,
// so a truncated or corrupt file cannot leave a half-loaded clipboard for the next paste to
// smear over somebody's map. Every read goes through a bounds-checked cursor, the payload is
// length-stamped and hashed, and a version this build does not know is refused by number rather
// than guessed at.
namespace Noggit::Ui::Tools::ChunkManipulator
{
  //! Bumped whenever the payload layout changes in a way an older reader cannot parse.
  //!
  //! Version 1 is the first format there has ever been -- nothing in the tree serialised chunk
  //! data before it -- so there is no backwards compatibility to keep and the reader accepts
  //! exactly this one value.
  constexpr std::uint32_t CHUNK_PACK_VERSION = 1;

  //! Eight bytes so the file type is legible in a hex dump, ending in 0x1A ("end of file" under
  //! DOS) so that `type`ing one in a console stops instead of spraying binary.
  constexpr char CHUNK_PACK_MAGIC[8] = {'N', 'O', 'G', 'C', 'H', 'N', 'K', '\x1A'};

  //! Fixed header size in bytes. Asserted against the writer in ChunkPack.cpp.
  constexpr std::size_t CHUNK_PACK_HEADER_SIZE = 48;

  //! The longest string the reader will allocate for, in bytes.
  //!
  //! Not a format limit -- a defence. A corrupt length field is otherwise a request to allocate
  //! whatever four bytes of garbage say, and a Windows path is capped far below this anyway.
  constexpr std::uint32_t CHUNK_PACK_MAX_STRING = 4096;

  //! The most chunks one pack may describe.
  //!
  //! 1024 x 1024 is every chunk on a 64 x 64 ADT map, so this cannot refuse a legitimate pack.
  //! The competing tool's own status line shows 3101 chunks, well inside it.
  constexpr std::uint32_t CHUNK_PACK_MAX_CHUNKS = 1024u * 1024u;

  //! Every failure mode of the reader and the writer. The message is written for the status bar,
  //! not for a log: it says what is wrong with the file in one sentence.
  class ChunkPackError : public std::runtime_error
  {
  public:
    using std::runtime_error::runtime_error;
  };

  //! A decoded pack, or the material for one.
  struct ChunkPack
  {
    std::uint32_t version = CHUNK_PACK_VERSION;
    //! The copy flags the pack was made with, so an importer can tell "liquid was not copied"
    //! from "liquid was copied and there was none".
    ChunkCopyFlags copy_flags = ChunkCopyFlags::NONE;
    std::uint32_t source_map_id = 0;
    std::string source_map_name;
    //! World position of the copy pivot, for the status line and for a same-map paste that wants
    //! to land where it came from.
    glm::vec3 pivot {0.0f, 0.0f, 0.0f};
    std::vector<CachedChunk> chunks;
  };

  //! Serialise. Throws ChunkPackError only for a pack this format cannot express (too many
  //! chunks, a path longer than CHUNK_PACK_MAX_STRING).
  [[nodiscard]]
  std::vector<char> encodeChunkPack (ChunkPack const& pack);

  //! Parse. Throws ChunkPackError on anything at all wrong, and never returns a partial result.
  [[nodiscard]]
  ChunkPack decodeChunkPack (char const* data, std::size_t size);

  //! Write to disk, atomically enough that a reader never sees a half-written file: the bytes go
  //! to a sibling temporary and are renamed into place.
  void writeChunkPackFile (std::filesystem::path const& path, ChunkPack const& pack);

  //! Read from disk. Throws ChunkPackError if the file is missing, unreadable, or not a pack.
  [[nodiscard]]
  ChunkPack readChunkPackFile (std::filesystem::path const& path);

  //! The well-known path the live cross-window clipboard uses.
  //!
  //! A copy writes it and a second editor process reads it, which is how "copied in another
  //! window" works without either process knowing about the other. Lives in the user's temp
  //! directory, so it is per-user and does not survive a reboot -- both of which are the right
  //! lifetime for a clipboard.
  [[nodiscard]]
  std::filesystem::path sharedClipboardPackPath();
}

#endif // NOGGIT_UI_TOOLS_CHUNKMANIPULATOR_CHUNKPACK_HPP
