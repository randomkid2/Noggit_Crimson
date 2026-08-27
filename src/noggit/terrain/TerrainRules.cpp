// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainRules.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>

namespace
{
  // 180 / pi. Written out because M_PI is not standard and MSVC only defines it behind
  // _USE_MATH_DEFINES, which would have to be set before every <cmath> in the TU.
  constexpr float RADIANS_TO_DEGREES = 57.2957795130823208768f;

  constexpr float INFINITY_F = std::numeric_limits<float>::infinity();

  std::string ruleLabel(std::size_t index)
  {
    return "rule " + std::to_string(index);
  }
}

namespace Noggit
{
  TerrainRange TerrainRange::any()
  {
    return TerrainRange{-INFINITY_F, INFINITY_F};
  }

  TerrainRange TerrainRange::atLeast(float min_value)
  {
    return TerrainRange{min_value, INFINITY_F};
  }

  TerrainRange TerrainRange::atMost(float max_value)
  {
    return TerrainRange{-INFINITY_F, max_value};
  }

  TerrainRange TerrainRange::between(float min_value, float max_value)
  {
    return TerrainRange{min_value, max_value};
  }

  bool TerrainRange::boundedBelow() const
  {
    // Tested against -infinity rather than with std::isfinite, so a NaN endpoint counts as a
    // constraint. isfinite(NaN) is false, which would silently promote a NaN bound to "no
    // constraint" -- the exact opposite of failing closed. As a constraint, NaN fails every
    // comparison in contains() and the rule matches nothing, which is visible and is what
    // validate() reports.
    return !(min == -INFINITY_F);
  }

  bool TerrainRange::boundedAbove() const
  {
    return !(max == INFINITY_F);
  }

  bool TerrainRange::bounded() const
  {
    return boundedBelow() || boundedAbove();
  }

  bool TerrainRange::empty() const
  {
    return min > max;
  }

  int TerrainRange::boundCount() const
  {
    return (boundedBelow() ? 1 : 0) + (boundedAbove() ? 1 : 0);
  }

  bool TerrainRange::contains(float value) const
  {
    if (empty())
    {
      return false;
    }

    bool const below = boundedBelow();
    bool const above = boundedAbove();

    // No constraint in either direction performs no test at all, so an unusable (NaN) sample
    // still matches a catch-all rule. See slopeDegreesFromNormal for where NaN comes from.
    if (!below && !above)
    {
      return true;
    }

    // Negated comparisons so NaN -- on either side -- falls out as "does not match".
    if (below && !(value >= min))
    {
      return false;
    }

    if (above && !(value <= max))
    {
      return false;
    }

    return true;
  }

  bool TerrainRule::matches(TerrainSample const& sample) const
  {
    return enabled
        && height.contains(sample.height)
        && slope.contains(sample.slope_degrees);
  }

  int TerrainRule::specificity() const
  {
    return height.boundCount() + slope.boundCount();
  }

  float slopeDegreesFromNormal(TerrainNormal const& normal)
  {
    float const length_squared = normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;

    if (!std::isfinite(length_squared) || length_squared <= 0.0f)
    {
      return std::numeric_limits<float>::quiet_NaN();
    }

    float const length = std::sqrt(length_squared);

    // std::abs on the Y component: see the header. Clamped because the division can land a hair
    // outside [0, 1] on a near-vertical normal, and std::acos of 1.0000001f is NaN.
    float cosine = std::abs(normal.y) / length;
    cosine = std::min(1.0f, std::max(0.0f, cosine));

    return std::acos(cosine) * RADIANS_TO_DEGREES;
  }

  float slopeDegreesFromGradient(float dh_dx, float dh_dz)
  {
    float const gradient_squared = dh_dx * dh_dx + dh_dz * dh_dz;

    if (std::isnan(gradient_squared))
    {
      return std::numeric_limits<float>::quiet_NaN();
    }

    // atan of +infinity is exactly pi/2, so an infinite gradient reads as a vertical wall rather
    // than as an error. That is the correct limit and needs no special case.
    return std::atan(std::sqrt(gradient_squared)) * RADIANS_TO_DEGREES;
  }

