// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/SafeFileWrite.hpp>
#include <noggit/Log.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>
#include <QtCore/QString>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
  struct NoggitPendingSaveFailure
  {
    std::string target;
    std::string reason;
  };

  // Function-local statics rather than file scope objects, because this translation unit is
  // collected into the noggit target by a recursive glob (CMakeLists.txt:206/208) that also feeds a
  // unity build, and a first-use-is-first-initialisation static has no ordering question to get
  // wrong no matter which blob it lands in.
  std::mutex& noggitPendingSaveFailureMutex()
  {
    static std::mutex mutex;
    return mutex;
  }

  std::vector<NoggitPendingSaveFailure>& noggitPendingSaveFailures()
  {
    static std::vector<NoggitPendingSaveFailure> failures;
    return failures;
  }

  // Saving a whole map walks a 64x64 grid, so a full disk can queue thousands of failures. The
  // dialog names this many and counts the rest, because a list nobody can read is the same as no
  // list; every one of them is in the log in full.
  constexpr std::size_t SAVE_FAILURE_LIST_LIMIT = 10;

  void noggitShowPendingSaveFailures()
  {
    std::vector<NoggitPendingSaveFailure> failures;

    {
      std::lock_guard<std::mutex> const lock (noggitPendingSaveFailureMutex());
      failures.swap (noggitPendingSaveFailures());
    }

    if (failures.empty())
    {
      return;
    }

    // reportSaveFailure has already logged every one of these, so a build running without widgets
    // still reports the failure; it just cannot put a window on screen.
    if (!qobject_cast<QApplication*> (QCoreApplication::instance()))
    {
      return;
    }

    QString detail;

    for (std::size_t i (0); i < failures.size() && i < SAVE_FAILURE_LIST_LIMIT; ++i)
    {
      detail += QString::fromStdString (failures[i].target)
              + QStringLiteral ("\n    ")
              + QString::fromStdString (failures[i].reason)
              + QStringLiteral ("\n\n");
    }

    if (failures.size() > SAVE_FAILURE_LIST_LIMIT)
    {
      detail += QStringLiteral ("... and %1 more, all of them in the log.")
                .arg (failures.size() - SAVE_FAILURE_LIST_LIMIT);
    }

    QMessageBox box;
    box.setIcon (QMessageBox::Critical);
    box.setWindowTitle (QStringLiteral ("Save failed"));
    box.setText (QStringLiteral ("Noggit could not save %1 file(s).").arg (failures.size()));
    box.setInformativeText
      ( QStringLiteral
          ( "The previous version of each of those files is still on disk exactly as it was. "
            "Nothing was overwritten or truncated, and the unsaved changes are still loaded in "
            "the editor.\n\n"
            "Do not close Noggit until a save succeeds, or those changes are lost. The usual "
            "causes are a full disk, a read-only project folder, and antivirus or the game "
            "client holding the file open."
          )
      );
    box.setDetailedText (detail);
    box.exec();
  }
}

bool Noggit::writeFileAtomically ( std::filesystem::path const& target
                                 , char const* data
                                 , std::size_t size
                                 , std::string* error_out
                                 )
{
  auto const fail
    ( [error_out] (std::string message) -> bool
      {
        if (error_out)
        {
          *error_out = std::move (message);
        }

        return false;
      }
    );

  if (error_out)
  {
    error_out->clear();
  }

  std::error_code error;

  // BlizzardArchive::ClientFile::save() created the parent directory before writing, and a map
  // saved for the first time has no World/Maps/<name>/ yet, so dropping this would break the
  // creation of every new map.
  std::filesystem::path const directory (target.parent_path());

  if (!directory.empty())
  {
    std::filesystem::create_directories (directory, error);

    // create_directories() returns false and leaves the error code clear when the directory is
    // already there, so the error code, not the return value, is what says it could not be made.
    if (error)
    {
      return fail ( "could not create the directory " + directory.string() + ": "
                  + error.message()
                  );
    }
  }

  std::filesystem::path const temporary (target.string() + ".tmp");

  {
    std::ofstream stream
      (temporary.string(), std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);

    if (!stream.is_open())
    {
      return fail ("could not open " + temporary.string() + " for writing");
    }

    // std::ostream::write() reads nothing through the pointer when the count is zero, but an
    // empty std::vector<char> is allowed to return a null data(), and handing that to write() is
    // not something the standard blesses. No ADT, WDT or WDL is ever empty; this guard is for the
    // callers that cannot know that.
    if (size)
    {
      stream.write (data, static_cast<std::streamsize> (size));
    }

    stream.close();

    // Checked AFTER close(), exactly as DBCFile::save does, because the failure this is mostly
    // guarding against -- a full disk -- is reported when the final buffer is flushed and not by
    // any individual write() call. A stream that looked healthy the whole way through can still
    // fail here, and the path this replaces never looked at the stream state at all.
    if (!stream)
    {
      std::error_code ignored;
      std::filesystem::remove (temporary, ignored);

      return fail ("writing " + temporary.string() + " failed; the disk may be full");
    }
  }

  std::string commit_error;

  if (!commitReplacementFile (temporary, target, &commit_error))
  {
    return fail (std::move (commit_error));
  }

  return true;
}

