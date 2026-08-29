// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "map_horizon.h"

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/map_index.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Misc.h>
#include <noggit/SafeFileWrite.hpp>
#include <noggit/Selection.h>
#include <noggit/WMOInstance.h>
#include <noggit/World.h>

#include <opengl/context.hpp>
#include <opengl/context.inl>

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_set>

// The MODF record is a fixed 64-byte on-disk layout, and every size this file computes for the
// chunk assumes it. ENTRY_MODF is declared as plain members plus a std::array<glm::vec3, 2>
// (MapHeaders.h:113-125), which lays out to exactly that on every supported target -- but the
// assumption is worth stating where it is relied on rather than discovered from a WDL the client
// refuses to load. MapTile.cpp writes MODF with the literal 0x40 for the same reason.
static_assert(sizeof(ENTRY_MODF) == 64, "MODF records are 64 bytes on disk");

struct color
{
  color(unsigned char r, unsigned char g, unsigned char b)
    : _r(r)
    , _g(g)
    , _b(b)
  {}

  uint32_t to_int() const {
    return (_b) | (_g << 8) | (_r << 16) | (uint32_t)(0xFFu << 24);
  }

  operator uint32_t () const {
    return to_int();
  }

  unsigned char _r;
  unsigned char _g;
  unsigned char _b;
};

struct ranged_color
{
  ranged_color (const color& c, const int16_t& start, const int16_t& stop)
    : _color (c)
    , _start (start)
    , _stop (stop)
  {}

  const color   _color;
  const int16_t _start;
  const int16_t _stop;
};

static inline color lerp_color(const color& start, const color& end, float t)
{
  return color ( (end._r) * t + (start._r) * (1.0 - t)
               , (end._g) * t + (start._g) * (1.0 - t)
               , (end._b) * t + (start._b) * (1.0 - t)
               );
}

static inline uint32_t color_for_height (int16_t height)
{
  static const ranged_color colors[] =
    { ranged_color (color (20, 149, 7), 0, 600)
    , ranged_color (color (137, 84, 21), 600, 1200)
    , ranged_color (color (96, 96, 96), 1200, 1600)
    , ranged_color (color (255, 255, 255), 1600, 0x7FFF)
    };
  static const size_t num_colors (sizeof (colors) / sizeof (ranged_color));

  if (height < colors[0]._start)
  {
    return color (0, 0, 255 + std::max (height / 2.0, -255.0));
  }
  else if (height >= colors[num_colors - 1]._stop)
  {
    return colors[num_colors]._color;
  }

  float t (1.0);
  size_t correct_color (num_colors - 1);

  for (size_t i (0); i < num_colors - 1; ++i)
  {
    if (height >= colors[i]._start && height < colors[i]._stop)
    {
      t = float(height - colors[i]._start) / float (colors[i]._stop - colors[i]._start);
      correct_color = i;
      break;
    }
  }

  return lerp_color(colors[correct_color]._color, colors[correct_color + 1]._color, t);
}

// =================================================================================================
// The WMO half of the horizon: MWMO, MWID and MODF.
// =================================================================================================
//
// WHAT THE HORIZON ACTUALLY DRAWS, MEASURED. The premise this work started from was that the
// Stormwind skyline seen from the Elwynn border comes out of the WDL. It does not. Azeroth.wdl,
// pulled from patch-2.MPQ of a stock 3.3.5a.12340 client, carries MWMO, MWID and MODF chunks of
// size 0, and Stormwind.wmo -- a 1488 x 1488 yard, 376 yard tall instance sitting in
// Azeroth_32_48.adt and Azeroth_31_49.adt -- appears in neither. Distant Stormwind is the client
// drawing an ordinary ADT WMO at long range. The WDL's MODF is a different and much rarer thing:
// of 106 stock WDLs, two use it, and those two are the evidence everything below is built on.
namespace
{
  // Smallest instance that goes on the horizon when its model has no authored _LOW variant.
  //
  // Blizzard's own selection is editorial rather than geometric: it ships a hand-made
  // "<name>_LOW.wmo" for the few buildings it wants on a skyline and lists exactly those. Custom
  // content has no such variant, so honouring _LOW and nothing else would leave every custom city
  // off the horizon -- which is the whole problem this code exists to fix. The fallback therefore
  // puts the FULL model in the WDL, which the client will draw quite happily, and that has to be
  // gated or a map with forty thousand wall segments would put all forty thousand on the skyline.
  //
  // Calibrated against data, not chosen by feel. Measured over the 15 stock horizon entries and a
  // 79-instance sample of ADT placements from Northrend_22_17, Northrend_31_20, Azeroth_32_48 and
  // Azeroth_31_49: the smallest thing Blizzard put on a horizon is 49.1 yards tall with a 2779
  // square yard footprint, so both thresholds sit just under that. The pair keeps 15 of the 15
  // stock horizon entries and admits seven models from the sample -- Stormwind, Dalaran,
  // Northshire Abbey, the Scarlet Onslaught docks, NH_Cathedral, MD_MountainCave and
  // ND_Human_Tower_Open. The largest instance it rejects is MD_Goldmine, 32.2 yards tall. That is
  // the landmark / wall-furniture split the gate is trying to make.
  constexpr float HORIZON_MIN_HEIGHT = 40.0f;
  constexpr float HORIZON_MIN_FOOTPRINT = 2500.0f;