  TerrainNormal normalFromTriangle(TerrainVertex const& a, TerrainVertex const& b, TerrainVertex const& c)
  {
    float const abx = b.x - a.x;
    float const aby = b.height - a.height;
    float const abz = b.z - a.z;

    float const acx = c.x - a.x;
    float const acy = c.height - a.height;
    float const acz = c.z - a.z;

    float nx = aby * acz - abz * acy;
    float ny = abz * acx - abx * acz;
    float nz = abx * acy - aby * acx;

    float const length_squared = nx * nx + ny * ny + nz * nz;

    if (!std::isfinite(length_squared) || length_squared <= 0.0f)
    {
      // Zero area: collinear or coincident corners. No plane exists, so no normal is invented.
      return TerrainNormal{0.0f, 0.0f, 0.0f};
    }

    float const length = std::sqrt(length_squared);
    nx /= length;
    ny /= length;
    nz /= length;

    // Orient upward, which is what makes the result independent of the corner order.
    if (ny < 0.0f)
    {
      nx = -nx;
      ny = -ny;
      nz = -nz;
    }

    return TerrainNormal{nx, ny, nz};
  }

  float slopeDegreesFromTriangle(TerrainVertex const& a, TerrainVertex const& b, TerrainVertex const& c)
  {
    return slopeDegreesFromNormal(normalFromTriangle(a, b, c));
  }

  TerrainRuleSet::TerrainRuleSet(std::vector<TerrainRule> initial_rules)
    : _rules(std::move(initial_rules))
  {
    rebuildOrder();
  }

  std::size_t TerrainRuleSet::addRule(TerrainRule rule)
  {
    std::size_t const index = _rules.size();
    _rules.push_back(std::move(rule));
    rebuildOrder();
    return index;
  }

  void TerrainRuleSet::clear()
  {
    _rules.clear();
    _order.clear();
  }

  std::size_t TerrainRuleSet::size() const
  {
    return _rules.size();
  }

  bool TerrainRuleSet::empty() const
  {
    return _rules.empty();
  }

  TerrainRule const& TerrainRuleSet::rule(std::size_t index) const
  {
    return _rules.at(index);
  }

  std::vector<TerrainRule> const& TerrainRuleSet::rules() const
  {
    return _rules;
  }

  std::vector<std::size_t> const& TerrainRuleSet::precedenceOrder() const
  {
    return _order;
  }

  void TerrainRuleSet::rebuildOrder()
  {
    _order.resize(_rules.size());
    std::iota(_order.begin(), _order.end(), std::size_t{0});

    // std::sort, not stable_sort: the comparator is a strict total order on its own -- the final
    // key is the list index, which is unique -- so stability would add nothing but cost. The full
    // ordering is documented on precedenceOrder().
    std::sort( _order.begin()
             , _order.end()
             , [this] (std::size_t left, std::size_t right)
               {
                 TerrainRule const& a = _rules[left];
                 TerrainRule const& b = _rules[right];

                 if (a.priority != b.priority)
                 {
                   return a.priority > b.priority;
                 }

                 int const specificity_a = a.specificity();
                 int const specificity_b = b.specificity();

                 if (specificity_a != specificity_b)
                 {
                   return specificity_a > specificity_b;
                 }

                 if (a.strength != b.strength)
                 {
                   return a.strength > b.strength;
                 }

                 if (a.texture != b.texture)
                 {
                   return a.texture < b.texture;
                 }

                 return left < right;
               }
             );
  }

  TerrainRuleResult TerrainRuleSet::evaluate(TerrainSample const& sample) const
  {
    for (std::size_t index : _order)
    {
      TerrainRule const& candidate = _rules[index];

      if (!candidate.matches(sample))
      {
        continue;
      }

      TerrainRuleResult result;
      result.matched = true;
      result.rule_index = index;
      result.texture = candidate.texture;
      result.alpha = candidate.strength;
      return result;
    }

    return TerrainRuleResult{};
  }

