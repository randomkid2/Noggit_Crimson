// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_3DOBJECT_HPP
#define NOGGIT_3DOBJECT_HPP

#include <glm/mat4x4.hpp>
#include <noggit/Selection.h>
#include <noggit/ContextObject.hpp>
#include <array>

namespace BlizzardArchive::Listfile
{
  class FileKey;
}

class AsyncObject;

enum SceneObjectTypes
{
  eMODEL,
  eWMO
};

class MapTile;

class SceneObject : public Selectable
{
public:
  SceneObject(SceneObjectTypes type, Noggit::NoggitRenderContext context);

  [[nodiscard]]
  bool isInsideRect(std::array<glm::vec3, 2> const* rect) const;

  [[nodiscard]]
  bool isDuplicateOf(SceneObject const& other);

  virtual void updateTransformMatrix();

  virtual void recalcExtents() = 0;
  virtual void ensureExtents() = 0;

  [[nodiscard]]
  virtual bool finishedLoading() = 0;

  void resetDirection();

  void normalizeDirection();

  [[nodiscard]]
  glm::mat4x4 transformMatrix() const;; // can't recalc extent directly there because recalc extents functions call this and it causes an infinite loop

  [[nodiscard]]
  glm::mat4x4 transformMatrixInverted() const;;

  [[nodiscard]]
  SceneObjectTypes which() const;;

  void refTile(MapTile* tile);
  void derefTile(MapTile* tile);

  [[nodiscard]]
  std::vector<MapTile*> const& getTiles() const;;

  [[nodiscard]]
  virtual AsyncObject* instance_model() const = 0;

  [[nodiscard]]
  virtual std::array<glm::vec3, 2> const& getExtents(); // axis aligned

  [[nodiscard]]
  virtual std::array<glm::vec3, 8> getBoundingBox() = 0; // non axis aligned

  [[nodiscard]]
  float getBoundingRadius();

  glm::vec3 const getServerPos() const;

  // Whether anything has moved, rotated or rescaled this instance since it was read off disk.
  //
  // An object whose bounding box crosses a tile border is written into the MDDF/MODF of EVERY
  // tile it touches, under one uid and one transform. Once the user moves it, the rows in the
  // tiles that are not loaded yet still hold the OLD transform, and when such a tile loads,
  // world_model_instances_storage sees a uid it already knows carrying a transform that
  // disagrees -- which is indistinguishable, on the transform alone, from a genuine uid
  // collision between two different objects. Renumbering it, which is the right answer for a
  // real collision, resurrects the object at the position the user moved it away from and then
  // saves it there. This flag is the missing piece of evidence: if THIS instance is the one that
  // moved, the disagreement is explained and the stale row is its own, not another object's.
  //
  // Never cleared, deliberately. It has to stay true across a save, because saving rewrites only
  // the tiles Noggit has loaded -- the stale rows in the rest of the map outlive the save and
  // keep arriving for as long as the session lasts.
  //
  // A plain bool rather than std::atomic<bool>: instances are stored BY VALUE in the storage's
  // maps (world_model_instances_storage.hpp:17-18), so an atomic member would delete the
  // implicit copy constructor those maps need. The loader thread that reads it already reads
  // pos, dir and scale unsynchronised through isDuplicateOf on the same code path, so this adds
  // no race that was not there before, and the value only ever goes false -> true.
  [[nodiscard]]
  bool wasTransformedThisSession() const { return _transformed_this_session; }

  // Sets the flag and, on the FIRST call only, snapshots the transform the instance is being
  // moved away from. Called out of Action::registerObjectTransformed before the mutation, so on
  // that first call pos/dir/scale still hold what the MDDF/MODF row this instance was built from
  // said -- which is precisely what every not-yet-loaded tile that also lists this object still
  // holds on disk. Later calls must not overwrite it: after the second move the current transform
  // is no longer what disk says, and the whole point of the snapshot is to be disk's version.
  void markTransformedThisSession();

  // Whether the incoming instance carries the exact transform this one was loaded at, i.e. is
  // this object's own leftover row rather than a different object that happens to share its uid.
  // Uses the same three epsilon comparisons as isDuplicateOf (SceneObject.cpp:50-52) because the
  // two rows are parsed from separate copies of the same bytes and must compare equal the same
  // way. Meaningless unless wasTransformedThisSession() is true; callers test that first.
  [[nodiscard]]
  bool matchesTransformAtLoad(SceneObject const& other) const;

  bool _grouped = false;

public:
  glm::vec3 pos;

  glm::vec3 dir;
  float scale = 1.f;
  unsigned int uid;
  int frame;

  // Note : First, need to check if the tile that contained it was rendered too
  bool _rendered_last_frame = false;

protected:
  SceneObjectTypes _type;

  glm::mat4x4 _transform_mat = glm::mat4x4();
  glm::mat4x4 _transform_mat_inverted = glm::mat4x4();
  std::array<glm::vec3, 2> extents; // axis aligned bounding box mni and max corners
  float bounding_radius;

  Noggit::NoggitRenderContext _context;

  std::vector<MapTile*> _tiles;

  bool _transformed_this_session = false;

  // The transform this instance was read off disk at, captured by the first
  // markTransformedThisSession() call. Only meaningful while _transformed_this_session is true;
  // the initialisers exist so a never-transformed instance cannot compare equal to anything real
  // by reading uninitialised memory. Scale defaults to 1.f to match the member it shadows.
  glm::vec3 _transform_at_load_pos = glm::vec3(0.f);
  glm::vec3 _transform_at_load_dir = glm::vec3(0.f);
  float _transform_at_load_scale = 1.f;
};

#endif //NOGGIT_3DOBJECT_HPP
