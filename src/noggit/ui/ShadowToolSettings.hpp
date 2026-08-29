// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_SHADOWTOOLSETTINGS_HPP
#define NOGGIT_UI_SHADOWTOOLSETTINGS_HPP

#include <noggit/rendering/ShadowBaker.hpp>

#include <QtWidgets/QWidget>

class MapView;

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace Noggit::Ui::Tools::UiCommon
{
  class ExtendedSlider;
}

namespace Noggit
{
  namespace Ui
  {
    // The settings panel for ShadowTool: the brush radius, the sun, and the bake.
    //
    // Shaped after ErosionToolSettings -- the Tool owns the widget, the widget owns the
    // parameters AND performs the operation. The bake in particular belongs here rather than in
    // the Tool, because the one thing it absolutely must not be is reachable from the Tool's
    // render or tick hooks: those run inside paintGL (MapView.cpp:5071, :5086) and this ends in a
    // multi-megabyte glReadPixels and then puts text on screen. A QPushButton::clicked handler is
    // delivered by the Qt event loop with no paint in progress, which is the property that makes
    // the whole thing safe.
    class ShadowToolSettings : public QWidget
    {
    public:
      ShadowToolSettings(MapView* map_view);

      void changeRadius(float change);

      [[nodiscard]]
      float brushRadius() const;

      QSize sizeHint() const override;

    private:
      // Reads the controls into a settings block. Already sanitized, so callers never handle a
      // half-validated one.
      [[nodiscard]]
      Noggit::Rendering::ShadowBakeSettings bakeSettings() const;

      // Renders the sun depth pass and thresholds it into the tile under the camera. The whole
      // GPU half is confined to this function.
      void bakeCurrentTile();

      // Erases MCSH across the tile under the camera, in one undo step. The counterpart to the
      // bake, and the only way back to an unshadowed tile short of 256 ctrl-Zs.
      void clearCurrentTile();

      void setStatus(QString const& text);

      MapView* _map_view;

      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _radius_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _sun_pitch_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _sun_yaw_slider;
      Noggit::Ui::Tools::UiCommon::ExtendedSlider* _bias_slider;
      QSpinBox* _threshold_spin;
      QSpinBox* _supersample_spin;
      QSpinBox* _caster_margin_spin;
      QComboBox* _resolution_combo;
      QCheckBox* _include_models_check;
      QCheckBox* _include_wmos_check;
      QPushButton* _bake_button;
      QPushButton* _clear_button;
      QLabel* _status_label;
    };
  }
}

#endif // NOGGIT_UI_SHADOWTOOLSETTINGS_HPP
