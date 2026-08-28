// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Alphamap.hpp>
#include <noggit/ContextObject.hpp>
#include <noggit/TextureManager.h>
#include <noggit/texturing/TextureLayerPolicy.hpp>

#include <cstdint>
#include <array>
#include <string>
#include <vector>

class Brush;
class MapTile;
class MapChunk;
struct MapChunkHeader;

namespace BlizzardArchive
{
  class ClientFile;
}

struct tmp_edit_alpha_values
{
  using alpha_layer = std::array<float, 64 * 64>;
  // use 4 "alphamaps" for an easier editing
  std::array<alpha_layer, 4> map;

  alpha_layer& operator[](std::size_t i)
  {
    return map.at(i);
  }
};

struct layer_info
{
    // uint32_t  textureID = 0;
    uint32_t  flags = 0;
    // uint32_t  ofsAlpha = 0;
    uint32_t  effectID = 0xFFFFFFFF; // default value, see https://wowdev.wiki/ADT/v18#MCLY_sub-chunk
};

class TextureSet
{
public:
  TextureSet() = delete;
  TextureSet(MapChunk* chunk, BlizzardArchive::ClientFile* f, size_t base
             , bool use_big_alphamaps, bool do_not_fix_alpha_map, bool do_not_convert_alphamaps
             , Noggit::NoggitRenderContext context, MapChunkHeader const& header);

  int addTexture(scoped_blp_texture_reference texture);
  void eraseTexture(size_t id);
  void eraseTextures();
  // return true if at least 1 texture has been erased
  bool eraseUnusedTextures();
  void swap_layers(int layer_1, int layer_2);
  bool replace_texture(scoped_blp_texture_reference const& texture_to_replace, scoped_blp_texture_reference replacement_texture);
  bool paintTexture(float xbase, float zbase, float x, float z, Brush* brush, float strength, float pressure, scoped_blp_texture_reference texture);
  bool stampTexture(float xbase, float zbase, float x, float z, Brush* brush, float strength, float pressure, scoped_blp_texture_reference texture, QImage* image, bool paint);
  bool replace_texture( float xbase
                      , float zbase
                      , float x
                      , float z
                      , float radius
                      , scoped_blp_texture_reference const& texture_to_replace
                      , scoped_blp_texture_reference replacement_texture
                      , bool entire_chunk = false
                      );
  //! Answers the question the brush asks: can this texture be painted here right now, under the
  //! layer budget policy the user has set? Under the default Skip policy this is exactly what it
  //! has always been -- "already present, or is there a free slot".
  //!
  //! The paintability OVERLAY must not go through here. See MapChunk::canPaintTexture.
  bool canPaintTexture(scoped_blp_texture_reference const& texture);

  //! The same question against an explicit policy rather than the process-wide one. A
  //! default-constructed TextureLayerAdmission is the strict, pre-Smart-Paint answer.
  bool canAdmitTexture(scoped_blp_texture_reference const& texture
                      , Noggit::TextureLayerAdmission const& admission);

  const std::string& filename(size_t id);

  size_t const& num() const;
  unsigned int flag(size_t id);
  unsigned int effect(size_t id);
  bool is_animated(std::size_t id) const;
  void change_texture_flag(scoped_blp_texture_reference const& tex, std::size_t flag, bool add);

  std::vector<std::vector<uint8_t>> save_alpha(bool big_alphamap);

  void convertToBigAlpha();
  void convertToOldAlpha();

  void merge_layers(size_t id1, size_t id2);
  bool removeDuplicate();

  scoped_blp_texture_reference texture(size_t id);

  int texture_id(scoped_blp_texture_reference const& texture);

  void uploadAlphamapData();

  bool apply_alpha_changes();

  void create_temporary_alphamaps_if_needed();

  void markDirty();

  void setEffect(size_t id, int value);

  std::array<std::uint16_t, 8> lod_texture_map();

  std::array<std::unique_ptr<Alphamap>, MAX_ALPHAMAPS>* getAlphamaps();;
  std::unique_ptr<tmp_edit_alpha_values>& getTempAlphamaps();;

  void setAlphamaps(const std::array<std::unique_ptr<Alphamap>, MAX_ALPHAMAPS>& newAlphamaps);

  int get_texture_index_or_add (scoped_blp_texture_reference texture, float target);

  //! The same, against an explicit layer budget policy. The two-argument form above is this one
  //! with TextureLayerAdmission::current(), which is how a brush stroke reaches Smart Paint
  //! without World::paintTexture or MapChunk::paintTexture growing a parameter.
  int get_texture_index_or_add ( scoped_blp_texture_reference texture
                               , float target
                               , Noggit::TextureLayerAdmission const& admission
                               );

