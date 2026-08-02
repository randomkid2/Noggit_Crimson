// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/PatchAssetPacker.hpp>

#include <noggit/AssetDependencies.hpp>
#include <noggit/Log.h>

#include <BaseArchive.hpp>
#include <ClientData.hpp>
#include <ClientFile.hpp>
#include <Exception.hpp>
#include <Listfile.hpp>
#include <MPQArchive.hpp>

#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
  // Files whose contents name other files. Everything else is a leaf and only has to be copied.
  //
  // .wdt is here with .adt because the two agree on the blocks this reads: a WDT for a global-WMO
  // map carries MWMO/MODF with the same NUL separated string block shape, so one parser covers
  // both and a map whose only object is its global WMO is not silently skipped.
  bool isTraversable(std::string const& path)
  {
    auto const has_extension
      ( [&path] (char const* extension)
        {
          std::size_t const length = std::strlen(extension);
          return path.size() > length && path.compare(path.size() - length, length, extension) == 0;
        }
      );

    return has_extension(".adt") || has_extension(".wdt")
        || has_extension(".m2") || has_extension(".wmo");
  }

  bool isAdtLike(std::string const& path)
  {
    return path.size() > 4
        && (path.compare(path.size() - 4, 4, ".adt") == 0 || path.compare(path.size() - 4, 4, ".wdt") == 0);
  }

  bool isWorldModel(std::string const& path)
  {
    return path.size() > 4 && path.compare(path.size() - 4, 4, ".wmo") == 0;
  }

  bool isModel(std::string const& path)
  {
    return path.size() > 3 && path.compare(path.size() - 3, 3, ".m2") == 0;
  }
}

std::string_view Noggit::assetOriginName(AssetOrigin origin)
{
  switch (origin)
  {
    case AssetOrigin::NotFound:          return "not found";
    case AssetOrigin::ProjectFolder:     return "project folder";
    case AssetOrigin::TargetArchive:     return "already in this patch";
    case AssetOrigin::BaseClientArchive: return "base client";
    case AssetOrigin::CustomArchive:     return "other patch";
  }

  return "not found";
}

std::string Noggit::PatchAssetPackerResult::summary() const
{
  std::ostringstream line;

  line << "Walked " << roots << " project file(s) and " << visited << " reference(s): "
       << added << " added";

  if (bytes_added)
  {
    line << " (" << (bytes_added / 1024u) << " KiB)";
  }

  line << ", " << skipped_base_client << " already in the base client"
       << ", " << skipped_in_project << " already in the project"
       << ", " << skipped_already_in_patch << " already in this patch"
       << ", " << optional_absent << " optional files absent (anims, LOD skins, specular maps)"
       << ", " << scan.distinctMissing() << " unresolved";

  if (scan.distinctUnreadable())
  {
    // Kept apart from unresolved because the fix differs: a missing file has to be shipped, an
    // unreadable one has to be replaced. Both render as nothing in game, which is exactly why
    // collapsing them wastes the reader's time.
    line << ", " << scan.distinctUnreadable() << " unreadable";
  }

  if (write_failures)
  {
    line << ", " << write_failures << " failed to write";
  }

  if (cancelled)
  {
    line << " (CANCELLED)";
  }

  return line.str();
}

std::string Noggit::PatchAssetPackerResult::toReport() const
{
  std::ostringstream report;

  report << summary() << "\n";

  if (scan.hasFailures())
  {
    // AssetScanResult::failures() returns Missing AND Unreadable together, and the two need
    // different sentences: ship the file versus replace the corrupt one. Printing them under one
    // heading that says "could not be resolved" mislabels every zero byte or truncated file as an
    // absent one and sends the reader looking for something that is right there.
    auto const heading
      ( [&report] (AssetStatus status)
        {
          if (status == AssetStatus::Missing)
          {
            report << "\nReferences that could not be resolved anywhere in the client data chain.\n"
                      "These will be missing in game; nothing was packed for them.\n\n";
          }
          else
          {
            report << "\nReferences that resolved but could not be read: empty, truncated, or a\n"
                      "format the chain rejected. The file is there and its contents are not.\n\n";
          }
        }
      );

    for (AssetStatus const status : { AssetStatus::Missing, AssetStatus::Unreadable })
    {
      bool printed_heading = false;

      // Ordered by kind, then by descending reference count, then by key, so two runs over an
      // unchanged project produce byte-identical text and a diff means something really changed.
      for (auto const& failure : scan.failures())
      {
        if (failure.status != status)
        {
          continue;
        }

        if (!printed_heading)
        {
          heading(status);
          printed_heading = true;
        }

        report << "  " << assetKindName(failure.kind) << "  " << failure.display_path
               << "  (" << failure.reference_count << " reference"
               << (failure.reference_count == 1 ? "" : "s") << ")\n";

        for (auto const& referrer : failure.referrers)
        {
          report << "      referenced by " << referrer << "\n";
        }
      }
    }
  }

  if (!write_errors.empty())
  {
    report << "\nFiles that resolved but could not be written to the archive.\n\n";

    for (auto const& error : write_errors)
    {
      report << "  " << error << "\n";
    }
  }

  return report.str();
}

