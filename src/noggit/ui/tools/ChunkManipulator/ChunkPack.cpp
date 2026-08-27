// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ChunkPack.hpp"

#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>

#include <algorithm>
#include <cstring>
#include <fstream>

using namespace Noggit::Ui::Tools::ChunkManipulator;

namespace
{
  // THE HEADER, byte for byte. Little-endian throughout, which is what every machine this
  // editor builds for is; a big-endian port would have to byte-swap here and nowhere else,
  // because every multi-byte value in the payload goes through the same two primitives below.
  //
  //     0   char[8]  magic                CHUNK_PACK_MAGIC
  //     8   u32      version              CHUNK_PACK_VERSION
  //    12   u32      copy_flags           the ChunkCopyFlags the pack was captured with
  //    16   u32      chunk_count
  //    20   u32      source_map_id
  //    24   f32[3]   pivot                world position of the copy pivot
  //    36   u32      payload_bytes        length of everything after byte 48
  //    40   u32      payload_hash         FNV-1a 32 of those bytes
  //    44   u32      reserved             must be zero
  //    48                                 payload starts
  //
  // 8 + 4 + 4 + 4 + 4 + 12 + 4 + 4 + 4 = 48, which is what CHUNK_PACK_HEADER_SIZE says.
  static_assert (CHUNK_PACK_HEADER_SIZE == 48, "header layout and constant disagree");

  constexpr std::size_t OFFSET_MAGIC = 0;
  constexpr std::size_t OFFSET_VERSION = 8;
  constexpr std::size_t OFFSET_COPY_FLAGS = 12;
  constexpr std::size_t OFFSET_CHUNK_COUNT = 16;
  constexpr std::size_t OFFSET_MAP_ID = 20;
  constexpr std::size_t OFFSET_PIVOT = 24;
  constexpr std::size_t OFFSET_PAYLOAD_BYTES = 36;
  constexpr std::size_t OFFSET_PAYLOAD_HASH = 40;
  constexpr std::size_t OFFSET_RESERVED = 44;

  //! FNV-1a, 32 bit. Chosen because it is eight lines with no table and no dependency; this is a
  //! corruption check on a local file, not a signature, and it is not asked to be one.
  std::uint32_t fnv1a32 (char const* data, std::size_t size)
  {
    std::uint32_t hash (2166136261u);

    for (std::size_t i (0); i < size; ++i)
    {
      hash ^= static_cast<std::uint8_t> (data[i]);
      hash *= 16777619u;
    }

    return hash;
  }

  // ---- writing ------------------------------------------------------------------------------

  struct Writer
  {
    std::vector<char> bytes;

    void raw (void const* source, std::size_t size)
    {
      auto const* const begin (static_cast<char const*> (source));
      bytes.insert (bytes.end(), begin, begin + size);
    }

    void u8 (std::uint8_t value) { raw (&value, sizeof (value)); }
    void u32 (std::uint32_t value) { raw (&value, sizeof (value)); }
    void u64 (std::uint64_t value) { raw (&value, sizeof (value)); }
    void i32 (std::int32_t value) { raw (&value, sizeof (value)); }
    void f32 (float value) { raw (&value, sizeof (value)); }

    void vec3 (glm::vec3 const& value)
    {
      f32 (value.x);
      f32 (value.y);
      f32 (value.z);
    }

    void string (std::string const& value)
    {
      if (value.size() > CHUNK_PACK_MAX_STRING)
      {
        throw ChunkPackError ("A texture or model path in this selection is longer than "
                              "the pack format allows (4096 bytes) and cannot be exported.");
      }

      u32 (static_cast<std::uint32_t> (value.size()));
      raw (value.data(), value.size());
    }

    template<typename T, std::size_t N>
    void array (std::array<T, N> const& value)
    {
      raw (value.data(), sizeof (T) * N);
    }
  };

  // ---- reading ------------------------------------------------------------------------------

  //! A bounds-checked cursor. Every single read in decodeChunkPack goes through it, which is
  //! what makes "truncated" a clean error message instead of a read past the end of the buffer.
  struct Reader
  {
    char const* data;
    std::size_t size;
    std::size_t at = 0;

    void raw (void* destination, std::size_t count)
    {
      if (count > size - at)
      {
        throw ChunkPackError ("The chunk pack ends in the middle of a record -- the file is "
                              "truncated or was not fully written.");
      }

      std::memcpy (destination, data + at, count);
      at += count;
    }

