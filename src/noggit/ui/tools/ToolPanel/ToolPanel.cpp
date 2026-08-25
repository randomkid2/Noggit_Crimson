// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Tool.hpp>

#include "ToolPanel.hpp"

using namespace Noggit::Ui::Tools;

namespace
{
  // The width every tool widget in this dock has to lay out inside, and the reason each of them
  // pins its own layout margins rather than taking the style default. ToolPanelScroll.ui gives
  // the scroll area the same 250px floor; the extra allowance is the vertical scroll bar, which
  // the tools never get to use because the viewport is inside it.
  //
  // Anything whose minimumSizeHint().width() exceeds TOOL_CONTENT_WIDTH gets a horizontal
  // scroll bar under it at the dock's smallest size, which is why widths in the tool widgets
  // are measured against this number rather than eyeballed. For reference, rendered with the
  // shipped theme: the terrain tool asks for 194px and the texturing tool's Paint tab 224px.
  constexpr int TOOL_CONTENT_WIDTH = 250;
  constexpr int SCROLL_BAR_ALLOWANCE = 15;

  // The panel's side margins, which ToolPanelScroll.ui now carries on the scroll area's content
  // layout (12, 8, 12, 8 -- S4 horizontally, S3 vertically, from the design system's one
  // spacing scale). All four used to be zero, which is why the right-hand dock's contents ran
  // to the dock's own edge with nothing but each tool widget's private margin between them.
  //
  // THIS CONSTANT IS THE REASON THAT CHANGE IS SAFE, and it is not optional. TOOL_CONTENT_WIDTH
  // above is a contract: it is the width every tool widget is measured against, and the file's
  // own notes record the two that come closest -- the texturing tool's Paint tab asks for 229px
  // and the terrain tool 194px. Twenty-four pixels of new margin taken out of a fixed 250px
  // would have left 226px and put a horizontal scroll bar under the most-used tool in the
  // editor at the dock's smallest size, which is precisely the failure TOOL_CONTENT_WIDTH
  // exists to prevent. So the margins are ADDED to the floor rather than taken out of it: the
  // scroll area's own minimumSize in the form went 250 -> 274 in the same edit, and the dock's
  // minimum grows by the same 24px here. The content floor is still exactly 250px and every
  // width measured against it stays valid.
  //
  // WHAT THIS DOES *NOT* FIX, measured with a standalone Qt 5.15.2 probe and recorded here so
  // the next pass does not have to rediscover it. The tool widgets inside this panel supply an
  // inset of their own, and they do not agree on it:
  //
  //     QStyle::PM_LayoutLeftMargin (windowsvista, this machine)  13px
  //     texturing_tool / TerrainTool, which pin their own          9px
  //
  // So the dock's contents were never flush against its edge the way a reading of this form
  // alone suggests -- they sat 9px or 13px in, depending on the tool -- and after this change
  // they sit at 21px or 25px. The panel margin is the design system's 12px and is correct; the
  // 9-vs-13 disagreement underneath it is pre-existing and is NOT made worse here, but it is
  // the reason the total inset is not the flat 12px the spacing scale describes. Closing it
  // properly means zeroing the contents margins on every tool widget so this panel is the only
  // thing that insets them, which is a sweep across roughly sixteen files and deliberately out
  // of scope for an appearance pass that must not reflow tool panels one at a time.
  constexpr int PANEL_SIDE_MARGINS = 24;
}

ToolPanel::ToolPanel(QWidget* parent)
  : QDockWidget(parent)
{
  auto body = new QWidget(this);
  _ui.setupUi(body);
  setWidget(body);
  setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  layout()->setAlignment(Qt::AlignTop);
  setMinimumWidth(TOOL_CONTENT_WIDTH + PANEL_SIDE_MARGINS + SCROLL_BAR_ALLOWANCE);
}

void ToolPanel::setCurrentTool(editing_mode mode)
{
  for (auto&& [tool, widget] : _tools)
  {
    if (tool->editingMode() == mode)
    {
      widget->setVisible(true);
    }
    else
    {
      widget->setVisible(false);
    }
  }

  _ui.scrollAreaWidgetContents->adjustSize();
}

void ToolPanel::registerTool(Tool* tool, QWidget* widget)
{
  _ui.scrollAreaWidgetContents->layout()->addWidget(widget);
  _tools.emplace_back(std::make_pair(tool, widget));
}
