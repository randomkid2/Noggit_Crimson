// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <ClientFile.hpp>
#include <math/frustum.hpp>
#include <math/ray.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h> // LogDebug
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/WMO.h>
#include <noggit/wmo_liquid.hpp>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
  // Only WotLK root/group files are supported. Anything else is rejected, not
  // adapted: see the comment on the parse helpers below.
  constexpr std::uint32_t WMO_VERSION_WOTLK = 17;

  // Derived from getSize()/getPos() rather than calling a helper on ClientFile, deliberately.
  // ClientFile lives in the blizzard-archive-library SUBMODULE, which points at a third-party
  // upstream this project cannot push to. Adding a method there would compile locally and then
  // fail for everyone who clones, because their submodule checkout would not have it. Both
  // accessors used here are long-standing public API.
  //
  // Clamped rather than allowed to wrap: seek() accepts a past-end offset, so getPos() can exceed
  // getSize(), and an unsigned subtraction there would produce an enormous "remaining" that makes
  // every size check below pass.
  std::size_t bytesLeft(BlizzardArchive::ClientFile const& f)
  {
    std::size_t const size (f.getSize());
    std::size_t const pos (f.getPos());

    return pos >= size ? 0 : size - pos;
  }

  // WMO parsing here is a fixed chunk *sequence*, not a fourcc-driven dispatch.
  // The offset of every chunk depends on the previous one being exactly the
  // chunk we expected and exactly as long as it claims, so a file from a later
  // expansion -- which adds, removes and reorders chunks -- desynchronises the
  // whole parse. Sizes are then read from the wrong offsets and fed straight to
  // resize() and to pointers aliased over the file buffer.
  //
  // Upstream guarded all of this with assert() only, and NDEBUG is defined in
  // the RelWithDebInfo configuration this project ships, so in the built binary
  // those guards did not exist at all. The helpers below turn the same
  // expectations into real runtime checks and report failure exactly the way
  // Model::finishLoading() already does (Model.cpp:64-69): log with the file
  // name and the reason, then throw. AsyncLoader::process() catches it
  // (AsyncLoader.cpp:108-121) and marks the object failed, so the user gets a
  // listed-but-unloadable entry instead of an out-of-bounds read.

  std::string fourccToString(std::uint32_t fourcc)
  {
    // fourccs are compared against multi-character literals, which put the
    // first character of the on-disk magic in the high byte. Print them back in
    // that order. A desynchronised parse hands us arbitrary bytes, so anything
    // unprintable is substituted rather than dumped into the log.
    std::string out;

    for (int shift : {24, 16, 8, 0})
    {
      char const c (static_cast<char>((fourcc >> shift) & 0xFF));
      out += (c >= 0x20 && c < 0x7F) ? c : '?';
    }

    return out;
  }

  [[noreturn]] void throwWmoParseError(std::string const& filename, std::string const& reason)
  {
    LogError << "Error loading WMO \"" << filename << "\". " << reason << std::endl;
    throw std::runtime_error("Error loading WMO \"" + filename + "\". " + reason);
  }

  // Reads the 8 byte header of a chunk the sequence requires to be here, checks
  // it is that chunk, and checks its declared payload actually fits in what is
  // left of the file. Returns the payload size.
  //
  // `is_container` exempts the size-fits-in-file check, and exists for exactly one chunk: MOGP.
  // MOGP is not a leaf chunk -- its declared payload spans the group header plus every chunk that
  // follows it, i.e. the whole rest of the group file. Real WMOs routinely OVERSTATE it by a few
  // dozen bytes; measured against shipping Draenor assets the excess was 32, 68 and 188 bytes on
  // three different files. Upstream never noticed because this check was an assert and NDEBUG
  // compiles asserts away, so the overshoot was simply read past.
  //
  // Rejecting on it is wrong, and was a real regression: it made legitimate content unloadable
  // while protecting nothing. Nothing aliases MOGP's payload as a block -- every chunk inside it
  // is read through this same function and bounds-checked on its own -- so the protection that
  // matters is still in force.
  std::uint32_t readChunkHeader( BlizzardArchive::ClientFile& f
                               , std::string const& filename
                               , std::uint32_t expected
                               , bool is_container = false
                               )
  {
    std::size_t const chunk_start (f.getPos());

    std::uint32_t fourcc (0);
    std::uint32_t size (0);

    if (f.read(&fourcc, 4) != 4 || f.read(&size, 4) != 4)
    {
      throwWmoParseError ( filename
                         , "File ends before the " + fourccToString(expected)
                           + " chunk header expected at offset " + std::to_string(chunk_start) + "."
                         );
    }

    if (fourcc != expected)
    {
      throwWmoParseError ( filename
                         , "Expected chunk " + fourccToString(expected) + " at offset "
                           + std::to_string(chunk_start) + " but found " + fourccToString(fourcc)
                           + ". The file is corrupt, or it is not a WotLK (v17) WMO."
                         );
    }

    // A chunk claiming more bytes than the file holds means the parse has
    // desynchronised or the file is truncated. Either way every offset from
    // here on is meaningless and the payload must not be aliased.
    if (size > bytesLeft(f) && !is_container)
    {
      throwWmoParseError ( filename
                         , "Chunk " + fourccToString(expected) + " at offset "
                           + std::to_string(chunk_start) + " claims " + std::to_string(size)
                           + " bytes but only " + std::to_string(bytesLeft(f))
                           + " remain in the file."
                         );
    }

    return size;
  }

  // The group file's trailing chunks are gated on group header flags rather
  // than always being present, and upstream deliberately tolerates a missing
  // one by rewinding the 8 header bytes and trying the next flag. That
  // tolerance is kept. A chunk that *is* present but claims more bytes than the
  // file holds is unambiguously corrupt, and aliasing its payload is exactly
  // the out-of-bounds read being removed here, so that still hard-fails.
  bool readOptionalChunkHeader( BlizzardArchive::ClientFile& f
                              , std::string const& filename
                              , std::uint32_t expected
                              , std::uint32_t* size
                              )
  {
    std::size_t const chunk_start (f.getPos());

    std::uint32_t fourcc (0);
    *size = 0;

    if (f.read(&fourcc, 4) != 4 || f.read(size, 4) != 4 || fourcc != expected)
    {
      LogError << "Broken header in WMO \"" << filename << "\": expected optional chunk "
               << fourccToString(expected) << " at offset " << chunk_start
               << ". Trying to continue reading." << std::endl;

      // Rewind to the saved position rather than getPos() - 8: after a short
      // read getPos() stops at the end of the buffer, so the old arithmetic
      // would have landed somewhere else entirely.
      f.seek(chunk_start);
      *size = 0;
      return false;
    }

    if (*size > bytesLeft(f))
    {
      throwWmoParseError ( filename
                         , "Chunk " + fourccToString(expected) + " at offset "
                           + std::to_string(chunk_start) + " claims " + std::to_string(*size)
                           + " bytes but only " + std::to_string(bytesLeft(f))
                           + " remain in the file."
                         );
    }

    return true;
  }

  // MVER is the first chunk of both the root and every group file. Rejecting a
  // non-v17 file here is the point of the exercise: later expansion formats are
  // not supported and must not be parsed on a best-effort basis.
  void checkWmoVersion(BlizzardArchive::ClientFile& f, std::string const& filename)
  {
    readChunkHeader(f, filename, 'MVER');

    std::uint32_t version (0);

    if (f.read(&version, 4) != 4)
    {
      throwWmoParseError(filename, "File ends inside the MVER chunk.");
    }

    if (version != WMO_VERSION_WOTLK)
    {
      throwWmoParseError ( filename
                         , "Wrong WMO version " + std::to_string(version) + ", expected "
                           + std::to_string(WMO_VERSION_WOTLK)
                           + " (WotLK 3.3.5a). WMOs from other expansions are not supported."
                         );
    }
  }

  // Every array chunk in a group file is read as `size` raw bytes into a vector
  // that was sized `size / sizeof(element)`. Integer division truncates, so a
  // payload that is not a whole number of elements makes that read write
  // `size % sizeof(element)` bytes past the end of the vector's storage -- e.g.
  // a MONR of 13 bytes resizes to one 12 byte normal and then reads 13. The
  // element size is not a free parameter of the format (MOVI is uint16 indices,
  // MOVT and MONR are 12 byte vectors, MOBA is a 24 byte batch record), so a
  // remainder means the chunk is malformed and every offset after it is
  // suspect. Reject rather than truncate, for the same reason readChunkHeader
  // rejects an oversized chunk.
  std::uint32_t elementCount( std::string const& filename
                            , std::uint32_t fourcc
                            , std::uint32_t size
                            , std::size_t element_size
                            )
  {
    if (size % element_size)
    {
      throwWmoParseError ( filename
                         , "Chunk " + fourccToString(fourcc) + " is " + std::to_string(size)
                           + " bytes, which is not a whole number of "
                           + std::to_string(element_size) + " byte elements."
                         );
    }

    return size / static_cast<std::uint32_t>(element_size);
  }

  // Reads a chunk payload that is a run of NUL terminated strings into a buffer
  // with a guaranteed terminator, so that an offset taken from elsewhere in the
  // file can be bounds checked before being turned into a std::string. Aliasing
  // the file buffer directly (what upstream did) cannot be made safe: there is
  // no guarantee the last byte of the chunk is a NUL.
  std::vector<char> readStringBlock(BlizzardArchive::ClientFile& f, std::uint32_t size)
  {
    std::vector<char> block (static_cast<std::size_t>(size) + 1, '\0');
    f.read(block.data(), size);
    return block;
  }

  // Offsets into a string block come from unrelated chunks, so they have to be
  // range checked before use. The trailing NUL added by readStringBlock() is
  // not a valid start offset, hence the strict comparison against size() - 1.
  bool stringBlockOffsetValid(std::vector<char> const& block, std::size_t offset)
  {
    return block.size() > 1 && offset < block.size() - 1;
  }
}


