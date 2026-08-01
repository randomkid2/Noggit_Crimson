// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/Toolbar.h>
#include <noggit/Tool.hpp>
#include <noggit/ui/FontNoggit.hpp>

namespace
{
  // The strip is icon-only, and hovering a tool used to show nothing but its bare name -- Qt
  // returns QAction::text() from toolTip() when no explicit tooltip is set. Nine of the sixteen
  // tools also have a number key, but that binding goes through MapView::addHotkey, a private
  // table rather than QAction::setShortcut, so Qt has no way to discover it and append it
  // itself. The only place it was written down was one line of prose in the Help window.
  //
  // THIS TABLE IS A COPY, and MapView owns the original (the MOD_none Key_1..Key_9 block in
  // MapView::setupHotkeys). It is deliberately only a tooltip: registering these as real
  // QAction shortcuts would put two independent handlers on the same key, and the MapView
  // binding carries a guard the QAction cannot express -- it declines to fire while an edit
  // action is open. Displaying the key costs nothing and cannot change what any key does; if a
  // binding is ever moved in MapView, the worst this can do is show a stale hint.
  QString toolShortcutHint (editing_mode mode)
  {
    switch (mode)
    {
      case editing_mode::ground:       return "1";
      case editing_mode::flatten_blur: return "2";
      case editing_mode::paint:        return "3";
      case editing_mode::holes:        return "4";
      case editing_mode::areaid:       return "5";
      case editing_mode::impass:       return "6";
      case editing_mode::water:        return "7";
      case editing_mode::mccv:         return "8";
      case editing_mode::object:       return "9";
      // minimap, stamp, light, scripting, chunk, area_trigger and erosion have no key bound.
      default:                         return QString();
    }
  }
}

namespace Noggit
{
  namespace Ui
  {
    toolbar::toolbar(std::vector<std::unique_ptr<Noggit::Tool>> const& tools, std::function<void (editing_mode)> set_editing_mode)
      : _set_editing_mode (set_editing_mode)
      , _tool_group(this)
    {
      setContextMenuPolicy(Qt::PreventContextMenu);
      setAllowedAreas(Qt::LeftToolBarArea);
      setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);

      for (auto&& tool : tools)
      {
          add_tool_icon(tool->editingMode(), tr(tool->name()), tool->icon());
      }
    }

    void toolbar::add_tool_icon(editing_mode mode, const QString& name, const FontNoggit::Icons& icon)
    {
      auto action = addAction(FontNoggitIcon{icon}, name);

      // Rich text so the name reads as a heading and the key as a subordinate hint. Qt applies
      // the tooltip palette to both, so no colour is hardcoded here and the theme stays in
      // charge. The name is escaped because it goes through tr() and a translator could
      // legitimately return a character that means something in markup.
      QString const shortcut (toolShortcutHint (mode));

      action->setToolTip
        ( shortcut.isEmpty()
        ? QString ("<b>%1</b>").arg (name.toHtmlEscaped())
        : QString ("<b>%1</b><br/><small>Shortcut: %2</small>")
            .arg (name.toHtmlEscaped(), shortcut)
        );

      connect (action, &QAction::triggered, [this, mode] () {
        _set_editing_mode (mode);
      });
      action->setActionGroup(&_tool_group);
      action->setCheckable(true);

      _tool_actions[mode] = action;
    }

    void toolbar::check_tool(editing_mode mode)
    {
      if (auto itr = _tool_actions.find(mode); itr != _tool_actions.end())
      {
        itr->second->setChecked(true);
      }
    }
  }
}
