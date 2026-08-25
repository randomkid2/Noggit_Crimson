// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include "Vector3Widget.hpp"
#include <noggit/MapHeaders.h>
#include <noggit/ui/DesignTokens.hpp>

#include <QLabel>
#include <QDoubleSpinBox>
#include <QHBoxLayout>

namespace
{
  // The three axis chips.
  //
  // THE DEFECT THIS REPLACES, measured rather than asserted: the labels were
  //     "QLabel { background-color : red; color : white; }"
  // and the same with `green` and `blue`. Those are Qt colour NAMES, i.e. pure #FF0000,
  // #008000 and #0000FF, and white text on pure red measures 3.99:1 -- under the 4.5:1 floor
  // and under it in the one place the value being typed is identified. Pure green managed
  // 5.14:1 and pure blue 8.59:1, so the three chips were also wildly unequal in weight: the X
  // chip screamed and failed, the Z chip receded and passed.
  //
  // The replacement is the design system's status triad with INK on it, which is the same
  // treatment every other filled token surface in the application gets:
  //
  //     X  BAD  #E86F62  ink 6.31:1
  //     Y  OK   #4FB87E  ink 7.79:1
  //     Z  INFO #6FAEDC  ink 8.04:1
  //
  // All three clear 4.5:1, they sit within 1.27:1 of each other so no axis shouts, and the
  // hues are 106.5 and 165.0 degrees apart from the accent, so an axis chip can never be
  // mistaken for "the thing you are acting on".
  //
  // A widget-level style sheet is the correct mechanism here and not a theme leak: the chip
  // colour is per-axis identity, the sheet has no selector that can tell the three apart, and
  // a sheet set on the widget itself is what guarantees the axis reads the same whether a
  // theme is loaded or not. RADIUS_INDICATOR rather than RADIUS_CONTROL because the chip is
  // 14px wide and a 5px radius on a 14px box eats most of the edge.
  QString axisChipStyle (char const* fill)
  {
    return QStringLiteral ("QLabel { background-color: %1; color: %2; border-radius: %3px; }")
      .arg (QString::fromLatin1 (fill))
      .arg (QString::fromLatin1 (Noggit::Ui::Design::INK))
      .arg (Noggit::Ui::Design::RADIUS_INDICATOR);
  }
}

namespace Noggit::Ui
{
    Vector3fWidget::Vector3fWidget(QWidget* parent)
        : QWidget(parent)
    {
        // The gap between one axis unit and the next. S3 is the design system's default
        // separation between two sibling controls, and is what this row already used.
        constexpr int spacing = Design::S3;

        _layout = new QHBoxLayout(this);
        _layout->setContentsMargins(0, 0, 0, 0);
        _layout->setSpacing(0);

        _xLabel = new QLabel("X", this);
        _xLabel->setStyleSheet(axisChipStyle(Design::BAD));
        _xLabel->setMaximumWidth(14);
        _xLabel->setAlignment(Qt::AlignCenter);
        _layout->addWidget(_xLabel);

        _xSpinbox = new QDoubleSpinBox(this);
        _xSpinbox->setMinimum(0);
        _xSpinbox->setMaximum(ZEROPOINT * 2);
        connect(_xSpinbox, &QDoubleSpinBox::textChanged, [=](auto) {
            emit valueChanged({
                static_cast<float>(_xSpinbox->value()),
                static_cast<float>(_ySpinbox->value()),
                static_cast<float>(_zSpinbox->value()) });
            });
        _layout->addWidget(_xSpinbox);
        _layout->addSpacing(spacing);

        _yLabel = new QLabel("Y", this);
        _yLabel->setStyleSheet(axisChipStyle(Design::OK));
        _yLabel->setMaximumWidth(14);
        _yLabel->setAlignment(Qt::AlignCenter);
        _layout->addWidget(_yLabel);

        _ySpinbox = new QDoubleSpinBox(this);
        _ySpinbox->setMinimum(0);
        _ySpinbox->setMaximum(ZEROPOINT * 2);
        connect(_ySpinbox, &QDoubleSpinBox::textChanged, [=](auto) {
            emit valueChanged({
                static_cast<float>(_xSpinbox->value()),
                static_cast<float>(_ySpinbox->value()),
                static_cast<float>(_zSpinbox->value()) });
            });
        _layout->addWidget(_ySpinbox);
        _layout->addSpacing(spacing);

        _zLabel = new QLabel("Z", this);
        _zLabel->setStyleSheet(axisChipStyle(Design::INFO));
        _zLabel->setMaximumWidth(14);
        _zLabel->setAlignment(Qt::AlignCenter);
        _layout->addWidget(_zLabel);

        _zSpinbox = new QDoubleSpinBox(this);
        _zSpinbox->setMinimum(0);
        _zSpinbox->setMaximum(ZEROPOINT * 2);
        connect(_zSpinbox, &QDoubleSpinBox::textChanged, [=](auto) {
            emit valueChanged({
                static_cast<float>(_xSpinbox->value()),
                static_cast<float>(_ySpinbox->value()),
                static_cast<float>(_zSpinbox->value()) });
            });
        _layout->addWidget(_zSpinbox);
    }

    void Vector3fWidget::clear()
    {
        _xSpinbox->clear();
        _ySpinbox->clear();
        _zSpinbox->clear();
    }

    void Vector3fWidget::setValue(float value[3])
    {
        _xSpinbox->setValue(value[0]);
        _ySpinbox->setValue(value[1]);
        _zSpinbox->setValue(value[2]);
    }
}
