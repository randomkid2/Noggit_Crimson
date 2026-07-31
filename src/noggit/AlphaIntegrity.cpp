// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AlphaIntegrity.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

using namespace Noggit;

namespace
{
  // Last and second-to-last index of a 64-wide alpha map. Named because the pairing of 63 with 62
  // is the entire border rule (Alphamap.cpp:125-131) and "63" on its own reads like a bound.
  constexpr std::size_t LAST = ALPHA_DIM - 1;
  constexpr std::size_t PENULTIMATE = ALPHA_DIM - 2;

  std::string plural(std::size_t count, char const* singular, char const* many)
  {
    return std::to_string(count) + " " + (count == 1 ? singular : many);
  }

  // Takes the excess out of the highest-index layers first. See OverOpacityRepair::ClipUpperLayers
  // for why that is the default.
  void clipUpperLayers(AlphaLayerStack& layers, std::size_t texel, int excess)
  {
    for (std::size_t layer = layers.size(); layer-- > 0;)
    {
      if (excess <= 0)
      {
        break;
      }

      int const value = layers.at(layer, texel);
      int const cut = std::min(value, excess);

      layers.set(layer, texel, static_cast<std::uint8_t>(value - cut));
      excess -= cut;
    }
  }

  // Scales every layer to 255/sum by largest-remainder apportionment.
  //
  // Largest remainder rather than round-then-fix because it is exact by construction: with
  // scaled[k] = value[k] * 255, sum(scaled) = 255 * sum, so sum(quotient) + leftover = 255 with
  // leftover = sum(remainder)/sum. Every remainder is strictly below sum, so leftover is strictly
  // below the number of non-zero remainders and each layer receives at most one extra unit --
  // which is why a zero layer can never be handed one, and why the result never exceeds the
  // input.
  void scaleProportionally(AlphaLayerStack& layers, std::size_t texel, int sum)
  {
    std::array<int, MAX_ALPHA_LAYERS> quotient{};
    std::array<int, MAX_ALPHA_LAYERS> remainder{};

    int assigned = 0;

    for (std::size_t layer = 0; layer < layers.size(); ++layer)
    {
      int const scaled = layers.at(layer, texel) * ALPHA_OPAQUE;

      quotient[layer] = scaled / sum;
      remainder[layer] = scaled % sum;
      assigned += quotient[layer];
    }

    for (int unit = ALPHA_OPAQUE - assigned; unit > 0; --unit)
    {
      std::size_t best = layers.size();

      for (std::size_t layer = 0; layer < layers.size(); ++layer)
      {
        // Strict >, so a tie goes to the lower layer index and the result is deterministic.
        if (remainder[layer] > 0 && (best == layers.size() || remainder[layer] > remainder[best]))
        {
          best = layer;
        }
      }

      if (best == layers.size())
      {
        break;
      }

      ++quotient[best];
      remainder[best] = 0;
    }

    for (std::size_t layer = 0; layer < layers.size(); ++layer)
    {
      layers.set(layer, texel, static_cast<std::uint8_t>(quotient[layer]));
    }
  }

  // Restores the duplicated border of one layer, counting only writes that changed a byte.
  //
  // Alphamap.cpp:125-131 runs its loop to 63 and then assigns the corner again, so three of its
  // writes land on texel (63,63) with the same value. Stopping the loop one short and doing the
  // corner once produces the identical final state -- every source it reads is outside the range
  // the loop writes -- while touching each border texel exactly once, which is what makes the
  // count meaningful and the operation idempotent.
  std::size_t restoreBorder(AlphaLayerStack& layers, std::size_t layer)
  {
    std::size_t repaired = 0;

    auto const copy_texel
    (
      [&] (std::size_t target, std::size_t source)
      {
        std::uint8_t const value = layers.at(layer, source);

        if (layers.at(layer, target) != value)
        {
          layers.set(layer, target, value);
          ++repaired;
        }
      }
    );

    for (std::size_t i = 0; i < LAST; ++i)
    {
      copy_texel(alphaTexel(i, LAST), alphaTexel(i, PENULTIMATE));
      copy_texel(alphaTexel(LAST, i), alphaTexel(PENULTIMATE, i));
    }

    copy_texel(alphaTexel(LAST, LAST), alphaTexel(PENULTIMATE, PENULTIMATE));

    return repaired;
  }

