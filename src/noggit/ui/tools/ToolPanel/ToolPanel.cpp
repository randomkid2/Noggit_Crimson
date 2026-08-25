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
}

ToolPanel::ToolPanel(QWidget* parent)
  : QDockWidget(parent)
{
  auto body = new QWidget(this);
  _ui.setupUi(body);
  setWidget(body);
  setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  layout()->setAlignment(Qt::AlignTop);
  setMinimumWidth(TOOL_CONTENT_WIDTH + SCROLL_BAR_ALLOWANCE);
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
