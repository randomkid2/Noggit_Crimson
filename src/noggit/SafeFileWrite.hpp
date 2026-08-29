// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_SAFEFILEWRITE_HPP
#define NOGGIT_SAFEFILEWRITE_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Noggit
{
  // Replace a file's contents without ever destroying the previous version first.
  //
  // Every caller in this program is saving something the user cannot reconstruct -- an ADT holds
  // the terrain, the textures, the water and the object placements for one 533-yard tile, and it
  // is the only copy. The write path used to open that exact file with std::ofstream in out mode,
  // which truncates it to zero bytes at open(), and only then started producing the replacement.
  // Anything that went wrong in the window between those two events -- a full disk, an antivirus
  // scanner holding the handle, a crash, a network drive dropping -- left a zero-length or
  // half-length ADT and no way back.
  //
  // This writes a sibling temp file, keeps a .bak of the previous version, and renames the temp
  // into place, so the destination only ever changes in one step. On any failure the destination
  // is byte-for-byte what it was before the call.
  //
  // Non-throwing, and returns false rather than reporting anything itself, so that a caller can
  // decide whether one failure should abort a batch. Use writeFileGuarded() when the answer is
  // "tell the user and keep going", which is what every save path in Noggit wants.
  //
  // error_out may be nullptr; when it is not, it receives a sentence naming what failed.
  bool writeFileAtomically ( std::filesystem::path const& target
                           , char const* data
                           , std::size_t size
                           , std::string* error_out
                           );

  // Same, plus reportSaveFailure() on failure. Returns true when the bytes are on disk.
  bool writeFileGuarded (std::filesystem::path const& target, std::vector<char> const& data);

  // The second half of writeFileAtomically, for writers that cannot hand over a buffer.
  //
  // md5translate.trs is produced by a QTextStream on a QIODevice opened with QIODevice::Text,
  // which rewrites every "\n" as "\r\n" on Windows. Rebuilding those bytes by hand to feed
  // writeFileAtomically would be a change to WHAT gets written, which is exactly what this work
  // is not allowed to do -- so that writer keeps its own QFile and QTextStream, points them at
  // `temporary`, and calls this to keep the .bak and swap the file in.
  //
  // The caller is responsible for having closed `temporary` and checked that writing it
  // succeeded. On failure the temp file is removed and `target` is untouched.
  bool commitReplacementFile ( std::filesystem::path const& temporary
                             , std::filesystem::path const& target
                             , std::string* error_out
                             );

  // Make a failed save impossible to miss.
  //
  // A save that fails silently is strictly worse than one that fails noisily: the user believes
  // their session is on disk, closes the editor, and discovers the loss later with nothing left in
  // memory to recover from. So this logs immediately AND queues a modal for the user.
  //
  // The modal is posted to the main thread through Qt::QueuedConnection rather than shown here,
  // for two reasons. Saving is reachable from World and MapIndex code that runs close to drawing,
  // and a modal opened inside a GL scope runs the event loop with no current context, which makes
  // an OpenGL::Scoped destructor throw during unwinding and terminates the process. And the post
  // is thread safe, whereas constructing a QMessageBox off the GUI thread is not.
  //
  // Failures reported before the event loop next turns are coalesced into a single dialog, so
  // saving 200 tiles onto a full disk produces one list and not 200 dialogs.
  void reportSaveFailure (std::filesystem::path const& target, std::string const& reason);
}

#endif // NOGGIT_SAFEFILEWRITE_HPP