  AlphaBorderMismatch checkBorder(AlphaLayerStack const& layers, std::size_t layer)
  {
    AlphaBorderMismatch mismatch;
    mismatch.layer = layer;

    auto const compare
    (
      [&] (std::size_t texel, std::size_t neighbour)
      {
        int const delta = static_cast<int>(layers.at(layer, texel))
                        - static_cast<int>(layers.at(layer, neighbour));

        if (delta == 0)
        {
          return false;
        }

        mismatch.first_texel = std::min(mismatch.first_texel, texel);
        mismatch.max_delta = std::max(mismatch.max_delta, std::abs(delta));
        return true;
      }
    );

    for (std::size_t row = 0; row < LAST; ++row)
    {
      if (compare(alphaTexel(row, LAST), alphaTexel(row, PENULTIMATE)))
      {
        ++mismatch.last_column_texels;
      }
    }

    for (std::size_t column = 0; column < LAST; ++column)
    {
      if (compare(alphaTexel(LAST, column), alphaTexel(PENULTIMATE, column)))
      {
        ++mismatch.last_row_texels;
      }
    }

    mismatch.corner = compare(alphaTexel(LAST, LAST), alphaTexel(PENULTIMATE, PENULTIMATE));

    return mismatch;
  }

  std::vector<std::size_t> emptyLayersOf(AlphaLayerStack const& layers)
  {
    std::vector<std::size_t> empty;

    for (std::size_t layer = 0; layer < layers.size(); ++layer)
    {
      if (layers.layerIsEmpty(layer))
      {
        empty.push_back(layer);
      }
    }

    return empty;
  }
}

std::uint8_t Noggit::alphaFromFloat(float value)
{
  // Written as a negated comparison so NaN takes this branch: every comparison against NaN is
  // false, and a NaN reaching the narrowing cast below would be undefined behaviour.
  if (!(value > 0.f))
  {
    return 0;
  }

  if (value >= static_cast<float>(ALPHA_OPAQUE))
  {
    return static_cast<std::uint8_t>(ALPHA_OPAQUE);
  }

  return static_cast<std::uint8_t>(std::lround(value));
}

bool AlphaLayerStack::addLayer(AlphaLayer const& layer)
{
  if (_layers.size() >= MAX_ALPHA_LAYERS)
  {
    return false;
  }

  _layers.push_back(layer);
  return true;
}

bool AlphaLayerStack::addLayer(std::uint8_t const* data)
{
  if (!data || _layers.size() >= MAX_ALPHA_LAYERS)
  {
    return false;
  }

  AlphaLayer layer;
  std::copy(data, data + ALPHA_TEXELS, layer.begin());
  _layers.push_back(layer);
  return true;
}

bool AlphaLayerStack::addLayerFromFloats(float const* data)
{
  if (!data || _layers.size() >= MAX_ALPHA_LAYERS)
  {
    return false;
  }

  AlphaLayer layer;

  for (std::size_t texel = 0; texel < ALPHA_TEXELS; ++texel)
  {
    layer[texel] = alphaFromFloat(data[texel]);
  }

  _layers.push_back(layer);
  return true;
}

bool AlphaLayerStack::addZeroLayer()
{
  if (_layers.size() >= MAX_ALPHA_LAYERS)
  {
    return false;
  }

  _layers.emplace_back();
  _layers.back().fill(0);
  return true;
}

std::size_t AlphaLayerStack::size() const
{
  return _layers.size();
}

bool AlphaLayerStack::empty() const
{
  return _layers.empty();
}

AlphaLayer const& AlphaLayerStack::layer(std::size_t layer_index) const
{
  return _layers.at(layer_index);
}

AlphaLayer& AlphaLayerStack::layer(std::size_t layer_index)
{
  return _layers.at(layer_index);
}

std::uint8_t AlphaLayerStack::at(std::size_t layer_index, std::size_t texel) const
{
  return _layers.at(layer_index).at(texel);
}

void AlphaLayerStack::set(std::size_t layer_index, std::size_t texel, std::uint8_t value)
{
  _layers.at(layer_index).at(texel) = value;
}

int AlphaLayerStack::sumAt(std::size_t texel) const
{
  int sum = 0;

  for (AlphaLayer const& layer : _layers)
  {
    sum += layer.at(texel);
  }

  return sum;
}

