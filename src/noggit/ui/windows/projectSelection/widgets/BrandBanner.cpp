// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/BrandBanner.hpp>
#include <noggit/ui/windows/projectSelection/widgets/LauncherArt.hpp>

#include <QBrush>
#include <QFont>
#include <QFontMetricsF>
#include <QIcon>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QSizePolicy>
#include <QString>

#include <algorithm>
#include <cmath>


namespace Noggit::Ui::Widget
{
  namespace
  {
    // ----------------------------------------------------------------------- METRICS --
    //
    // One struct per scale, so the two bands cannot drift apart in the ways that matter and can
    // differ in the one way that does. Every field is LOGICAL pixels or a font weight.
    struct Metrics
    {
      int height;
      int mark_extent;
      int mark_left;
      int wordmark_gap;
      int title_pixels;
      int sub_pixels;
      int title_tracking;
      int sub_tracking;
      int leading;
      int scrim_fade;
    };

    //! THE FULL BAND, on the project-selection window.
    //!
    //!   height        96 for the mark plus 24 of air above and below -- the design system's
    //!                 SPACE_24, the step reserved for the gap between major regions.
    //!                 96 + 24 + 24 = 144.
    //!   mark_extent   the product mark at the size the mockup asks for. The 28px version this
    //!                 replaced was a favicon standing in for a logo.
    //!   mark_left     inset from the left edge of the window. The banner is FULL BLEED -- it
    //!                 runs edge to edge rather than inside the window's SPACE_24 margin -- so
    //!                 the mark carries its own inset, one step above the body margin so the
    //!                 band reads as a header rather than as the first row of the content.
    //!   title_pixels  44px is 2.9x the window-title rank (15px). The band has one job and the
    //!                 wordmark is it.
    //!   sub_pixels    19px, tracked wider than the line above it, which is the standard way a
    //!                 two-line wordmark makes the shorter word span the longer one.
    //!   scrim_fade    how far past the wordmark the scrim takes to reach nothing. 240 is a
    //!                 sixth of the 1180 the launcher opens at -- long enough that the ramp
    //!                 cannot be seen as an edge, short enough that most of the picture is left
    //!                 at full veiled strength.
    constexpr Metrics FULL_METRICS { 144, 96, 32, 24, 44, 19, 9, 14, 6, 240 };

    //! THE COMPACT STRIP, on the map-selection window.
    //!
    //!   height        32 for the mark plus SPACE_16 above and below: 32 + 16 + 16 = 64. The map
    //!                 list is what the user is here to read, and a 144px band above a list is a
    //!                 poster with a list underneath it.
    //!   title_pixels  16px, one step above the sheet's 15px window-title rank, so the wordmark
    //!                 is still the largest thing in the strip without competing with the map
    //!                 names below it. That is 2.75x smaller than the launcher's 44.
    //!   sub_pixels    9px. Small enough that "CRIMSON" is a mark rather than a word to read --
    //!                 which is why the tracking below is 6px on a 9px face, two thirds of the
    //!                 em, where the launcher's is 14 on 19.
    //!   trackings     4 and 6 rather than 9 and 14: tracking that is not scaled with the face
    //!                 turns a small wordmark into loose letters. Measured with the real fonts,
    //!                 these two put "NOGGIT" and "CRIMSON" within a few pixels of the same
    //!                 advance, which is the effect the two-line lockup depends on.
    //!   scrim_fade    120, half the full band's, in proportion with everything else.
    constexpr Metrics COMPACT_METRICS { 64, 32, 16, 12, 16, 9, 4, 6, 2, 120 };

    Metrics const& metricsFor (BrandBanner::Scale scale)
    {
      return scale == BrandBanner::Scale::Compact ? COMPACT_METRICS : FULL_METRICS;
    }

    //! Where the metallic ramp turns over. 0.55 rather than 0.5 so the bright half is the
    //! slightly larger one, which is what makes a vertical ramp read as lit from above rather
    //! than as a gradient.
    constexpr qreal WORDMARK_RAMP_TURN = 0.55;

    QFont wordmarkFont (int pixel_size, int weight, int tracking)
    {
      QFont font (UiFonts::interfaceFont (pixel_size));
      font.setWeight (weight);

      // Tracking in ABSOLUTE pixels rather than per-mille, because QFont::PercentageSpacing is a
      // percentage of the character's own advance and therefore tracks unevenly across a mixed
      // string. Every letter here gets the same gap.
      font.setLetterSpacing (QFont::AbsoluteSpacing, tracking);
      return font;
    }

