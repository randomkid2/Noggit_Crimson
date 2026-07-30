// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/GmCommands.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/database/TileCoordinates.hpp>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using namespace Noggit::Database;

namespace
{
  constexpr char const* GO_COMMAND = ".go xyz";

  // Character classification is written out rather than taken from <cctype> for two reasons, and
  // the first is the point of this whole file: std::isdigit and friends consult the C locale,
  // which is exactly what this module has to be immune to. The second is that passing a plain
  // char with the high bit set to them is undefined behaviour, and pasted text is where a stray
  // 0x96 en-dash arrives from. The command syntax is ASCII, so ASCII tests are also the correct
  // tests rather than merely the safe ones.
  bool isAsciiDigit(char c)
  {
    return c >= '0' && c <= '9';
  }

  bool isAsciiSpace(char c)
  {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  }

  // Only spaces and tabs separate a label from its colon. A newline may not: `.gps` output puts
  // one label per value on a line and a label whose colon is on the next line does not exist,
  // so accepting one would only let a stray "X" at the end of a line capture the next line's
  // number.
  bool isBlank(char c)
  {
    return c == ' ' || c == '\t';
  }

  bool isIdentifierStart(char c)
  {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }

  bool isIdentifierChar(char c)
  {
    return isIdentifierStart(c) || isAsciiDigit(c);
  }

  std::string loweredSpan(std::string const& text, std::size_t begin, std::size_t end)
  {
    std::string out;
    out.reserve(end - begin);

    for (std::size_t i = begin; i < end; ++i)
    {
      char const c (text[i]);

      out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }

    return out;
  }

  // Fixed notation, classic locale, trailing zeros trimmed, no "-0".
  std::string formatNumber(double value)
  {
    if (!std::isfinite(value))
    {
      throw GmCommandError
        ("Refusing to format a non-finite coordinate as a GM command. A NaN or infinite position"
         " means the value was computed from bad input; the command parser does not accept \"nan\""
         " and substituting any finite value would teleport the user somewhere real.");
    }

    // The classic locale is imbued rather than inherited. Under a global locale with a comma
    // decimal separator the stream would write "-9500,25", which the server splits into two
    // arguments -- so the command does not fail, it teleports the GM to a different place.
    //
    // std::fixed because the default format switches to scientific notation for small
    // magnitudes, and "1e-05" is not a number the command parser reads.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(GM_COORDINATE_DECIMALS) << value;

    std::string text (out.str());

    // Every value between -1e-7 and -0.0 formats to "-0.000000". It is a legal number and the
    // server accepts it, but it reads as a bug to whoever is checking the command and it does
    // not match the "0" the same position produces once stored. Drop the sign when no
    // significant digit survived the rounding.
    bool const all_zero (text.find_first_of("123456789") == std::string::npos);

    if (all_zero && !text.empty() && text.front() == '-')
    {
      text.erase(text.begin());
    }

    std::size_t const point (text.find('.'));

    if (point != std::string::npos)
    {
      std::size_t last (text.size());

      while (last > point + 1 && text[last - 1] == '0')
      {
        --last;
      }

      // Nothing but zeros after the point, so the point goes too: "58." is a number some parsers
      // accept and none require.
      if (last == point + 1)
      {
        last = point;
      }

      text.erase(last);
    }

    return text;
  }

  struct ScannedNumber
  {
    double value = 0.0;
    std::size_t end = 0;             // one past the last character consumed
  };