    std::uint8_t u8() { std::uint8_t v; raw (&v, sizeof (v)); return v; }
    std::uint32_t u32() { std::uint32_t v; raw (&v, sizeof (v)); return v; }
    std::uint64_t u64() { std::uint64_t v; raw (&v, sizeof (v)); return v; }
    std::int32_t i32() { std::int32_t v; raw (&v, sizeof (v)); return v; }
    float f32() { float v; raw (&v, sizeof (v)); return v; }

    glm::vec3 vec3()
    {
      float const x (f32());
      float const y (f32());
      float const z (f32());
      return {x, y, z};
    }

    std::string string()
    {
      std::uint32_t const length (u32());

      if (length > CHUNK_PACK_MAX_STRING)
      {
        throw ChunkPackError ("The chunk pack declares a string longer than the format allows; "
                              "the file is corrupt.");
      }

      std::string value (length, '\0');
      raw (value.data(), length);
      return value;
    }

    template<typename T, std::size_t N>
    void array (std::array<T, N>& value)
    {
      raw (value.data(), sizeof (T) * N);
    }
  };

  // ---- payload, one chunk at a time ----------------------------------------------------------
  //
  // The present mask reuses the ChunkCopyFlags bit values, so a block's presence bit and the
  // check box that produced it are literally the same number and cannot drift. Blocks are always
  // written in the order below and the reader walks the same order, which is why no block needs
  // a length of its own.

  void writeChunk (Writer& out, CachedChunk const& chunk)
  {
    ChunkCache const& data (chunk.data);

    std::uint32_t present (0);
    auto const mark ([&present] (ChunkCopyFlags flag) { present |= static_cast<std::uint32_t> (flag); });

    if (data.terrain_height)  mark (ChunkCopyFlags::TERRAIN);
    if (data.vertex_colors)   mark (ChunkCopyFlags::VERTEX_COLORS);
    if (data.shadows)         mark (ChunkCopyFlags::SHADOWS);
    if (data.liquid_layers)   mark (ChunkCopyFlags::LIQUID);
    if (data.textures)        mark (ChunkCopyFlags::TEXTURES);
    if (data.alphamaps)       mark (ChunkCopyFlags::ALPHAMAPS);
    if (data.layers_info)     mark (ChunkCopyFlags::GROUND_EFFECT_IDS);
    if (data.doodad_stencil)  mark (ChunkCopyFlags::GROUND_EFFECT_EXCLUSION);
    // ONE bit for both object classes, and it is MODELS. M2s and WMOs share a block because each
    // entry already carries its own ChunkManipulatorObjectTypes tag, so a second presence bit
    // would only be able to disagree with the tags. Which classes were WANTED is in the pack
    // header's copy_flags; which were FOUND is in the entries.
    if (data.objects)         mark (ChunkCopyFlags::MODELS);
    if (data.holes)           mark (ChunkCopyFlags::HOLES);
    if (data.flags)           mark (ChunkCopyFlags::FLAGS);
    if (data.area_id)         mark (ChunkCopyFlags::AREA_ID);

    out.i32 (chunk.rel_x);
    out.i32 (chunk.rel_z);
    out.i32 (chunk.source.globalX());
    out.i32 (chunk.source.globalZ());
    out.u32 (present);

    if (data.terrain_height)
    {
      out.array (*data.terrain_height);
    }

    if (data.vertex_colors)
    {
      out.array (data.vertex_colors->colors);
      out.u8 (data.vertex_colors->has_mccv_runtime ? 1 : 0);
      out.u8 (data.vertex_colors->has_mccv_header ? 1 : 0);
    }

    if (data.shadows)
    {
      out.array (*data.shadows);
    }

    if (data.liquid_layers)
    {
      out.u32 (static_cast<std::uint32_t> (data.liquid_layers->size()));

      for (ChunkLiquidLayerCache const& layer : *data.liquid_layers)
      {
        out.i32 (layer.liquid_id);
        out.u64 (layer.subchunks);
        out.array (layer.height);
        out.array (layer.depth);

        for (glm::vec2 const& uv : layer.uv)
        {
          out.f32 (uv.x);
          out.f32 (uv.y);
        }
      }
    }

    if (data.textures)
    {
      out.u32 (static_cast<std::uint32_t> (data.textures->n_textures));
      out.u32 (static_cast<std::uint32_t> (data.textures->textures.size()));

      for (std::string const& name : data.textures->textures)
      {
        out.string (name);
      }
    }

    if (data.alphamaps)
    {
      for (std::size_t i (0); i < MAX_ALPHAMAPS; ++i)
      {
        out.u8 (data.alphamaps->present[i] ? 1 : 0);

        if (data.alphamaps->present[i])
        {
          out.array (data.alphamaps->maps[i]);
        }
      }

      out.u8 (data.alphamaps->tmp_edit_values ? 1 : 0);

      if (data.alphamaps->tmp_edit_values)
      {
        for (std::array<float, 64 * 64> const& layer : *data.alphamaps->tmp_edit_values)
        {
          out.array (layer);
        }
      }
    }

    if (data.layers_info)
    {
      for (layer_info const& info : *data.layers_info)
      {
        out.u32 (info.flags);
        out.u32 (info.effectID);
      }
    }

    if (data.doodad_stencil)
    {
      out.array (*data.doodad_stencil);
    }

    if (data.objects)
    {
      out.u32 (static_cast<std::uint32_t> (data.objects->size()));

      for (ChunkObjectCacheEntry const& object : *data.objects)
      {
        out.u8 (static_cast<std::uint8_t> (object.type));
        // A FileKey can carry a path, a FileDataID, or both (Listfile.hpp:50-60). 3.3.5 has no
        // FileDataIDs, but writing both costs four bytes and means a pack made by a future
        // build that does use them survives the round trip.
        out.string (object.file_key.hasFilepath() ? object.file_key.filepath() : std::string());
        out.u32 (object.file_key.hasFileDataID() ? object.file_key.fileDataID() : 0u);
        out.vec3 (object.pos);
        out.vec3 (object.dir);
        out.f32 (object.scale);
      }
    }

    if (data.holes)
    {
      out.i32 (*data.holes);
    }

    if (data.flags)
    {
      out.u32 (data.flags->value);
    }

    if (data.area_id)
    {
      out.u32 (*data.area_id);
    }
  }