  //! The "<path>_LOW.wmo" companion of `wmo_path` when the client data has one, else empty.
  //!
  //! This is the opt-in signal Blizzard uses and it is honoured with no size gate at all: an
  //! author who made a low-detail variant has already said the model belongs on the skyline, and
  //! second-guessing that with a bounding box would drop small landmarks somebody built the
  //! variant for.
  std::string horizon_variant_path(std::string const& wmo_path)
  {
    // Case-insensitive, because a listfile path, an MWMO string and a hand-typed path disagree on
    // case constantly and the archive lookup below does not care either.
    std::string lowered (wmo_path);
    std::transform ( lowered.begin(), lowered.end(), lowered.begin()
                   , [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
                   );

    if (lowered.size() < 4 || lowered.compare(lowered.size() - 4, 4, ".wmo") != 0)
    {
      return {};
    }

    std::string const candidate (wmo_path.substr(0, wmo_path.size() - 4) + "_LOW.wmo");

    return Noggit::Application::NoggitApplication::instance()->clientData()->exists(candidate)
      ? candidate
      : std::string();
  }

  //! True when an instance is big enough to be worth drawing at horizon range on its own merits.
  bool is_horizon_scale(std::array<glm::vec3, 2> const& extents)
  {
    // Noggit's frame: y is up, x and z are the ground plane. The extents are the world-space
    // axis-aligned box, which is what MODF stores and therefore what the numbers above were
    // measured from.
    float const height (extents[1].y - extents[0].y);
    float const footprint ((extents[1].x - extents[0].x) * (extents[1].z - extents[0].z));

    return height >= HORIZON_MIN_HEIGHT && footprint >= HORIZON_MIN_FOOTPRINT;
  }

  //! Append the horizon-worthy WMO placements of one loaded tile to `out`.
  //!
  //! `seen` carries across tiles and is why this takes it by reference: a WMO wider than one ADT
  //! is listed in the MODF of every ADT it touches, under the same uniqueID, and a WDL that named
  //! it four times would draw it four times.
  void collect_tile_horizon_wmos( World* world
                                , MapTile* tile
                                , std::vector<Noggit::wdl_wmo_placement>& out
                                , std::unordered_set<std::uint32_t>& seen
                                , std::size_t& too_small
                                )
  {
    std::vector<std::uint32_t> const* const uids (tile->get_uids());

    if (!uids)
    {
      return;
    }

    for (std::uint32_t const uid : *uids)
    {
      if (!seen.insert(uid).second)
      {
        continue;
      }

      // The same route MapTile::save takes to turn a tile's uid list into placements
      // (MapTile.cpp:607-632), so the WDL record is built from the identical object the ADT
      // record is built from. Anything else would let the two drift.
      auto const model (world->get_model(uid));

      if (!model || model.value().index() != eEntry_Object)
      {
        continue;
      }

      SceneObject* const object (std::get<selected_object_type>(model.value()));

      if (object->which() != eWMO)
      {
        continue;
      }

      WMOInstance* const instance (static_cast<WMOInstance*>(object));
      std::string const source_path (instance->wmo->file_key().filepath());
      std::string horizon_path (horizon_variant_path(source_path));

      if (horizon_path.empty())
      {
        // getExtents() only recomputes while the WMO has finished loading
        // (WMOInstance::ensureExtents, WMOInstance.cpp:277-283); on one that has not, it returns
        // the box read out of the ADT's own MODF. Both answers are the right one to write here,
        // which is why no wait is needed: the second IS the record we are mirroring.
        if (!is_horizon_scale(instance->getExtents()))
        {
          ++too_small;
          continue;
        }

        horizon_path = source_path;
      }

      ENTRY_MODF entry;

      // nameID is assigned when MWMO is written, not here. Everything else is copied from the
      // instance in the same order and with the same conversions MapTile::save uses
      // (MapTile.cpp:902-923), because the measured rule is that a WDL record equals the ADT
      // record it mirrors in every field but the name.
      entry.nameID = 0;
      entry.uniqueID = instance->uid;

      entry.pos[0] = instance->pos.x;
      entry.pos[1] = instance->pos.y;
      entry.pos[2] = instance->pos.z;

      entry.rot[0] = instance->dir.x;
      entry.rot[1] = instance->dir.y;
      entry.rot[2] = instance->dir.z;

      entry.extents = instance->getExtents();

      entry.flags = instance->mFlags;
      entry.doodadSet = instance->doodadset();
      entry.nameSet = instance->mNameset;

      // scale * 1024, matching MapTile::save, and NOT the 0 Blizzard writes. Measured: all 15
      // stock WDL records and all 79 sampled ADT records hold 0 in this field, so the 3.3.5
      // client plainly ignores it -- it would otherwise scale every stock WMO to nothing. Writing
      // what Noggit writes into its own ADTs is what keeps "the WDL record equals the ADT record"
      // true for files this editor produced, which is the invariant the whole scheme rests on.
      entry.scale = static_cast<std::uint16_t>(instance->scale * 1024.0f);

      out.push_back({horizon_path, entry});
    }
  }

  //! Write MWMO, MWID and MODF for `placements`, advancing `cur_pos` past all three.
  //!
  //! Always writes all three chunks, empty ones included. Every stock WDL has them present at
  //! size 0 when it has no objects, and our own reader walks the file sequentially rather than by
  //! offset, so a missing chunk would be read as whatever follows it.
  void write_wdl_objects( util::sExtendableArray& wdl
                        , int& cur_pos
                        , std::vector<Noggit::wdl_wmo_placement> const& placements
                        )
  {
    // First-appearance order, which is what Blizzard's files use: Northrend.wdl's MODF nameIDs
    // run 0, 0, 1, 2 against an MWMO of NH_CATHEDRAL_LOW, VALGARDE_IC_LOW, OILPLATFORM_LOW. The
    // walk that fills `placements` is a fixed y-then-x scan of the tile grid, so the order is
    // reproducible across runs on unchanged data and two saves diff cleanly.
    std::vector<std::string> ordered_names;
    std::vector<std::uint32_t> name_offsets;
    std::vector<std::uint32_t> name_id_of_placement;

    name_id_of_placement.reserve(placements.size());

    std::uint32_t running_offset (0);

    for (Noggit::wdl_wmo_placement const& placement : placements)
    {
      std::string const stored (misc::normalize_adt_filename(placement.filename));

      auto const existing (std::find(ordered_names.begin(), ordered_names.end(), stored));

      if (existing != ordered_names.end())
      {
        name_id_of_placement.push_back
          (static_cast<std::uint32_t>(std::distance(ordered_names.begin(), existing)));
        continue;
      }

      name_id_of_placement.push_back(static_cast<std::uint32_t>(ordered_names.size()));
      ordered_names.push_back(stored);
      name_offsets.push_back(running_offset);
      running_offset += static_cast<std::uint32_t>(stored.size() + 1);
    }

    // ---- MWMO ---------------------------------------------------------------------------------
    int const mwmo_position (cur_pos);
    wdl.Extend(8);
    SetChunkHeader(wdl, cur_pos, 'MWMO', 0);
    cur_pos += 8;

    for (std::string const& name : ordered_names)
    {
      // c_str() gives size() + 1 bytes counting the terminator, which is what MWMO stores: the
      // block is a run of NUL-terminated strings with no padding, and MWID's offsets below are
      // the running sums of exactly these lengths.
      wdl.Insert(cur_pos, static_cast<unsigned long>(name.size() + 1), name.c_str());
      cur_pos += static_cast<int>(name.size() + 1);
      wdl.GetPointer<sChunkHeader>(mwmo_position)->mSize += static_cast<int>(name.size() + 1);
    }

    // ---- MWID ---------------------------------------------------------------------------------
    //
    // BYTE OFFSETS INTO MWMO, not indices. Measured on Northrend.wdl, whose MWID is {0, 68, 115}
    // for three names of 67, 46 and 59 characters -- running sums of strlen + 1. Writing indices
    // there produces a file that looks right in a hex editor and names the wrong model.
    int const mwid_size (static_cast<int>(4 * ordered_names.size()));
    wdl.Extend(8 + mwid_size);
    SetChunkHeader(wdl, cur_pos, 'MWID', mwid_size);

    {
      // memcpy rather than a typed store, because MWMO leaves the file misaligned and everything
      // after it inherits that: in Northrend.wdl the MWID header is at 0xC3 and its data at 0xCB,
      // which is 3 mod 4. SetChunkHeader above has the same exposure and is deliberately left
      // alone -- it is shared with every other chunk writer in the tree and changing it is not
      // this file's business -- but the two loops here are new code and are written safely.
      auto const mwid_bytes (wdl.GetPointer<char>(cur_pos + 8));

      for (std::size_t i (0); i < name_offsets.size(); ++i)
      {
        std::uint32_t const offset (name_offsets[i]);

        std::memcpy(mwid_bytes.get() + i * sizeof(std::uint32_t), &offset, sizeof(offset));
      }
    }

    cur_pos += 8 + mwid_size;

    // ---- MODF ---------------------------------------------------------------------------------
    int const modf_size (static_cast<int>(sizeof(ENTRY_MODF) * placements.size()));
    wdl.Extend(8 + modf_size);
    SetChunkHeader(wdl, cur_pos, 'MODF', modf_size);

    {
      // memcpy for the same reason as MWID above and as the reader: with Northrend's three names
      // the MODF header lands at 0xD7 and the first record at 0xDF, so assigning an ENTRY_MODF
      // through that pointer is a misaligned store of six floats -- undefined behaviour that x86
      // forgives and other targets do not.
      auto const modf_bytes (wdl.GetPointer<char>(cur_pos + 8));

      for (std::size_t i (0); i < placements.size(); ++i)
      {
        ENTRY_MODF record (placements[i].placement);
        record.nameID = name_id_of_placement[i];

        std::memcpy(modf_bytes.get() + i * sizeof(ENTRY_MODF), &record, sizeof(ENTRY_MODF));
      }
    }

    cur_pos += 8 + modf_size;
  }
}

namespace Noggit
{

map_horizon::map_horizon(const std::string& basename, const MapIndex * const index)
{
  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << ".wdl";
  _filename = filename.str();

  if (!Application::NoggitApplication::instance()->clientData()->exists(_filename))
  {
    LogError << "file \"World\\Maps\\" << basename << "\\" << basename << ".wdl\" does not exist." << std::endl;

    // The minimap image is allocated even on this path, and it has to be: the original returned
    // here leaving _qt_minimap default-constructed, i.e. 0x0 with a null data pointer. Every
    // subsequent update_minimap_tile then called QImage::setPixel on it, which bounds-checks
    // against width()/height() and answers with a qWarning per pixel rather than a crash -- 256
    // of them per tile. "Generate new WDL" on a map that has none walks every flagged tile, so
    // on a full 64x64 map that is up to 64 * 64 * 256 = 1048576 formatted warnings written to
    // stderr, which is where the operation appears to hang. set_minimap does exactly what the
    // successful path does at the end of this constructor, and with no _tiles loaded it paints
    // every existing ADT in the "tile exists but the WDL has no data for it" colour, which is a
    // true description of a map with no WDL.
    set_minimap(index);
    return;
  }

  BlizzardArchive::ClientFile wdl_file (_filename, Application::NoggitApplication::instance()->clientData());

  uint32_t fourcc;
  uint32_t size;

  bool done = false;

  do
  {
    wdl_file.read(&fourcc, 4);
    wdl_file.read(&size, 4);

    switch (fourcc)
    {
      case 'MVER':
      {
        uint32_t version;
        wdl_file.read(&version, 4);
        assert(size == 4 && version == 18);

        break;
      }
      // todo: handle those too ?
      case 'MWMO':
      {
        {
            char const* lCurPos = reinterpret_cast<char const*>(wdl_file.getPointer());
            char const* lEnd = lCurPos + size;
        
            while (lCurPos < lEnd)
            {
                mWMOFilenames.push_back(BlizzardArchive::ClientData::normalizeFilenameInternal(std::string(lCurPos)));
                lCurPos += strlen(lCurPos) + 1;
            }
        }
        wdl_file.seekRelative(size);
        break;
      }
      case 'MWID':
          // Skipped on purpose, and it is not a TODO. MWID holds the byte offset of each MWMO
          // string inside the MWMO block; the reader above has already split that block on its
          // NUL bytes, which produces the same list in the same order. Keeping the offsets would
          // give us a second, redundant statement of the name order that the writer rebuilds from
          // scratch anyway -- and the two could then disagree.
          wdl_file.seekRelative(size);
          break;
      case 'MODF':
      {
        // Read through memcpy rather than by casting the file pointer, which is what the
        // commented-out original did. MODF's data does not begin on a 4-byte boundary: MWMO holds
        // NUL-terminated strings with no padding after them, so in Northrend.wdl the header sits
        // at 0xD7 and the first record at 0xDF -- 223, which is 3 mod 4. Reading a float array
        // through a misaligned ENTRY_MODF* is undefined behaviour that happens to work on x86 and
        // does not elsewhere.
        for (uint32_t offset = 0; offset + sizeof(ENTRY_MODF) <= size; offset += sizeof(ENTRY_MODF))
        {
          ENTRY_MODF entry;
          std::memcpy(&entry, wdl_file.getPointer() + offset, sizeof(ENTRY_MODF));

          if (entry.nameID >= mWMOFilenames.size())
          {
            // Every stock WDL puts MWMO before MODF, so this only fires on a malformed file. It
            // is reported rather than assumed away, because the alternative -- indexing the name
            // table out of bounds -- reads whatever follows it and writes that path back out.
            LogError << "wdl MODF entry " << (offset / sizeof(ENTRY_MODF)) << " names MWMO index "
                     << entry.nameID << " but only " << mWMOFilenames.size()
                     << " name(s) were read; the entry is dropped." << std::endl;
            continue;
          }

          _wdl_wmo_instances.push_back({mWMOFilenames[entry.nameID], entry});
        }

        wdl_file.seekRelative(size);
        break;
      }
      case 'MAOF':
      {
        assert(size == 64 * 64 * sizeof(uint32_t));

        uint32_t mare_offsets[64][64];
        wdl_file.read(mare_offsets, 64 * 64 * sizeof(uint32_t));

        // - MARE and MAHO by offset ---------------------------
        for (size_t y(0); y < 64; ++y)
        {
          for (size_t x(0); x < 64; ++x)
          {
            if (!mare_offsets[y][x])
            {
              continue;
            }

            wdl_file.seek(mare_offsets[y][x]);
            wdl_file.read(&fourcc, 4);
            wdl_file.read(&size, 4);

            assert(fourcc == 'MARE');
            assert(size == 0x442);

            _tiles[y][x] = std::make_unique<map_horizon_tile>();

            //! \todo There also is MAHO giving holes into this heightmap.
            wdl_file.read(_tiles[y][x]->height_17, 17 * 17 * sizeof(int16_t));
            wdl_file.read(_tiles[y][x]->height_16, 16 * 16 * sizeof(int16_t));

            if (wdl_file.getPos() < wdl_file.getSize())
            {
                wdl_file.read(&fourcc, 4);
                if (fourcc == 'MAHO')
                {
                    wdl_file.read(&size, 4);
                    assert(size == 0x20);
                    wdl_file.read(_tiles[y][x]->holes, 16 * sizeof(int16_t));
                }
            }

          }
        }
        done = true;
        break;
      }
      default:
        LogError << "unknown chunk in wdl: code=" << fourcc << std::endl;
        wdl_file.seekRelative(size);
        break;
    }
  } while (!done && !wdl_file.isEof());

  wdl_file.close();

  set_minimap(index);
}

void map_horizon::update_minimap_tile(int y, int x, bool has_data = false )
{
    if (_tiles[y][x])
    {
        //! \todo There also is a second heightmap appended which has additional 16*16 pixels.
        //! \todo There also is MAHO giving holes into this heightmap.

        for (int j(0); j < 16; ++j)
        {
            for (int i(0); i < 16; ++i)
            {
                //! \todo R and B are inverted here
                _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color_for_height(_tiles[y][x]->height_17[j][i]));
            }
        }
    }
    // the adt exist but there's no data in the wdl
    else if (has_data)
    {
        for (int j(0); j < 16; ++j)
        {
            for (int i(0); i < 16; ++i)
            {
                _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color(200, 100, 25));
            }
        }
    }
}