  // Reads one number starting exactly at `at`, or nothing when the text there is not a number
  // this module is willing to read.
  //
  // The shape is validated by hand FIRST and only the validated span is converted. std::stod is
  // unusable here on three counts: it consults the global C locale, so "-9500.25" reads as -9500
  // under an LC_NUMERIC that uses a comma; it throws rather than reporting how far it got, so
  // "58abc" cannot be distinguished from "58"; and it silently skips leading whitespace, which
  // hides where one field ended and the next began. std::from_chars is locale-independent by
  // definition and reports both the error and the endpoint.
  //
  // Validating the shape first also keeps from_chars away from the "inf" and "nan" spellings it
  // would otherwise accept, since a span holding a letter never reaches it.
  std::optional<ScannedNumber> scanNumber(std::string const& text, std::size_t at)
  {
    std::size_t i (at);

    if (i < text.size() && (text[i] == '-' || text[i] == '+'))
    {
      ++i;
    }

    std::size_t const integer_begin (i);

    while (i < text.size() && isAsciiDigit(text[i]))
    {
      ++i;
    }

    bool const has_integer_digits (i > integer_begin);
    bool has_fraction_digits (false);

    // The point is consumed only when a digit follows it, so a sentence ending in "58." leaves
    // the full stop behind for the caller to reject rather than swallowing it as part of a
    // number.
    if (i < text.size() && text[i] == '.')
    {
      std::size_t j (i + 1);
      std::size_t const fraction_begin (j);

      while (j < text.size() && isAsciiDigit(text[j]))
      {
        ++j;
      }

      if (j > fraction_begin)
      {
        has_fraction_digits = true;
        i = j;
      }
    }

    if (!has_integer_digits && !has_fraction_digits)
    {
      return std::nullopt;
    }

    if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
    {
      std::size_t j (i + 1);

      if (j < text.size() && (text[j] == '-' || text[j] == '+'))
      {
        ++j;
      }

      std::size_t const exponent_begin (j);

      while (j < text.size() && isAsciiDigit(text[j]))
      {
        ++j;
      }

      if (j > exponent_begin)
      {
        i = j;
      }

      // An 'e' with no digits after it belongs to a word, not to the number. It is left in place
      // and the check below rejects the whole token.
    }

    // A number glued to a word is not a number: "58abc" and "0x10" and a bare "1e" all land
    // here. Reading the leading digits and discarding the rest would answer confidently with
    // half of whatever the user actually pasted.
    if (i < text.size() && isIdentifierChar(text[i]))
    {
      return std::nullopt;
    }

    ScannedNumber scanned;
    scanned.end = i;

    // from_chars does not recognise a leading '+', so it is stepped over rather than passed on.
    char const* const first (text.data() + at + (text[at] == '+' ? 1 : 0));
    char const* const last (text.data() + i);

    std::from_chars_result const result
      (std::from_chars(first, last, scanned.value, std::chars_format::general));

    // Anything short of consuming the whole validated span is a disagreement between this
    // scanner and the conversion, and a literal too large for a double (result_out_of_range)
    // is a rejection rather than an infinity: 1e400 is not a coordinate.
    if (result.ec != std::errc() || result.ptr != last)
    {
      return std::nullopt;
    }

    return scanned;
  }

  enum class Field
  {
    NONE,
    X,
    Y,
    Z,
    MAP,
    ORIENTATION
  };

  // Labels match in full and never by prefix or suffix. That is not fastidiousness: real `.gps`
  // output prints "ZoneX:" and "ZoneY:" two lines below "X:" and "Y:", and "GroundZ:" and
  // "FloorZ:" below "Z:". A suffix match would read the zone-local coordinate as the world one,
  // which is a plausible number in the wrong frame -- the worst kind of wrong answer.
  Field fieldForLabel(std::string const& label)
  {
    if (label == "x")
    {
      return Field::X;
    }

    if (label == "y")
    {
      return Field::Y;
    }

    if (label == "z")
    {
      return Field::Z;
    }

    if (label == "map" || label == "mapid" || label == "map_id")
    {
      return Field::MAP;
    }

    if (label == "o" || label == "or" || label == "ori" || label == "orientation"
        || label == "facing")
    {
      return Field::ORIENTATION;
    }

    return Field::NONE;
  }

  struct LabelledFields
  {
    std::optional<double> x;
    std::optional<double> y;
    std::optional<double> z;
    std::optional<double> map;
    std::optional<double> orientation;
  };