    //! The width of the wider of the two wordmark lines, from the real fonts. Used both for the
    //! widget's minimum width and for where the scrim stops being solid, so the two can never
    //! disagree about how much room the wordmark takes.
    qreal wordmarkAdvance (Metrics const& metrics)
    {
      QFontMetricsF const title
        (wordmarkFont (metrics.title_pixels, QFont::DemiBold, metrics.title_tracking));
      QFontMetricsF const sub
        (wordmarkFont (metrics.sub_pixels, QFont::Bold, metrics.sub_tracking));

      return std::max ( title.horizontalAdvance (QStringLiteral ("NOGGIT"))
                      , sub.horizontalAdvance (QStringLiteral ("CRIMSON"))
                      );
    }
  }

  BrandBanner::BrandBanner (QWidget* parent)
    : BrandBanner (Scale::Full, parent)
  {}

  BrandBanner::BrandBanner (Scale scale, QWidget* parent)
    : QWidget (parent)
    , _scale (scale)
    // Defaults that are only ever seen if a theme declares none of the qproperty- values below.
    // They are the shipped theme's own, so an unstyled banner is dark and legible rather than
    // black on black.
    , _band_left (0x17, 0x12, 0x0F)
    , _band_right (0x0B, 0x09, 0x08)
    , _scrim_ink (0x0B, 0x09, 0x08)
    , _rule_ink (0xE5, 0x40, 0x5C)
    , _wordmark_top (0xF3, 0xF0, 0xE9)
    , _wordmark_mid (0xE4, 0xDF, 0xD7)
    , _wordmark_bottom (0xA9, 0xA2, 0x96)
    , _wordmark_accent (0xE5, 0x40, 0x5C)
  {
    Metrics const& metrics (metricsFor (_scale));

    // TWO NAMES FOR ONE TYPE. The shipped sheet dresses both bands through the type selector
    // Noggit--Ui--Widget--BrandBanner, so today these two names change nothing -- they exist so
    // that a theme which wants the strip and the band to differ has a handle for each without
    // having to reach for a parent-window selector, which Qt style sheets cannot express.
    setObjectName (_scale == Scale::Compact
                     ? QStringLiteral ("map-header-banner")
                     : QStringLiteral ("project-banner"));

    setFixedHeight (metrics.height);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Fixed);