int AlphaLayerStack::baseWeightAt(std::size_t texel) const
{
  return ALPHA_OPAQUE - sumAt(texel);
}

bool AlphaLayerStack::layerIsEmpty(std::size_t layer_index) const
{
  AlphaLayer const& values = _layers.at(layer_index);
  return std::all_of(values.begin(), values.end(), [] (std::uint8_t v) { return v == 0; });
}

bool AlphaLayerStack::operator==(AlphaLayerStack const& other) const
{
  return _layers == other._layers;
}

bool AlphaLayerStack::operator!=(AlphaLayerStack const& other) const
{
  return !(*this == other);
}

std::string_view Noggit::alphaDefectName(AlphaDefect defect)
{
  switch (defect)
  {
    case AlphaDefect::OverOpaque:     return "over-opaque";
    case AlphaDefect::EmptyLayer:     return "empty layer";
    case AlphaDefect::BorderMismatch: return "border mismatch";
  }

  return "unknown";
}

AlphaIntegrityOptions AlphaIntegrityOptions::forChunk(bool uses_big_alphamaps, bool do_not_fix_alpha_map)
{
  AlphaIntegrityOptions options;
  // Big alpha and compressed MCAL both carry 4096 real bytes; only the 4-bit path duplicates, and
  // only when the chunk did not ask to be left alone.
  options.check_border = !uses_big_alphamaps && !do_not_fix_alpha_map;
  return options;
}

std::size_t AlphaBorderMismatch::total() const
{
  return last_column_texels + last_row_texels + (corner ? 1u : 0u);
}

bool AlphaIntegrityReport::clean() const
{
  return over_opaque_texels == 0 && empty_layers.empty() && border_mismatches.empty();
}

bool AlphaIntegrityReport::has(AlphaDefect defect) const
{
  switch (defect)
  {
    case AlphaDefect::OverOpaque:     return over_opaque_texels != 0;
    case AlphaDefect::EmptyLayer:     return !empty_layers.empty();
    case AlphaDefect::BorderMismatch: return !border_mismatches.empty();
  }

  return false;
}

std::size_t AlphaIntegrityReport::borderMismatchTexels() const
{
  std::size_t total = 0;

  for (AlphaBorderMismatch const& mismatch : border_mismatches)
  {
    total += mismatch.total();
  }

  return total;
}

std::string AlphaIntegrityReport::summary() const
{
  std::ostringstream out;
  out << plural(layer_count, "alpha layer", "alpha layers") << ": ";

  if (clean())
  {
    out << "clean";
    return out.str();
  }

  bool first = true;

  auto const separate
  (
    [&]
    {
      if (!first)
      {
        out << ", ";
      }

      first = false;
    }
  );

  if (over_opaque_texels != 0)
  {
    separate();
    out << plural(over_opaque_texels, "over-opaque texel", "over-opaque texels")
        << " (max +" << max_excess << ")";
  }

  if (!empty_layers.empty())
  {
    separate();
    out << plural(empty_layers.size(), "empty layer", "empty layers");
  }

  if (!border_mismatches.empty())
  {
    separate();
    out << plural(borderMismatchTexels(), "border texel", "border texels") << " unduplicated";
  }

  return out.str();
}

std::string AlphaIntegrityReport::toReport() const
{
  std::ostringstream out;
  out << "Alpha map integrity\n";
  out << "  " << summary() << "\n";

  if (over_opaque_texels != 0)
  {
    out << "  over-opaque: " << over_opaque_texels << " texel(s), worst excess " << max_excess
        << ", first at row " << alphaRow(first_over_opaque_texel)
        << " column " << alphaColumn(first_over_opaque_texel) << "\n";
    // The consequence, spelled out, because "sum > 255" does not obviously mean "a texture is
    // missing from the ground": layer 0 is what is left of 255 (texture_set.cpp:187).
    out << "    layer 0 cannot show at those texels\n";
  }

  for (std::size_t layer : empty_layers)
  {
    out << "  layer " << (layer + 1) << " is empty: paints nothing, costs an MCLY entry and its"
        << " texture reference\n";
  }

  for (AlphaBorderMismatch const& mismatch : border_mismatches)
  {
    out << "  layer " << (mismatch.layer + 1) << " border: "
        << mismatch.last_column_texels << " in the last column, "
        << mismatch.last_row_texels << " in the last row"
        << (mismatch.corner ? ", plus the corner" : "")
        << "; worst delta " << mismatch.max_delta
        << ", first at row " << alphaRow(mismatch.first_texel)
        << " column " << alphaColumn(mismatch.first_texel) << "\n";
  }

  return out.str();
}