  void recordFirst(std::optional<double>& field, double value)
  {
    if (!field.has_value())
    {
      field = value;
    }
  }

  // Collects every `label: value` pair in the text, in one pass, ignoring everything else.
  //
  // The FIRST occurrence of each label wins, and that is a measured decision rather than
  // laziness: a worldserver `.gps` message names the map twice. It opens with
  // "Map: 0 (Eastern Kingdoms) Zone: ..." and ends with "Have height data (Map: 1 VMap: 1)",
  // where that second "Map:" is a flag saying a height map was loaded and not a map id at all.
  // Rejecting the duplicate would reject the single most likely thing a user pastes; preferring
  // the last would answer "map 1" for a position in Azeroth.
  LabelledFields scanLabels(std::string const& text)
  {
    LabelledFields found;
    std::size_t i (0);

    while (i < text.size())
    {
      if (!isIdentifierStart(text[i]))
      {
        ++i;
        continue;
      }

      // Maximal identifier runs, so scanning can never start part-way through a word. This is
      // what keeps "ZoneX" from being seen as an "X" preceded by noise.
      std::size_t const begin (i);

      while (i < text.size() && isIdentifierChar(text[i]))
      {
        ++i;
      }

      Field const field (fieldForLabel(loweredSpan(text, begin, i)));

      if (field == Field::NONE)
      {
        continue;
      }

      std::size_t j (i);

      while (j < text.size() && isBlank(text[j]))
      {
        ++j;
      }

      if (j >= text.size() || (text[j] != ':' && text[j] != '='))
      {
        continue;
      }

      ++j;

      while (j < text.size() && isBlank(text[j]))
      {
        ++j;
      }

      std::optional<ScannedNumber> const number (scanNumber(text, j));

      if (!number.has_value())
      {
        continue;
      }

      switch (field)
      {
        case Field::X:           recordFirst(found.x, number->value); break;
        case Field::Y:           recordFirst(found.y, number->value); break;
        case Field::Z:           recordFirst(found.z, number->value); break;
        case Field::MAP:         recordFirst(found.map, number->value); break;
        case Field::ORIENTATION: recordFirst(found.orientation, number->value); break;
        case Field::NONE:        break;
      }

      i = number->end;
    }

    return found;
  }

  // Words this module will step over in front of a coordinate list. Anything else is a
  // rejection: skipping unrecognised words would let "he died at -9500 70 58" parse, and a
  // sentence mentioning three numbers is not a coordinate someone asked to be read.
  bool isCommandWord(std::string const& word)
  {
    return word == "go" || word == "gox" || word == "goxyz" || word == "xyz"
        || word == "tele" || word == "teleport" || word == "port" || word == "worldport"
        || word == "gps";
  }

  // Reads the text as an optional command prefix followed by a separated list of numbers.
  std::optional<std::vector<double>> scanNumberList(std::string const& text)
  {
    std::size_t i (0);

    while (i < text.size() && isAsciiSpace(text[i]))
    {
      ++i;
    }

    // The '.' command prefix is optional. A coordinate copied out of a chat log frequently loses
    // it, and "go xyz ..." is no less clearly a command than ".go xyz ...". It is only treated
    // as a prefix when a word follows, so a bare ".5 .7 .8" is still three numbers.
    if (i + 1 < text.size() && text[i] == '.' && isIdentifierStart(text[i + 1]))
    {
      ++i;
    }

    while (i < text.size() && isIdentifierStart(text[i]))
    {
      std::size_t const begin (i);

      while (i < text.size() && isIdentifierChar(text[i]))
      {
        ++i;
      }

      if (!isCommandWord(loweredSpan(text, begin, i)))
      {
        return std::nullopt;
      }

      while (i < text.size() && isAsciiSpace(text[i]))
      {
        ++i;
      }
    }

    std::vector<double> values;

    while (true)
    {
      std::optional<ScannedNumber> const number (scanNumber(text, i));

      if (!number.has_value())
      {
        return std::nullopt;
      }

      values.push_back(number->value);
      i = number->end;

      // A separator run is whitespace with at most one comma or semicolon in it. Allowing more
      // would accept "-9500,,70,58", where the empty field is evidence that something was lost
      // in the copy rather than a style of writing.
      std::size_t const separator_begin (i);
      bool comma_seen (false);

      while (i < text.size())
      {
        if (isAsciiSpace(text[i]))
        {
          ++i;
        }
        else if ((text[i] == ',' || text[i] == ';') && !comma_seen)
        {
          comma_seen = true;
          ++i;
        }
        else
        {
          break;
        }
      }

      if (i == text.size())
      {
        return values;
      }

      // Something that is neither a separator nor the end of the text follows the number.
      // Carrying on would mean guessing what it was.
      if (i == separator_begin)
      {
        return std::nullopt;
      }
    }
  }