  std::size_t TerrainRuleSet::evaluateRanked( TerrainSample const& sample
                                            , TerrainRuleResult* out
                                            , std::size_t max_results
                                            ) const
  {
    if (!out || max_results == 0)
    {
      return 0;
    }

    std::size_t written = 0;

    for (std::size_t index : _order)
    {
      if (written == max_results)
      {
        break;
      }

      TerrainRule const& candidate = _rules[index];

      if (!candidate.matches(sample))
      {
        continue;
      }

      TerrainRuleResult& result = out[written];
      result.matched = true;
      result.rule_index = index;
      result.texture = candidate.texture;
      result.alpha = candidate.strength;
      ++written;
    }

    return written;
  }

  std::size_t TerrainRuleSet::matchCount(TerrainSample const& sample) const
  {
    std::size_t count = 0;

    for (TerrainRule const& candidate : _rules)
    {
      if (candidate.matches(sample))
      {
        ++count;
      }
    }

    return count;
  }

  std::vector<std::string> TerrainRuleSet::distinctTextures() const
  {
    std::vector<std::string> textures;
    textures.reserve(_rules.size());

    for (TerrainRule const& candidate : _rules)
    {
      if (candidate.enabled)
      {
        textures.push_back(candidate.texture);
      }
    }

    std::sort(textures.begin(), textures.end());
    textures.erase(std::unique(textures.begin(), textures.end()), textures.end());

    return textures;
  }

  std::vector<std::string> TerrainRuleSet::validate() const
  {
    std::vector<std::string> problems;
    bool has_catch_all = false;

    for (std::size_t index = 0; index < _rules.size(); ++index)
    {
      TerrainRule const& candidate = _rules[index];

      if (!candidate.enabled)
      {
        // A disabled rule's numbers are not in effect, so complaining about them is noise.
        continue;
      }

      if (candidate.texture.empty())
      {
        problems.push_back(ruleLabel(index) + ": empty texture identifier");
      }

      if (candidate.height.empty())
      {
        problems.push_back(ruleLabel(index) + ": height range is inverted and can never match");
      }

      if (candidate.slope.empty())
      {
        problems.push_back(ruleLabel(index) + ": slope range is inverted and can never match");
      }

      if (std::isnan(candidate.height.min) || std::isnan(candidate.height.max))
      {
        problems.push_back(ruleLabel(index) + ": height range endpoint is not a number");
      }

      if (std::isnan(candidate.slope.min) || std::isnan(candidate.slope.max))
      {
        problems.push_back(ruleLabel(index) + ": slope range endpoint is not a number");
      }

      // Slope is steepness from horizontal and cannot leave [0, 90] (slopeDegreesFromNormal), so a
      // range wholly outside it is a unit mistake -- radians typed into a degree field, or a
      // negative lower bound copied from a signed convention.
      if (!candidate.slope.empty())
      {
        if (candidate.slope.boundedBelow() && candidate.slope.min > TERRAIN_SLOPE_MAX_DEGREES)
        {
          problems.push_back(ruleLabel(index) + ": slope range lies above 90 degrees and can never match");
        }

        if (candidate.slope.boundedAbove() && candidate.slope.max < TERRAIN_SLOPE_MIN_DEGREES)
        {
          problems.push_back(ruleLabel(index) + ": slope range lies below 0 degrees and can never match");
        }
      }

      if (candidate.strength == 0)
      {
        problems.push_back(ruleLabel(index) + ": blend strength is 0 and paints nothing");
      }

      // A catch-all needs a texture to fall back TO. An unbounded rule with no texture identifier
      // is already reported above and covers nothing, so counting it here would suppress the
      // warning that the set has no usable fallback -- hiding the second defect behind the first.
      if (!candidate.texture.empty() && !candidate.height.bounded() && !candidate.slope.bounded())
      {
        has_catch_all = true;
      }
    }

    if (!_rules.empty() && !has_catch_all)
    {
      problems.push_back("no catch-all rule: points matching nothing are left untouched");
    }

    return problems;
  }

