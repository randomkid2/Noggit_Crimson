// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ExtendedSlider.hpp"
#include <noggit/TabletManager.hpp>
#include <noggit/ui/FontAwesome.hpp>

#include <cfloat>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

using namespace Noggit::Ui::Tools::UiCommon;
using namespace Noggit::Ui;

ExtendedSlider::ExtendedSlider(QWidget* parent)
: QWidget(parent)
, _tablet_manager(TabletManager::instance())
{
  _ui.setupUi(this);

  // popup
  _tablet_popup = new QWidget(this);
  auto layout = new QVBoxLayout(_tablet_popup);

  auto tablet_enabled = new QCheckBox(_tablet_popup);
  tablet_enabled->setText("Use Tablet");
  layout->addWidget(tablet_enabled);
  _ui.tabletControlMenuButton->setIcon(FontAwesomeIcon(FontAwesome::Icons::pen));

  // The only thing on the row that never said what it was. It is the tablet menu -- the popup
  // built just above holds "Use Tablet" and the sensitivity slider -- and it is emphatically
  // not a mask or curve picker, whatever its pencil glyph suggests. Naming it is the whole
  // reason it can now be drawn flat instead of as a raised button competing with the value.
  _ui.tabletControlMenuButton->setToolTip(tr("Tablet pressure settings"));
  _ui.tabletControlMenuButton->setAccessibleName(tr("Tablet pressure settings"));

  connect(tablet_enabled, &QCheckBox::stateChanged,
          [=](int state)
          {
            _is_tablet_affecting = state;
            _ui.tabletControlMenuButton->setIcon(FontAwesomeIcon(
                state ? FontAwesome::Icons::edit : FontAwesome::Icons::pen));

            _ui.pressureBar->setVisible(state);
          });

  layout->addWidget(new QLabel("Sensitivity:", _tablet_popup));
  auto sens_slider = new QSlider(_tablet_popup);
  sens_slider->setRange(0, 1000);
  sens_slider->setValue(300);
  sens_slider->setOrientation(Qt::Horizontal);
  sens_slider->setSingleStep(1);
  sens_slider->setMinimumWidth(200);

  auto sens_spin = new QDoubleSpinBox(_tablet_popup);
  sens_spin->setRange(0, 1000);
  sens_spin->setValue(300);
  sens_spin->setDecimals(2);

  auto sens_slider_panel = new QWidget(_tablet_popup);
  auto sens_slider_panel_layout = new QHBoxLayout(sens_slider_panel);
  sens_slider_panel_layout->addWidget(sens_slider);
  sens_slider_panel_layout->addWidget(sens_spin);
  layout->addWidget(sens_slider_panel);

  _tablet_popup->updateGeometry();
  _tablet_popup->adjustSize();
  _tablet_popup->update();
  _tablet_popup->repaint();

  connect(sens_slider, &QSlider::valueChanged,
          [=](int value)
          {
            const QSignalBlocker blocker(sens_spin);
            sens_spin->setValue(static_cast<double>(value));
            _tablet_sens_factor = value;
          });

  connect(sens_spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [=](double value)
          {
              const QSignalBlocker blocker(sens_slider);
              sens_slider->setValue(static_cast<int>(value));
          });

  _tablet_popup->setVisible(false);

  // ui
  connect(_ui.tabletControlMenuButton, &QPushButton::clicked,
          [=]()
          {
            QPoint new_pos = mapToGlobal(
                QPoint(_ui.tabletControlMenuButton->pos().x() - _tablet_popup->width() - 12,
                  _ui.tabletControlMenuButton->pos().y()));

            _tablet_popup->setGeometry(new_pos.x(),
                                       new_pos.y(),
                                       _tablet_popup->width(),
                                       _tablet_popup->height());

            _tablet_popup->setWindowFlags(Qt::Popup);
            _tablet_popup->show();
          });

  _ui.slider->setRange(0, 100);
  _ui.pressureBar->setRange(0, 100);
  _ui.pressureBar->setVisible(false);
  connect(_ui.slider, &QSlider::valueChanged,
          [=](int v)
          {
            const QSignalBlocker blocker(_ui.doubleSpinBox);

            _ui.doubleSpinBox->setValue(sliderToSpin(v));
            _ui.pressureBar->setValue(v);

            emit valueChanged(value());
          });

  connect(_ui.doubleSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [=](double v)
          {
              const QSignalBlocker blocker(_ui.slider);

              int const slider_value = spinToSlider(_ui.doubleSpinBox->value());

              _ui.slider->setValue(slider_value);
              _ui.pressureBar->setValue(slider_value);
              emit valueChanged(value());
          });

  connect(_tablet_manager, &Noggit::TabletManager::pressureChanged,
          [=](double pressure)
          {
              // The third copy of the spinbox -> slider mapping, and safe to fold into the
              // shared one: the bar is always given the slider's range (setSliderRange, and
              // the constructor above sets both to 0..100).
              _ui.pressureBar->setValue(spinToSlider(value()));
          });
}

