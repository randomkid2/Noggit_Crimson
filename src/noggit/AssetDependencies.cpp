// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AssetDependencies.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_set>

namespace
{
  // Chunk magics as they appear ON DISK, which is the reverse of the four character code every
  // loader in this tree writes as a multi-character literal. MapTile.cpp:148 compares a uint32 read
  // from the file against 'MVER' (0x4D564552 on MSVC), and the bytes in the file are 'R','E','V','M'.
  // Comparing the bytes directly avoids depending on multi-character literal evaluation order,
  // which is implementation defined.
  constexpr char const* MAGIC_MVER = "REVM";
  constexpr char const* MAGIC_MTEX = "XETM";
  constexpr char const* MAGIC_MMDX = "XDMM";
  constexpr char const* MAGIC_MWMO = "OMWM";

  constexpr char const* MAGIC_MOHD = "DHOM";
  constexpr char const* MAGIC_MOTX = "XTOM";
  constexpr char const* MAGIC_MOMT = "TMOM";
  constexpr char const* MAGIC_MOSB = "BSOM";
  constexpr char const* MAGIC_MODN = "NDOM";
  constexpr char const* MAGIC_MODD = "DDOM";

  // WMOMaterial is 0x40 bytes; the three texture offsets sit at these byte positions. Derived by
  // counting the members of the struct at src/noggit/wmo_liquid.hpp:35-68, in order:
  // flags(4) shader(4) blend_mode(4) texture_offset_1(4) sidn_color(4) frame_sidn_color(4)
  // texture_offset_2(4) diffuse_color(4) ground_type(4) texture_offset_3(4) ...
  constexpr std::size_t WMO_MATERIAL_SIZE = 0x40;
  constexpr std::size_t WMO_MATERIAL_TEXTURE_OFFSET_1 = 12;
  constexpr std::size_t WMO_MATERIAL_TEXTURE_OFFSET_2 = 24;
  // Noggit never resolves this one (WMO.cpp only reads offsets 1 and 2, and 2 only for shaders
  // 3/5/6/21/23). The client does, so a packer that follows Noggit exactly ships a WMO missing a
  // texture that renders in game and not in the editor.
  constexpr std::size_t WMO_MATERIAL_TEXTURE_OFFSET_3 = 36;

  // MODD entry: 0x28 bytes whose first dword is a 24 bit name offset plus 8 flag bits.
  constexpr std::size_t WMO_DOODAD_DEF_SIZE = 0x28;
  constexpr std::uint32_t WMO_DOODAD_NAME_OFFSET_MASK = 0x00FFFFFFu;

  // MD20 header field byte offsets. Read by offset rather than by including ModelHeaders.h, which
  // opens with <glm/glm.hpp> -- and the standalone test target has no glm include path. Counted
  // from src/noggit/ModelHeaders.h:21-102; the three used here land on the documented WotLK
  // positions (nViews 0x44, nTextures 0x50, ofsTextures 0x54).
  constexpr std::size_t M2_OFS_N_ANIMATIONS = 28;
  constexpr std::size_t M2_OFS_OFS_ANIMATIONS = 32;
  constexpr std::size_t M2_OFS_N_VIEWS = 68;
  constexpr std::size_t M2_OFS_N_TEXTURES = 80;
  constexpr std::size_t M2_OFS_OFS_TEXTURES = 84;
  constexpr std::size_t M2_HEADER_MINIMUM = 88;

  // ModelTextureDef: type, flags, nameLen, nameOfs (ModelHeaders.h:240-245).
  constexpr std::size_t M2_TEXTURE_DEF_SIZE = 16;
  constexpr std::size_t M2_TEXTURE_DEF_NAME_LENGTH = 8;
  constexpr std::size_t M2_TEXTURE_DEF_NAME_OFFSET = 12;

  // ModelAnimation (ModelHeaders.h:112-130): int16 animID, int16 subAnimID, then 60 more bytes.
  constexpr std::size_t M2_ANIMATION_SIZE = 64;