  //! Every layer's total and peak alpha over the chunk, in one 4096-texel pass.
  Noggit::LayerAlphaProfile layerAlphaProfile() const;

  //! The layer that contributes the least to what this chunk actually looks like, or -1 when the
  //! chunk holds no textures. See the implementation for how "least" is defined and why ties break
  //! towards the higher slot.
  int getLeastVisibleLayer() const;

  //! LAYER REPLACEMENT: put `texture` into slot `layer`, keeping or clearing that slot's alpha.
  //! Returns false when the chunk has no such slot -- this never invents intermediate layers.
  bool replaceLayerTexture ( std::size_t layer
                           , scoped_blp_texture_reference texture
                           , Noggit::LayerAlphaHandling alpha_handling
                           );

  //! TEXTURE DUPLICATES: fold every layer that references a texture an earlier layer already
  //! holds into that earlier layer. Returns the number of layers removed.
  int purgeDuplicateLayers();

  //! TEXTURES BELOW THRESHOLD: remove every layer whose peak alpha anywhere on the chunk is at
  //! most `threshold`. Returns the number of layers removed. Never empties a chunk.
  int purgeLayersBelowThreshold(std::uint8_t threshold);

  //! Drop layers 1..n-1 and leave the chunk showing nothing but its base texture. Returns the
  //! number of layers removed.
  int clearOverlayLayers();

  //! Restores the invariant six raw dereference sites in this class assume: every layer from 1 to
  //! nTextures-1 has an alpha plane. Create-only -- see the implementation for why a surplus plane
  //! is left alone. Returns true if it had to create one.
  bool ensureAlphamapConsistency();

  //! The layer this policy is willing to sacrifice on a full chunk, or -1 when it is willing to
  //! sacrifice none. Does not modify anything.
  int pickEvictableLayer(Noggit::TextureLayerAdmission const& admission) const;

  auto getDoodadMappingBase(void) -> std::uint16_t*;
  std::array<std::uint16_t, 8> const& getDoodadMapping();
  std::array<std::array<std::uint8_t, 8>, 8> const getDoodadMappingReadable(); // get array of readable values
  uint8_t const getDoodadActiveLayerIdAt(unsigned int unit_x, unsigned int unit_y);

  std::array<std::uint8_t, 8> _doodadStencil; // doodads disabled if 1; WoD: may be an explicit MCDD chunk
                                                // this is actually uint1_t[8][8] (8*8 -> 1 bit each)
  auto getDoodadStencilBase(void) -> std::uint8_t*;
  bool const getDoodadDisabledAt(int x, int y); // max is 8
  void setDetailDoodadsExclusion(float xbase, float zbase, glm::vec3 const& pos, float radius, bool big, bool add);

  auto getEffectForLayer(std::size_t idx) const -> unsigned;
  layer_info* getMCLYEntries();;
  void setNTextures(size_t n);;
  std::vector<scoped_blp_texture_reference>* getTextures();;

  // get the weight of each texture in a chunk unit
  std::array<float, 4> get_textures_weight_for_unit(unsigned int unit_x, unsigned int unit_y);

  void updateDoodadMapping();

private:

  uint8_t sum_alpha(size_t offset) const;

  void alphas_to_big_alpha(uint8_t* dest);
  void alphas_to_old_alpha(uint8_t* dest);

  void update_lod_texture_map(); // todo: remove. WHAT?

  MapChunk* _chunk;

  std::vector<scoped_blp_texture_reference> textures;
  std::array<std::unique_ptr<Alphamap>, MAX_ALPHAMAPS> alphamaps;

  // Mists Heightmapping
  std::vector<scoped_blp_texture_reference> heightTextures;
  std::array<texture_heightmapping_data, 4> heightMappingData;

  size_t nTextures;

  // byte[8][8] // can store the 2bits value in a byte, but might never be higher than 3 or layer count.
  std::array<std::uint16_t, 8> _doodadMapping; // "predTex", It is used to determine which detail doodads to show.Values are an array of two bit unsigned integers, naming the layer.
                                                // this is actually uint2_t[8][8] (8*8 -> 2 bit each)
                                                // getting the layer id from the two bits :  MCLY textureLayer entry ID (can be only one of: 00 | 01 | 10 | 11)
  // bool[8][8]

  bool _need_lod_texture_map_update = false;

  layer_info _layers_info[4];

  std::unique_ptr<tmp_edit_alpha_values> tmp_edit_values;

  bool _do_not_convert_alphamaps;

  static constexpr std::array<std::uint8_t, 256 * 256> make_alpha_lookup_array();

  static std::array<std::uint8_t, 256 * 256> alpha_convertion_lookup;

  Noggit::NoggitRenderContext _context;
};