  CachedChunk readChunk (Reader& in)
  {
    CachedChunk chunk;
    ChunkCache& data (chunk.data);

    chunk.rel_x = in.i32();
    chunk.rel_z = in.i32();

    std::int32_t const source_global_x (in.i32());
    std::int32_t const source_global_z (in.i32());
    chunk.source = SelectedChunkIndex::fromGlobal (source_global_x, source_global_z);

    std::uint32_t const present (in.u32());
    auto const has ([present] (ChunkCopyFlags flag)
                    { return (present & static_cast<std::uint32_t> (flag)) != 0; });

    if (has (ChunkCopyFlags::TERRAIN))
    {
      std::array<float, 145> heights {};
      in.array (heights);
      data.terrain_height = heights;
    }

    if (has (ChunkCopyFlags::VERTEX_COLORS))
    {
      ChunkVertexColorCache colors;
      in.array (colors.colors);
      colors.has_mccv_runtime = in.u8() != 0;
      colors.has_mccv_header = in.u8() != 0;
      data.vertex_colors = colors;
    }

    if (has (ChunkCopyFlags::SHADOWS))
    {
      std::array<std::uint8_t, 64 * 64> shadows {};
      in.array (shadows);
      data.shadows = shadows;
    }

    if (has (ChunkCopyFlags::LIQUID))
    {
      std::uint32_t const layer_count (in.u32());

      // A layer is 81 * (1 + 1 + 2) floats plus 12 bytes of header, so a count larger than the
      // bytes left cannot be honest. The cursor would catch it anyway, one read at a time, but
      // refusing before reserving is what stops a corrupt count from asking for a gigabyte.
      if (layer_count > 64)
      {
        throw ChunkPackError ("The chunk pack declares an impossible number of liquid layers on "
                              "one chunk; the file is corrupt.");
      }

      std::vector<ChunkLiquidLayerCache> layers (layer_count);

      for (ChunkLiquidLayerCache& layer : layers)
      {
        layer.liquid_id = in.i32();
        layer.subchunks = in.u64();
        in.array (layer.height);
        in.array (layer.depth);

        for (glm::vec2& uv : layer.uv)
        {
          uv.x = in.f32();
          uv.y = in.f32();
        }
      }

      data.liquid_layers = std::move (layers);
    }

    if (has (ChunkCopyFlags::TEXTURES))
    {
      ChunkTextureCache textures;
      textures.n_textures = in.u32();
      std::uint32_t const name_count (in.u32());

      if (textures.n_textures > 4 || name_count > 4)
      {
        throw ChunkPackError ("The chunk pack declares more than four texture layers on one "
                              "chunk, which no ADT can have; the file is corrupt.");
      }

      textures.textures.reserve (name_count);

      for (std::uint32_t i (0); i < name_count; ++i)
      {
        textures.textures.emplace_back (in.string());
      }

      data.textures = std::move (textures);
    }

    if (has (ChunkCopyFlags::ALPHAMAPS))
    {
      ChunkAlphamapCache alphamaps;

      for (std::size_t i (0); i < MAX_ALPHAMAPS; ++i)
      {
        alphamaps.present[i] = in.u8() != 0;

        if (alphamaps.present[i])
        {
          in.array (alphamaps.maps[i]);
        }
      }

      if (in.u8() != 0)
      {
        std::array<std::array<float, 64 * 64>, 4> temporary {};

        for (std::array<float, 64 * 64>& layer : temporary)
        {
          in.array (layer);
        }

        alphamaps.tmp_edit_values = temporary;
      }

      data.alphamaps = std::move (alphamaps);
    }

    if (has (ChunkCopyFlags::GROUND_EFFECT_IDS))
    {
      std::array<layer_info, 4> infos {};

      for (layer_info& info : infos)
      {
        info.flags = in.u32();
        info.effectID = in.u32();
      }

      data.layers_info = infos;
    }

    if (has (ChunkCopyFlags::GROUND_EFFECT_EXCLUSION))
    {
      std::array<std::uint8_t, 8> stencil {};
      in.array (stencil);
      data.doodad_stencil = stencil;
    }

    if (has (ChunkCopyFlags::MODELS))
    {
      std::uint32_t const object_count (in.u32());

      // The smallest an object record can be is 37 bytes: 1 type + 4 path length + 0 path
      // + 4 file id + 12 position + 12 direction + 4 scale. A count that needs more bytes than
      // the payload has left cannot be honest, and refusing here is what stops a corrupt count
      // from reserving gigabytes before the cursor gets a chance to complain.
      if (object_count > (in.size - in.at) / 37)
      {
        throw ChunkPackError ("The chunk pack declares more objects on one chunk than the file "
                              "has room for; the file is corrupt.");
      }

      std::vector<ChunkObjectCacheEntry> objects;
      objects.reserve (object_count);

      for (std::uint32_t i (0); i < object_count; ++i)
      {
        ChunkObjectCacheEntry object;

        std::uint8_t const type (in.u8());

        if (type > static_cast<std::uint8_t> (ChunkManipulatorObjectTypes::WMO))
        {
          throw ChunkPackError ("The chunk pack names an object type this build does not know; "
                                "the file is corrupt.");
        }

        object.type = static_cast<ChunkManipulatorObjectTypes> (type);

        std::string const path (in.string());
        std::uint32_t const file_data_id (in.u32());

        if (path.empty() && file_data_id == 0)
        {
          throw ChunkPackError ("The chunk pack carries an object with neither a file path nor a "
                                "file id; the file is corrupt.");
        }

        object.file_key = path.empty()
          ? BlizzardArchive::Listfile::FileKey (file_data_id)
          : BlizzardArchive::Listfile::FileKey (path, file_data_id);

        object.pos = in.vec3();
        object.dir = in.vec3();
        object.scale = in.f32();

        objects.emplace_back (std::move (object));
      }

      data.objects = std::move (objects);
    }

    if (has (ChunkCopyFlags::HOLES))
    {
      data.holes = in.i32();
    }

    if (has (ChunkCopyFlags::FLAGS))
    {
      mcnk_flags flags;
      flags.value = in.u32();
      data.flags = flags;
    }

    if (has (ChunkCopyFlags::AREA_ID))
    {
      data.area_id = in.u32();
    }

    return chunk;
  }
}

