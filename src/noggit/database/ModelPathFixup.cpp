// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/ModelPathFixup.hpp>

#include <string_view>

using namespace Noggit::Database;

namespace
{
  // The MPQ chain's own separator, at the StormLib boundary: MPQArchive::openFile and
  // MPQArchive::exists both push the key through ClientData::normalizeFilenameWoW
  // (MPQArchive.cpp:39,311), which upper-cases and converts '/' to '\\'. Emitting backslash here
  // means the string reads like the path the archive is really asked for.
  constexpr char SEPARATOR = '\\';

  // Extensions this module recognises. Anything else is not a model reference -- notably .wmo,
  // which is a different loader entirely and must not be quietly turned into a .m2 key.
  constexpr std::string_view EXTENSION_MDX = "mdx";
  constexpr std::string_view EXTENSION_MDL = "mdl";
  constexpr std::string_view EXTENSION_M2 = "m2";

  constexpr std::string_view EXTENSION_OUT = ".m2";

  // Explicit set rather than std::isspace. That takes an int, is locale-sensitive, and has
  // undefined behaviour for negative values -- which is every byte >= 0x80 once a signed char is
  // promoted. DBC string tables routinely contain such bytes, so passing one through isspace is
  // a real crash on a debug CRT, not a theoretical one.
  bool isTrimmableSpace(char c)
  {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
  }

  // Bytes that mean the input is not a model path at all, rather than a model path needing
  // repair.
  bool isForbiddenByte(char c)
  {
    unsigned char const byte = static_cast<unsigned char>(c);

    // Control bytes and DEL. A model name arriving with one of these did not come from a valid
    // string-table entry -- most likely getString resolved a bogus offset and handed back a
    // pointer into record data. Rejecting is how that surfaces as a named failure instead of as
    // a nonsense filename in a "could not open" message.
    if (byte < 0x20u || byte == 0x7Fu)
    {
      return true;
    }

    // ':' would make the key absolute ("C:\...") or name an NTFS alternate data stream.
    // DirectoryArchive::openFile joins the key onto the project directory with fs::path's
    // operator/ (DirectoryArchive.cpp:22), and that operator DISCARDS the left-hand side when the
    // right-hand side is absolute. So a colon here escapes the project directory.
    return c == ':';
  }

  // ASCII-only case-insensitive compare, hand-folded for the same reason the header gives for not
  // case-folding the whole path: ::tolower is locale-sensitive, and under a Turkish LC_CTYPE
  // (Qt calls setlocale(LC_ALL, "") at startup) 'I' does not fold to 'i'. ".MDX" must be
  // recognised on every machine, not on most of them.
  bool equalsIgnoringAsciiCase(std::string_view lhs, std::string_view rhs)
  {
    if (lhs.size() != rhs.size())
    {
      return false;
    }

    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
      char left = lhs[i];

      if (left >= 'A' && left <= 'Z')
      {
        left = static_cast<char>(left - 'A' + 'a');
      }

      if (left != rhs[i])
      {
        return false;
      }
    }

    return true;
  }

  // True when any '\\'-delimited component is "." or "..".
  //
  // Neither can appear in a real MPQ key, and ".." is actively dangerous for the same
  // fs::path-join reason as ':': a key of "..\\..\\somewhere\\evil.m2" resolves outside the
  // project directory when the archive chain falls through to DirectoryArchive.
  bool hasDotComponent(std::string const& path)
  {
    std::size_t component_begin = 0;

    for (;;)
    {
      std::size_t const separator = path.find(SEPARATOR, component_begin);
      std::size_t const length = (separator == std::string::npos)
                               ? path.size() - component_begin
                               : separator - component_begin;

      std::string_view const component (path.data() + component_begin, length);

      if (component == "." || component == "..")
      {
        return true;
      }

      if (separator == std::string::npos)
      {
        return false;
      }

      component_begin = separator + 1;
    }
  }
}

std::string ModelPathFixup::toM2Path(std::string const& dbc_name)
{
  std::size_t begin = 0;
  std::size_t end = dbc_name.size();

  while (begin < end && isTrimmableSpace(dbc_name[begin]))
  {
    ++begin;
  }

  while (end > begin && isTrimmableSpace(dbc_name[end - 1]))
  {
    --end;
  }

  std::size_t const trimmed_length = end - begin;

  // Length is checked after trimming so trailing padding in a fixed-width string cannot push an
  // otherwise fine name over the cap.
  if (trimmed_length == 0 || trimmed_length > MAX_MODEL_PATH_LENGTH)
  {
    return {};
  }

  std::string normalised;
  normalised.reserve(trimmed_length);

  for (std::size_t i = begin; i < end; ++i)
  {
    char const raw = dbc_name[i];

    if (isForbiddenByte(raw))
    {
      return {};
    }

    char const c = (raw == '/') ? SEPARATOR : raw;

    if (c == SEPARATOR && (normalised.empty() || normalised.back() == SEPARATOR))
    {
      // Collapses a run of separators, and drops the run entirely when it is leading. MPQ keys
      // are relative; a leading separator makes every lookup miss, and it is a common artefact
      // of a path pasted out of a listfile viewer.
      continue;
    }

    normalised += c;
  }

  // Only reachable when the input was nothing but separators.
  if (normalised.empty())
  {
    return {};
  }

  if (hasDotComponent(normalised))
  {
    return {};
  }

  std::size_t const last_separator = normalised.find_last_of(SEPARATOR);
  std::size_t const name_begin = (last_separator == std::string::npos) ? 0 : last_separator + 1;
  std::size_t const dot = normalised.find_last_of('.');

  // The dot has to sit inside the FINAL component. A dot in a directory name is not an
  // extension, and treating it as one is precisely the bug in WMO.cpp:180 and in
  // ClientData::normalizeFilenameInternal. A trailing separator also lands here (name_begin ==
  // size), which is correct: a directory is not a model.
  if (dot == std::string::npos || dot < name_begin)
  {
    return {};
  }

  // ".mdx" on its own, or "Creature\\Rat\\.mdx", names no model.
  if (dot == name_begin)
  {
    return {};
  }

  std::string_view const extension (std::string_view(normalised).substr(dot + 1));

  if (!equalsIgnoringAsciiCase(extension, EXTENSION_MDX)
   && !equalsIgnoringAsciiCase(extension, EXTENSION_MDL)
   && !equalsIgnoringAsciiCase(extension, EXTENSION_M2))
  {
    return {};
  }

  // Rewriting even an already-.m2 extension is intentional: it costs nothing and guarantees the
  // output has one spelling, so ".M2" and ".mdx" inputs naming the same asset produce the same
  // key. Without that, a cache keyed on this string would hold two entries for one model.
  normalised.resize(dot);
  normalised += EXTENSION_OUT;

  return normalised;
}

bool ModelPathFixup::looksLikeModelPath(std::string const& path)
{
  return !toM2Path(path).empty();
}