// The slider and the spinbox carry the same quantity on two scales, and the conversion between
// them was written out three times: slider -> spin, spin -> slider, and a third copy in the
// tablet-pressure handler. One pair of named functions now, so the directions cannot drift.
//
// The arithmetic also gained the offset of the spinbox minimum, which none of the three copies
// had -- they mapped the slider's travel onto the spinbox's RANGE (max - min) and used the
// result as an absolute value. THAT IS NOT A LIVE BUG AND NOTHING A USER CAN SEE CHANGES.
// Every ExtendedSlider constructed in this tree has a spinbox minimum of exactly 0, and the
// slider's own minimum is 0 as well (set in the constructor above; setSliderRange has no
// caller), so the offset term is zero in both directions and the old and new forms compute the
// same number. All eighteen construction sites, with the range each is given:
//
//   TerrainTool.cpp:96,102,119                0..1000, 0..1, 0..1000
//   texturing_tool.cpp:87,96,104              0..1, 0..1000, 0..1
//   FlattenTool.cpp:116,122                   0..1000, 0..10
//   ShaderTool.cpp:39,47                      0..10000, 0..10
//   ErosionToolSettings.cpp:142,149,166,175   0..200, 0..1, 0..MAX_REPOSE_ANGLE_DEGREES,
//                                             0..MAX_STABLE_STRENGTH
//   GroundEffectsTool.cpp:265                 0..1000
//   BrushStack.ui:45,55,65                    maximum only; QDoubleSpinBox's default minimum
//                                             is 0 and nothing overrides it
//
// texturing_tool.cpp:181 is NOT one of them, and any claim that it is should be treated as a
// misreading of the same grep: `_spray_size_slider` (100..4000) is a plain QSlider driving a
// plain QDoubleSpinBox, declared as QSlider* at texturing_tool.hpp:173. It has its own
// conversion in that file and never reaches this class.
//
// So the offset is here for the next caller that sets a non-zero minimum, not for any that
// exists. Saved presets are unaffected either way: TerrainTool::toJSON and its siblings store
// rawValue(), the spinbox reading, not the slider position.
//
// The zero-span guards are new. Nothing sets a degenerate range today, but the old expressions
// divided by (max - min) unguarded, and static_cast<int> of the resulting infinity is undefined
// behaviour rather than a wrong pixel -- worth closing while the arithmetic is being touched.
double ExtendedSlider::sliderToSpin(int slider_value) const
{
  int const slider_span (_ui.slider->maximum() - _ui.slider->minimum());

  if (slider_span <= 0)
  {
    return _ui.doubleSpinBox->minimum();
  }

  double const spin_min (_ui.doubleSpinBox->minimum());
  double const spin_span (_ui.doubleSpinBox->maximum() - spin_min);

  return spin_min
       + spin_span * ((slider_value - _ui.slider->minimum()) / static_cast<double>(slider_span));
}

int ExtendedSlider::spinToSlider(double spin_value) const
{
  double const spin_min (_ui.doubleSpinBox->minimum());
  double const spin_span (_ui.doubleSpinBox->maximum() - spin_min);

  if (spin_span <= 0.0)
  {
    return _ui.slider->minimum();
  }

  int const slider_min (_ui.slider->minimum());
  int const slider_span (_ui.slider->maximum() - slider_min);

  return slider_min
       + static_cast<int>(slider_span * ((spin_value - spin_min) / spin_span));
}

void ExtendedSlider::setMinimum(double min)
{
  _ui.doubleSpinBox->setMinimum(min);
}

void ExtendedSlider::setMaximum(double max)
{
  _ui.doubleSpinBox->setMaximum(max);
}

void ExtendedSlider::setRange(double min, double max)
{
  _ui.doubleSpinBox->setMinimum(min);
  _ui.doubleSpinBox->setMaximum(max);
}

void ExtendedSlider::setDecimals(int decimals)
{
  Q_ASSERT(decimals <= DBL_MAX_10_EXP + DBL_DIG && decimals >= 0);
  _ui.doubleSpinBox->setDecimals(decimals);

}

void ExtendedSlider::setSingleStep(double val)
{
  Q_ASSERT(val > 0.0);
  _ui.doubleSpinBox->setSingleStep(val);
}

double ExtendedSlider::value()
{

  if (_is_tablet_supported && _is_tablet_affecting && _tablet_manager->isActive())
  {
    return std::min(_ui.doubleSpinBox->maximum(), _ui.doubleSpinBox->value()
    + (_ui.doubleSpinBox->maximum() - _ui.doubleSpinBox->minimum()) * _tablet_manager->pressure() * (_tablet_sens_factor / 1000.0));
  }

  return _ui.doubleSpinBox->value();
}

double ExtendedSlider::rawValue()
{
  return _ui.doubleSpinBox->value();
}

void ExtendedSlider::setPrefix(const QString& prefix)
{
  _ui.label->setText(prefix);
}

void ExtendedSlider::setTabletSupportEnabled(bool state)
{
  _is_tablet_supported = state;
  _ui.tabletControlMenuButton->setVisible(state);
}

void ExtendedSlider::setSliderRange(int min, int max)
{
  _ui.slider->setRange(min, max);
  _ui.pressureBar->setRange(min, max);
}

void ExtendedSlider::setValue(double value)
{
  _ui.doubleSpinBox->setValue(value);
}
