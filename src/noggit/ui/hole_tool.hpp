// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_HOLE_TOOL_HPP
#define NOGGIT_UI_HOLE_TOOL_HPP

#include <QtWidgets/QWidget>

namespace Noggit
{
  namespace Ui::Tools::UiCommon
  {
    class ExtendedSlider;
  }

  namespace Ui
  {
    class hole_tool : public QWidget
    {
    Q_OBJECT

    public:
      hole_tool(QWidget* parent = nullptr);

      void changeRadius(float change);

      float brushRadius() const;

      void setRadius(float radius);

    private:

      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _radius_slider;

      float _radius = 15.0f;

    };
  }
}

#endif // NOGGIT_UI_HOLE_TOOL_HPP
