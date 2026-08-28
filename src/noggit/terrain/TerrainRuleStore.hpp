// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINRULESTORE_HPP
#define NOGGIT_TERRAINRULESTORE_HPP

#include <noggit/terrain/TerrainRules.hpp>

#include <cstddef>
#include <vector>

namespace Noggit
{
  // The one home for the rule set the user authored, and for the Live Auto Texture switch.
  //
  // WHY THIS EXISTS. Until now the rules lived in AutoTextureDialog::_rules and died with the
  // dialog. That was sufficient while the dialog was the only thing that could paint from them.
  // Live Auto Texture adds a second reader -- the terrain stroke, which has no dialog and no
  // MapView-independent place to look -- and two readers of one rule set means the rule set has to
  // outlive both. A second copy of the rules, edited somewhere else, is exactly the failure this
  // avoids: the user would preview one set and the stroke would paint another.
  //
  // PROCESS-WIDE, not per MapView. A rule set is a description of a WORLD ("rock above 60
  // degrees"), not of a map, and the alternative -- hanging it off MapView -- would silently reset
  // it every time the user went back to the menu and reopened a map, which is the commonest way to
  // switch between two ADTs while building the same rule set.
  //
  // WHAT PERSISTS, and the two halves deliberately differ:
  //
  //   - The RULES persist to QSettings. Retyping eight rules at the start of every session is pure
  //     friction and there is nothing dangerous about remembering them; nothing is painted until
  //     something asks.
  //   - The LIVE SWITCH does NOT persist. It is off at every application start, always. Live Auto
  //     Texture overwrites hand-painted alpha as a side effect of an unrelated gesture, and a
  //     destructive mode that silently survives a restart is the kind of thing a user discovers
  //     three hours into a shoreline. Re-arming it costs one click in a dialog the user has to open
  //     anyway to check the rules are still the ones they want.
  //
  // NOT a QObject, and there is no change signal. There is exactly one writer (the Automatic
  // Texturing dialog, of which one exists at a time) and one reader per direction, and the reader
  // asks at the moment it needs an answer -- the stroke hook reads the rules when a stroke ends,
  // the dialog reads the switch when it is constructed. Nothing has to be told. Inheriting QObject
  // for signals nobody connects would add a moc pass and an API that looks live and is not; adding
  // one later, if a second reader ever appears, breaks no caller.
  class TerrainRuleStore
  {
    public:
      static TerrainRuleStore* instance();

      // The rules as authored, in list order. TerrainRuleSet's precedence is content-based, so this
      // order is presentational; see TerrainRuleSet::precedenceOrder.
      std::vector<TerrainRule> const& rules() const;

      // Replaces the whole list and writes it to QSettings, but only when the new list actually
      // differs from the old one. The dialog calls this on every path that can touch a rule,
      // including the ones that turn out not to have, and rewriting the settings on every keystroke
      // would put a registry write behind every arrow key in a spin box.
      void setRules(std::vector<TerrainRule> rules);

      // A TerrainRuleSet built from rules(). Rebuilt on demand rather than cached, matching what
      // AutoTextureDialog already does and for the same reason: the set recomputes its precedence
      // order on every mutation, and the list is edited keystroke by keystroke.
      TerrainRuleSet ruleSet() const;

      // False at every application start. See the note above the class.
      bool liveAutoEnabled() const;
      void setLiveAutoEnabled(bool enabled);

      // True when a live pass would have something to do: the switch is on, there is at least one
      // enabled rule, and the set carries no problem that would block Apply. Asked by the stroke
      // hook before it does any work at all.
      //
      // The "no catch-all" complaint is treated as advisory here exactly as AutoTextureDialog
      // treats it, because a rule set that paints only cliffs is a legitimate thing to want and
      // necessarily has no catch-all.
      bool liveAutoRunnable() const;

    private:
      TerrainRuleStore();

      void load();
      void save() const;

      std::vector<TerrainRule> _rules;

      // Not written to QSettings and not read from it. Deliberate; see the class note.
      bool _live_auto_enabled = false;
  };
}

#endif // NOGGIT_TERRAINRULESTORE_HPP