AlphaIntegrityReport Noggit::checkAlphaIntegrity( AlphaLayerStack const& layers
                                                , AlphaIntegrityOptions const& options
                                                )
{
  AlphaIntegrityReport report;
  report.layer_count = layers.size();

  if (layers.empty())
  {
    return report;
  }

  if (options.check_overflow)
  {
    for (std::size_t texel = 0; texel < ALPHA_TEXELS; ++texel)
    {
      int const excess = layers.sumAt(texel) - ALPHA_OPAQUE;

      if (excess <= 0)
      {
        continue;
      }

      if (report.over_opaque_texels == 0)
      {
        report.first_over_opaque_texel = texel;
      }

      ++report.over_opaque_texels;
      report.max_excess = std::max(report.max_excess, excess);
    }
  }

  if (options.check_empty_layers)
  {
    report.empty_layers = emptyLayersOf(layers);
  }

  if (options.check_border)
  {
    for (std::size_t layer = 0; layer < layers.size(); ++layer)
    {
      AlphaBorderMismatch const mismatch = checkBorder(layers, layer);

      if (mismatch.total() != 0)
      {
        report.border_mismatches.push_back(mismatch);
      }
    }
  }

  return report;
}

bool AlphaRepairResult::changed() const
{
  return over_opaque_texels_repaired != 0 || border_texels_repaired != 0;
}

std::string AlphaRepairResult::summary() const
{
  if (!changed())
  {
    return "no repair needed";
  }

  std::ostringstream out;
  out << "repaired " << plural(over_opaque_texels_repaired, "over-opaque texel", "over-opaque texels")
      << " and " << plural(border_texels_repaired, "border texel", "border texels");

  if (!layers_to_drop.empty())
  {
    out << "; " << plural(layers_to_drop.size(), "layer", "layers") << " now empty and removable";
  }

  return out.str();
}

AlphaRepairResult Noggit::repairAlphaIntegrity( AlphaLayerStack const& layers
                                              , AlphaIntegrityOptions const& options
                                              , AlphaRepairOptions const& repair
                                              )
{
  AlphaRepairResult result;
  result.layers = layers;

  if (layers.empty())
  {
    return result;
  }

  if (repair.repair_over_opacity)
  {
    for (std::size_t texel = 0; texel < ALPHA_TEXELS; ++texel)
    {
      int const sum = result.layers.sumAt(texel);

      if (sum <= ALPHA_OPAQUE)
      {
        continue;
      }

      if (repair.over_opacity_strategy == OverOpacityRepair::ScaleProportionally)
      {
        scaleProportionally(result.layers, texel, sum);
      }
      else
      {
        clipUpperLayers(result.layers, texel, sum - ALPHA_OPAQUE);
      }

      ++result.over_opaque_texels_repaired;
    }
  }

  // options.check_border is the authority on whether the border is a duplicate at all; the repair
  // flag can only decline a repair that would otherwise be legitimate, never authorise one that
  // would overwrite real data.
  if (repair.repair_border && options.check_border)
  {
    for (std::size_t layer = 0; layer < result.layers.size(); ++layer)
    {
      result.border_texels_repaired += restoreBorder(result.layers, layer);
    }
  }

  result.layers_to_drop = emptyLayersOf(result.layers);

  return result;
}

std::string AlphaChunkAddress::label() const
{
  std::ostringstream out;
  out << "ADT " << tile_x << "," << tile_z << " chunk " << chunk_x << "," << chunk_z;
  return out.str();
}

bool AlphaChunkAddress::operator==(AlphaChunkAddress const& other) const
{
  return tile_x == other.tile_x && tile_z == other.tile_z
      && chunk_x == other.chunk_x && chunk_z == other.chunk_z;
}

