// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainRuleStore.hpp>

#include <QtCore/QSettings>
#include <QtCore/QString>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
  // QSettings array name and key names. Spelled out once so the reader and the writer cannot
  // disagree about them.
  constexpr char const* RULES_ARRAY = "auto_texture/rules";

  // Exact text of the one TerrainRuleSet::validate() complaint that is advisory rather than fatal
  // (TerrainRules.cpp:449), repeated from AutoTextureDialog.cpp because the two have to agree about
  // which problems block a paint. Matched by value because TerrainRules.hpp states the wording is
  // stable enough to assert on.
  constexpr char const* NO_CATCH_ALL_PROBLEM
    = "no catch-all rule: points matching nothing are left untouched";

  // Endpoint equality that counts two NaNs as the same endpoint.
  //
  // A NaN endpoint is a real state of a TerrainRange -- it constrains and it matches nothing
  // (TerrainRules.cpp:54) -- and `a != b` is TRUE for a NaN against itself, so a plain comparison
  // would report an unchanged rule list as changed on every commit and re-save it every keystroke.
  bool sameEndpoint(float lhs, float rhs)
  {
    return (std::isnan(lhs) && std::isnan(rhs)) || lhs == rhs;
  }

  bool sameRange(Noggit::TerrainRange const& lhs, Noggit::TerrainRange const& rhs)
  {
    return sameEndpoint(lhs.min, rhs.min) && sameEndpoint(lhs.max, rhs.max);
  }

  bool sameRule(Noggit::TerrainRule const& lhs, Noggit::TerrainRule const& rhs)
  {
    return lhs.texture == rhs.texture
        && sameRange(lhs.slope, rhs.slope)
        && sameRange(lhs.height, rhs.height)
        && lhs.strength == rhs.strength
        && lhs.priority == rhs.priority
        && lhs.enabled == rhs.enabled;
  }

  bool sameRules(std::vector<Noggit::TerrainRule> const& lhs, std::vector<Noggit::TerrainRule> const& rhs)
  {
    if (lhs.size() != rhs.size())
    {
      return false;
    }

    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
      if (!sameRule(lhs[i], rhs[i]))
      {
        return false;
      }
    }

    return true;
  }

  // An endpoint is stored as a BOUNDED FLAG plus a value, never as the value alone.
  //
  // TerrainRange spells "no constraint" with an infinity, and QSettings round-trips a float through
  // QVariant and then through an INI or registry string. Neither storage has a portable spelling for
  // an infinity: the registry backend writes it as a double whose text form is platform-dependent,
  // and the INI backend writes "inf", which QVariant::toFloat parses back as 0. A rule saved as
  // "slope >= no minimum" would reload as "slope >= 0", which is a different rule that matches
  // everything -- silently, at the next application start.
  void writeRange( QSettings& settings
                 , QString const& prefix
                 , Noggit::TerrainRange const& range
                 )
  {
    settings.setValue(prefix + "_min_bounded", range.boundedBelow());
    settings.setValue(prefix + "_max_bounded", range.boundedAbove());

    // isfinite rather than boundedBelow alone: a NaN endpoint CONSTRAINS as far as TerrainRange is
    // concerned, and writing it would come back as 0 on the next load and turn a rule that matches
    // nothing into a rule that matches half the map. A NaN endpoint is dropped to its infinity
    // instead, which is the only lossy case here and the only one where losing the value is safer
    // than keeping it.
    settings.setValue
      (prefix + "_min", std::isfinite(range.min) ? range.min : 0.0f);
    settings.setValue
      (prefix + "_max", std::isfinite(range.max) ? range.max : 0.0f);
  }

  Noggit::TerrainRange readRange(QSettings const& settings, QString const& prefix)
  {
    // Default-constructed, i.e. infinite in both directions; an absent or unset flag leaves the
    // endpoint unconstrained rather than substituting the stored number.
    Noggit::TerrainRange range;

    if (settings.value(prefix + "_min_bounded", false).toBool())
    {
      range.min = settings.value(prefix + "_min", 0.0f).toFloat();
    }

    if (settings.value(prefix + "_max_bounded", false).toBool())
    {
      range.max = settings.value(prefix + "_max", 0.0f).toFloat();
    }

    return range;
  }
}

namespace Noggit
{
  TerrainRuleStore::TerrainRuleStore()
  {
    load();
  }

