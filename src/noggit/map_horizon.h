// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/frustum.hpp>

#include <noggit/MapHeaders.h>
#include <noggit/tool_enums.hpp>

#include <opengl/texture.hpp>
#include <opengl/scoped.hpp>
#include <opengl/shader.fwd.hpp>

#include <QtGui/QImage>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MapIndex;
class MapTile;
class MapView;
class World;

namespace Noggit
{

struct map_horizon_tile
{
    int16_t height_17[17][17];
    int16_t height_16[16][16];
    int16_t holes[16];
};

//! One WMO placement destined for the WDL's MODF chunk -- the distant buildings the client draws
//! in the horizon band, past the range at which it streams ADTs in.
//!
//! MEASURED AGAINST BLIZZARD DATA, not taken from a wiki. Of the 106 stock 3.3.5a WDLs readable
//! from the client MPQ chain, exactly two carry a non-empty MODF: Northrend.wdl (4 entries, 3
//! names) and IsleOfConquest.wdl (11 entries, 9 names). All 15 were compared field by field
//! against the MODF record carrying the same uniqueID in the ADT underneath, and all 15 are that
//! ADT record with bytes 4..63 -- uniqueID, position, rotation, extents, flags, doodadSet,
//! nameSet and the trailing uint16 -- byte-for-byte IDENTICAL, and only `nameID` repointed at a
//! second MWMO entry naming "<the same path>_LOW.wmo". A horizon placement is therefore not a new
//! placement: it is the tile's own placement wearing a lower-detail model.
//!
//! The filename is carried as a string rather than as the nameID it was read with, because the
//! MWMO table is rebuilt from scratch on every save. A stored index would name a different model
//! the moment one entry was added or dropped.
struct wdl_wmo_placement
{
    //! Client-data path, in whatever case it was read in. misc::normalize_adt_filename puts it
    //! into the upper-case backslash form MWMO stores, at write time and nowhere else.
    std::string filename;

    //! `nameID` is ignored here and reassigned when MWMO is written; every other field is the
    //! record as it will reach the file.
    ENTRY_MODF placement;
};

struct map_horizon_batch
{
  map_horizon_batch ()
    : vertex_start (0)
    , vertex_count (0)
  {}

  map_horizon_batch (uint32_t _vertex_start, uint32_t _vertex_count)
    : vertex_start(_vertex_start)
    , vertex_count(_vertex_count)
  {}

  uint32_t vertex_start;
  uint32_t vertex_count;
};

class map_horizon
{
public:
  struct render
  {
    render(const map_horizon& horizon);

    void draw(glm::mat4x4 const& model_view
             , glm::mat4x4 const& projection
             , MapIndex *index
             , const glm::vec3& color
             , const float& cull_distance
             , const math::frustum& frustum
             , const glm::vec3& camera 
             , display_mode display
             );

    map_horizon_batch _batches[64][64];

    OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vaos;
    GLuint const& _vao = _vaos[0];
    OpenGL::Scoped::buffers<2> _buffers;
    GLuint const& _index_buffer = _buffers[0];
    GLuint const& _vertex_buffer = _buffers[1];
    std::unique_ptr<OpenGL::program> _map_horizon_program;
  };

  class minimap : public OpenGL::texture
  {
  public:
    minimap(const map_horizon& horizon);
  };

  map_horizon(const std::string& basename, const MapIndex * const index);

  void update_minimap_tile(int y, int x, bool has_data);

  void set_minimap(const MapIndex* const index, bool set_empty = false);

  void remove_horizon_tile(int y, int x);

  Noggit::map_horizon_tile* get_horizon_tile(int y, int x);

  QImage _qt_minimap;

  void update_horizon_tile(MapTile* mTile);

  //! Returns false when the WDL could not be written, having already told the user.
  //!
  //! The caller must not report a successful save without consulting this. A WDL now carries the
  //! MWMO/MWID/MODF name and placement blocks for distant buildings, so losing one is not merely
  //! a cosmetic low-detail horizon any more.
  bool save_wdl(World* world, bool regenerate = false);

private:
  int16_t getWdlheight(MapTile* tile, float x, float y);

  std::string _filename;

  //! The MWMO table exactly as it was read, so a MODF nameID can be resolved to a path. Not the
  //! table that gets written back: that one is rebuilt from _wdl_wmo_instances.
  std::vector<std::string> mWMOFilenames;

  //! The WDL's own MODF, as read and as it will be written back.
  //!
  //! Before this existed, save_wdl emitted a zero-size MODF unconditionally, so opening a map
  //! that has distant buildings and saving its WDL deleted them -- Northrend loses four, Isle of
  //! Conquest eleven. A non-regenerating save now re-exports this untouched; a regenerating one
  //! rebuilds it from the ADTs.
  std::vector<wdl_wmo_placement> _wdl_wmo_instances;

  std::unique_ptr<map_horizon_tile> _tiles[64][64];
};

}
