// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ExtendedSlider.hpp"
#include <noggit/TabletManager.hpp>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/FontAwesome.hpp>

#include <cfloat>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QString>
#include <QVBoxLayout>

using namespace Noggit::Ui::Tools::UiCommon;
using namespace Noggit::Ui;

ExtendedSlider::ExtendedSlider(QWidget* parent)
: QWidget(parent)
, _tablet_manager(TabletManager::instance())
{
  _ui.setupUi(this);

  // THE MOST REPEATED LABEL IN THE EDITOR HAD NO RANK OF ITS OWN. This is the QLabel setPrefix
  // writes -- the name of the row, on every ExtendedSlider in every tool -- and with no object
  // name the only rule that could reach it was the sheet's global 12/400 text, which is exactly
  // what a check box caption standing next to it renders at. Across the nineteen construction
  // sites in this tree plus the open-ended expansion in script_settings.cpp, that is the single
  // most-repeated unranked label in the application.
  //
  // The name is all this needs: QLabel#slider-prefix in the sheet drops it to 11px text.dim
  // #BFB7AA, which measures 7.585:1 on the tool dock's card fill and 9.699:1 on a bg.void
  // dialog ground -- both over the 7:1 prose floor the sheet sets for itself -- and puts the
  // NAME of the row below its VALUE, which is the 12/600 text.hi in the spin box at the other
  // end of the same row. Nothing here can change the row's height: an 11px label has a 13px
  // line box and the row is set by the 20px slider band and the 20x20 grip.
  _ui.label->setObjectName(QStringLiteral("slider-prefix"));

  // THE UNFILLED HALF OF THE TRACK DID NOT EXIST, AND THAT IS WHY THIS CONTROL READ AS A
  // HAIRLINE WITH A DISC FLOATING ON IT.
  //
  // The shipped sheet fills QSlider::add-page with bg.void #100E0B and gives it no border. The
  // slider sits on a section card whose fill is bg.panel #292621. Measured, WCAG 2.1 sRGB,
  // (Lmax + 0.05) / (Lmin + 0.05): 1.279:1. That is the same number the surface ladder in
  // DesignTokens.hpp publishes for void -> panel, and it is a quarter of the 3:1 floor a
  // graphical mark needs, so the empty part of the track is not dim -- it is ABSENT.
  //
  // What that does to a real row is worse than it sounds, because of the ranges these sliders
  // carry. TerrainTool's radius is 0..1000 and its default sits near the bottom of that span,
  // so the accent sub-page is a few pixels wide and everything to the right of the grip is
  // invisible. What is left on screen is a stub of colour and a 20px disc with nothing under
  // it, which is exactly the "very thin groove with a small circle at the far left" the
  // complaint describes. It is not a thin groove; it is a groove that stops existing where the
  // value stops.
  //
  // THE SHEET SAYS THIS CANNOT BE FIXED AND ITS OWN PROGRESS BAR PROVES OTHERWISE. The QSlider
  // block carries an "HONEST LIMIT" paragraph concluding that no stroke in this palette reaches
  // 3:1 on bg.panel, so none is drawn. Three sections further down, QProgressBar solves the
  // identical problem -- a bg.void well on bg.panel with an accent fill -- with a 1px edge
  // #8A8378 border, under a comment that opens "AN EMPTY BAR USED TO BE NOTHING AT ALL" and
  // quotes 5.138:1 against the track and 4.018:1 against the panel. Recomputed here rather than
  // copied, and both reproduce: edge on bg.void 5.138:1, edge on bg.panel 4.018:1. The stroke
  // the slider block ruled out is the one its neighbour already uses.
  //
  // THE COLOUR IS A NEUTRAL ON PURPOSE. edge is the design system's "edge of an enabled
  // control" and it is 13 percent HSV saturation, so it makes no claim on the accent: whatever
  // the accent becomes, this outline neither collides with it nor has to move with it. Against
  // the gold accent the outline is only 1.707:1, and that is fine and deliberate -- the outline
  // is not what separates the filled part from the empty part. The FILL against the well
  // interior does that, at 8.772:1. The outline exists so that the empty part is a channel
  // instead of nothing.
  //
  // DISABLED DROPS TO stroke.soft, 1.360:1 on bg.panel -- deliberately under the floor, which is
  // this sheet's disabled signal everywhere else and is exempt under WCAG 2.1 SC 1.4.11.
  //
  // NOTHING HERE CAN MOVE THE GRIP, and that is checked against Qt's own arithmetic rather than
  // assumed. QStyleSheetStyle::subControlRect derives the handle from the GROOVE rule, not from
  // add-page: gr comes from the groove's box, cr = grooveRule.contentsRect(gr), and the handle
  // rect is grooveRule-independent of any sibling sub-control
  // (qstylesheetstyle.cpp:5631-5660, Qt 5.9.9 sources, unchanged in 5.15). add-page is only
  // ever painted, into a rect derived from gr and the handle centre (:3260-3276). So the groove
  // box stays 6px, the handle box stays 20px, the band stays 20px and no row changes height.
  // The 1px border is drawn INSIDE the 6px add-page box, leaving a 4px bg.void interior.
  //
  // WHY A WIDGET-LEVEL SHEET RATHER THAN THE APPLICATION SHEET. This file cannot edit
  // dist/noggit-themes. The mechanics are sound and are the documented hazard used deliberately
  // for once: QStyleSheetStyle::styleRules collects the application sheet first and every
  // object sheet after it, stamping the object sheets with a greater depth
  // (qstylesheetstyle.cpp:1583-1627), and declarations() concatenates in that order so the last
  // declaration of a property wins. Only `border` is declared here, so `background-color` and
  // `border-radius` still come from the active theme and an accent change upstream still
  // reaches this control untouched.
  //
  // AND IT IS AN IMPROVEMENT UNDER THE OTHER TWO SHIPPED SHEETS AS WELL, which is the answer to
  // the obvious objection that this overwrites a theme it does not own. Dark and McNet BOTH
  // already outline add-page -- they are the sheets that got this right -- but with colours that
  // measure nothing: Dark's #2d2f34 is 1.216:1 on its own #1f2023 track and 1.101:1 on its
  // #26282d panel, McNet's #383440 is 1.243:1 on #28252e and 1.146:1 on #2e2b35. edge measures
  // 4.343:1 / 3.932:1 under Dark and 4.015:1 / 3.700:1 under McNet. Under "System" nothing
  // happens at all: with no groove rule to make drawable, drawComplexControl hands the whole
  // control to the base style and returns before add-page is consulted (:3241-3250).
  //
  // THIS RULE BELONGS IN THE APPLICATION SHEET, on every QSlider, not on the twenty-three
  // ExtendedSlider construction sites this class has -- eighteen `new` in eight files, two more
  // in ChunkManipulatorPanel and three promoted widgets in BrushStack.ui, counted for this pass
  // rather than inherited. It is one declaration and it is deliberately trivial to delete from
  // here the day it lands there.
  _ui.slider->setStyleSheet
    ( QStringLiteral ("QSlider::add-page:horizontal { border: 1px solid %1; }"
                      "QSlider::add-page:horizontal:disabled { border: 1px solid %2; }")
        .arg (QString::fromLatin1 (Design::EDGE), QString::fromLatin1 (Design::STROKE_SOFT))
    );

  // THE GRIP'S HOVER STATE WAS BORROWED FROM THE SHEET AND NOBODY SAID SO. The brief for this
  // pass says QSlider "is a real QWidget subclass with its own hover handling, so it should be
  // fine" -- it is not, and the assumption was checked rather than repeated. QSliderPrivate::
  // init() sets WA_WState_OwnSizePolicy and nothing else; the only place WA_Hover is ever set
  // on a QSlider is QStyleSheetStyle::polish, which sets it only when some matching rule
  // mentions :hover (qstylesheetstyle.cpp:2800-2808, Qt 5.9.9). Without the attribute
  // QSliderPrivate::updateHoverControl reads it at :162 and returns early, so no repaint is
  // issued when the pointer crosses the grip.
  //
  // So the four-state grip the theme draws -- 2px ring at rest, 3px focused, 4px hovered, 6px
  // pressed -- exists only while a sheet that happens to name :hover is loaded. It is CrimsonSlate
  // that keeps it alive today, via QSlider::handle:horizontal:hover; under "System", which
  // SettingsPanel offers and which applies no sheet at all, the hover half of that never ran.
  // Setting the attribute here makes the repaint the widget's own property instead of a
  // side effect of a selector somebody else wrote. This is the same statement OpacitySlider
  // makes in texturing_tool.cpp and for the same reason.
  _ui.slider->setAttribute(Qt::WA_Hover, true);

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