Noggit::PatchAssetPacker::PatchAssetPacker( BlizzardArchive::ClientData* client_data
                                          , BlizzardArchive::Archive::MPQArchive* target_archive
                                          , PatchAssetPackerOptions options
                                          )
  : _client_data (client_data)
  , _target_archive (target_archive)
  , _options (options)
{
}

Noggit::PatchAssetPacker::~PatchAssetPacker()
{
  removeStagingDirectory();
}

void Noggit::PatchAssetPacker::removeStagingDirectory()
{
  if (!_staging_created)
  {
    return;
  }

  std::error_code error;
  fs::remove_all(_staging_directory, error);

  if (error)
  {
    LogError << "PatchAssetPacker: could not remove staging directory "
             << _staging_directory.string() << ": " << error.message() << std::endl;
  }

  _staging_created = false;
}

std::filesystem::path Noggit::PatchAssetPacker::nextStagingPath()
{
  if (!_staging_created)
  {
    // Numbered files in one flat directory rather than a mirror of the asset tree. The staged name
    // never reaches the archive -- addFile takes the stored name separately -- so nothing is gained
    // by recreating the paths, and a flat directory cannot hit MAX_PATH or fail to create an
    // intermediate directory halfway through a pack.
    auto const stamp
      (std::chrono::steady_clock::now().time_since_epoch().count());

    _staging_directory
      = fs::temp_directory_path() / ("noggit-patch-deps-" + std::to_string(static_cast<std::uint64_t>(stamp)));

    fs::create_directories(_staging_directory);
    _staging_created = true;
  }

  return _staging_directory / (std::to_string(_staging_counter++) + ".bin");
}

Noggit::AssetOrigin Noggit::PatchAssetPacker::originOf(std::string const& path) const
{
  BlizzardArchive::Listfile::FileKey const key (path);

  try
  {
    // Disk first, exactly as ClientFile does. existsOnDisk goes through the throwing overload of
    // std::filesystem::exists (ClientData.cpp:582), which raises for anything other than "not
    // found" -- a disconnected network share is enough.
    if (_client_data->existsOnDisk(key))
    {
      return AssetOrigin::ProjectFolder;
    }
  }
  catch (...)
  {
  }

  auto const* archives = _client_data->loadedArchives();

  if (!archives)
  {
    return AssetOrigin::NotFound;
  }

  // Reverse, because ClientData::readFile resolves in reverse and the LAST loaded archive wins.
  // Asking any other one first would attribute the file to an archive the client will not read it
  // from.
  for (auto it = archives->rbegin(); it != archives->rend(); ++it)
  {
    bool present = false;

    try
    {
      present = (*it)->exists(key, _client_data->locale_mode());
    }
    catch (...)
    {
      continue;
    }

    if (!present)
    {
      continue;
    }

    if (_target_archive && (*it)->path() == _target_archive->path())
    {
      return AssetOrigin::TargetArchive;
    }

    return isBaseClientArchiveName((*it)->path())
      ? AssetOrigin::BaseClientArchive
      : AssetOrigin::CustomArchive;
  }

  return AssetOrigin::NotFound;
}

void Noggit::PatchAssetPacker::collectRoots( std::vector<QueuedReference>& queue
                                           , PatchAssetPackerResult& result
                                           ) const
{
  std::string const project_path (_client_data->projectPath());

  std::error_code error;

  if (!fs::is_directory(project_path, error) || error)
  {
    return;
  }

  // Every ADT and WDT in the project is a root, whether or not the user ever loaded that tile.
  // That is the structural reason AssetScanCollector cannot be used here: it walks
  // MapIndex::loaded_tiles(), and a project folder routinely holds tiles nobody visited this
  // session.
  //
  // M2 and WMO files in the project are roots too. They are already packed by the existing pass,
  // but their own dependencies -- a custom building's textures, or its group files if the user
  // copied only the root in by hand -- may live anywhere in the chain.
  // The iteration itself can throw part way through -- a directory that disappears, or one the
  // process cannot descend into on a network share -- and losing the roots already found because
  // of a file nobody asked about would be the wrong trade.
  try
  {
    fs::recursive_directory_iterator iterator
      (project_path, fs::directory_options::skip_permission_denied, error);

    if (error)
    {
      return;
    }

    for (auto const& entry : iterator)
    {
      std::error_code entry_error;

      if (!entry.is_regular_file(entry_error) || entry_error)
      {
        continue;
      }

      std::string const relative
        (fs::relative(entry.path(), fs::path(project_path), entry_error).string());

      if (entry_error || relative.empty())
      {
        continue;
      }

      std::string const path (normalizeReferencePath(relative));

      if (path.empty() || !isTraversable(path))
      {
        continue;
      }

      // A WMO group file names nothing; opening it as a root would find no MOHD and report a parse
      // failure for a file that is perfectly good.
      if (isWmoGroupFileName(path))
      {
        continue;
      }

      QueuedReference root;
      root.path = path;
      root.referrer = "project folder";
      root.kind = assetKindFromPath(path);
      root.necessity = ReferenceNecessity::Required;
      root.traversable = true;
      root.is_root = true;

      queue.push_back(std::move(root));
      ++result.roots;
    }
  }
  catch (std::exception const& exception)
  {
    LogError << "PatchAssetPacker: stopped enumerating the project folder: "
             << exception.what() << std::endl;
  }
}