void map_horizon::set_minimap(const MapIndex* const index, bool set_empty)
{
    _qt_minimap = QImage(16 * 64, 16 * 64, QImage::Format_ARGB32);
    _qt_minimap.fill(Qt::transparent);

    if (set_empty)
        return;

    for (int y(0); y < 64; ++y)
    {
        for (int x(0); x < 64; ++x)
        {
            update_minimap_tile(y, x, index->hasTile(TileIndex(x, y)));
        }
    }
}

void map_horizon::remove_horizon_tile(int y, int x)
{
    _tiles[y][x].reset();

    for (int j(0); j < 16; ++j)
    {
        for (int i(0); i < 16; ++i)
        {
            _qt_minimap.setPixel(x * 16 + i, y * 16 + j, color(255, 25, 25));
        }
    }
}

Noggit::map_horizon_tile* map_horizon::get_horizon_tile(int y, int x)
{
    return _tiles[y][x].get();
}

int16_t map_horizon::getWdlheight(MapTile* tile, float x, float y)
{
    int cx = std::min(std::max(static_cast<int>(x / CHUNKSIZE), 0), 15);
    int cy = std::min(std::max(static_cast<int>(y / CHUNKSIZE), 0), 15);

    x -= cx * CHUNKSIZE;
    y -= cy * CHUNKSIZE;

    int row = static_cast<int>(y / (UNITSIZE * 0.5f) + 0.5f);
    int col = static_cast<int>((x - UNITSIZE * 0.5f * (row % 2)) / UNITSIZE + 0.5f);
    bool inner = (row % 2) == 1;

    if (row < 0 || col < 0 || row > 16 || col >(inner ? 8 : 9))
        return 0;

    // truncate and clamp the float value
    auto chunk = tile->getChunk(cx, cy);
    // float height = heights[cy * 16 + cx][17 * (row / 2) + (inner ? 9 : 0) + col];
    if (!chunk)
        return 0.0f;

    float height = chunk->getHeightmap()[17 * (row / 2) + (inner ? 9 : 0) + col].y;
    return std::min(std::max(static_cast<int16_t>(height), static_cast<int16_t>(SHRT_MIN)), static_cast<int16_t>(SHRT_MAX));
}