    // The wordmark is PAINTED, so assistive technology has nothing to read off this widget. This
    // is the whole of that debt and it is paid here.
    setAccessibleName (tr ("Noggit Crimson"));
  }

  QSize BrandBanner::minimumSizeHint() const
  {
    Metrics const& metrics (metricsFor (_scale));

    // Measured from the real fonts rather than guessed, so the window's own minimum can never be
    // narrower than the thing the banner has to draw. The trailing mark_left is the mirror of the
    // leading inset, so the wordmark is not flush against the right edge on a window squeezed to
    // its minimum.
    return QSize
      ( metrics.mark_left + metrics.mark_extent + metrics.wordmark_gap
          + static_cast<int> (std::ceil (wordmarkAdvance (metrics))) + metrics.mark_left
      , metrics.height
      );
  }

  QSize BrandBanner::sizeHint() const { return minimumSizeHint(); }

  void BrandBanner::setBandLeft (QColor const& colour) { _band_left = colour; invalidate(); }
  void BrandBanner::setBandRight (QColor const& colour) { _band_right = colour; invalidate(); }
  void BrandBanner::setScrimInk (QColor const& colour) { _scrim_ink = colour; invalidate(); }
  void BrandBanner::setRuleInk (QColor const& colour) { _rule_ink = colour; invalidate(); }

  void BrandBanner::setWordmarkTop (QColor const& colour) { _wordmark_top = colour; update(); }
  void BrandBanner::setWordmarkMid (QColor const& colour) { _wordmark_mid = colour; update(); }

  void BrandBanner::setWordmarkBottom (QColor const& colour)
  {
    _wordmark_bottom = colour;
    update();
  }

  void BrandBanner::setWordmarkAccent (QColor const& colour)
  {
    _wordmark_accent = colour;
    update();
  }

  void BrandBanner::invalidate()
  {
    _texture = QPixmap();
    update();
  }

  void BrandBanner::paintEvent (QPaintEvent* event)
  {
    Q_UNUSED (event)

    Metrics const& metrics (metricsFor (_scale));

    qreal const ratio (devicePixelRatioF());

    QSize const device
      (qRound (width() * ratio), qRound (height() * ratio));

    qreal const text_left (metrics.mark_left + metrics.mark_extent + metrics.wordmark_gap);

    if (_texture.isNull() || _texture.size() != device || _texture.devicePixelRatio() != ratio)
    {
      // WHERE THE SCRIM STOPS BEING SOLID: the far edge of the wordmark plus the same inset the
      // mark carries on the left, so the darkness the wordmark stands on is symmetric with the
      // band's own margin rather than stopping at the last glyph. The advance is measured from
      // the fonts that are about to draw it, so a font substitution moves the scrim with the
      // text instead of leaving it behind.
      qreal const scrim_hold
        (text_left + wordmarkAdvance (metrics) + metrics.mark_left);

      _texture = LauncherArt::bannerTexture
        ( size(), ratio, _band_left, _band_right, _scrim_ink, _rule_ink
        , scrim_hold, scrim_hold + metrics.scrim_fade
        );
    }

    // Compared in LOGICAL units. The previous guard tested the device width against
    // mark_extent * ratio, but the icon engine returns its own source size -- 256 for :/icon --
    // which never equalled that, so the guard was true on every paint and the mark was rebuilt
    // from the QIcon on every single paintEvent.
    qreal const cached_logical (_mark.isNull() ? 0.0 : _mark.width() / _mark.devicePixelRatio());

    if (_mark.isNull() || std::abs (cached_logical - metrics.mark_extent) > 0.5)
    {
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
      _mark = QIcon (":/icon").pixmap
        (QSize ( qRound (metrics.mark_extent * ratio)
               , qRound (metrics.mark_extent * ratio)
               ));

      if (!_mark.isNull() && _mark.width() > 0)
      {
        _mark.setDevicePixelRatio (qreal (_mark.width()) / qreal (metrics.mark_extent));
      }
    }

    QPainter painter (this);
    painter.setRenderHint (QPainter::Antialiasing, true);
    painter.setRenderHint (QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint (QPainter::TextAntialiasing, true);

    painter.drawPixmap (QPointF (0.0, 0.0), _texture);

    qreal const mark_top ((height() - metrics.mark_extent) / 2.0);
    painter.drawPixmap (QPointF (metrics.mark_left, mark_top), _mark);

    QFont const title
      (wordmarkFont (metrics.title_pixels, QFont::DemiBold, metrics.title_tracking));
    QFont const sub
      (wordmarkFont (metrics.sub_pixels, QFont::Bold, metrics.sub_tracking));

    QFontMetricsF const title_metrics (title);
    QFontMetricsF const sub_metrics (sub);

    qreal const block ( title_metrics.ascent() + title_metrics.descent()
                      + metrics.leading
                      + sub_metrics.ascent() + sub_metrics.descent()
                      );

    qreal const block_top ((height() - block) / 2.0);
    qreal const title_baseline (block_top + title_metrics.ascent());
    qreal const sub_baseline
      ( title_baseline + title_metrics.descent() + metrics.leading + sub_metrics.ascent());

    // THE METALLIC RAMP. A pen whose BRUSH is a gradient fills the glyphs with that gradient --
    // this is the only route to it, and it is the reason the wordmark is painted rather than
    // being a QLabel. The gradient spans exactly the inked box of the line so the ramp is the
    // same on every letter regardless of where the window sits.
    QLinearGradient metal
      ( QPointF (0.0, title_baseline - title_metrics.ascent())
      , QPointF (0.0, title_baseline + title_metrics.descent())
      );
    metal.setColorAt (0.0, _wordmark_top);
    metal.setColorAt (WORDMARK_RAMP_TURN, _wordmark_mid);
    metal.setColorAt (1.0, _wordmark_bottom);

    painter.setFont (title);
    painter.setPen (QPen (QBrush (metal), 0.0));
    painter.drawText (QPointF (text_left, title_baseline), QStringLiteral ("NOGGIT"));

    painter.setFont (sub);
    painter.setPen (QPen (_wordmark_accent));
    painter.drawText (QPointF (text_left, sub_baseline), QStringLiteral ("CRIMSON"));
  }
}
