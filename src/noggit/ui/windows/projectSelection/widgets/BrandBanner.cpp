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
    //! 96 for the mark plus 24 of air above and below -- the design system's SPACE_24, the step
    //! reserved for the gap between major regions. 96 + 24 + 24 = 144.
    constexpr int BANNER_HEIGHT = 144;

    //! The product mark, at the size the mockup asks for. The 28px version this replaces was a
    //! favicon standing in for a logo.
    constexpr int MARK_EXTENT = 96;

    //! Inset from the left edge of the window. The banner is FULL BLEED -- it runs edge to edge
    //! rather than inside the window's SPACE_24 margin -- so the mark carries its own inset, and
    //! it is one step above the body margin so the band reads as a header rather than as the
    //! first row of the content.
    constexpr int MARK_LEFT = 32;

    //! Between the mark and the wordmark.
    constexpr int WORDMARK_LEFT_GAP = 24;

    //! "NOGGIT". 44px is 2.9x the window-title rank (15px) -- the band has one job and the
    //! wordmark is it.
    constexpr int WORDMARK_TITLE_PIXELS = 44;

    //! "CRIMSON". 19px, tracked wider than the line above it, which is the standard way a
    //! two-line wordmark makes the shorter word span the longer one.
    constexpr int WORDMARK_SUB_PIXELS = 19;

    //! Tracking, in ABSOLUTE pixels rather than per-mille, because QFont::PercentageSpacing is a
    //! percentage of the character's own advance and therefore tracks unevenly across a mixed
    //! string. Every letter here gets the same gap.
    constexpr int WORDMARK_TITLE_TRACKING = 9;
    constexpr int WORDMARK_SUB_TRACKING = 14;

    //! Between the two baselines' inked boxes.
    constexpr int WORDMARK_LEADING = 6;

    //! Where the metallic ramp turns over. 0.55 rather than 0.5 so the bright half is the
    //! slightly larger one, which is what makes a vertical ramp read as lit from above rather
    //! than as a gradient.
    constexpr qreal WORDMARK_RAMP_TURN = 0.55;

    QFont wordmarkFont (int pixel_size, int weight, int tracking)
    {
      QFont font (UiFonts::interfaceFont (pixel_size));
      font.setWeight (weight);
      font.setLetterSpacing (QFont::AbsoluteSpacing, tracking);
      return font;
    }
  }

  BrandBanner::BrandBanner (QWidget* parent)
    : QWidget (parent)
    // Defaults that are only ever seen if a theme declares none of the qproperty- values below.
    // They are the shipped theme's own, so an unstyled banner is dark and legible rather than
    // black on black.
    , _band_left (0x17, 0x12, 0x0F)
    , _band_right (0x0B, 0x09, 0x08)
    , _contour_ink (0xE5, 0x40, 0x5C)
    , _rule_ink (0xE5, 0x40, 0x5C)
    , _wordmark_top (0xF3, 0xF0, 0xE9)
    , _wordmark_mid (0xE4, 0xDF, 0xD7)
    , _wordmark_bottom (0xA9, 0xA2, 0x96)
    , _wordmark_accent (0xE5, 0x40, 0x5C)
  {
    setObjectName ("project-banner");
    setFixedHeight (BANNER_HEIGHT);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Fixed);

    // The wordmark is PAINTED, so assistive technology has nothing to read off this widget. This
    // is the whole of that debt and it is paid here.
    setAccessibleName (tr ("Noggit Crimson"));
  }

  QSize BrandBanner::minimumSizeHint() const
  {
    QFontMetricsF const title
      (wordmarkFont (WORDMARK_TITLE_PIXELS, QFont::DemiBold, WORDMARK_TITLE_TRACKING));
    QFontMetricsF const sub
      (wordmarkFont (WORDMARK_SUB_PIXELS, QFont::Bold, WORDMARK_SUB_TRACKING));

    // Measured from the real fonts rather than guessed, so the window's own minimum can never be
    // narrower than the thing the banner has to draw.
    qreal const wordmark
      ( std::max ( title.horizontalAdvance (QStringLiteral ("NOGGIT"))
                 , sub.horizontalAdvance (QStringLiteral ("CRIMSON"))
                 )
      );

    // The trailing MARK_LEFT is the mirror of the leading inset, so the wordmark is not flush
    // against the right edge on a window squeezed to its minimum.
    return QSize
      ( MARK_LEFT + MARK_EXTENT + WORDMARK_LEFT_GAP
          + static_cast<int> (std::ceil (wordmark)) + MARK_LEFT
      , BANNER_HEIGHT
      );
  }

  QSize BrandBanner::sizeHint() const { return minimumSizeHint(); }

  void BrandBanner::setBandLeft (QColor const& colour) { _band_left = colour; invalidate(); }
  void BrandBanner::setBandRight (QColor const& colour) { _band_right = colour; invalidate(); }
  void BrandBanner::setContourInk (QColor const& colour) { _contour_ink = colour; invalidate(); }
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

    qreal const ratio (devicePixelRatioF());

    QSize const device
      (qRound (width() * ratio), qRound (height() * ratio));

    if (_texture.isNull() || _texture.size() != device || _texture.devicePixelRatio() != ratio)
    {
      _texture = LauncherArt::bannerTexture
        (size(), ratio, _band_left, _band_right, _contour_ink, _rule_ink);
    }

    if (_mark.isNull() || _mark.devicePixelRatio() != ratio)
    {
      // The same trap the 28px mark fell into: QIcon::pixmap takes a LOGICAL size and returns a
      // ratio-1 bitmap, so on this display's ratio of 2 a 96px request produced 96 device pixels
      // stretched across 192. Ask for the device size and stamp the ratio back on.
      _mark = QIcon (":/icon").pixmap
        (QSize (qRound (MARK_EXTENT * ratio), qRound (MARK_EXTENT * ratio)));
      _mark.setDevicePixelRatio (ratio);
    }

    QPainter painter (this);
    painter.setRenderHint (QPainter::Antialiasing, true);
    painter.setRenderHint (QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint (QPainter::TextAntialiasing, true);

    painter.drawPixmap (QPointF (0.0, 0.0), _texture);

    qreal const mark_top ((height() - MARK_EXTENT) / 2.0);
    painter.drawPixmap (QPointF (MARK_LEFT, mark_top), _mark);

    QFont const title
      (wordmarkFont (WORDMARK_TITLE_PIXELS, QFont::DemiBold, WORDMARK_TITLE_TRACKING));
    QFont const sub
      (wordmarkFont (WORDMARK_SUB_PIXELS, QFont::Bold, WORDMARK_SUB_TRACKING));

    QFontMetricsF const title_metrics (title);
    QFontMetricsF const sub_metrics (sub);

    qreal const block ( title_metrics.ascent() + title_metrics.descent()
                      + WORDMARK_LEADING
                      + sub_metrics.ascent() + sub_metrics.descent()
                      );

    qreal const block_top ((height() - block) / 2.0);
    qreal const title_baseline (block_top + title_metrics.ascent());
    qreal const sub_baseline
      ( title_baseline + title_metrics.descent() + WORDMARK_LEADING + sub_metrics.ascent());

    qreal const text_left (MARK_LEFT + MARK_EXTENT + WORDMARK_LEFT_GAP);

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
