// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Tool.hpp>
#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>

#include "ToolPanel.hpp"

using namespace Noggit::Ui::Tools;

namespace
{
  // The width every tool widget in this dock has to lay out inside now lives in
  // ToolWidgetStyle.hpp, because the tool widgets themselves need it: it is the floor
  // toolColumn/toolForm pin on each of them. ToolPanelScroll.ui does NOT repeat that floor --
  // its scroll area asks for 274px, the 250px floor plus the 12px of side margin the form adds
  // on each side, which is exactly what the paragraph below describes. The extra allowance here
  // is the vertical scroll bar, which the tools never get to use because the viewport is inside
  // it.
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
  // THE SECOND INSET IS NOW GONE, and this note records what it was so the measurement is not
  // lost with it. The tool widgets inside this panel used to supply an inset of their own, and
  // they did not agree on it:
  //
  //     QStyle::PM_LayoutLeftMargin (windowsvista, this machine)  13px
  //     texturing_tool / TerrainTool, which pinned their own       9px
  //
  // So the dock's contents were never flush against its edge the way a reading of the form
  // alone suggests -- they sat 21px or 25px in once this panel's own 12px was added, depending
  // on which tool was showing, and the left edge of the content MOVED by 4px when the user
  // pressed a key to change tool. ToolWidgetStyle.hpp closes that for the eleven tool widgets it
  // reaches -- of the fifteen registered, which its own header block lists -- each of which now
  // takes its column from toolColumn() or toolForm(), both of which zero the contents margins.
  // For those eleven this panel is the only thing that insets a tool and the total is the flat
  // 12px the spacing scale describes; the remaining four still carry whatever the style hands
  // them.
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
  setMinimumWidth
    (ToolPanelStyle::TOOL_CONTENT_WIDTH + PANEL_SIDE_MARGINS + SCROLL_BAR_ALLOWANCE);
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