WMO::WMO(BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
  : AsyncObject(file_key)
  , _context(context)
  , _renderer(this)
{
}

WMO::~WMO()
{
}

void WMO::finishLoading ()
{
  std::string const fname (_file_key.filepath());

  BlizzardArchive::ClientFile f(fname, Noggit::Application::NoggitApplication::instance()->clientData());
  if (f.isEof()) {
    // Throw rather than return: returning left `finished` false forever, so
    // every wait_until_loaded() on this WMO blocked instead of seeing a failure.
    throwWmoParseError(fname, "The file is empty or could not be read.");
  }

  uint32_t size;

  float ff[3];

  // - MVER ----------------------------------------------

  checkWmoVersion(f, fname);

  // - MOHD ----------------------------------------------

  uint32_t const mohd_size (readChunkHeader(f, fname, 'MOHD'));

  // The fields below are read one by one and total 0x40 bytes regardless of
  // what MOHD declares, so the declared size has to cover them.
  if (mohd_size < 0x40)
  {
    throwWmoParseError(fname, "MOHD chunk is only " + std::to_string(mohd_size)
                              + " bytes, expected at least 64.");
  }

  CArgb ambient_color;
  unsigned int nTextures, nGroups, nP, nLights, nModels, nDoodads, nDoodadSets;
  // header
  f.read (&nTextures, 4);
  f.read (&nGroups, 4);
  f.read (&nP, 4);
  f.read (&nLights, 4);
  f.read (&nModels, 4);
  f.read (&nDoodads, 4);
  f.read (&nDoodadSets, 4);
  f.read (&ambient_color, 4);
  f.read (&WmoId, 4);
  f.read (ff, 12);
  extents[0] = ::glm::vec3 (ff[0], ff[1], ff[2]);
  f.read (ff, 12);
  extents[1] = ::glm::vec3 (ff[0], ff[1], ff[2]);
  f.read(&flags, 2);

  f.seekRelative (2);

  ambient_light_color.x = static_cast<float>(ambient_color.r) / 255.f;
  ambient_light_color.y = static_cast<float>(ambient_color.g) / 255.f;
  ambient_light_color.z = static_cast<float>(ambient_color.b) / 255.f;
  ambient_light_color.w = static_cast<float>(ambient_color.a) / 255.f;

  // - MOTX ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOTX');

  // Texture names are addressed by byte offset from MOMT, so the block needs a
  // guaranteed terminator and the offsets need range checking (see below).
  std::vector<char> const texbuf (readStringBlock(f, size));

  // - MOMT ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOMT');

  std::size_t const num_materials (size / 0x40);
  materials.resize (num_materials);

  // note: used to map to size_t, but our other values don't support that.
  //std::map<std::uint32_t, std::size_t> texture_offset_to_inmem_index;
  std::map<std::uint32_t, std::uint32_t> texture_offset_to_inmem_index;

  auto load_texture
    ( [&] (std::uint32_t ofs)
      {
        // The offset comes from the material, not from MOTX, so it can point
        // anywhere. Out of range is treated like the empty-name case upstream
        // already handled, rather than being fatal: a single bad material
        // reference does not mean the chunk sequence has desynchronised.
        char const* texture
          ( stringBlockOffsetValid(texbuf, ofs) && texbuf[ofs]
            ? &texbuf[ofs]
            : "textures/shanecube.blp"
          );

        auto const mapping
          (texture_offset_to_inmem_index.emplace(ofs, static_cast<std::uint32_t>(textures.size())));

        if (mapping.second)
        {
          textures.emplace_back(texture, _context);
        }
        return mapping.first->second;
      }
    );

  for (size_t i(0); i < num_materials; ++i)
  {
    f.read(&materials[i], sizeof(WMOMaterial));

    uint32_t shader = materials[i].shader;
    bool use_second_texture = (shader == 6 || shader == 5 || shader == 3 || shader == 21 || shader == 23);

    materials[i].texture1 = load_texture(materials[i].texture_offset_1);
    if (use_second_texture)
    {
      materials[i].texture2 = load_texture(materials[i].texture_offset_2);
    }
  }

  // - MOGN ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOGN');

  // Copied out rather than aliased: WMOGroup's name offset is not guaranteed to
  // land on a NUL terminated string inside the chunk.
  std::vector<char> const groupnames (readStringBlock(f, size));

  // - MOGI ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOGI');

  // Each MOGI entry is 0x20 bytes and the count comes from MOHD, so a group
  // count that outruns the chunk means the two disagree - read no further.
  if (static_cast<std::size_t>(nGroups) * 0x20 > size)
  {
    throwWmoParseError(fname, "MOHD declares " + std::to_string(nGroups)
                              + " groups but MOGI only holds " + std::to_string(size / 0x20) + ".");
  }

  groups.reserve(nGroups);
  for (unsigned int i (0); i < nGroups; ++i) {
    groups.emplace_back (this, &f, i, groupnames);
  }

  // - MOSB ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOSB');

  // Read (not aliased) for the same terminator reason as MOGN; this also
  // advances past the chunk, which the trailing seekRelative used to do.
  std::vector<char> const skybox_name (readStringBlock(f, size));

  if (size > 4)
  {
    std::string path = BlizzardArchive::ClientData::normalizeFilenameInternal(std::string (skybox_name.data()));
    auto from = std::string("mdx");
    auto to = std::string("m2");
    size_t start_pos = 0;
    while ((start_pos = path.find(from, start_pos)) != std::string::npos) {
        path.replace(start_pos, from.length(), to);
        start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
    }

    if (path.length())
    {
      if (Noggit::Application::NoggitApplication::instance()->clientData()->exists(path))
      {
        skybox = scoped_model_reference(path, _context);
      }
    }
  }

  // - MOPV ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOPV');

  f.seekRelative (size);

  /*
  std::vector<glm::vec3> portal_vertices;

  for (size_t i (0); i < size / 12; ++i) {
    f.read (ff, 12);
    portal_vertices.push_back(glm::vec3(ff[0], ff[2], -ff[1]));
  }

   */

  // - MOPT ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOPT');

  f.seekRelative (size);

  // - MOPR ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOPR');

  f.seekRelative (size);

  // - MOVV ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOVV');

  f.seekRelative (size);

  // - MOVB ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOVB');

  f.seekRelative (size);

  // - MOLT ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOLT');

  // WMOLight::init reads 0x30 bytes per light; the count is MOHD's, so the two
  // have to agree or the parse has desynchronised.
  if (static_cast<std::size_t>(nLights) * 0x30 > size)
  {
    throwWmoParseError(fname, "MOHD declares " + std::to_string(nLights)
                              + " lights but MOLT only holds " + std::to_string(size / 0x30) + ".");
  }

  lights.reserve(nLights);
  for (size_t i (0); i < nLights; ++i) {
    WMOLight l;
    l.init (&f);
    lights.push_back (l);
  }

  // - MODS ----------------------------------------------

  size = readChunkHeader(f, fname, 'MODS');

  if (static_cast<std::size_t>(nDoodadSets) * 32 > size)
  {
    throwWmoParseError(fname, "MOHD declares " + std::to_string(nDoodadSets)
                              + " doodad sets but MODS only holds " + std::to_string(size / 32) + ".");
  }

  doodadsets.reserve(nDoodadSets);
  for (size_t i (0); i < nDoodadSets; ++i) {
    WMODoodadSet dds;
    f.read (&dds, 32);
    doodadsets.push_back (dds);
  }

  // - MODN ----------------------------------------------

  size = readChunkHeader(f, fname, 'MODN');

  // Doodad file names, addressed by the 24 bit offset in each MODD entry.
  std::vector<char> const ddnames (readStringBlock(f, size));

  // - MODD ----------------------------------------------

  size = readChunkHeader(f, fname, 'MODD');

  modelis.reserve(size / 0x28);
  for (size_t i (0); i < size / 0x28; ++i)
  {
    struct
    {
      uint32_t name_offset : 24;
      uint32_t flag_AcceptProjTex : 1;
      uint32_t flag_0x2 : 1;
      uint32_t flag_0x4 : 1;
      uint32_t flag_0x8 : 1;
      uint32_t flags_unused : 4;
    } x;

    size_t after_entry (f.getPos() + 0x28);
    f.read (&x, sizeof (x));

    // Upstream did `ddnames + x.name_offset` with ddnames possibly null (empty
    // MODN) and the offset never checked, so a desynchronised or malformed file
    // handed ModelInstance a pointer into unrelated memory to strlen.
    if (!stringBlockOffsetValid(ddnames, x.name_offset))
    {
      throwWmoParseError(fname, "MODD entry " + std::to_string(i) + " references doodad name offset "
                                + std::to_string(x.name_offset) + ", outside the "
                                + std::to_string(ddnames.size() - 1) + " byte MODN chunk.");
    }

    modelis.emplace_back(ddnames.data() + x.name_offset, &f, _context);
    model_nearest_light_vector.emplace_back();

    f.seek (after_entry);
  }

  // - MFOG ----------------------------------------------

  size = readChunkHeader(f, fname, 'MFOG');

  int nfogs = size / 0x30;
  fogs.reserve(nfogs);

  for (size_t i (0); i < nfogs; ++i)
  {
    WMOFog fog;
    fog.init (&f);
    fogs.push_back (std::move(fog));
  }

  for (auto& group : groups)
    group.load();

  finished = true;
  _state_changed.notify_all();
}

void WMO::waitForChildrenLoaded()
{
  for (auto& tex : textures)
  {
    tex.get()->wait_until_loaded();
  }

  for (auto& doodad : modelis)
  {
    doodad.model->wait_until_loaded();
    doodad.model->waitForChildrenLoaded();
  }
}

std::vector<float> WMO::intersect (math::ray const& ray, bool do_exterior) const
{
  std::vector<float> results;

  if (!finishedLoading() || loading_failed())
  {
    return results;
  }

  for (auto& group : groups)
  {
    if (!do_exterior && !group.is_indoor())
          continue;

    group.intersect (ray, &results);
  }

  if (!do_exterior && results.size())
  {
      // dirty way to find the furthest face and ignore invisible faces, cleaner way would be to do a direction check on faces
      // float max = *std::max_element(std::begin(results), std::end(results));
      // results.clear();
      // results.push_back(max);

      // other way, ignore the closest intersect, works well
      if (results.size() > 1)
      {
        auto it = std::min_element(results.begin(), results.end());
        results.erase(it);
      }
  }

  return results;
}

std::map<uint32_t, std::vector<wmo_doodad_instance>> WMO::doodads_per_group(uint16_t doodadset) const
{
  std::map<uint32_t, std::vector<wmo_doodad_instance>> doodads;

  if (doodadset >= doodadsets.size())
  {
    LogError << "Invalid doodadset for instance of wmo " << _file_key.stringRepr() << std::endl;
    return doodads;
  }

  auto const& dset = doodadsets[doodadset];
  uint32_t start = dset.start, end = start + dset.size;

  for (int i = 0; i < groups.size(); ++i)
  {
    for (uint16_t ref : groups[i].doodad_ref())
    {
      // `end` comes from the doodad set's own start/size fields, which are not
      // validated against MODD, so the range check alone does not keep `ref`
      // inside modelis.
      if (ref >= start && ref < end && ref < modelis.size())
      {
        doodads[i].push_back(modelis[ref]);
      }
    }
  }

  return doodads;
}

[[nodiscard]]
bool WMO::is_hidden() const
{
  return _hidden;
}

void WMO::toggle_visibility()
{
  _hidden = !_hidden;
}

void WMO::show()
{
  _hidden = false;
}

void WMO::hide()
{
  _hidden = true;
}

[[nodiscard]]
bool WMO::is_required_when_saving() const
{
  return true;
}

[[nodiscard]]
Noggit::Rendering::WMORender* WMO::renderer()
{
  return &_renderer;
}

void WMOLight::init(BlizzardArchive::ClientFile* f)
{
  char type[4];
  f->read(&type, 4);
  f->read(&color, 4);
  f->read(&pos, 12);
  f->read(&intensity, 4);
  f->read(unk, 4 * 5);
  f->read(&r, 4);

  pos = glm::vec3(pos.x, pos.z, -pos.y);

  // rgb? bgr? hm
  float fa = ((color & 0xff000000) >> 24) / 255.0f;
  float fr = ((color & 0x00ff0000) >> 16) / 255.0f;
  float fg = ((color & 0x0000ff00) >> 8) / 255.0f;
  float fb = ((color & 0x000000ff)) / 255.0f;

  fcolor = glm::vec4(fr, fg, fb, fa);
  fcolor *= intensity;
  fcolor.w = 1.0f;

  /*
  // light logging
  gLog("Light %08x @ (%4.2f,%4.2f,%4.2f)\t %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f, %4.2f\t(%d,%d,%d,%d)\n",
  color, pos.x, pos.y, pos.z, intensity,
  unk[0], unk[1], unk[2], unk[3], unk[4], r,
  type[0], type[1], type[2], type[3]);
  */
}

void WMOLight::setup(GLint)
{
  // not used right now -_-
}

void WMOLight::setupOnce(GLint, glm::vec3, glm::vec3)
{
  //glm::vec4position(dir, 0);
  //glm::vec4position(0,1,0,0);

  //glm::vec4ambient = glm::vec4(light_color * 0.3f, 1);
  //glm::vec4diffuse = glm::vec4(light_color, 1);


  //gl.enable(light);
}



WMOGroup::WMOGroup(WMO *_wmo, BlizzardArchive::ClientFile* f, int _num, std::vector<char> const& names)
  : wmo(_wmo)
  , num(_num)
  , _renderer(this)
{
  // extract group info from f
  std::uint32_t flags; // not used, the flags are in the group header
  f->read(&flags, 4);
  float ff[3];
  f->read(ff, 12);
  VertexBoxMax = glm::vec3(ff[0], ff[1], ff[2]);
  f->read(ff, 12);
  VertexBoxMin = glm::vec3(ff[0], ff[1], ff[2]);
  int nameOfs;
  f->read(&nameOfs, 4);

  //! \todo  get proper name from group header and/or dbc?
  // The offset is a MOGN offset stored in MOGI, so it is only trustworthy if it
  // actually lands inside MOGN. Upstream added it to a raw pointer unchecked and
  // let std::string run off the end of the file buffer looking for a NUL.
  if (nameOfs > 0 && stringBlockOffsetValid(names, static_cast<std::size_t>(nameOfs))) {
    name = std::string(names.data() + nameOfs);
  }
  else name = "(no name)";
}

WMOGroup::WMOGroup(WMOGroup const& other)
  : BoundingBoxMin(other.BoundingBoxMin)
  , BoundingBoxMax(other.BoundingBoxMax)
  , VertexBoxMin(other.VertexBoxMin)
  , VertexBoxMax(other.VertexBoxMax)
  , use_outdoor_lights(other.use_outdoor_lights)
  , name(other.name)
  , wmo(other.wmo)
  , header(other.header)
  , center(other.center)
  , rad(other.rad)
  , num(other.num)
  , fog(other.fog)
  , _doodad_ref(other._doodad_ref)
  , _batches(other._batches)
  , _vertices(other._vertices)
  , _normals(other._normals)
  , _texcoords(other._texcoords)
  , _texcoords_2(other._texcoords_2)
  , _vertex_colors(other._vertex_colors)
  , _indices(other._indices)
  , _renderer(this)
{
  if (other.lq)
  {
    lq = std::make_unique<wmo_liquid>(*other.lq.get());
  }
}

namespace
{
  glm::vec4 colorFromInt(unsigned int col)
  {
    GLubyte r, g, b, a;
    a = (col & 0xFF000000) >> 24;
    r = (col & 0x00FF0000) >> 16;
    g = (col & 0x0000FF00) >> 8;
    b = (col & 0x000000FF);
    return glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
  }
}


void WMOGroup::load()
{
  // open group file
  std::stringstream curNum;
  curNum << "_" << std::setw (3) << std::setfill ('0') << num;

  std::string fname = wmo->file_key().filepath();
  fname.insert (fname.find (".wmo"), curNum.str ());

  BlizzardArchive::ClientFile f(fname, Noggit::Application::NoggitApplication::instance()->clientData());
  if (f.isEof()) {
    // Throwing propagates out of WMO::finishLoading() and fails the whole WMO,
    // which is correct: a group file we cannot read leaves this group with no
    // geometry and no initialised render batches.
    throwWmoParseError(fname, "The group file is empty or could not be read.");
  }

  uint32_t size;

  // - MVER ----------------------------------------------

  checkWmoVersion(f, fname);

  // - MOGP ----------------------------------------------

  uint32_t const mogp_size (readChunkHeader(f, fname, 'MOGP', true));

  // MOGP is a container: its payload is the group header followed by every
  // chunk read below, so it has to be at least header sized.
  if (mogp_size < sizeof (wmo_group_header))
  {
    throwWmoParseError(fname, "MOGP chunk is only " + std::to_string(mogp_size)
                              + " bytes, expected at least " + std::to_string(sizeof (wmo_group_header)) + ".");
  }

  f.read (&header, sizeof (wmo_group_header));

  // The root WMO may legitimately carry no MFOG entries at all, in which case
  // the "downport hack" below indexed fogs[0] on an empty vector.
  if (wmo->fogs.empty())
  {
    fog = -1;
  }
  else
  {
    unsigned fog_index = header.fogs[0];

    // downport hack
    if (fog_index >= wmo->fogs.size())
    {
        fog_index = 0;
    }
    WMOFog &wf = wmo->fogs[fog_index];

    if (wf.r2 <= 0) fog = -1; // default outdoor fog..?
    else fog = header.fogs[0];
  }

  BoundingBoxMin = ::glm::vec3 (header.box1[0], header.box1[2], -header.box1[1]);
  BoundingBoxMax = ::glm::vec3 (header.box2[0], header.box2[2], -header.box2[1]);

  // - MOPY ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOPY');
  f.seekRelative (size);

  // - MOVI ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOVI');

  _indices.resize (elementCount (fname, 'MOVI', size, sizeof (uint16_t)));

  f.read (_indices.data (), size);

  // - MOVT ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOVT');

  // Validated before the alias below is formed, so that `vertices` is only ever
  // built over a payload that really is a whole number of vertices.
  std::uint32_t const vertex_count (elementCount (fname, 'MOVT', size, sizeof (::glm::vec3)));

  // let's hope it's padded to 12 bytes, not 16...
  // readChunkHeader has already established that `size` bytes really are left
  // in the file, so the aliased read below stays inside the buffer.
  ::glm::vec3 const* vertices = reinterpret_cast< ::glm::vec3 const*>(f.getPointer ());

  VertexBoxMin = ::glm::vec3 (std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
  VertexBoxMax = ::glm::vec3 (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

  rad = 0;

  _vertices.resize(vertex_count);

  for (size_t i = 0; i < _vertices.size(); ++i)
  {
    _vertices[i] = glm::vec3(vertices[i].x, vertices[i].z, -vertices[i].y);

    ::glm::vec3& v = _vertices[i];

    if (v.x < VertexBoxMin.x) VertexBoxMin.x = v.x;
    if (v.y < VertexBoxMin.y) VertexBoxMin.y = v.y;
    if (v.z < VertexBoxMin.z) VertexBoxMin.z = v.z;
    if (v.x > VertexBoxMax.x) VertexBoxMax.x = v.x;
    if (v.y > VertexBoxMax.y) VertexBoxMax.y = v.y;
    if (v.z > VertexBoxMax.z) VertexBoxMax.z = v.z;
  }

  center = (VertexBoxMax + VertexBoxMin) * 0.5f;
  rad = glm::distance(center, VertexBoxMax);

  f.seekRelative (size);

  // - MONR ----------------------------------------------

  size = readChunkHeader(f, fname, 'MONR');

  _normals.resize (elementCount (fname, 'MONR', size, sizeof (::glm::vec3)));

  f.read (_normals.data(), size);

  for (auto& n : _normals)
  {
    n = {n.x, n.z, -n.y};
  }

  // - MOTV ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOTV');

  _texcoords.resize (elementCount (fname, 'MOTV', size, sizeof (glm::vec2)));

  f.read (_texcoords.data (), size);

  // - MOBA ----------------------------------------------

  size = readChunkHeader(f, fname, 'MOBA');

  _batches.resize (elementCount (fname, 'MOBA', size, sizeof (wmo_batch)));
  f.read (_batches.data (), size);

  // MOVI indexes MOVT. Nothing downstream re-checks this, and intersect() runs
  // _vertices[_indices[i]] on every click, so a file whose index buffer outruns
  // its vertex buffer would read out of bounds long after loading.
  for (uint16_t const index : _indices)
  {
    if (index >= _vertices.size())
    {
      throwWmoParseError(fname, "MOVI references vertex " + std::to_string(index)
                                + " but MOVT only holds " + std::to_string(_vertices.size()) + ".");
    }
  }

  // intersect() walks _indices[batch.index_start .. index_start + index_count).
  for (wmo_batch const& batch : _batches)
  {
    // index_count is a triangle *list* count. intersect() steps i by 3 from
    // index_start and dereferences _indices[i], [i+1] and [i+2], and
    // WMOGroupRender feeds index_count straight to a GL_TRIANGLES draw call, so
    // a count that is not a multiple of 3 has no valid reading -- the format has
    // no partial triangle. Left unchecked, the final iteration reads up to two
    // elements past the batch, and past the end of _indices entirely whenever
    // this is the last batch (the bound check below only proves the batch itself
    // fits).
    //
    // Rejected at load rather than by bounding intersect()'s loop, for three
    // reasons: this is one check over a short vector once per group file,
    // whereas the loop runs per triangle per batch on every click in the
    // viewport; bounding the loop would leave the file loaded, so the renderer
    // would still issue draw calls from a batch table already proved
    // inconsistent; and a malformed batch table means the group's geometry is
    // not trustworthy in the first place, which is a load failure, not a
    // picking edge case.
    if (batch.index_count % 3)
    {
      throwWmoParseError(fname, "MOBA batch declares " + std::to_string(batch.index_count)
                                + " indices, which is not a whole number of triangles.");
    }

    if (static_cast<std::size_t>(batch.index_start) + batch.index_count > _indices.size())
    {
      throwWmoParseError(fname, "MOBA batch covers indices " + std::to_string(batch.index_start)
                                + ".." + std::to_string(static_cast<std::size_t>(batch.index_start) + batch.index_count)
                                + " but MOVI only holds " + std::to_string(_indices.size()) + ".");
    }
  }

  // Both fix_vertex_color_alpha() and load_mocv() index
  // _batches[transparency_batches_count - 1].
  if (header.transparency_batches_count > _batches.size())
  {
    throwWmoParseError(fname, "Group header declares " + std::to_string(header.transparency_batches_count)
                              + " transparency batches but MOBA only holds "
                              + std::to_string(_batches.size()) + ".");
  }

  _renderer.initRenderBatches();

  // - MOLR ----------------------------------------------
  if (header.flags.has_light)
  {
    if (readOptionalChunkHeader (f, fname, 'MOLR', &size))
    {
      f.seekRelative (size);
    }
  }
  // - MODR ----------------------------------------------
  if (header.flags.has_doodads)
  {
    if (readOptionalChunkHeader (f, fname, 'MODR', &size))
    {
      _doodad_ref.resize (elementCount (fname, 'MODR', size, sizeof (int16_t)));
      f.read (_doodad_ref.data (), size);
    }
  }
  // - MOBN ----------------------------------------------
  if (header.flags.has_bsp_tree)
  {
    if (readOptionalChunkHeader (f, fname, 'MOBN', &size))
    {
      f.seekRelative(size);
    }
  }
  // - MOBR ----------------------------------------------
  if (header.flags.has_bsp_tree)
  {
    if (readOptionalChunkHeader (f, fname, 'MOBR', &size))
    {
      f.seekRelative (size);
      // std::vector<uint16_t> bsp_indices;
      // bsp_indices.resize(size / sizeof(uint16_t));
      // f.read(bsp_indices.data(), size);
      // _bsp_indices = bsp_indices;
    }
  }

  if (header.flags.flag_0x400)
  {
    // - MPBV ----------------------------------------------
    if (readOptionalChunkHeader (f, fname, 'MPBV', &size))
    {
      f.seekRelative (size);
    }

    // - MPBP ----------------------------------------------
    if (readOptionalChunkHeader (f, fname, 'MPBP', &size))
    {
      f.seekRelative (size);
    }

    // - MPBI ----------------------------------------------
    if (readOptionalChunkHeader (f, fname, 'MPBI', &size))
    {
      f.seekRelative (size);
    }

    // - MPBG ----------------------------------------------
    if (readOptionalChunkHeader (f, fname, 'MPBG', &size))
    {
      f.seekRelative (size);
    }
  }
  // - MOCV ----------------------------------------------
  if (header.flags.has_vertex_color)
  {
    if (readOptionalChunkHeader (f, fname, 'MOCV', &size))
    {
      // load_mocv() aliases the payload as uint32_t colours, so it is checked
      // here on the same terms as every other array chunk before being handed
      // a size it would otherwise divide and trust.
      elementCount (fname, 'MOCV', size, sizeof (std::uint32_t));

      load_mocv(f, size);
    }
  }
  // - MLIQ ----------------------------------------------
  if (header.flags.has_water)
  {
    if (readOptionalChunkHeader (f, fname, 'MLIQ', &size))
    {
      WMOLiquidHeader hlq;

      if (size < 0x1E)
      {
        throwWmoParseError(fname, "MLIQ chunk is only " + std::to_string(size)
                                  + " bytes, expected at least 30 for its header.");
      }

      f.read(&hlq, 0x1E);

      // wmo_liquid::initGeometry aliases the file buffer for
      // (A + 1) * (B + 1) vertices followed by A * B tiles, all taken from this
      // header, and never checks them against the chunk. Negative or oversized
      // counts walked straight off the end of the file.
      if (hlq.A < 0 || hlq.B < 0)
      {
        throwWmoParseError(fname, "MLIQ declares a negative liquid tile count ("
                                  + std::to_string(hlq.A) + "x" + std::to_string(hlq.B) + ").");
      }

      std::size_t const liquid_bytes
        ( 0x1E
        + static_cast<std::size_t>(hlq.A + 1) * static_cast<std::size_t>(hlq.B + 1) * sizeof (WmoLiquidVertex)
        + static_cast<std::size_t>(hlq.A) * static_cast<std::size_t>(hlq.B) * sizeof (SMOLTile)
        );

      if (liquid_bytes > size)
      {
        throwWmoParseError(fname, "MLIQ declares a " + std::to_string(hlq.A) + "x" + std::to_string(hlq.B)
                                  + " liquid grid needing " + std::to_string(liquid_bytes)
                                  + " bytes but the chunk is only " + std::to_string(size) + ".");
      }

      lq = std::make_unique<wmo_liquid> ( &f
          , hlq
          // , wmo->materials[hlq.material_id] // some models have mat_id = -1, eg "world/wmo/dungeon/md_fishinghole/md_fishingholeice_001.wmo"
          , header.group_liquid
          , (bool)wmo->flags.use_liquid_type_dbc_id
          , (bool)header.flags.ocean
      );

      // creating the wmo liquid doesn't move the position
      f.seekRelative(size - 0x1E);
    }
  }
  if (header.flags.has_mori_morb)
  {
    // - MORI ----------------------------------------------
    if (readOptionalChunkHeader (f, fname, 'MORI', &size))
    {
      f.seekRelative (size);
    }

    // - MORB ----------------------------------------------
    if (readOptionalChunkHeader (f, fname, 'MORB', &size))
    {
      f.seekRelative (size);
    }
  }

  // - MOTV ----------------------------------------------
  if (header.flags.has_two_motv)
  {
    if (readOptionalChunkHeader (f, fname, 'MOTV', &size))
    {
      _texcoords_2.resize(elementCount (fname, 'MOTV', size, sizeof (glm::vec2)));
      f.read(_texcoords_2.data(), size);
    }
  }
  // - MOCV ----------------------------------------------
  if (header.flags.use_mocv2_for_texture_blending)
  {
    if (readOptionalChunkHeader (f, fname, 'MOCV', &size))
    {
      std::vector<CImVector> mocv_2(elementCount (fname, 'MOCV', size, sizeof (CImVector)));
      f.read(mocv_2.data(), size);

      for (int i = 0; i < mocv_2.size(); ++i)
      {
        float alpha = static_cast<float>(mocv_2[i].a) / 255.f;

        // the second mocv is used for texture blending only
        if (header.flags.has_vertex_color)
        {
          // The first MOCV chunk sizes _vertex_colors, and nothing guarantees
          // the second one is no longer than the first.
          if (i >= _vertex_colors.size())
          {
            break;
          }

          _vertex_colors[i].w = alpha;
        }
        else // no vertex coloring, only texture blending with the alpha
        {
          _vertex_colors.emplace_back(0.f, 0.f, 0.f, alpha);
        }
      }
    }
  }

  //dl_light = 0;
  // "real" lighting?
  if (header.flags.indoor && header.flags.has_vertex_color)
  {
    ::glm::vec3 dirmin(1, 1, 1);
    float lenmin;

    for (auto doodad : _doodad_ref)
    {
      if (doodad >= wmo->modelis.size())
      {
          continue;
          LogError << "The WMO file currently loaded is potentially corrupt. Non-existing doodad referenced." << std::endl;
      }

      lenmin = 999999.0f * 999999.0f;
      ModelInstance& mi = wmo->modelis[doodad];
      for (unsigned int j = 0; j < wmo->lights.size(); j++)
      {
        WMOLight& l = wmo->lights[j];
        ::glm::vec3 dir = l.pos - mi.pos;

        float ll = glm::length(dir) * glm::length(dir);
        if (ll < lenmin)
        {
          lenmin = ll;
          dirmin = dir;
        }
      }
      wmo->model_nearest_light_vector[doodad] = dirmin;
    }

    use_outdoor_lights = false;
  }
  else
  {
    use_outdoor_lights = true;
  }
}

void WMOGroup::load_mocv(BlizzardArchive::ClientFile& f, uint32_t size)
{
  uint32_t const* colors = reinterpret_cast<uint32_t const*> (f.getPointer());
  _vertex_colors.resize(size / sizeof(uint32_t));

  for (size_t i(0); i < size / sizeof(uint32_t); ++i)
  {
    _vertex_colors[i] = colorFromInt(colors[i]);
  }

  if (wmo->flags.do_not_fix_vertex_color_alpha)
  {
    int interior_batchs_start = 0;

    if (header.transparency_batches_count > 0)
    {
      interior_batchs_start = _batches[header.transparency_batches_count - 1].vertex_end + 1;
    }

    for (int n = interior_batchs_start; n < _vertex_colors.size(); ++n)
    {
      _vertex_colors[n].w = header.flags.exterior ? 1.f : 0.f;
    }
  }
  else
  {
    fix_vertex_color_alpha();
  }

  // there's no read so this is required
  f.seekRelative(size);
}

void WMOGroup::fix_vertex_color_alpha()
{
  int interior_batchs_start = 0;

  if (header.transparency_batches_count > 0)
  {
    interior_batchs_start = _batches[header.transparency_batches_count - 1].vertex_end + 1;
  }

  glm::vec4 wmo_ambient_color;

  if (wmo->flags.use_unified_render_path)
  {
    wmo_ambient_color = {0.f, 0.f, 0.f, 0.f};
  }
  else
  {
    wmo_ambient_color = wmo->ambient_light_color;
    // w is not used, set it to 0 to avoid changing the vertex color alpha
    wmo_ambient_color.w = 0.f;
  }

  for (int i = 0; i < _vertex_colors.size(); ++i)
  {
    auto& color = _vertex_colors[i];
    float r = color.x;
    float g = color.y;
    float b = color.z;
    float a = color.w;

    // I removed the color = color/2 because it's just multiplied by 2 in the shader afterward in blizzard's code
    if (i >= interior_batchs_start)
    {
      r += ((r * a / 64.f) - wmo_ambient_color.x);
      g += ((g * a / 64.f) - wmo_ambient_color.y);
      r += ((b * a / 64.f) - wmo_ambient_color.z);
    }
    else
    {
      r -= wmo_ambient_color.x;
      g -= wmo_ambient_color.y;
      b -= wmo_ambient_color.z;

      r = (r * (1.f - a));
      g = (g * (1.f - a));
      b = (b * (1.f - a));
    }

    color.x = std::min(255.f, std::max(0.f, r));
    color.y = std::min(255.f, std::max(0.f, g));
    color.z = std::min(255.f, std::max(0.f, b));
    color.w = 1.f; // default value used in the shader so I simplified it here,
                   // it can be overriden by the 2nd mocv chunk
  }
}

bool WMOGroup::is_visible( glm::mat4x4 const& transform
                         , math::frustum const& frustum
                         , float const& cull_distance
                         , glm::vec3 const& camera
                         , display_mode display
                         ) const
{
   // glm::vec3 pos = transform * glm::vec4(center, 0);
   // 
    // glm::vec3 pos = transform[3] * glm::vec4(center, 1.0f);
    // glm::vec3 test_pos = transform[3] + glm::vec4(center, 0);
    // glm::vec3 test_pos2 = transform[3];


    // TODO center is just the center of the group vertex box, and rad is distance from box max to center.
    // to do operation on group we need to get its true position
    // 
    // adjusted group transform mat = 
    glm::vec3 pos = transform *  glm::vec4(center, 1.0f);

  float dist = display == display_mode::in_3D
    ? glm::distance(pos, camera) - rad
    : std::abs(pos.y - camera.y) - rad;

  // Camera is within the bounding sphere, always draw
  if (dist < 0)
      return true;

  float cull = cull_distance;

  if (dist > cull_distance)
      return false;


  if (!frustum.intersects(pos + BoundingBoxMin, pos + BoundingBoxMax))
  {
    return false;
  }

  return true;
}

[[nodiscard]]
std::vector<uint16_t> WMOGroup::doodad_ref() const
{
  return _doodad_ref;
}

[[nodiscard]]
bool WMOGroup::has_skybox() const
{
  return header.flags.skybox;
}

[[nodiscard]]
bool WMOGroup::is_indoor() const
{
  return header.flags.indoor;
}

[[nodiscard]]
Noggit::Rendering::WMOGroupRender* WMOGroup::renderer()
{
  return &_renderer;
}

void WMOGroup::intersect (math::ray const& ray, std::vector<float>* results) const
{
  if (!ray.intersect_bounds (VertexBoxMin, VertexBoxMax))
  {
    return;
  }

  //! \todo Also allow clicking on doodads and liquids.
  for (auto&& batch : _batches)
  {
    for (size_t i (batch.index_start); i < batch.index_start + batch.index_count; i += 3)
    {
      // TODO : only intersect visible triangles
      // TODO : option to only check collision
      if ( auto&& distance
         = ray.intersect_triangle ( _vertices[_indices[i + 0]]
                                  , _vertices[_indices[i + 1]]
                                  , _vertices[_indices[i + 2]]
                                  )
         )
      {
        results->emplace_back (*distance);
      }
    }
  }
}

/*
void WMOGroup::drawLiquid ( glm::mat4x4 const& transform
                          , liquid_render& render
                          , bool // draw_fog
                          , int animtime
                          )
{
  // draw liquid
  //! \todo  culling for liquid boundingbox or something
  if (lq) 
  { 
    gl.enable(GL_BLEND);
    gl.depthMask(GL_TRUE);

    lq->draw ( transform, render, animtime);

    gl.disable(GL_BLEND);
  }
}
*/

void WMOGroup::setupFog (bool draw_fog, std::function<void (bool)> setup_fog)
{
  if (use_outdoor_lights || fog == -1) {
    setup_fog (draw_fog);
  }
  else {
    wmo->fogs[fog].setup();
  }
}

void WMOFog::init(BlizzardArchive::ClientFile* f)
{
  f->read(this, 0x30);
  color = glm::vec4(((color1 & 0x00FF0000) >> 16) / 255.0f, ((color1 & 0x0000FF00) >> 8) / 255.0f,
    (color1 & 0x000000FF) / 255.0f, ((color1 & 0xFF000000) >> 24) / 255.0f);
  float temp;
  temp = pos.y;
  pos.y = pos.z;
  pos.z = -temp;
  fogstart = fogstart * fogend * 1.5f;
  fogend *= 1.5;
}

void WMOFog::setup()
{

}

decltype (WMOManager::_) WMOManager::_;

void WMOManager::report()
{
  std::string output = "Still in the WMO manager:\n";
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const& key, WMO const&)
            {
              output += " - " + key.stringRepr() + "\n";
            }
          );
  LogDebug << output;
}

void WMOManager::clear_hidden_wmos()
{
  _.apply ( [&] (BlizzardArchive::Listfile::FileKey const&, WMO& wmo)
            {
              wmo.show();
            }
          );
}

void WMOManager::unload_all(Noggit::NoggitRenderContext context)
{
    _.context_aware_apply(
        [&] (BlizzardArchive::Listfile::FileKey const&, WMO& wmo)
        {
            wmo.renderer()->unload();
        }
        , context
    );
}

bool wmo_triangle_material_info::isTransFace() const
{
  return flags.flag_0x01 && (flags.detail || flags.render);
}

bool wmo_triangle_material_info::isColor() const
{
  return !flags.collision;
}

bool wmo_triangle_material_info::isRenderFace() const
{
  return flags.render && !flags.detail;
}

bool wmo_triangle_material_info::isCollidable() const
{
  return flags.collision || isRenderFace();
}

bool wmo_triangle_material_info::isCollision() const
{
  return texture == 0xff;
}