void map_horizon::update_horizon_tile(MapTile* mTile)
{
    auto tile_index = mTile->index;

    // calculate the heightmap as a short array
    float x, y;
    for (int i = 0; i < 17; i++)
    {
        for (int j = 0; j < 17; j++)
        {
            // outer - correct
            x = j * CHUNKSIZE;
            y = i * CHUNKSIZE;

            if (!_tiles[tile_index.z][tile_index.x].get()) // tile has not been initialised
                //     continue;
            {
                _tiles[tile_index.z][tile_index.x] = std::make_unique<map_horizon_tile>();
                // do we need to use memcpy as well ?
            }
            // only works for initialised
            _tiles[tile_index.z][tile_index.x].get()->height_17[i][j] = getWdlheight(mTile, x, y);

            // inner - close enough; correct values appear to use some form of averaging
            if (i < 16 && j < 16)
                _tiles[tile_index.z][tile_index.x].get()->height_16[i][j] = getWdlheight(mTile, x + CHUNKSIZE / 2.0f, y + CHUNKSIZE / 2.0f);
        }
    }
    // Holes
    for (int i = 0; i < 16; ++i)
    {
        std::bitset<16>wdlHoleMask(0);

        for (int j = 0; j < 16; ++j)
        {
            auto chunk = mTile->getChunk(j, i);
            if (!chunk)
                continue;
            // the ordering seems to be : short array = Y axis, flags values = X axis and the values are for a whole chunk.

            std::bitset<16> holeBits(chunk->getHoleMask());

            if (holeBits.count() == 16) // if all holes are set in a chunk
                wdlHoleMask.set(j, true);
        }
        _tiles[tile_index.z][tile_index.x].get()->holes[i] = static_cast<int16_t>(wdlHoleMask.to_ulong());
    }

    update_minimap_tile(tile_index.z, tile_index.x, true);
}