namespace Noggit::Ui::Tools::ChunkManipulator
{
  std::vector<char> encodeChunkPack (ChunkPack const& pack)
  {
    if (pack.chunks.size() > CHUNK_PACK_MAX_CHUNKS)
    {
      throw ChunkPackError ("This selection has more chunks than a pack can describe.");
    }

    Writer payload;
    payload.string (pack.source_map_name);

    for (CachedChunk const& chunk : pack.chunks)
    {
      writeChunk (payload, chunk);
    }

    std::vector<char> file;
    file.resize (CHUNK_PACK_HEADER_SIZE);

    std::memcpy (file.data() + OFFSET_MAGIC, CHUNK_PACK_MAGIC, sizeof (CHUNK_PACK_MAGIC));

    auto const put_u32
      ( [&file] (std::size_t offset, std::uint32_t value)
        { std::memcpy (file.data() + offset, &value, sizeof (value)); }
      );

    put_u32 (OFFSET_VERSION, CHUNK_PACK_VERSION);
    put_u32 (OFFSET_COPY_FLAGS, static_cast<std::uint32_t> (pack.copy_flags));
    put_u32 (OFFSET_CHUNK_COUNT, static_cast<std::uint32_t> (pack.chunks.size()));
    put_u32 (OFFSET_MAP_ID, pack.source_map_id);
    std::memcpy (file.data() + OFFSET_PIVOT, &pack.pivot, sizeof (float) * 3);
    put_u32 (OFFSET_PAYLOAD_BYTES, static_cast<std::uint32_t> (payload.bytes.size()));
    put_u32 (OFFSET_PAYLOAD_HASH, fnv1a32 (payload.bytes.data(), payload.bytes.size()));
    put_u32 (OFFSET_RESERVED, 0);

    file.insert (file.end(), payload.bytes.begin(), payload.bytes.end());
    return file;
  }