  float TerrainCoverageEntry::averageAlpha() const
  {
    if (sample_count == 0)
    {
      return 0.0f;
    }

    return static_cast<float>(alpha_total) / static_cast<float>(sample_count);
  }

  void TerrainRuleCoverage::addSample(TerrainRuleResult const& result)
  {
    ++_samples;

    if (!result.matched)
    {
      ++_unmatched;
      return;
    }

    std::string key(result.texture);

    auto const position = std::lower_bound
      ( _entries.begin()
      , _entries.end()
      , key
      , [] (std::pair<std::string, Entry> const& entry, std::string const& value)
        {
          return entry.first < value;
        }
      );

    // Kept sorted by insertion so iteration is deterministic without depending on hashing.
    auto const target = (position != _entries.end() && position->first == key)
                      ? position
                      : _entries.insert(position, {std::move(key), Entry{}});

    ++target->second.sample_count;
    target->second.alpha_total += result.alpha;
    target->second.max_alpha = std::max(target->second.max_alpha, result.alpha);
  }

  std::size_t TerrainRuleCoverage::sampleCount() const
  {
    return _samples;
  }

  std::size_t TerrainRuleCoverage::matchedCount() const
  {
    return _samples - _unmatched;
  }

  std::size_t TerrainRuleCoverage::unmatchedCount() const
  {
    return _unmatched;
  }

  std::size_t TerrainRuleCoverage::distinctTextures() const
  {
    return _entries.size();
  }

  std::vector<TerrainCoverageEntry> TerrainRuleCoverage::entries() const
  {
    std::vector<TerrainCoverageEntry> out;
    out.reserve(_entries.size());

    for (auto const& entry : _entries)
    {
      TerrainCoverageEntry coverage;
      coverage.texture = entry.first;
      coverage.sample_count = entry.second.sample_count;
      coverage.alpha_total = entry.second.alpha_total;
      coverage.max_alpha = entry.second.max_alpha;
      out.push_back(std::move(coverage));
    }

    std::sort( out.begin()
             , out.end()
             , [] (TerrainCoverageEntry const& a, TerrainCoverageEntry const& b)
               {
                 if (a.sample_count != b.sample_count)
                 {
                   return a.sample_count > b.sample_count;
                 }

                 return a.texture < b.texture;
               }
             );

    return out;
  }

  std::vector<TerrainCoverageEntry> TerrainRuleCoverage::topEntries(std::size_t max_entries) const
  {
    std::vector<TerrainCoverageEntry> out = entries();

    if (out.size() > max_entries)
    {
      out.resize(max_entries);
    }

    return out;
  }

  void TerrainRuleCoverage::merge(TerrainRuleCoverage const& other)
  {
    _samples += other._samples;
    _unmatched += other._unmatched;

    for (auto const& entry : other._entries)
    {
      auto const position = std::lower_bound
        ( _entries.begin()
        , _entries.end()
        , entry.first
        , [] (std::pair<std::string, Entry> const& existing, std::string const& value)
          {
            return existing.first < value;
          }
        );

      auto const target = (position != _entries.end() && position->first == entry.first)
                        ? position
                        : _entries.insert(position, {entry.first, Entry{}});

      target->second.sample_count += entry.second.sample_count;
      target->second.alpha_total += entry.second.alpha_total;
      target->second.max_alpha = std::max(target->second.max_alpha, entry.second.max_alpha);
    }
  }

  std::string TerrainRuleCoverage::summary() const
  {
    return std::to_string(_samples) + " samples, "
         + std::to_string(matchedCount()) + " matched, "
         + std::to_string(_unmatched) + " unmatched, "
         + std::to_string(_entries.size()) + " textures";
  }
}
