// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/windows/noggitWindow/widgets/MapSelectionArt.hpp>

#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QSize>

#include <algorithm>
#include <cmath>


namespace Noggit::Ui::Widget::MapSelectionArt
{
  namespace
  {
    //! Stroke weight as a fraction of the mark's extent, so a 14px magnifier and a 64px pin
    //! carry the same visual weight rather than the same absolute one. 1/12 puts a 16px glyph on
    //! a 1.333px stroke and a 64px glyph on 5.333 -- which at this display's device pixel ratio
    //! of 2 is 2.67 and 10.67 real pixels, i.e. never the sub-pixel line that antialiases into
    //! grey mush.
    constexpr qreal STROKE_FRACTION = 1.0 / 12.0;

    //! Below this the fraction above would ask for a stroke thinner than a device pixel at
    //! ratio 1. Clamped rather than allowed, because a hairline that disappears on a
    //! non-HiDPI display is worse than one that is a touch heavy.
    constexpr qreal STROKE_MINIMUM = 1.0;

    QPixmap newLayer (int extent, qreal ratio)
    {
      QPixmap layer ( std::max (1, qRound (extent * ratio))
                    , std::max (1, qRound (extent * ratio))
                    );
      layer.setDevicePixelRatio (ratio);
      layer.fill (Qt::transparent);
      return layer;
    }

    qreal strokeFor (int extent)
    {
      return std::max (STROKE_MINIMUM, extent * STROKE_FRACTION);
    }

    QPen outlinePen (int extent, QColor const& ink)
    {
      QPen pen (ink);
      pen.setWidthF (strokeFor (extent));
      pen.setCapStyle (Qt::RoundCap);
      pen.setJoinStyle (Qt::RoundJoin);
      return pen;
    }

    void prepare (QPainter& painter)
    {
      painter.setRenderHint (QPainter::Antialiasing, true);
      painter.setRenderHint (QPainter::SmoothPixmapTransform, true);
    }
  }

  QPixmap magnifierGlyph (int extent, QColor const& ink, qreal ratio)
  {
    if (extent <= 0 || ratio <= 0.0)
    {
      return QPixmap();
    }

    QPixmap target (newLayer (extent, ratio));

    QPainter painter (&target);
    prepare (painter);
    painter.setPen (outlinePen (extent, ink));
    painter.setBrush (Qt::NoBrush);

    // EVERY FRACTION BELOW IS OF THE DRAWABLE BOX, which is the pixmap inset by half the pen
    // width on all four sides. Working in the inset box rather than subtracting the stroke from
    // each radius is what lets the proportions be stated as plain fractions and stay true at
    // every size, and it is the only way the round caps cannot be clipped by the pixmap edge.
    qreal const inset (strokeFor (extent) * 0.5);
    qreal const box (extent - (inset * 2.0));

    // The lens fills the top-left of the box: centred at 0.40, 0.40 with a radius of 0.34, so it
    // spans 0.06 to 0.74 and leaves the bottom-right quarter for the handle.
    qreal const radius (box * 0.34);
    QPointF const centre (inset + box * 0.40, inset + box * 0.40);

    painter.drawEllipse (centre, radius, radius);

    // The handle starts ON the rim rather than at the centre, at 45 degrees down-right, so the
    // join is a tangent meeting and not a chord crossing the lens. 45 is also the only diagonal
    // on which an antialiased stroke keeps an even weight. The rim point is
    // 0.40 + 0.34 x sqrt(0.5) = 0.6404 of the box in both axes, and the handle runs from there
    // to the bottom-right corner of the box.
    qreal const diagonal (std::sqrt (0.5));

    painter.drawLine
      ( QPointF (centre.x() + radius * diagonal, centre.y() + radius * diagonal)
      , QPointF (inset + box, inset + box)
      );

    painter.end();

    return target;
  }