  ChunkPack decodeChunkPack (char const* data, std::size_t size)
  {
    if (size < CHUNK_PACK_HEADER_SIZE)
    {
      throw ChunkPackError ("This file is too short to be a chunk pack.");
    }

    if (std::memcmp (data + OFFSET_MAGIC, CHUNK_PACK_MAGIC, sizeof (CHUNK_PACK_MAGIC)) != 0)
    {
      throw ChunkPackError ("This is not a Noggit chunk pack.");
    }

    auto const get_u32
      ( [data] (std::size_t offset)
        {
          std::uint32_t value;
          std::memcpy (&value, data + offset, sizeof (value));
          return value;
        }
      );

    std::uint32_t const version (get_u32 (OFFSET_VERSION));

    // Refused by NUMBER, loudly. A pack from a future build may look parseable for a while and
    // then stop, and half a pasted map is far worse than a refused paste.
    if (version != CHUNK_PACK_VERSION)
    {
      throw ChunkPackError ("This chunk pack is version " + std::to_string (version)
                            + "; this build of Noggit reads version "
                            + std::to_string (CHUNK_PACK_VERSION) + " only.");
    }

    std::uint32_t const payload_bytes (get_u32 (OFFSET_PAYLOAD_BYTES));

    // Exact, not "at least". A short payload is a truncated file; a long one is a file with
    // something appended to it, and neither is something to paste over a map.
    if (payload_bytes != size - CHUNK_PACK_HEADER_SIZE)
    {
      throw ChunkPackError ("This chunk pack says it holds "
                            + std::to_string (payload_bytes) + " bytes of data but the file has "
                            + std::to_string (size - CHUNK_PACK_HEADER_SIZE)
                            + "; it is truncated or has been appended to.");
    }

    if (fnv1a32 (data + CHUNK_PACK_HEADER_SIZE, payload_bytes) != get_u32 (OFFSET_PAYLOAD_HASH))
    {
      throw ChunkPackError ("This chunk pack fails its own checksum; the file is corrupt.");
    }

    std::uint32_t const chunk_count (get_u32 (OFFSET_CHUNK_COUNT));

    if (chunk_count > CHUNK_PACK_MAX_CHUNKS)
    {
      throw ChunkPackError ("This chunk pack claims more chunks than a map has.");
    }

    ChunkPack pack;
    pack.version = version;
    pack.copy_flags = static_cast<ChunkCopyFlags> (get_u32 (OFFSET_COPY_FLAGS));
    pack.source_map_id = get_u32 (OFFSET_MAP_ID);
    std::memcpy (&pack.pivot, data + OFFSET_PIVOT, sizeof (float) * 3);

    Reader in {data + CHUNK_PACK_HEADER_SIZE, payload_bytes, 0};
    pack.source_map_name = in.string();
    pack.chunks.reserve (std::min<std::size_t> (chunk_count, 4096));

    for (std::uint32_t i (0); i < chunk_count; ++i)
    {
      pack.chunks.emplace_back (readChunk (in));
    }

    // Trailing bytes inside a payload that passed its length and hash check mean the writer and
    // the reader disagree about the layout, which is a bug rather than a corrupt file -- and
    // exactly the bug a version bump exists to prevent. Say so rather than paste the prefix.
    if (in.at != in.size)
    {
      throw ChunkPackError ("This chunk pack has " + std::to_string (in.size - in.at)
                            + " bytes left over after its last chunk; it does not match the "
                              "format this build expects.");
    }

    return pack;
  }