bool map_horizon::save_wdl(World* world, bool regenerate)
{
    world->wait_for_all_tile_updates();

    std::stringstream filename;
    filename << "World\\Maps\\" << world->basename << "\\" << world->basename << ".wdl";
    //Log << "Saving WDL \"" << filename << "\"." << std::endl;

    // ---- pass one: resolve the terrain, and the objects that go with it ------------------------
    //
    // The tile walk used to be inside the MAOF loop, resolving and writing one tile at a time.
    // It cannot stay there now. MWMO, MWID and MODF sit BEFORE MAOF in the file -- measured on
    // Northrend.wdl, where MWMO begins at 0x0C and MAOF at 0x1DF -- so what goes in them has to be
    // known before the first MAOF byte is written, and a WMO placement only becomes readable once
    // its tile is loaded. Splitting the walk out still costs exactly ONE load per tile: the heights
    // it resolves are stored in _tiles, which outlives the unload, so the write loop below reads
    // them back rather than loading anything again.
    bool has_horizon_data[64][64] = {};

    std::vector<wdl_wmo_placement> collected_objects;
    std::unordered_set<std::uint32_t> collected_uids;
    std::size_t objects_too_small (0);

    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            TileIndex index(x, y);

            if (!world->mapIndex.hasTile(index))
            {
                continue;
            }

            // May legitimately be null: the map had no WDL to read one from.
            Noggit::map_horizon_tile* horizon_tile = get_horizon_tile(y, x);

            // load tile and extract WDL data
            if (!horizon_tile || regenerate)
            {
                bool unload = !world->mapIndex.tileLoaded(index) && !world->mapIndex.tileAwaitingLoading(index);
                MapTile* mTile = world->mapIndex.loadTile(index, false, false, false);

                // THE crash. MapIndex::hasTile only reads bit 0 of the WDT's MAIN entry for
                // this tile (map_index.cpp:815-818); it does not check that the ADT behind it
                // exists. MapIndex::loadTile does check, and returns nullptr when the file is
                // missing (map_index.cpp:530-533). The original guarded only the
                // wait_until_loaded call with "if (mTile)" and then passed the same pointer
                // straight into update_horizon_tile, whose first statement is
                // "auto tile_index = mTile->index" (map_horizon.cpp:638). A WDT that lists a
                // tile whose ADT is not there is exactly the shape of the broken map somebody
                // reaches for "Generate new WDL" to repair, so the repair tool crashed on the
                // maps that needed it.
                if (mTile)
                {
                    mTile->wait_until_loaded();
                    update_horizon_tile(mTile);

                    // Before the unload, necessarily: unloading the tile can drop the last
                    // reference to its WMO instances, and everything below copies out of them by
                    // value precisely so that nothing survives this scope as a pointer.
                    if (regenerate)
                    {
                        collect_tile_horizon_wmos
                            (world, mTile, collected_objects, collected_uids, objects_too_small);
                    }

                    if (unload)
                        world->mapIndex.unloadTile(index);

                    horizon_tile = get_horizon_tile(y, x);
                }
                else
                {
                    LogError << "WDL generation: tile " << x << "_" << y << " is flagged in the"
                                " WDT but its ADT could not be loaded; writing it as empty."
                             << std::endl;
                }
            }

            has_horizon_data[y][x] = horizon_tile != nullptr;
        }
    }

    if (regenerate)
    {
        // Replaced wholesale, and only when regenerating. A plain save re-exports what was read,
        // which is the behaviour that stops a WDL round trip from deleting distant buildings the
        // editor cannot see; a regenerating one is the user asking for the table to be rebuilt
        // from the ADTs, and a merge of the two would leave placements behind for objects that
        // have since been deleted.
        _wdl_wmo_instances = std::move(collected_objects);

        Log << "WDL objects: " << _wdl_wmo_instances.size() << " WMO placement(s) written to MODF, "
            << objects_too_small << " skipped as too small for the horizon." << std::endl;
    }

    // ---- pass two: write the file --------------------------------------------------------------
    util::sExtendableArray wdlFile;
    int curPos = 0;

    // MVER
    //  {
    wdlFile.Extend(8 + 0x4);
    SetChunkHeader(wdlFile, curPos, 'MVER', 4);

    // MVER data
    *(wdlFile.GetPointer<int>(8)) = 18; // write version 18
    curPos += 8 + 0x4;
    //  }

    // MWMO, MWID and MODF, which were three empty chunks and a "TODO : MODF" before this.
    write_wdl_objects(wdlFile, curPos, _wdl_wmo_instances);

    //uint32_t mare_offsets[64][64] = { 0 };
    // MAOF
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MAOF', 64 * 64 * 4);
    curPos += 8;
    wdlFile.Extend(64 * 64 * 4);
    uint mareoffset = curPos + 64 * 64 * 4;

    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            // Resolved in pass one, which is what lets the failure cases be handled at all.
            // Writing the MAOF offset and the MARE header first commits the file to containing a
            // MARE for this tile, so a later "we have no data" has nowhere to go but out of the
            // function -- which is what the original did, abandoning the whole WDL, silently,
            // several hundred tiles in.
            Noggit::map_horizon_tile* const horizon_tile
                (has_horizon_data[y][x] ? get_horizon_tile(y, x) : nullptr);

            // write offset in MAOF entry
            *(wdlFile.GetPointer<uint>(curPos)) = horizon_tile ? mareoffset : 0;
            curPos += 4;

            if (horizon_tile)
            {
                // MARE Header
                //  {
                wdlFile.Extend(8);
                SetChunkHeader(wdlFile, mareoffset, 'MARE', (2 * (17 * 17)) + (2 * (16 * 16))); // outer heights+inner heights
                mareoffset += 8;

                wdlFile.Insert(mareoffset, sizeof(Noggit::map_horizon_tile::height_17), reinterpret_cast<char*>(&horizon_tile->height_17));
                mareoffset += sizeof(Noggit::map_horizon_tile::height_17);
                wdlFile.Insert(mareoffset, sizeof(Noggit::map_horizon_tile::height_16), reinterpret_cast<char*>(&horizon_tile->height_16));
                mareoffset += sizeof(Noggit::map_horizon_tile::height_16);

                // MAHO (maparea holes) MAHO was added in WOTLK ?
                //  {
                wdlFile.Extend(8);
                SetChunkHeader(wdlFile, mareoffset, 'MAHO', (2 * 16)); // 1 hole mask for each chunk
                mareoffset += 8;
                wdlFile.Extend(32);
                for (int i = 0; i < 16; ++i)
                {
                    wdlFile.Insert(mareoffset, 2, (char*)&horizon_tile->holes[i]);
                    mareoffset += 2;
                }
            }
            // A zero MAOF offset is how a WDL says "no terrain here" -- our own reader skips those
            // entries (map_horizon.cpp:501-504) and so does the client. Emitting one for a tile we
            // could not read is therefore a well-formed answer, and the file stays valid and
            // complete instead of not being written at all.
        }
    }
    // The same replacement made for the ADT and the WDT, and this was the last writer in the tree
    // still opening its destination with std::ofstream in out mode -- which truncates at open(),
    // before a single replacement byte exists.
    //
    // It became the sharpest of the three in this same round. save_wdl now re-exports MWMO/MWID/
    // MODF, so a failed write no longer costs a regenerable low-detail heightmap: it destroys
    // exactly the distant-building placements the change was written to preserve. A zero-byte WDL
    // is also read by the client rather than ignored.
    //
    // Bytes unchanged: wdlFile.all_data() is precisely what setBuffer received, and the writer
    // emits it with one ostream::write, as ClientFile::save did.
    BlizzardArchive::Listfile::FileKey const file_key (filename.str());

    std::filesystem::path const target
      ( Noggit::Application::NoggitApplication::instance()->clientData()->getDiskPath (file_key) );

    if (!Noggit::writeFileGuarded (target, wdlFile.all_data()))
    {
      // Deliberately still refreshes the minimap below. The in-memory horizon is correct and is
      // what the minimap draws from; refusing to update it would add a second, cosmetic symptom
      // to a failure the user has already been shown a dialog about.
      set_minimap(&world->mapIndex);
      return false;
    }

    set_minimap(&world->mapIndex);
    return true;
}