  QPixmap mapPinGlyph (int extent, QColor const& ink, qreal ratio)
  {
    if (extent <= 0 || ratio <= 0.0)
    {
      return QPixmap();
    }

    QPixmap target (newLayer (extent, ratio));

    QPainter painter (&target);
    prepare (painter);
    painter.setPen (outlinePen (extent, ink));
    painter.setBrush (Qt::NoBrush);

    qreal const inset (strokeFor (extent) * 0.5);
    qreal const box (extent - (inset * 2.0));

    // THE TEARDROP, built from the tangents rather than from two hand-placed curves, so the
    // silhouette has no corner where the head meets the spike at any size.
    //
    // The head is a circle of radius r centred at c; the point is at distance d directly below
    // it. The two lines from the point that touch the circle leave the centre at
    // +/- acos(r / d) either side of straight down, so the arc that forms the head is everything
    // EXCEPT the 2 x acos(r / d) that the point subtends.
    //
    // r = 0.28 of the box and d = 0.54, so r / d = 0.518519 and acos(0.518519) = 58.7671
    // degrees: the head is a 242.4659-degree arc and the spike is 117.5341 degrees wide at the
    // centre. Qt measures arc angles from three o'clock with positive counter-clockwise, and
    // straight down is 270, so the arc runs from 270 + 58.7671 = 328.7671 counter-clockwise by
    // 242.4659 and closes back to the point.
    //
    // The extremes are 0.22 and 0.78 across and 0.08 and 0.90 down, so the whole mark sits
    // inside the drawable box with the bottom tenth spare.
    qreal const radius (box * 0.28);
    QPointF const centre (inset + box * 0.50, inset + box * 0.36);
    qreal const drop (box * 0.54);

    // std::acos returns radians, and MSVC does not define M_PI without _USE_MATH_DEFINES, which
    // is not a define this translation unit has any business setting for a whole compilation.
    constexpr qreal DEGREES_PER_RADIAN = 57.295779513082320876798154814105;

    qreal const half_angle
      (std::acos (std::clamp (radius / drop, -1.0, 1.0)) * DEGREES_PER_RADIAN);

    QPainterPath pin;
    pin.moveTo (QPointF (centre.x(), centre.y() + drop));
    pin.arcTo ( QRectF (centre.x() - radius, centre.y() - radius, radius * 2.0, radius * 2.0)
              , 270.0 + half_angle
              , 360.0 - (2.0 * half_angle)
              );
    pin.closeSubpath();

    painter.drawPath (pin);

    // The hole. 0.40 of the head's radius: large enough to read as a hole at the 14px this is
    // never drawn at, small enough that the ring around it stays thicker than the stroke.
    painter.drawEllipse (centre, radius * 0.40, radius * 0.40);

    painter.end();

    return target;
  }

  QPixmap chevronGlyph (int extent, QColor const& ink, qreal ratio)
  {
    if (extent <= 0 || ratio <= 0.0)
    {
      return QPixmap();
    }

    QPixmap target (newLayer (extent, ratio));

    QPainter painter (&target);
    prepare (painter);
    painter.setPen (outlinePen (extent, ink));
    painter.setBrush (Qt::NoBrush);

    qreal const inset (strokeFor (extent) * 0.5);
    qreal const box (extent - (inset * 2.0));
    qreal const middle (inset + box * 0.5);

    // A chevron is TALLER THAN IT IS WIDE -- 0.32 of the box across against 0.56 down, a ratio of
    // 1.75 -- because a chevron drawn to fill a square box reads as a caret, and a caret means
    // "expand me" where this one has to mean "there is more this way".
    qreal const half_width (box * 0.16);
    qreal const half_height (box * 0.28);

    QPainterPath chevron;
    chevron.moveTo (QPointF (middle - half_width, middle - half_height));
    chevron.lineTo (QPointF (middle + half_width, middle));
    chevron.lineTo (QPointF (middle - half_width, middle + half_height));

    painter.drawPath (chevron);
    painter.end();

    return target;
  }