  // A model declaring more views than this is malformed, not a model with a lot of LODs. WotLK
  // ships 1 to 4. The cap keeps a corrupt header from queueing four billion probes.
  constexpr unsigned MAX_MODEL_VIEWS = 8;

  bool magicIs(char const* data, char const* magic)
  {
    return std::memcmp(data, magic, 4) == 0;
  }

  std::uint32_t readUint32(char const* data)
  {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
  }

  std::int16_t readInt16(char const* data)
  {
    std::int16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
  }

  bool endsWith(std::string_view text, std::string_view suffix)
  {
    return text.size() >= suffix.size()
        && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  // Case insensitive suffix test, ASCII range only.
  //
  // The extension filters below run on RAW strings straight out of a chunk, before
  // normalizeReferencePath has folded the case, and those strings really are mixed case: MOTX and
  // MODN in Blizzard's own WMOs spell them "World\\...\\Foo.BLP" and "...\\Bar.MDX". A case
  // sensitive filter here silently drops every one of them. Lowercasing is done by an explicit
  // range test rather than ::tolower, which has undefined behaviour for the bytes >= 0x80 that
  // these blocks do carry.
  bool endsWithNoCase(std::string_view text, std::string_view lowercase_suffix)
  {
    if (text.size() < lowercase_suffix.size())
    {
      return false;
    }

    std::string_view const tail (text.substr(text.size() - lowercase_suffix.size()));

    for (std::size_t index = 0; index < tail.size(); ++index)
    {
      char character = tail[index];

      if (character >= 'A' && character <= 'Z')
      {
        character = static_cast<char>(character - 'A' + 'a');
      }

      if (character != lowercase_suffix[index])
      {
        return false;
      }
    }

    return true;
  }

  std::string padded(unsigned value, std::size_t width)
  {
    std::string digits (std::to_string(value));

    while (digits.size() < width)
    {
      digits.insert(digits.begin(), '0');
    }

    return digits;
  }

  bool isModelExtension(std::string_view path)
  {
    return endsWithNoCase(path, ".m2") || endsWithNoCase(path, ".mdx") || endsWithNoCase(path, ".mdl");
  }

  // Appends one reference, deduplicating against what is already in `out`.
  //
  // Deduplication here is not redundant with the caller's visited set: a WMO with 200 materials
  // over 6 textures would otherwise return 200 entries, and the caller pays a probe or a queue
  // entry for each.
  void addReference( std::vector<Noggit::AssetReference>& out
                   , std::unordered_set<std::string>& seen
                   , std::string_view raw_path
                   , Noggit::ReferenceNecessity necessity
                   , bool traversable
                   )
  {
    std::string path (Noggit::normalizeReferencePath(raw_path));

    if (path.empty())
    {
      return;
    }

    if (!seen.insert(path).second)
    {
      return;
    }

    Noggit::AssetReference reference;
    reference.kind = Noggit::assetKindFromPath(path);
    reference.necessity = necessity;
    reference.traversable = traversable;
    reference.path = std::move(path);

    out.push_back(std::move(reference));
  }

  // Every NUL terminated string in a chunk payload, empty runs skipped.
  template <typename FunctionT>
  void forEachStringInBlock(char const* block, std::size_t size, FunctionT&& callback)
  {
    std::size_t position = 0;

    while (position < size)
    {
      std::size_t const start = position;

      while (position < size && block[position] != '\0')
      {
        ++position;
      }

      if (position > start)
      {
        callback(std::string_view(block + start, position - start));
      }

      ++position;
    }
  }

  // One string at a byte offset into a NUL separated block, bounds checked the way
  // WMO::stringBlockOffsetValid (WMO.cpp:256-259) does it. Returns an empty view for an offset
  // outside the block, which is how a malformed material is dropped rather than made fatal.
  std::string_view stringAtOffset(char const* block, std::size_t block_size, std::uint32_t offset)
  {
    if (!block || offset >= block_size)
    {
      return {};
    }

    std::size_t const length = ::strnlen(block + offset, block_size - offset);

    return std::string_view(block + offset, length);
  }

  // Walks a chunked file, calling back with (magic, payload, payload_size).
  //
  // Stops at the first chunk whose declared size runs past the end of the buffer rather than
  // clamping it: past that point every subsequent offset is guesswork.
  template <typename FunctionT>
  void forEachChunk(char const* data, std::size_t size, FunctionT&& callback)
  {
    std::size_t position = 0;

    while (position + 8 <= size)
    {
      char const* const magic = data + position;
      std::uint32_t const chunk_size = readUint32(data + position + 4);
      std::size_t const payload = position + 8;

      if (chunk_size > size - payload)
      {
        return;
      }

      callback(magic, data + payload, static_cast<std::size_t>(chunk_size));

      position = payload + chunk_size;
    }
  }
}

std::string Noggit::normalizeReferencePath(std::string_view raw)
{
  // normalizeAssetKey trims, ASCII-lowercases, collapses separator runs to '\\' and drops leading
  // and trailing separators. Everything after this is the two things it deliberately does not do.
  std::string path (normalizeAssetKey(raw));

  for (char& character : path)
  {
    if (character == '\\')
    {
      character = '/';
    }
  }

  if (endsWith(path, ".mdx") || endsWith(path, ".mdl"))
  {
    path.erase(path.size() - 4);
    path.append(".m2");
  }

  return path;
}

std::string Noggit::archiveStoredName(std::string_view path)
{
  std::string stored (normalizeReferencePath(path));

  for (char& character : stored)
  {
    if (character == '/')
    {
      character = '\\';
    }
  }

  return stored;
}

std::string Noggit::stripExtension(std::string_view path)
{
  std::size_t const separator = path.find_last_of("/\\");
  std::size_t const dot = path.find_last_of('.');

  if (dot == std::string_view::npos)
  {
    return std::string(path);
  }

  if (separator != std::string_view::npos && dot < separator)
  {
    return std::string(path);
  }

  return std::string(path.substr(0, dot));
}

std::string Noggit::skinFileName(std::string_view model_path, unsigned view_index)
{
  std::string base (stripExtension(model_path));

  if (base.empty())
  {
    return {};
  }

  return base + padded(view_index, 2) + ".skin";
}

std::string Noggit::animFileName(std::string_view model_path, int animation_id, int sub_animation_id)
{
  if (animation_id < 0 || sub_animation_id < 0)
  {
    return {};
  }

  std::string base (stripExtension(model_path));

  if (base.empty())
  {
    return {};
  }

  return base
       + padded(static_cast<unsigned>(animation_id), 4)
       + "-"
       + padded(static_cast<unsigned>(sub_animation_id), 2)
       + ".anim";
}

std::string Noggit::wmoGroupFileName(std::string_view world_model_path, unsigned group_index)
{
  std::string base (stripExtension(world_model_path));

  if (base.empty())
  {
    return {};
  }

  return base + "_" + padded(group_index, 3) + ".wmo";
}

bool Noggit::isWmoGroupFileName(std::string_view path)
{
  if (!endsWith(path, ".wmo"))
  {
    return false;
  }

  std::string_view stem (path.substr(0, path.size() - 4));
  std::size_t const underscore = stem.find_last_of('_');

  if (underscore == std::string_view::npos)
  {
    return false;
  }

  std::string_view const digits (stem.substr(underscore + 1));

  if (digits.size() < 3)
  {
    return false;
  }

  return std::all_of( digits.begin(), digits.end()
                    , [] (char character) { return character >= '0' && character <= '9'; }
                    );
}

std::string Noggit::tilesetSpecularName(std::string_view texture_path)
{
  std::string const path (normalizeReferencePath(texture_path));

  if (path.rfind("tileset/", 0) != 0 || !endsWith(path, ".blp"))
  {
    return {};
  }

  return path.substr(0, path.size() - 4) + "_s.blp";
}

bool Noggit::isBaseClientArchiveName(std::string_view archive_file_name)
{
  // Only the file name matters, and case does not: the templates spell the extension ".MPQ" while
  // the dialog and the settings spell it ".mpq".
  std::size_t const separator = archive_file_name.find_last_of("/\\");

  std::string name
    ( separator == std::string_view::npos
      ? std::string(archive_file_name)
      : std::string(archive_file_name.substr(separator + 1))
    );

  for (char& character : name)
  {
    if (character >= 'A' && character <= 'Z')
    {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }

  if (!endsWith(name, ".mpq"))
  {
    // A directory patch, or something that is not an archive at all. Never stock.
    return false;
  }

  std::string const stem (name.substr(0, name.size() - 4));

  static constexpr std::string_view STOCK[] =
    { "common", "common-2", "lichking", "expansion", "base", "patch", "patch-2", "patch-3" };

  for (std::string_view candidate : STOCK)
  {
    if (stem == candidate)
    {
      return true;
    }
  }

  // Locale archives. The locale code is always four letters (ClientData::Locales), so matching the
  // shape rather than the list keeps this correct for a locale nobody here has installed.
  auto const isLocaleCode
    ( [] (std::string_view text)
      {
        return text.size() == 4
            && std::all_of( text.begin(), text.end()
                          , [] (char character) { return character >= 'a' && character <= 'z'; }
                          );
      }
    );

  static constexpr std::string_view LOCALE_PREFIXES[] =
    { "locale-", "speech-", "expansion-locale-", "expansion-speech-"
    , "lichking-locale-", "lichking-speech-"
    };

  for (std::string_view prefix : LOCALE_PREFIXES)
  {
    if (stem.rfind(prefix, 0) == 0 && isLocaleCode(std::string_view(stem).substr(prefix.size())))
    {
      return true;
    }
  }

  if (stem.rfind("patch-", 0) == 0)
  {
    std::string_view const tail (std::string_view(stem).substr(6));

    // patch-<locale>
    if (isLocaleCode(tail))
    {
      return true;
    }

    // patch-<locale>-2 and -3 shipped with the client; -4 and up are the user's.
    if (tail.size() == 6 && isLocaleCode(tail.substr(0, 4)) && tail[4] == '-'
        && (tail[5] == '2' || tail[5] == '3'))
    {
      return true;
    }
  }

  return false;
}

bool Noggit::collectAdtReferences(char const* data, std::size_t size, std::vector<AssetReference>& out)
{
  if (!data || size < 8 || !magicIs(data, MAGIC_MVER))
  {
    return false;
  }

  std::unordered_set<std::string> seen;

  forEachChunk( data, size
              , [&] (char const* magic, char const* payload, std::size_t payload_size)
                {
                  if (magicIs(magic, MAGIC_MTEX))
                  {
                    forEachStringInBlock( payload, payload_size
                                        , [&] (std::string_view name)
                                          {
                                            addReference(out, seen, name, ReferenceNecessity::Required, false);

                                            std::string const specular (tilesetSpecularName(name));

                                            if (!specular.empty())
                                            {
                                              addReference(out, seen, specular, ReferenceNecessity::Optional, false);
                                            }
                                          }
                                        );
                  }
                  else if (magicIs(magic, MAGIC_MMDX) || magicIs(magic, MAGIC_MWMO))
                  {
                    forEachStringInBlock( payload, payload_size
                                        , [&] (std::string_view name)
                                          {
                                            addReference(out, seen, name, ReferenceNecessity::Required, true);
                                          }
                                        );
                  }
                }
              );

  return true;
}

bool Noggit::collectModelReferences( std::string_view model_path
                                   , char const* data
                                   , std::size_t size
                                   , std::vector<AssetReference>& out
                                   )
{
  if (!data || size < M2_HEADER_MINIMUM || std::memcmp(data, "MD20", 4) != 0)
  {
    return false;
  }

  std::unordered_set<std::string> seen;

  std::uint32_t const texture_count = readUint32(data + M2_OFS_N_TEXTURES);
  std::uint32_t const texture_offset = readUint32(data + M2_OFS_OFS_TEXTURES);

  for (std::uint32_t index = 0; index < texture_count; ++index)
  {
    std::size_t const entry
      (static_cast<std::size_t>(texture_offset) + static_cast<std::size_t>(index) * M2_TEXTURE_DEF_SIZE);

    if (entry > size || size - entry < M2_TEXTURE_DEF_SIZE)
    {
      break;
    }

    // type != 0 is a runtime supplied skin with no name in the file at all.
    if (readUint32(data + entry) != 0)
    {
      continue;
    }

    std::uint32_t const name_length = readUint32(data + entry + M2_TEXTURE_DEF_NAME_LENGTH);
    std::uint32_t const name_offset = readUint32(data + entry + M2_TEXTURE_DEF_NAME_OFFSET);

    // A type-0 slot with a zero length name is legal and the loader skips it with a debug log
    // (Model.cpp:344-348). Silence here too: it is not a missing file.
    if (!name_length || name_offset >= size)
    {
      continue;
    }

    std::size_t const available = std::min<std::size_t>(name_length, size - name_offset);
    // strnlen rather than "the declared length minus the terminator": some exporters write the
    // length without accounting for the NUL and some with it (Model.cpp:352-354 patches around
    // exactly that), and an embedded NUL would otherwise put a garbage tail in the path.
    std::size_t const length = ::strnlen(data + name_offset, available);

    if (!length)
    {
      continue;
    }

    addReference(out, seen, std::string_view(data + name_offset, length), ReferenceNecessity::Required, false);
  }

  std::uint32_t const view_count = std::min<std::uint32_t>(readUint32(data + M2_OFS_N_VIEWS), MAX_MODEL_VIEWS);

  for (std::uint32_t view = 0; view < view_count; ++view)
  {
    // View 0 is what Noggit itself loads and what every renderer needs; a model with no
    // <base>00.skin has no geometry. The higher LODs are marked Optional because a hand made model
    // that declares four views and ships one is common in custom content, and reporting three
    // "missing" files for every such model would drown the report.
    addReference( out, seen
                , skinFileName(model_path, view)
                , view == 0 ? ReferenceNecessity::Required : ReferenceNecessity::Optional
                , false
                );
  }

  std::uint32_t const animation_count = readUint32(data + M2_OFS_N_ANIMATIONS);
  std::uint32_t const animation_offset = readUint32(data + M2_OFS_OFS_ANIMATIONS);

  for (std::uint32_t index = 0; index < animation_count; ++index)
  {
    std::size_t const entry
      (static_cast<std::size_t>(animation_offset) + static_cast<std::size_t>(index) * M2_ANIMATION_SIZE);

    if (entry > size || size - entry < 4)
    {
      break;
    }

    std::string const name
      (animFileName(model_path, readInt16(data + entry), readInt16(data + entry + 2)));

    if (!name.empty())
    {
      // Always Optional: a sequence whose data is inline has no .anim file, and nothing in this
      // tree reads the flag bit that says which is which. Model.cpp:498 probes for the same reason.
      addReference(out, seen, name, ReferenceNecessity::Optional, false);
    }
  }

  return true;
}

bool Noggit::collectWorldModelReferences( std::string_view world_model_path
                                        , char const* data
                                        , std::size_t size
                                        , std::vector<AssetReference>& out
                                        )
{
  if (!data || size < 8)
  {
    return false;
  }

  std::unordered_set<std::string> seen;

  bool found_header = false;
  std::uint32_t group_count = 0;

  char const* texture_block = nullptr;
  std::size_t texture_block_size = 0;
  char const* doodad_name_block = nullptr;
  std::size_t doodad_name_block_size = 0;

  forEachChunk( data, size
              , [&] (char const* magic, char const* payload, std::size_t payload_size)
                {
                  if (magicIs(magic, MAGIC_MOHD))
                  {
                    // nTextures, then nGroups. Only nGroups is trustworthy enough to act on, and
                    // WMO.cpp:401 cross-checks it against MOGI before using it.
                    if (payload_size >= 8)
                    {
                      found_header = true;
                      group_count = readUint32(payload + 4);
                    }
                  }
                  else if (magicIs(magic, MAGIC_MOTX))
                  {
                    texture_block = payload;
                    texture_block_size = payload_size;

                    forEachStringInBlock( payload, payload_size
                                        , [&] (std::string_view name)
                                          {
                                            if (endsWithNoCase(name, ".blp"))
                                            {
                                              addReference(out, seen, name, ReferenceNecessity::Required, false);
                                            }
                                          }
                                        );
                  }
                  else if (magicIs(magic, MAGIC_MOMT))
                  {
                    for (std::size_t entry = 0; entry + WMO_MATERIAL_SIZE <= payload_size; entry += WMO_MATERIAL_SIZE)
                    {
                      for (std::size_t field : { WMO_MATERIAL_TEXTURE_OFFSET_1
                                               , WMO_MATERIAL_TEXTURE_OFFSET_2
                                               , WMO_MATERIAL_TEXTURE_OFFSET_3
                                               })
                      {
                        std::string_view const name
                          (stringAtOffset(texture_block, texture_block_size, readUint32(payload + entry + field)));

                        // "textures/shanecube.blp" is what WMO.cpp:356-360 substitutes for an
                        // invalid offset; a real one never names it, so nothing has to be excluded
                        // here -- an out of range offset simply yields an empty view.
                        if (!name.empty())
                        {
                          addReference(out, seen, name, ReferenceNecessity::Required, false);
                        }
                      }
                    }
                  }
                  else if (magicIs(magic, MAGIC_MOSB))
                  {
                    // WMO.cpp:420 requires size > 4 before treating MOSB as a name.
                    if (payload_size > 4)
                    {
                      std::string_view const name (stringAtOffset(payload, payload_size, 0));

                      // Traversable: the skybox is an M2 with its own skins and textures, and it
                      // appears in neither WMO::textures nor WMO::modelis, so AssetScan never sees
                      // it. Optional because the loader only takes it if it exists.
                      if (!name.empty())
                      {
                        addReference(out, seen, name, ReferenceNecessity::Optional, true);
                      }
                    }
                  }
                  else if (magicIs(magic, MAGIC_MODN))
                  {
                    doodad_name_block = payload;
                    doodad_name_block_size = payload_size;

                    forEachStringInBlock( payload, payload_size
                                        , [&] (std::string_view name)
                                          {
                                            if (isModelExtension(name))
                                            {
                                              addReference(out, seen, name, ReferenceNecessity::Required, true);
                                            }
                                          }
                                        );
                  }
                  else if (magicIs(magic, MAGIC_MODD))
                  {
                    for (std::size_t entry = 0; entry + WMO_DOODAD_DEF_SIZE <= payload_size; entry += WMO_DOODAD_DEF_SIZE)
                    {
                      std::uint32_t const name_offset
                        (readUint32(payload + entry) & WMO_DOODAD_NAME_OFFSET_MASK);

                      std::string_view const name
                        (stringAtOffset(doodad_name_block, doodad_name_block_size, name_offset));

                      if (!name.empty())
                      {
                        addReference(out, seen, name, ReferenceNecessity::Required, true);
                      }
                    }
                  }
                }
              );

  if (!found_header)
  {
    return false;
  }

  // Group files last, so the chunk driven references keep their file order in the result and the
  // groups arrive as one contiguous, index ordered run.
  //
  // This is the reference AssetScan cannot produce and whose absence is the failure the whole
  // feature exists to prevent: a root packed without its groups loads in no client.
  for (std::uint32_t group = 0; group < group_count; ++group)
  {
    addReference(out, seen, wmoGroupFileName(world_model_path, group), ReferenceNecessity::Required, false);
  }

  return true;
}