  void writeChunkPackFile (std::filesystem::path const& path, ChunkPack const& pack)
  {
    std::vector<char> const bytes (encodeChunkPack (pack));

    // Written to a sibling and renamed, so that a second editor watching the shared clipboard
    // path can never open a file that is still being written. std::filesystem::rename is a
    // replace on Windows for an existing destination.
    std::filesystem::path temporary (path);
    temporary += ".part";

    {
      std::ofstream stream (temporary, std::ios::binary | std::ios::trunc);

      if (!stream)
      {
        throw ChunkPackError ("Could not open " + temporary.string() + " for writing.");
      }

      stream.write (bytes.data(), static_cast<std::streamsize> (bytes.size()));

      if (!stream)
      {
        throw ChunkPackError ("Could not write " + temporary.string() + ".");
      }
    }

    std::error_code error;
    std::filesystem::rename (temporary, path, error);

    if (error)
    {
      std::filesystem::remove (temporary, error);
      throw ChunkPackError ("Could not move the finished pack into place at " + path.string() + ".");
    }
  }

  ChunkPack readChunkPackFile (std::filesystem::path const& path)
  {
    std::ifstream stream (path, std::ios::binary | std::ios::ate);

    if (!stream)
    {
      throw ChunkPackError ("Could not open " + path.string() + ".");
    }

    std::streamoff const size (stream.tellg());

    if (size < 0)
    {
      throw ChunkPackError ("Could not measure " + path.string() + ".");
    }

    // 4 GiB, and the number is not arbitrary: payload_bytes is a u32, so the largest payload the
    // header can even describe is 4,294,967,295 bytes and a longer file could not be a valid pack
    // whatever it contained. This cap exists so that pointing Import at a DVD image fails on the
    // size rather than on the allocation.
    //
    // It is NOT "every chunk on a map with room to spare". A fully populated chunk encodes to
    // 12,077 bytes (the arithmetic is in ChunkClipboard::publishSharedClipboard), so 4 GiB is
    // about 355,000 such chunks against the 1,048,576 CHUNK_PACK_MAX_CHUNKS allows -- a pack of a
    // whole 64 x 64 map with everything ticked would be roughly 12.7 GB and this format cannot
    // express it. Nothing in the editor can produce one either: it would need the entire map
    // resident and selected. Should that ever change, the payload length field is what has to
    // grow, and growing it is a version bump.
    constexpr std::streamoff MAX_PACK_BYTES (4ll * 1024 * 1024 * 1024);

    if (size > MAX_PACK_BYTES)
    {
      throw ChunkPackError ("This file is far too large to be a chunk pack.");
    }

    std::vector<char> bytes (static_cast<std::size_t> (size));
    stream.seekg (0);
    stream.read (bytes.data(), size);

    if (stream.gcount() != size)
    {
      throw ChunkPackError ("Could not read all of " + path.string() + ".");
    }

    return decodeChunkPack (bytes.data(), bytes.size());
  }

  std::filesystem::path sharedClipboardPackPath()
  {
    QString const directory
      (QStandardPaths::writableLocation (QStandardPaths::TempLocation));

    // QStandardPaths can return an empty string when the platform has no such location. Falling
    // back to the working directory keeps the cross-window clipboard working rather than
    // silently writing to the filesystem root.
    QString const base (directory.isEmpty() ? QDir::currentPath() : directory);

    return std::filesystem::path (base.toStdWString()) / L"noggit_crimson_chunk_clipboard.ncp";
  }
}