bool Noggit::commitReplacementFile ( std::filesystem::path const& temporary
                                   , std::filesystem::path const& target
                                   , std::string* error_out
                                   )
{
  if (error_out)
  {
    error_out->clear();
  }

  std::error_code error;
  std::filesystem::path const backup (target.string() + ".bak");

  // Best effort and deliberately not fatal, as in DBCFile::save. Copied rather than renamed: a
  // rename would take the original away, and a failure in the swap below would then leave the
  // destination missing entirely, which is the outcome this function exists to prevent.
  if (std::filesystem::exists (target, error))
  {
    std::error_code backup_error;
    std::filesystem::copy_file
      (target, backup, std::filesystem::copy_options::overwrite_existing, backup_error);

    if (backup_error)
    {
      LogError << "Could not write the backup " << backup.string() << ": "
               << backup_error.message() << ". Continuing; the new file is still written."
               << std::endl;
    }
  }

  // The one moment the destination changes. std::filesystem::rename replaces an existing regular
  // file, and on Windows the MSVC implementation is MoveFileExW with MOVEFILE_REPLACE_EXISTING,
  // so a reader sees either the whole old file or the whole new one and never a partial one.
  std::filesystem::rename (temporary, target, error);

  if (error)
  {
    std::error_code ignored;
    std::filesystem::remove (temporary, ignored);

    if (error_out)
    {
      *error_out = "could not replace " + target.string() + ": " + error.message();
    }

    return false;
  }

  return true;
}

bool Noggit::writeFileGuarded (std::filesystem::path const& target, std::vector<char> const& data)
{
  std::string error;

  if (writeFileAtomically (target, data.data(), data.size(), &error))
  {
    return true;
  }

  reportSaveFailure (target, error);

  return false;
}

void Noggit::reportSaveFailure (std::filesystem::path const& target, std::string const& reason)
{
  LogError << "SAVE FAILED for " << target.string() << ": " << reason
           << ". The previous file was left untouched and the edits are still in memory."
           << std::endl;

  bool post_flush (false);

  {
    std::lock_guard<std::mutex> const lock (noggitPendingSaveFailureMutex());

    // Only the report that finds the queue empty posts a flush. Every failure raised before the
    // event loop next turns joins that same dialog, which is what turns 200 failed tiles on a
    // full disk into one list instead of 200 modals.
    post_flush = noggitPendingSaveFailures().empty();
    noggitPendingSaveFailures().push_back ({target.string(), reason});
  }

  if (!post_flush)
  {
    return;
  }

  QCoreApplication* const application (QCoreApplication::instance());

  // Posted, not shown inline. Saving is reached from World and MapIndex code that runs close to
  // drawing, and running a modal event loop inside a GL scope leaves OpenGL::Scoped destructors
  // to throw with no current context, which terminates the process during unwinding. The post is
  // also the only thread safe way to do this: a QMessageBox may only be built on the GUI thread,
  // and nothing guarantees that a tile save is on it.
  if ( !application
    || !QMetaObject::invokeMethod
         (application, [] { noggitShowPendingSaveFailures(); }, Qt::QueuedConnection)
     )
  {
    // Release the latch. A queue that will never be drained would make this one failed post
    // suppress the dialog for every later failure in the session.
    std::lock_guard<std::mutex> const lock (noggitPendingSaveFailureMutex());
    noggitPendingSaveFailures().clear();
  }
}
