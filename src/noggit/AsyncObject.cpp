// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncObject.h>
#include <noggit/Log.h>
#include <noggit/MissingPlacementLog.hpp>

 AsyncObject::AsyncObject(BlizzardArchive::Listfile::FileKey file_key) : _file_key(std::move(file_key)) {}

[[nodiscard]]
 BlizzardArchive::Listfile::FileKey const& AsyncObject::file_key() const
{
  return _file_key;
}

[[nodiscard]]
 bool AsyncObject::finishedLoading() const
{
  return finished.load();
}

[[nodiscard]]
 bool AsyncObject::loading_failed() const
{
  return _loading_failed;
}

 void AsyncObject::wait_until_loaded()
{
  if (finished.load())
  {
    return;
  }

  std::unique_lock<std::mutex> lock(_mutex);

  _state_changed.wait
  (lock
    , [&]
    {
      return finished.load();
    }
  );
}

 void AsyncObject::error_on_loading()
{
  LogError << "File " << (_file_key.hasFilepath() ? _file_key.filepath() : std::to_string(_file_key.fileDataID()))
    << " could not be loaded" << std::endl;

  // THE COLLECTION POINT. This is the one place in the program where "this asset does not load"
  // is decided, for every asset type, so it is the only place a register of failures can be
  // complete. Until this line the failure produced the LogError above and nothing else: no
  // caller, no observer, no accumulation, and the mapper's only route to it was to close Noggit
  // and open log.txt.
  //
  // THIS RUNS ON AN AsyncLoader WORKER THREAD (AsyncLoader::process, which calls this from its
  // two catch blocks). MissingPlacementLog is STL-only and takes its own mutex; it must never
  // grow anything that touches Qt, glm or the scene graph, because none of those may be reached
  // from here. The placements that reference this file, and the client-data probe that splits
  // Missing from Unreadable, are resolved later on the GUI thread -- see MissingObjectsPanel.
  //
  // Only models are registered. recordFileFailure ignores everything else by extension: a failed
  // .blp already falls back to textures/shanecube.blp at TextureManager.cpp:520, and a failed
  // .adt is a tile-level problem with its own path, so listing either would bury the rows that
  // can actually be acted on.
  if (_file_key.hasFilepath())
  {
    Noggit::MissingPlacementLog::instance().recordFileFailure(_file_key.filepath());
  }

  _loading_failed = true;
  finished = true;
  _state_changed.notify_all();
}

[[nodiscard]]
 bool AsyncObject::is_required_when_saving() const
{
  return false;
}

[[nodiscard]]
 async_priority AsyncObject::loading_priority() const
{
  return async_priority::medium;
}