  QPixmap plusGlyph (int extent, QColor const& ink, qreal ratio)
  {
    if (extent <= 0 || ratio <= 0.0)
    {
      return QPixmap();
    }

    QPixmap target (newLayer (extent, ratio));

    QPainter painter (&target);
    prepare (painter);
    painter.setPen (outlinePen (extent, ink));

    qreal const inset (strokeFor (extent) * 0.5);
    qreal const box (extent - (inset * 2.0));
    qreal const middle (inset + box * 0.5);

    // 0.34 either side of centre, so the plus spans 0.68 of the drawable box. A plus drawn edge
    // to edge reads as a crosshair; held off the edge it reads as an operator.
    qreal const arm (box * 0.34);

    painter.drawLine (QPointF (middle - arm, middle), QPointF (middle + arm, middle));
    painter.drawLine (QPointF (middle, middle - arm), QPointF (middle, middle + arm));

    painter.end();

    return target;
  }

  QPixmap emblem ( QIcon const& crest
                 , int extent
                 , QColor const& fill
                 , QColor const& ring
                 , qreal ratio
                 )
  {
    if (extent <= 0 || ratio <= 0.0)
    {
      return QPixmap();
    }

    //! The ring, in logical pixels. One, because it is a hairline on a 36px disc and the disc's
    //! own edge is what the eye reads; two would make the emblem a button.
    constexpr qreal RING_WIDTH = 1.0;

    //! How far the crest is held off the ring. 0.14 of the extent leaves a 36px emblem a 30px
    //! crest inside a 5px surround, which is the same proportion the action tiles on the
    //! launcher use (20 of 40 is airier; a crest is a picture and needs less).
    constexpr qreal CREST_INSET = 0.14;

    QPixmap target (newLayer (extent, ratio));

    QPainter painter (&target);
    prepare (painter);

    qreal const e (extent);

    // The disc is inset by half the ring so the stroke straddles the pixmap edge rather than
    // spilling over it, which is the same half-pen rule every glyph above works to.
    QRectF const disc
      (RING_WIDTH * 0.5, RING_WIDTH * 0.5, e - RING_WIDTH, e - RING_WIDTH);

    painter.setPen (Qt::NoPen);
    painter.setBrush (fill);
    painter.drawEllipse (disc);

    if (!crest.isNull())
    {
      qreal const inset (e * CREST_INSET);
      QRectF const box (inset, inset, e - (inset * 2.0), e - (inset * 2.0));

      // ASK BIG, THEN STATE THE LOGICAL SIZE FROM WHAT CAME BACK.
      //
      // This used to request size * ratio and then setDevicePixelRatio(ratio), which was right
      // when Qt::AA_UseHighDpiPixmaps was NOT set. It is set now (ApplicationEntry.cpp), and with
      // it QIcon::pixmap(QSize) already multiplies the size it is given by the device ratio and
      // stamps the ratio on the result -- so passing a device size AND re-stamping applied the
      // ratio twice and the art came out proportionally oversized.
      //
      // Deriving the ratio from the returned pixmap instead is correct with the flag on or off,
      // and correct whatever size the engine actually chose to hand back: logical width is
      // width() / (width() / target) == target, exactly, by construction. The large request is
      // kept deliberately -- it is what makes the engine rasterise at full device resolution
      // rather than at the logical size, which is the whole point of the exercise.
      QPixmap art
        (crest.pixmap (QSize ( qRound (box.width() * ratio)
                             , qRound (box.height() * ratio)
                             )));

      if (!art.isNull() && art.width() > 0)
      {
        art.setDevicePixelRatio (qreal (art.width()) / box.width());

        // Clipped to the disc as well as inset inside it. The nine expansion crests are already
        // round with transparent corners, so the clip normally removes nothing -- it is here so
        // that a theme or a future expansion shipping a SQUARE picture cannot put a square in
        // the middle of a row of circles.
        QPainterPath clip;
        clip.addEllipse (disc);
        painter.setClipPath (clip);
        painter.drawPixmap (box.topLeft(), art);
        painter.setClipping (false);
      }
    }

    painter.setPen (QPen (ring, RING_WIDTH));
    painter.setBrush (Qt::NoBrush);
    painter.drawEllipse (disc);

    painter.end();

    return target;
  }
}