map_horizon::minimap::minimap(const map_horizon& horizon)
{
  std::vector<uint32_t> texture(1024 * 1024);

  for (size_t y (0); y < 64; ++y)
  {
    for (size_t x (0); x < 64; ++x)
    {
      if (!horizon._tiles[y][x])
        continue;

      //! \todo There also is a second heightmap appended which has additional 16*16 pixels.

      // use the (nearly) full resolution available to us.
      // the data is layed out as a triangle fans with with 17 outer values
      // and 16 midpoints per tile. which in turn means:
      //      _tiles[y][x]->height_17[16][16] == _tiles[y][x + 1]->height_17[0][0]
      for (size_t j (0); j < 16; ++j)
      {
        for (size_t i (0); i < 16; ++i)
        {
          texture[(y * 16 + j) * 1024 + x * 16 + i] = color_for_height (horizon._tiles[y][x]->height_17[j][i]);
        }
      }
    }
  }

  bind();
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_BGRA, GL_UNSIGNED_BYTE, texture.data());
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

map_horizon::render::render(const map_horizon& horizon)
{
  std::vector<glm::vec3> vertices;

  for (size_t y (0); y < 64; ++y)
  {
    for (size_t x (0); x < 64; ++x)
    {
      if (!horizon._tiles[y][x])
        continue;

      _batches[y][x] = map_horizon_batch (static_cast<std::uint32_t>(vertices.size()), 17 * 17 + 16 * 16);

      for (size_t j (0); j < 17; ++j)
      {
        for (size_t i (0); i < 17; ++i)
        {
          vertices.emplace_back ( TILESIZE * (x + i / 16.0f)
                                , horizon._tiles[y][x]->height_17[j][i]
                                , TILESIZE * (y + j / 16.0f)
                                );
        }
      }

      for (size_t j (0); j < 16; ++j)
      {
        for (size_t i (0); i < 16; ++i)
        {
          vertices.emplace_back ( TILESIZE * (x + (i + 0.5f) / 16.0f)
                                , horizon._tiles[y][x]->height_16[j][i]
                                , TILESIZE * (y + (j + 0.5f) / 16.0f)
                                );
        }
      }
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3> (_vertex_buffer, vertices, GL_STATIC_DRAW);
}

static inline uint32_t outer_index(const map_horizon_batch &batch, int y, int x)
{
  return batch.vertex_start + y * 17 + x;
};

static inline uint32_t inner_index(const map_horizon_batch &batch, int y, int x)
{
  return batch.vertex_start + 17 * 17 + y * 16 + x;
};

void map_horizon::render::draw( glm::mat4x4 const& model_view
                              , glm::mat4x4 const& projection
                              , MapIndex *index
                              , const glm::vec3& color
                              , const float& cull_distance
                              , const math::frustum& frustum
                              , const glm::vec3& camera 
                              , display_mode display
                              )
{
  std::vector<uint32_t> indices;

  const TileIndex current_index(camera);
  const int lrr = 2;

  for (size_t y (current_index.z - lrr); y <= current_index.z + lrr; ++y)
  {
    for (size_t x (current_index.x - lrr); x < current_index.x + lrr; ++x)
    {
      // x and y are unsigned so negative signed int value are positive and > 63
      if (x > 63 || y > 63)
      {
        continue;
      }

      map_horizon_batch const& batch = _batches[y][x];

      if (batch.vertex_count == 0)
        continue;

      for (int j (0); j < 16; ++j)
      {
        for (int i (0); i < 16; ++i)
        {
          // do not draw over visible chunks

          /* TODO: when this optimization is turned off, we end up with inconsistent rendering between chunks and horizon batches.
           * Potentially it is caused by inconsistent coordinate space in visibility checking or chunk update system.
          if (index->tileLoaded({y, x}) && index->getTile({y, x})->getChunk(j, i)->is_visible(cull_distance, frustum, camera, display))
          {
            //continue;
          }
          */

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i));
          indices.push_back (outer_index (batch, j + 1, i + 1));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i + 1));
          indices.push_back (outer_index (batch, j, i + 1));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j, i + 1));
          indices.push_back (outer_index (batch, j, i));
        }
      }
    }
  }

  if (_map_horizon_program)
  {
    gl.bufferSubData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(_index_buffer, 0, indices);
  }
  else
  {
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(_index_buffer, indices, GL_DYNAMIC_DRAW);

    _map_horizon_program.reset
      ( new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("horizon_vs") }
          , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("horizon_fs") }
          }
      );
  
    _vaos.upload();
  }
   

  OpenGL::Scoped::use_program shader {*_map_horizon_program.get()};

  OpenGL::Scoped::vao_binder const _ (_vao);

  shader.uniform ("model_view", model_view);
  shader.uniform ("projection", projection);
  shader.uniform ("color", glm::vec3(color.x, color.y, color.z));

  shader.attrib ("position", _vertex_buffer, 3, GL_FLOAT, GL_FALSE, 0, 0);

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder (_index_buffer);

  
  gl.drawElements (GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
}

}
