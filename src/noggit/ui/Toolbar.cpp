// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/Toolbar.h>
#include <noggit/Tool.hpp>
#include <noggit/ui/FontNoggit.hpp>

#include <QtCore/QSize>

namespace
{
  // Kept beside the ViewToolbar's own TOOLBAR_ICON_EXTENT rather than shared through a header:
  // the two bars are separate widgets with separate sheets, and a single constant would imply a
  // coupling that does not exist. They agree at 20 because the design system gives both the same
  // 34x34 button.
  constexpr int TOOL_STRIP_ICON_EXTENT = 20;

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

      // The sixteen glyphs on the left strip, which sits over the 3D view for the whole session
      // and is therefore the first thing anyone judges this editor by.
      //
      // This was never set anywhere -- not here, not in MapView, and not in the theme, which
      // gives qproperty-iconSize to the window controls and the dock buttons but to no tool bar.
      // The size fell through to QStyle::PM_ToolBarIconSize, a platform default that knows
      // nothing about these buttons.
      //
      // MEASURED, not assumed, with a standalone Qt 5.15.2 probe on this machine -- and the
      // number is much worse than the 24 that would be a reasonable guess:
      //
      //     style                 windowsvista
      //     PM_ToolBarIconSize    36
      //     QToolBar::iconSize()  36x36, both with and without the CrimsonSlate sheet applied
      //
      // The design system asks for a 20px icon in a 34x34 button and the sheet's rule leaves a
      // content box of roughly 22px, so every glyph on this strip was being rasterised at 36px
      // and then resampled down by Qt to about 22 -- a 61% reduction. These are
      // FontNoggitIconEngine icons and the engine rasterises the glyph at exactly rect.height(),
      // so this number IS the glyph's real pixel size and not a hint: asking for 20 draws at 20,
      // one device pixel per pixel, instead of drawing at 36 and softening it on the way down.
      // That resample is a large part of why the chrome read as muddy.
      //
      // It does not touch the BUTTON box. That is min-width/min-height in the sheet, which
      // overrides setFixedSize from C++ and is deliberately left to the sheet.
      setIconSize(QSize(TOOL_STRIP_ICON_EXTENT, TOOL_STRIP_ICON_EXTENT));

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