  std::optional<std::uint16_t> mapFromNumber(double value)
  {
    // A map id is an identifier, not a measurement. A fractional value is a misread field rather
    // than a number wanting rounding, a negative one cannot be a map at all, and anything past
    // GM_MAX_MAP_ID does not fit the `map` column. Every one of the three, if coerced, would
    // answer with a real map that is the wrong map.
    if (!std::isfinite(value) || value < 0.0 || value > static_cast<double>(GM_MAX_MAP_ID))
    {
      return std::nullopt;
    }

    if (std::floor(value) != value)
    {
      return std::nullopt;
    }

    return static_cast<std::uint16_t>(value);
  }

  // Last gate before a parse result escapes. scanNumber cannot currently produce a non-finite
  // value -- it rejects both the "inf"/"nan" spellings and an out-of-range literal -- so this
  // holds the guarantee rather than fixing a known hole: a NaN leaving here would flow straight
  // into tile arithmetic and out into an INSERT.
  std::optional<ParsedCoordinate> readableOrNothing(ParsedCoordinate const& parsed)
  {
    if (!std::isfinite(parsed.position.x)
        || !std::isfinite(parsed.position.y)
        || !std::isfinite(parsed.position.z))
    {
      return std::nullopt;
    }

    return parsed;
  }
}

std::string Noggit::Database::teleportCommand
  (std::uint16_t map, WorldPosition const& position, double orientation)
{
  double const facing (TileCoordinates::normaliseOrientation(orientation));

  // normaliseOrientation folds a NaN or an infinity to zero, so `facing` is always finite and
  // only the position can refuse to format.
  return std::string(GO_COMMAND) + " "
       + formatNumber(position.x) + " "
       + formatNumber(position.y) + " "
       + formatNumber(position.z) + " "
       + std::to_string(static_cast<std::uint32_t>(map)) + " "
       + formatNumber(facing);
}

std::string Noggit::Database::goCommandFor(CreatureSpawn const& spawn)
{
  return teleportCommand(spawn.map, spawn.position, spawn.orientation);
}

std::string Noggit::Database::goCommandFor(GameObjectSpawn const& spawn)
{
  // rotation0..3 is what the core actually faces a gameobject by; the `orientation` column rides
  // alongside it and the two disagree in rows written by other tools. Sending the GM to look at
  // the object means believing the quaternion whenever one was supplied -- the same precedence
  // ChangesetBuilder applies when it emits the row, so the command and the changeset cannot
  // disagree about which way the thing points.
  //
  // The default-constructed quaternion is the identity, which is indistinguishable from "nobody
  // set this", so it defers to the orientation column. Comparing it by exact equality is correct
  // here and is not the float-equality mistake the schema notes warn about: the question is
  // whether a field was touched, not whether two rotations agree.
  bool const rotation_supplied
    (  spawn.rotation.r0 != 0.0 || spawn.rotation.r1 != 0.0
    || spawn.rotation.r2 != 0.0 || spawn.rotation.r3 != 1.0
    );

  double const orientation
    ( rotation_supplied
        ? TileCoordinates::orientationForQuaternion(spawn.rotation)
        : spawn.orientation
    );

  return teleportCommand(spawn.map, spawn.position, orientation);
}