void AlphaScanResult::addChunk( AlphaChunkAddress const& address
                              , AlphaIntegrityReport const& report
                              , std::size_t max_offenders
                              )
{
  ++_chunks_scanned;

  if (report.clean())
  {
    return;
  }

  ++_chunks_with_defects;

  if (report.has(AlphaDefect::OverOpaque))
  {
    ++_chunks_over_opaque;
    _over_opaque_texels += report.over_opaque_texels;
    _max_excess = std::max(_max_excess, report.max_excess);
  }

  if (report.has(AlphaDefect::EmptyLayer))
  {
    ++_chunks_with_empty_layers;
    _empty_layers += report.empty_layers.size();
  }

  if (report.has(AlphaDefect::BorderMismatch))
  {
    ++_chunks_with_border_mismatch;
    _border_mismatch_texels += report.borderMismatchTexels();
  }

  if (_offenders.size() < max_offenders)
  {
    _offenders.push_back(address);
  }
  else
  {
    ++_offenders_omitted;
  }
}

std::size_t AlphaScanResult::chunksScanned() const { return _chunks_scanned; }
std::size_t AlphaScanResult::chunksWithDefects() const { return _chunks_with_defects; }

std::size_t AlphaScanResult::chunksWith(AlphaDefect defect) const
{
  switch (defect)
  {
    case AlphaDefect::OverOpaque:     return _chunks_over_opaque;
    case AlphaDefect::EmptyLayer:     return _chunks_with_empty_layers;
    case AlphaDefect::BorderMismatch: return _chunks_with_border_mismatch;
  }

  return 0;
}

std::size_t AlphaScanResult::overOpaqueTexels() const { return _over_opaque_texels; }
std::size_t AlphaScanResult::borderMismatchTexels() const { return _border_mismatch_texels; }
std::size_t AlphaScanResult::emptyLayerCount() const { return _empty_layers; }
int AlphaScanResult::maxExcess() const { return _max_excess; }
std::vector<AlphaChunkAddress> const& AlphaScanResult::offenders() const { return _offenders; }
std::size_t AlphaScanResult::offendersOmitted() const { return _offenders_omitted; }
bool AlphaScanResult::clean() const { return _chunks_with_defects == 0; }

void AlphaScanResult::merge(AlphaScanResult const& other, std::size_t max_offenders)
{
  _chunks_scanned += other._chunks_scanned;
  _chunks_with_defects += other._chunks_with_defects;
  _chunks_over_opaque += other._chunks_over_opaque;
  _chunks_with_empty_layers += other._chunks_with_empty_layers;
  _chunks_with_border_mismatch += other._chunks_with_border_mismatch;
  _over_opaque_texels += other._over_opaque_texels;
  _border_mismatch_texels += other._border_mismatch_texels;
  _empty_layers += other._empty_layers;
  _max_excess = std::max(_max_excess, other._max_excess);
  _offenders_omitted += other._offenders_omitted;

  for (AlphaChunkAddress const& address : other._offenders)
  {
    if (_offenders.size() < max_offenders)
    {
      _offenders.push_back(address);
    }
    else
    {
      ++_offenders_omitted;
    }
  }
}

std::string AlphaScanResult::summary() const
{
  std::ostringstream out;
  out << plural(_chunks_scanned, "chunk", "chunks") << " scanned, ";

  if (clean())
  {
    out << "no alpha map defects";
    return out.str();
  }

  out << _chunks_with_defects << " with defects: "
      << _chunks_over_opaque << " over-opaque, "
      << _chunks_with_empty_layers << " with empty layers, "
      << _chunks_with_border_mismatch << " with an unduplicated border";

  return out.str();
}

std::string AlphaScanResult::toReport() const
{
  std::ostringstream out;
  out << "Alpha map integrity scan\n";
  out << "  " << summary() << "\n";

  if (clean())
  {
    return out.str();
  }

  if (_chunks_over_opaque != 0)
  {
    out << "  over-opaque: " << _over_opaque_texels << " texel(s) across " << _chunks_over_opaque
        << " chunk(s), worst excess " << _max_excess << "\n";
  }

  if (_chunks_with_empty_layers != 0)
  {
    out << "  empty layers: " << _empty_layers << " across " << _chunks_with_empty_layers
        << " chunk(s)\n";
  }

  if (_chunks_with_border_mismatch != 0)
  {
    out << "  unduplicated border: " << _border_mismatch_texels << " texel(s) across "
        << _chunks_with_border_mismatch << " chunk(s)\n";
  }

  for (AlphaChunkAddress const& address : _offenders)
  {
    out << "    " << address.label() << "\n";
  }

  if (_offenders_omitted != 0)
  {
    out << "    ... and " << _offenders_omitted << " more\n";
  }

  return out.str();
}
