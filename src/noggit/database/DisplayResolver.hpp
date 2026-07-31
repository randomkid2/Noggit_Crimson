// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_DISPLAYRESOLVER_HPP
#define NOGGIT_DATABASE_DISPLAYRESOLVER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Noggit::Database
{
  // What a display id resolved to, or why it did not.
  struct ResolvedModel
  {
    bool resolved = false;

    // Backslash-separated, .m2, already through ModelPathFixup. Empty when !resolved.
    std::string model_path;

    // Product of every scale factor in the chain. 1.0 when unresolved.
    //
    // Never 0. A zero scale collapses the model to a point, which on screen is indistinguishable
    // from "failed to load" -- and would send whoever investigates hunting the model loader
    // rather than the DBC row. SpawnPlacement::placementFor guards this a second time; agreeing
    // with it here means the two cannot disagree about what an unresolved scale is.
    double scale = 1.0;

    // Non-empty exactly when !resolved, and empty exactly when resolved. Enforced by
    // construction in DisplayResolver.cpp -- a caller may rely on either direction.
    std::string failure_reason;

    // Replaceable-texture skins for this display id: the M2 texture `type` each one fills, and
    // the .blp to fill it with. Empty for gameobjects, which have no such textures, and empty for
    // a creature whose CreatureDisplayInfo row names no variation.
    //
    // This is the missing half of drawing a creature. A creature M2 does not contain its own skin:
    // the mesh is shared and the image is supplied per display id, which is how one wolf model
    // serves a dozen differently coloured wolves. Noggit's loader replaces every such texture with
    // `tileset/generic/black.blp` (Model.cpp:353), so without these a creature is a black
    // silhouette -- correctly placed and scaled, but unrecognisable.
    //
    // Types are 11, 12 and 13 in order, matching TextureVariation[0..2]. The .blp sits in the same
    // directory as the model, which is the client's own convention rather than anything stored.
    std::vector<std::pair<int, std::string>> skins;
  };

  // display_id -> model path and scale, with the DBC chain walked once per id and then cached.
  //
  // The caching is not an optimisation, it is the reason this class exists. DBCFile::getByID is a
  // LINEAR SCAN over every record in the file (DBCFile.cpp:144-153) -- CreatureModelData.dbc has
  // thousands of them -- and the creature chain needs two such scans. Resolving per spawn per
  // frame would put an O(spawns * records) scan inside the draw path. Misses are cached too: a
  // miss costs a full scan of the file plus an exception throw, so a repeated miss is strictly
  // MORE expensive than a repeated hit, and unresolvable display ids are common in real world
  // data (creature.modelid is 0 for most rows).
  //
  // Nothing here throws. That is deliberate and it is the second reason this class exists:
  // getByID throws DBCFile::NotFound on a miss, ClientFile's constructor throws when the DBC is
  // absent from the archive chain, and DBCFile::Record::getString does raw pointer arithmetic
  // into the string table guarded only by an assert() that RelWithDebInfo -- the configuration
  // this project ships -- compiles out. All of that is contained here and reported as
  // resolved == false with a reason, because an exception escaping into WorldRender::draw takes
  // the frame, and then the editor, with it.
  //
  // NOT thread-safe, and deliberately not synchronised: DBCFile itself is not either (getByID
  // hands out a Record holding a raw pointer into a std::vector that addRecord can reallocate),
  // so a mutex here would buy nothing while implying a safety that does not exist. Call it from
  // the render thread only.
  class DisplayResolver
  {
    public:
      // display_id is creature.modelid when non-zero, otherwise the chosen
      // creature_template.modelidN. 0 resolves to !resolved with a plain reason, not an error --
      // it is the normal encoding of "no model override" and the caller sees it constantly.
      ResolvedModel creatureModel(std::uint32_t display_id);

      // display_id is gameobject_template.displayId. 0 is handled as above.
      ResolvedModel gameObjectModel(std::uint32_t display_id);

      // Drop every cached result. Needed after the DBCs are reopened for a different client
      // install or after a DBC edit, either of which invalidates every path in here.
      void clearCache();

      // Total cached entries across both chains, hits and misses alike.
      std::size_t cacheSize() const;

    private:
      std::unordered_map<std::uint32_t, ResolvedModel> _creature_cache;
      std::unordered_map<std::uint32_t, ResolvedModel> _gameobject_cache;
  };
}

#endif