std::optional<ParsedCoordinate> Noggit::Database::parseCoordinate(std::string const& text)
{
  ParsedCoordinate parsed;

  // A comma sitting directly between two digits is ambiguous and must be refused before any
  // number is read.
  //
  // The scanner terminates a number at a comma, which is right for the "-9500, 70, 58" list the
  // module is required to accept. But it makes "-9500,25 70,5 58,125" -- what a user on a
  // comma-decimal locale gets when they copy a coordinate out of a tool that respects
  // LC_NUMERIC -- read as SIX integers rather than three reals. The positional path then shifts
  // every field along, inventing a map id and an orientation out of fraction digits, and the
  // labelled path silently drops the fractional part. Either way the caller is handed a
  // confident answer about a position tens of yards from the one pasted.
  //
  // "-9500,70,58" cannot be told apart from "-9500.70" followed by "58" by any rule, so the
  // only honest response to digit-comma-digit is to decline. A comma followed by whitespace is
  // unambiguously a separator and stays acceptable.
  for (std::size_t i = 1; i + 1 < text.size(); ++i)
  {
    if (text[i] == ',' && isAsciiDigit(text[i - 1]) && isAsciiDigit(text[i + 1]))
    {
      return std::nullopt;
    }
  }

  // Labels first. They are the only shape that survives being surrounded by prose, and a text
  // carrying them says what each number is instead of leaving it to argument order.
  LabelledFields const labelled (scanLabels(text));

  if (labelled.x.has_value() && labelled.y.has_value() && labelled.z.has_value())
  {
    parsed.position.x = *labelled.x;
    parsed.position.y = *labelled.y;
    parsed.position.z = *labelled.z;

    if (labelled.map.has_value())
    {
      std::optional<std::uint16_t> const map (mapFromNumber(*labelled.map));

      if (!map.has_value())
      {
        // The text named a map and the value is not one. Answering with the position alone would
        // drop the field the user was most explicit about and land them on whichever map the
        // editor happened to have open.
        return std::nullopt;
      }

      parsed.map = *map;
      parsed.has_map = true;
    }

    if (labelled.orientation.has_value())
    {
      parsed.has_orientation = true;
      parsed.orientation = TileCoordinates::normaliseOrientation(*labelled.orientation);
    }

    return readableOrNothing(parsed);
  }

  // A partial set of labels is not a case to fall back from. "x: -9500 y: 70" is a coordinate
  // with a missing z, and the positional reading below rejects it on its own terms: it begins
  // with a word that is not a command.
  std::optional<std::vector<double>> const values (scanNumberList(text));

  if (!values.has_value())
  {
    return std::nullopt;
  }

  // The command's own argument order is `.go xyz #x #y [#z [#mapid [#orientation]]]`, so the
  // FOURTH number is the map and not the orientation. There is no other evidence available for a
  // four-number paste, and the two readings differ by a whole world: guessing orientation would
  // silently drop a map id the user did supply.
  //
  // Two numbers are refused even though the command accepts them, because there the server looks
  // the terrain height up for itself and this module has no terrain to look in. A z of 0 is
  // underground almost everywhere in the world.
  if (values->size() < 3 || values->size() > 5)
  {
    return std::nullopt;
  }

  parsed.position.x = (*values)[0];
  parsed.position.y = (*values)[1];
  parsed.position.z = (*values)[2];

  if (values->size() >= 4)
  {
    std::optional<std::uint16_t> const map (mapFromNumber((*values)[3]));

    if (!map.has_value())
    {
      return std::nullopt;
    }

    parsed.map = *map;
    parsed.has_map = true;
  }

  if (values->size() >= 5)
  {
    parsed.has_orientation = true;
    parsed.orientation = TileCoordinates::normaliseOrientation((*values)[4]);
  }

  return readableOrNothing(parsed);
}
