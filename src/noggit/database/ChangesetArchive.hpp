// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_CHANGESETARCHIVE_HPP
#define NOGGIT_DATABASE_CHANGESETARCHIVE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace Noggit::Database
{
  // Raised when the archive cannot do what it was asked.
  //
  // Every failure path here throws, including ones another component might reasonably shrug
  // off. A backup that quietly did not happen is worse than no backup at all: the user stops
  // keeping their own copy of a changeset the moment they believe the tool is keeping one, so
  // the only acceptable outcomes are "stored" and "told loudly".
  class ArchiveError : public std::runtime_error
  {
    public:
      explicit ArchiveError(std::string const& message)
        : std::runtime_error(message) {}
  };

  // One stored changeset, as reported by ChangesetArchive::list().
  struct ArchiveEntry
  {
    // Bare file name, never a path. Hand it straight back to load().
    std::string file_name;

    // The SANITISED label, decoded from the file name. Not necessarily the string that was
    // passed to store() -- see ChangesetArchive::sanitiseLabel.
    std::string label;

    // Seconds since the Unix epoch, exactly the value handed to store().
    //
    // Decoded from the file name rather than read from the filesystem, so it survives the
    // archive being copied, zipped or moved between machines. A last-write-time would not, and
    // "the backup is from some time on Tuesday, probably" is not a recovery story.
    std::uint64_t timestamp = 0;

    std::uintmax_t size_bytes = 0;
  };

  // Timestamped, restorable backups of every emitted changeset.
  //
  // The milestone requirement is narrow and worth restating exactly: a changeset the user has
  // deleted must still be recoverable. Everything in this class follows from that one sentence.
  //
  //   - Nothing is ever overwritten. A name that is already taken gets the next sequence number
  //     instead of replacing what is there, so no store() can destroy an earlier one.
  //   - Nothing is deleted except by prune(), and prune() only ever deletes files this class
  //     both wrote and can still recognise.
  //   - A partially written file is left on disk rather than cleaned up, and named in the
  //     exception. Half a changeset a human can inspect beats none.
  //
  // File names look like this:
  //
  //   <YYYYMMDD>-<HHMMSS>-<NNN>-<label>.sql       20260801-160000-000-tile_49_31.sql
  //
  // Everything before the label is fixed width, zero padded and UTC, which makes a plain
  // lexicographic sort of the names a chronological sort. That is the only promise the naming
  // scheme makes, and it is what lets list(), prune() and a directory listing in any file
  // manager agree with one another for free.
  //
  // UTC rather than local time for two reasons: an hour repeats every autumn in any zone with
  // daylight saving, which would put two names out of order for a whole hour once a year; and
  // two archives from machines in different zones cannot be merged and read in order at all.
  //
  // <NNN> is a per-second sequence number, allocated across the whole second rather than per
  // label, so a burst of stores inside one second still sorts in the order it happened.
  //
  // Uses std::filesystem, no database and no Qt, so it is testable on a bare machine like the
  // rest of this layer.
  class ChangesetArchive
  {
    public:
      // Longest sanitised label that survives into a file name.
      //
      // The full name is then at most 20 + 48 + 4 = 72 characters, which leaves room for a
      // deeply nested archive root under the legacy 260-character Windows path limit. A label
      // is provenance for a reviewer, not a description; anything longer is a comment inside
      // the .sql.
      static constexpr std::size_t MAX_LABEL_LENGTH = 48;

      // The root is explicit and mandatory. Nothing here derives a location from the
      // environment: a backup directory the user did not choose is one they will not find.
      //
      // Throws ArchiveError on an empty path, which would otherwise resolve relative to the
      // process working directory and scatter changesets wherever Noggit happened to be
      // started from. Refused at construction rather than at first store(), so the
      // misconfiguration cannot survive long enough to matter.
      explicit ChangesetArchive(std::filesystem::path root);

      std::filesystem::path const& root() const { return _root; }

      // Writes `sql` verbatim and returns the path written.
      //
      // `timestamp` is seconds since the Unix epoch. It is a PARAMETER rather than a clock read
      // so that ordering, collisions and the whole naming scheme are testable without waiting
      // for real time to pass, and so a caller batching several changesets can stamp them all
      // with one instant.
      //
      // Creates the root if it is absent. That is the one place this class prefers doing the
      // work to complaining: refusing to store because a directory does not exist yet would
      // lose exactly the file it exists to keep.
      //
      // Throws ArchiveError when the root cannot be created or is not a directory, when the
      // write fails or comes up short, when a thousand changesets already share the second, or
      // when `timestamp` is past 9999-12-31T23:59:59Z -- which in practice means milliseconds
      // were passed where seconds were expected.
      std::filesystem::path store
        ( std::string const& sql
        , std::string const& label
        , std::uint64_t timestamp
        );

      // Every stored changeset, newest first.
      //
      // Only files this class recognises are reported: directly in the root, not through a
      // symbolic link, and named to the scheme above. A hand-written .sql or a stray note in
      // the archive directory is not a backup this class made and is not presented as one --
      // which is also what keeps prune() from deleting it.
      //
      // Throws ArchiveError when the root is absent or unreadable. An unreadable archive
      // reported as an empty one is the failure this class is least allowed to have.
      std::vector<ArchiveEntry> list() const;

      // The exact bytes of a stored changeset.
      //
      // `file_name` must be a bare name as returned by list(). Throws ArchiveError when it
      // names a directory component, a root, "." or ".." -- a label is caller-supplied text and
      // a name derived from one must never be able to reach outside the archive -- and when the
      // file is absent or unreadable.
      std::string load(std::string const& file_name) const;

      // Deletes all but the newest `keep_newest` changesets and returns how many were removed.
      //
      // keep_newest == 0 empties the archive of everything this class wrote; a value at or
      // above the current count removes nothing and returns 0.
      //
      // Only files list() reports are candidates, so nothing outside the root, nothing behind a
      // symbolic link and nothing another tool put there can be deleted. Throws ArchiveError if
      // a deletion fails, reporting how many had already gone so the caller knows the archive
      // is in a partially pruned state.
      std::size_t prune(std::size_t keep_newest);

      // The label as it will appear in a file name.
      //
      // Public because it is half the security boundary of this class and deserves to be
      // tested and reasoned about directly. Every byte outside [A-Za-z0-9._-] becomes an
      // underscore, runs collapse to one, leading and trailing separators are dropped and the
      // result is capped at MAX_LABEL_LENGTH. An empty result becomes a fixed default, because
      // substituting a name cannot lose a changeset and refusing to store one can.
      //
      // Bytes above 0x7F are replaced rather than kept: on Windows a narrow std::string reaching
      // std::filesystem::path is interpreted in the active code page, not as UTF-8, so a UTF-8
      // label written through would produce a mojibake file name that no longer matches the
      // bytes the caller passed. Payloads are UTF-8 safe -- they are written verbatim -- but
      // file names are deliberately ASCII.
      static std::string sanitiseLabel(std::string const& label);

    private:
      // A listed file, plus the sequence number decoded from its name. The sequence is an
      // implementation detail of the naming scheme, so it does not appear in ArchiveEntry.
      struct StoredFile
      {
        ArchiveEntry entry;
        unsigned int sequence = 0;
      };

      // Unsorted. Shared by list() and the next-name allocation in store(), so both agree
      // exactly on which files belong to this archive.
      std::vector<StoredFile> collect() const;

      std::string nextFileName(std::uint64_t timestamp, std::string const& label) const;

      void requireRoot() const;
      void ensureRoot();

      std::filesystem::path _root;
  };
}

#endif