Noggit::PatchAssetPackerResult Noggit::PatchAssetPacker::run(ProgressFn const& progress)
{
  PatchAssetPackerResult result;

  if (!_client_data || !_target_archive)
  {
    return result;
  }

  std::vector<QueuedReference> roots;
  collectRoots(roots, result);

  std::deque<QueuedReference> queue (roots.begin(), roots.end());
  std::unordered_set<std::string> visited;

  for (auto const& root : roots)
  {
    visited.insert(root.path);
  }

  std::vector<char> buffer;
  std::vector<AssetReference> children;

  while (!queue.empty())
  {
    QueuedReference const item (std::move(queue.front()));
    queue.pop_front();

    ++result.visited;

    if (progress && !progress("Collecting referenced assets", result.visited, result.visited + queue.size(), item.path))
    {
      result.cancelled = true;
      return result;
    }

    AssetOrigin const origin (originOf(item.path));

    if (origin == AssetOrigin::NotFound)
    {
      // Optional references are the ones the loaders themselves probe for -- external .anim files,
      // a WMO skybox, a tileset's _s.blp companion. Absence is the normal case and recording it
      // would bury the names that matter.
      if (item.necessity == ReferenceNecessity::Required)
      {
        result.scan.addReference(item.kind, item.path, item.referrer, AssetStatus::Missing);
      }
      else
      {
        ++result.optional_absent;
      }

      continue;
    }

    bool const wanted_for_pack
      ( !item.is_root
     && ( origin == AssetOrigin::CustomArchive
       || (origin == AssetOrigin::BaseClientArchive && _options.include_base_client_assets)
        )
      );

    bool const traverse (item.traversable && isTraversable(item.path));

    AssetStatus status = AssetStatus::Present;

    if (traverse || wanted_for_pack)
    {
      buffer.clear();

      try
      {
        // One read, not a probe followed by a read. ClientData::exists repeats the whole disk stat
        // plus per-archive walk, so exists() + ClientFile is two full lookups of the same path.
        BlizzardArchive::ClientFile file (item.path, _client_data);

        std::size_t const size = file.getSize();

        // A zero byte payload is not usable and is not distinguishable from absent once it is
        // inside an archive: ClientData::readFile skips empty files outright (ClientData.cpp:426).
        // Guarding explicitly matters because MPQArchive::writeFile's assert(buf_size) compiles to
        // nothing under NDEBUG, which RelWithDebInfo defines.
        if (file.isEof() || !size || !file.getBuffer())
        {
          status = AssetStatus::Unreadable;
        }
        else
        {
          buffer.assign(file.getBuffer(), file.getBuffer() + size);
        }
      }
      catch (...)
      {
        // Present according to the probe but unreadable in practice: a corrupt archive entry, or a
        // project file that vanished between the probe and the read.
        status = AssetStatus::Unreadable;
      }
    }

    result.scan.addReference(item.kind, item.path, item.referrer, status);

    switch (origin)
    {
      case AssetOrigin::ProjectFolder:
        if (!item.is_root)
        {
          ++result.skipped_in_project;
        }
        break;
      case AssetOrigin::TargetArchive:
        if (!item.is_root)
        {
          ++result.skipped_already_in_patch;
        }
        break;
      case AssetOrigin::BaseClientArchive:
        if (!item.is_root && !_options.include_base_client_assets)
        {
          ++result.skipped_base_client;
        }
        break;
      default:
        break;
    }

    if (status != AssetStatus::Present)
    {
      continue;
    }

    if (wanted_for_pack)
    {
      try
      {
        fs::path const staged_path (nextStagingPath());
        std::ofstream out (staged_path, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);

        if (out.is_open())
        {
          out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
          out.close();

          StagedFile staged;
          // BACKSLASHES. MPQ resolves a file by hashing its name, so what is stored has to be
          // byte-for-byte what the client asks for -- and the client asks for
          // World\Maps\Expansion01\Expansion01_29_32.adt. This is the same conversion addFile
          // applies (MPQArchive.cpp:153-154), computed here so the name is one this code chose.
          staged.stored_name = archiveStoredName(item.path);
          staged.disk_path = staged_path;
          staged.size = buffer.size();

          _staged.push_back(std::move(staged));
        }
        else
        {
          ++result.write_failures;
          result.write_errors.push_back(item.path + ": could not create a staging file");
        }
      }
      catch (std::exception const& exception)
      {
        ++result.write_failures;
        result.write_errors.push_back(item.path + ": " + exception.what());
      }
    }

    if (!traverse)
    {
      continue;
    }

    children.clear();

    if (isAdtLike(item.path))
    {
      collectAdtReferences(buffer.data(), buffer.size(), children);
    }
    else if (isModel(item.path))
    {
      collectModelReferences(item.path, buffer.data(), buffer.size(), children);
    }
    else if (isWorldModel(item.path))
    {
      collectWorldModelReferences(item.path, buffer.data(), buffer.size(), children);
    }

    for (auto& child : children)
    {
      if (child.path.empty() || !visited.insert(child.path).second)
      {
        continue;
      }

      QueuedReference queued;
      queued.path = std::move(child.path);
      queued.referrer = item.path;
      queued.kind = child.kind;
      queued.necessity = child.necessity;
      queued.traversable = child.traversable;

      queue.push_back(std::move(queued));
    }
  }

  // ---- phase 2: write -------------------------------------------------------------------------

  if (_staged.empty())
  {
    return result;
  }

  bool opened = false;

  try
  {
    opened = _target_archive->openForWritting();
  }
  catch (std::exception const& exception)
  {
    result.write_errors.push_back(std::string("could not open the archive for writing: ") + exception.what());
  }

  if (!opened)
  {
    result.write_failures += _staged.size();
    return result;
  }

  std::size_t written = 0;

  for (auto const& staged : _staged)
  {
    ++written;

    if (progress && !progress("Adding referenced assets", written, _staged.size(), staged.stored_name))
    {
      result.cancelled = true;
      break;
    }

    try
    {
      // setFilepath, NOT the normalising FileKey constructor.
      //
      // Handing addFile a std::string would run it through ClientData::normalizeFilenameInternal,
      // whose .mdx rewrite is std::regex_replace(name, std::regex(".mdx"), ".m2") -- '.' matches
      // ANY character and every match is replaced, so a path such as creature/amdxthing/foo.m2
      // would be stored as creature/.m2thing/foo.m2 and hash to something no client will ever ask
      // for. That same function's ::tolower is also undefined for the bytes >= 0x80 these names
      // carry. setFilepath stores the string verbatim, and addFile's own '/' -> '\\' pass is then
      // a no-op on a name that is already backslash separated and lowercase.
      BlizzardArchive::Listfile::FileKey stored_key;
      stored_key.setFilepath(staged.stored_name);

      bool const success
        (_target_archive->addFile( stored_key
                                 , staged.disk_path.string()
                                 , _client_data->locale_mode()
                                 , 0
                                 , _options.compress
                                 ));

      if (success)
      {
        ++result.added;
        result.bytes_added += staged.size;
      }
      else
      {
        ++result.write_failures;
        result.write_errors.push_back(staged.stored_name + ": the archive refused the file");
      }
    }
    catch (std::exception const& exception)
    {
      ++result.write_failures;
      result.write_errors.push_back(staged.stored_name + ": " + exception.what());
    }
    catch (...)
    {
      ++result.write_failures;
      result.write_errors.push_back(staged.stored_name + ": unhandled exception");
    }
  }

  try
  {
    if (_options.compact && result.added)
    {
      if (progress)
      {
        progress("Compacting archive", _staged.size(), _staged.size(), std::string());
      }

      _target_archive->compactArchive();
    }
  }
  catch (std::exception const& exception)
  {
    result.write_errors.push_back(std::string("compacting failed: ") + exception.what());
  }

  // Unconditional: leaving the archive open for writing would leave every later read going through
  // a writable handle, and closeToReadOnly is the only thing that puts the read-only stream flag
  // back.
  try
  {
    _target_archive->closeToReadOnly();
  }
  catch (std::exception const& exception)
  {
    result.write_errors.push_back(std::string("could not reopen the archive read-only: ") + exception.what());
  }

  removeStagingDirectory();

  return result;
}