  TerrainRuleStore* TerrainRuleStore::instance()
  {
    // Function-local static rather than a file-scope one: QSettings is read in the constructor and
    // a file-scope instance would run that before QCoreApplication had set the organisation and
    // application names, against which QSettings resolves its default scope. The rules would be
    // read from -- and later written to -- a different place than every other setting in the
    // editor.
    static TerrainRuleStore store;
    return &store;
  }

  std::vector<TerrainRule> const& TerrainRuleStore::rules() const
  {
    return _rules;
  }

  void TerrainRuleStore::setRules(std::vector<TerrainRule> rules)
  {
    if (sameRules(_rules, rules))
    {
      return;
    }

    _rules = std::move(rules);
    save();
  }

  TerrainRuleSet TerrainRuleStore::ruleSet() const
  {
    return TerrainRuleSet(_rules);
  }

  bool TerrainRuleStore::liveAutoEnabled() const
  {
    return _live_auto_enabled;
  }

  void TerrainRuleStore::setLiveAutoEnabled(bool enabled)
  {
    // Nothing else to do: the flag is not persisted and nothing is notified. See the class note for
    // why both of those are on purpose.
    _live_auto_enabled = enabled;
  }

  bool TerrainRuleStore::liveAutoRunnable() const
  {
    if (!_live_auto_enabled || _rules.empty())
    {
      return false;
    }

    bool any_enabled = false;

    for (TerrainRule const& rule : _rules)
    {
      any_enabled = any_enabled || rule.enabled;
    }

    if (!any_enabled)
    {
      return false;
    }

    // Built here rather than held, and this is the reason the check is not free: validate() is the
    // same gate AutoTextureDialog::onApply re-runs rather than trusting its own button state, and a
    // live pass has no button to trust at all.
    TerrainRuleSet const set (ruleSet());

    for (std::string const& problem : set.validate())
    {
      if (problem != NO_CATCH_ALL_PROBLEM)
      {
        return false;
      }
    }

    return true;
  }

  void TerrainRuleStore::load()
  {
    QSettings settings;

    _rules.clear();

    int const count = settings.beginReadArray(RULES_ARRAY);

    for (int i = 0; i < count; ++i)
    {
      settings.setArrayIndex(i);

      TerrainRule rule;
      rule.texture = settings.value("texture").toString().toStdString();
      rule.height = readRange(settings, "height");
      rule.slope = readRange(settings, "slope");
      rule.priority = settings.value("priority", 0).toInt();

      // Clamped rather than trusted. strength is a std::uint8_t written straight into an alphamap
      // texel, and QSettings hands back whatever is in the file -- including a number a hand-edited
      // INI put there. An out-of-range int narrowed by a plain cast would wrap, so 300 would become
      // 44 and a rule the user believes paints at full strength would paint at a sixth of it.
      // static_cast<int> on the default, not TERRAIN_ALPHA_MAX itself: it is a std::uint8_t, and
      // QVariant has constructors taking int, uint and bool, none of which an unsigned char matches
      // exactly -- the call is ambiguous rather than merely surprising.
      int const strength
        = settings.value("strength", static_cast<int>(TERRAIN_ALPHA_MAX)).toInt();
      rule.strength = static_cast<std::uint8_t>
        (strength < 0 ? 0 : (strength > TERRAIN_ALPHA_MAX ? TERRAIN_ALPHA_MAX : strength));

      rule.enabled = settings.value("enabled", true).toBool();

      _rules.push_back(std::move(rule));
    }

    settings.endArray();
  }

  void TerrainRuleStore::save() const
  {
    QSettings settings;

    // remove() before beginWriteArray, because beginWriteArray only overwrites the indices it
    // writes and leaves any higher ones in place. Without it, shrinking a list from eight rules to
    // three leaves rules 3..7 in the file, and the next load reads eight rules back -- five of them
    // deleted ones.
    settings.remove(RULES_ARRAY);

    settings.beginWriteArray(RULES_ARRAY, static_cast<int>(_rules.size()));

    for (std::size_t i = 0; i < _rules.size(); ++i)
    {
      settings.setArrayIndex(static_cast<int>(i));

      TerrainRule const& rule = _rules[i];

      settings.setValue("texture", QString::fromStdString(rule.texture));
      writeRange(settings, "height", rule.height);
      writeRange(settings, "slope", rule.slope);
      settings.setValue("priority", rule.priority);
      settings.setValue("strength", static_cast<int>(rule.strength));
      settings.setValue("enabled", rule.enabled);
    }

    settings.endArray();
  }
}
