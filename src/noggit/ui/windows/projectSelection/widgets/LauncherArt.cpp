// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/LauncherArt.hpp>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QLine>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>


namespace Noggit::Ui::Widget::LauncherArt
{
  namespace
  {
    // ------------------------------------------------------------- the noise field --

    //! The lattice the contour field is interpolated from. 14 x 6 over a 1180 x 144 banner puts
    //! a lattice cell at roughly 84 x 24 logical pixels, which is the scale at which the
    //! contours read as landform rather than as noise: below about 40px the isolines crowd into
    //! a moire, above about 150px there are too few closed loops to look like terrain.
    constexpr int LATTICE_WIDTH = 14;
    constexpr int LATTICE_HEIGHT = 6;

    //! Nine contour levels at (n / 10) of the field's 0..1 range. Ten or more and adjacent
    //! isolines touch at the 4px sampling step used for the banner; fewer than about seven and
    //! the field reads as a set of unrelated blobs.
    constexpr int CONTOUR_LEVELS = 9;

    //! A 32-bit linear congruential generator with the Numerical Recipes constants. Deliberately
    //! not std::mt19937: the sequence has to be identical on every machine and every launch, and
    //! a std::uniform_real_distribution is not required by the standard to produce the same
    //! values across implementations even from the same engine and seed. Four lines of arithmetic
    //! that cannot drift are worth more here than statistical quality nobody can see.
    class Lcg
    {
    public:
      explicit Lcg (std::uint32_t seed)
        : _state (seed)
      {}

      std::uint32_t nextU32()
      {
        _state = _state * 1664525u + 1013904223u;
        return _state;
      }

      qreal nextF() { return static_cast<qreal> (nextU32()) / 4294967296.0; }

    private:
      std::uint32_t _state;
    };

    qreal smoothstep (qreal t) { return t * t * (3.0 - 2.0 * t); }

    //! FNV-1a over the UTF-8 bytes, folded to 32 bits. Used to seed a project's artwork from its
    //! path so the same project draws the same tile on every machine, with no state stored
    //! anywhere and nothing to migrate.
    std::uint32_t pathSeed (QString const& path)
    {
      QByteArray const utf8 (path.toUtf8());

      std::uint64_t hash (1469598103934665603ull);

      for (char const byte : utf8)
      {
        hash ^= static_cast<unsigned char> (byte);
        hash *= 1099511628211ull;
      }

      return static_cast<std::uint32_t> (hash ^ (hash >> 32));
    }

    QColor withAlpha (QColor const& colour, qreal alpha)
    {
      QColor out (colour);
      out.setAlphaF (std::clamp (alpha, 0.0, 1.0));
      return out;
    }

    // ------------------------------------------------------------------ the caches --

    // Deliberately leaked, exactly like the icon cache in FontAwesome.cpp and for the same
    // reason: a function-local static is destroyed during static destruction, which runs AFTER
    // QApplication has gone, and a QPixmap is a handle into the platform integration. Tearing a
    // container of them down at that point is a warning or a crash on the way out of the process
    // rather than anything a user can act on. One process-lifetime allocation is the price.
    template<typename Key>
    std::map<Key, QPixmap>& pixmapCache()
    {
      static auto& cache (*new std::map<Key, QPixmap>());
      return cache;
    }

    //! Ratios are quantised to hundredths before they enter a cache key, so a fractional scale
    //! factor cannot make the key space unbounded.
    int ratioKey (qreal ratio) { return std::max (1, qRound (ratio * 100.0)); }

    QPixmap newLayer (QSize const& logical, qreal ratio)
    {
      QPixmap layer ( std::max (1, qRound (logical.width() * ratio))
                    , std::max (1, qRound (logical.height() * ratio))
                    );
      layer.setDevicePixelRatio (ratio);
      layer.fill (Qt::transparent);
      return layer;
    }
  }

  void paintContourField ( QPainter& painter
                         , QSizeF const& area
                         , std::uint32_t seed
                         , QColor const& ink
                         , qreal alpha_low
                         , qreal alpha_high
                         , int step
                         )
  {
    if (step < 1 || area.width() < step || area.height() < step)
    {
      return;
    }

    Lcg rng (seed);

    qreal lattice[LATTICE_HEIGHT][LATTICE_WIDTH];

    for (int y (0); y < LATTICE_HEIGHT; ++y)
    {
      for (int x (0); x < LATTICE_WIDTH; ++x)
      {
        lattice[y][x] = rng.nextF();
      }
    }

    int const columns (std::max (2, static_cast<int> (std::ceil (area.width() / step))));
    int const rows (std::max (2, static_cast<int> (std::ceil (area.height() / step))));

    // The field is evaluated ONCE into a node grid and every level then marches over the same
    // numbers. Evaluating it per level would repeat the interpolation nine times for no gain and
    // -- worse -- any drift between two evaluations would open gaps between adjacent isolines.
    std::vector<qreal> node (static_cast<std::size_t> ((columns + 1) * (rows + 1)));

    for (int j (0); j <= rows; ++j)
    {
      qreal const v ((static_cast<qreal> (j) / rows) * (LATTICE_HEIGHT - 1));
      int const y0 (std::clamp (static_cast<int> (v), 0, LATTICE_HEIGHT - 2));
      qreal const fy (smoothstep (std::clamp (v - y0, 0.0, 1.0)));

      for (int i (0); i <= columns; ++i)
      {
        qreal const u ((static_cast<qreal> (i) / columns) * (LATTICE_WIDTH - 1));
        int const x0 (std::clamp (static_cast<int> (u), 0, LATTICE_WIDTH - 2));
        qreal const fx (smoothstep (std::clamp (u - x0, 0.0, 1.0)));

        qreal const top (lattice[y0][x0] * (1.0 - fx) + lattice[y0][x0 + 1] * fx);
        qreal const bottom (lattice[y0 + 1][x0] * (1.0 - fx) + lattice[y0 + 1][x0 + 1] * fx);

        node[static_cast<std::size_t> (j) * (columns + 1) + i] = top * (1.0 - fy) + bottom * fy;
      }
    }

    painter.setBrush (Qt::NoBrush);

    for (int level (1); level <= CONTOUR_LEVELS; ++level)
    {
      qreal const threshold (static_cast<qreal> (level) / (CONTOUR_LEVELS + 1.0));
      qreal const alpha ( alpha_low
                        + (alpha_high - alpha_low)
                            * (static_cast<qreal> (level - 1) / (CONTOUR_LEVELS - 1.0))
                        );

      QVector<QLineF> lines;

      for (int j (0); j < rows; ++j)
      {
        for (int i (0); i < columns; ++i)
        {
          qreal const a (node[static_cast<std::size_t> (j) * (columns + 1) + i]);
          qreal const b (node[static_cast<std::size_t> (j) * (columns + 1) + i + 1]);
          qreal const c (node[static_cast<std::size_t> (j + 1) * (columns + 1) + i + 1]);
          qreal const d (node[static_cast<std::size_t> (j + 1) * (columns + 1) + i]);

          // Marching squares. Bit 3 is the top-left corner, then clockwise: top-right,
          // bottom-right, bottom-left. Cases 0 and 15 are wholly inside or wholly outside the
          // level and carry no isoline.
          int const code ( (a > threshold ? 8 : 0) | (b > threshold ? 4 : 0)
                         | (c > threshold ? 2 : 0) | (d > threshold ? 1 : 0)
                         );

          if (code == 0 || code == 15)
          {
            continue;
          }

          // The four edge crossings, computed LAZILY. Only an edge whose two corners straddle
          // the threshold is ever asked for, so the denominator is never zero -- computing all
          // four up front would divide by zero on the edges this case does not cross.
          auto const top
            ([&] { return QPointF ((i + (threshold - a) / (b - a)) * step, qreal (j) * step); });
          auto const right
            ([&] { return QPointF (qreal (i + 1) * step, (j + (threshold - b) / (c - b)) * step); });
          auto const bottom
            ([&] { return QPointF ((i + (threshold - d) / (c - d)) * step, qreal (j + 1) * step); });
          auto const left
            ([&] { return QPointF (qreal (i) * step, (j + (threshold - a) / (d - a)) * step); });

          switch (code)
          {
            case 1:  case 14: lines.append (QLineF (left(), bottom())); break;
            case 2:  case 13: lines.append (QLineF (bottom(), right())); break;
            case 3:  case 12: lines.append (QLineF (left(), right())); break;
            case 4:  case 11: lines.append (QLineF (top(), right())); break;
            case 6:  case 9:  lines.append (QLineF (top(), bottom())); break;
            case 7:  case 8:  lines.append (QLineF (left(), top())); break;

            // The two ambiguous saddles. Both diagonals are legal; the pair chosen here keeps
            // the isoline separating the two HIGH corners, which is what a contour map does.
            case 5:
              lines.append (QLineF (left(), top()));
              lines.append (QLineF (bottom(), right()));
              break;
            case 10:
              lines.append (QLineF (top(), right()));
              lines.append (QLineF (left(), bottom()));
              break;

            default: break;
          }
        }
      }

      painter.setPen (QPen (withAlpha (ink, alpha), 1.0));
      painter.drawLines (lines);
    }
  }

  QPixmap bannerTexture ( QSize const& logical
                        , qreal ratio
                        , QColor const& band_left
                        , QColor const& band_right
                        , QColor const& scrim_ink
                        , QColor const& rule_ink
                        , qreal scrim_hold
                        , qreal scrim_fade
                        )
  {
    //! The artwork behind the wordmark. 2880x480, registered in resources.qrc.
    constexpr char const* BACKDROP = ":/banner-backdrop";

    //! THE VEIL, over the whole band. 0.5255 is the smallest alpha that puts text.hi #F3F0E9 at
    //! 7:1 over the brightest pixel a 144-logical crop presents at the launcher's own width
    //! (#FE777E, relative luminance 0.35771) when the veil colour is band.right, and 0.5608 when
    //! it is band.left; the gradient uses both, and 0.58 clears the harder of the two.
    //!
    //! IT IS A TAMING LAYER AND NOT A GUARANTEE, which is worth stating plainly because the
    //! obvious reading of the number above is that it guarantees 7:1 everywhere and it does not.
    //! Squeezed to 800 logical the cover fit stops being width-driven, the crop turns
    //! horizontal, more of the picture comes into view, and the worst surviving pixel measures
    //! #784D3C -- text.hi 6.307:1. Under the prose floor, over the body floor, and accepted
    //! because NO TEXT IS DRAWN PAST THE SCRIM: what sits out there is decoration and, on the
    //! map-selection window, one opaque button that supplies its own fill. Every measurement in
    //! this comment was taken by compositing this exact pipeline over the real asset, not by
    //! hand.
    constexpr qreal VEIL_ALPHA = 0.58;

    //! THE SCRIM, under the wordmark only, and this one IS the guarantee. The binding constraint
    //! is brand.crimson #E5405C, the colour of the second wordmark line: it needs its background
    //! under relative luminance 0.007996 to hold 4.5:1, and the smallest single alpha that does
    //! that is 0.9137. Composed with the veil this gives 1 - (1 - 0.58) x (1 - 0.86) = 0.9412 --
    //! and because the scrim is at FULL strength from x = 0 to scrim_hold at every width, that
    //! figure does not move when the window is resized.
    //!
    //! Against the brightest pixel the artwork contains anywhere, #FF9F7A at luminance 0.474614,
    //! the pair land on #19120F: text.hi 16.260:1, brand.crimson 4.600:1. Against the darkest
    //! they land on #0C0908: text.hi 17.437:1, brand.crimson 4.932:1. Both floors hold at every
    //! width and every device pixel ratio.
    constexpr qreal SCRIM_ALPHA = 0.86;

    //! The rule that closes the band, brightest under the mark and fading right.
    constexpr qreal RULE_HEIGHT = 2.0;

    if (logical.isEmpty() || ratio <= 0.0)
    {
      return QPixmap();
    }

    QPixmap target (newLayer (logical, ratio));

    QRectF const box (0.0, 0.0, logical.width(), logical.height());

    QPainter painter (&target);
    painter.setRenderHint (QPainter::Antialiasing, true);
    painter.setRenderHint (QPainter::SmoothPixmapTransform, true);

    // The band is drawn FIRST and unconditionally, so it is both the ground the artwork sits on
    // and the whole banner when the artwork cannot be decoded. A backdrop that fails to load is
    // a legitimate state -- the JPEG needs Qt's qjpeg plugin, which a hand-assembled deployment
    // can be missing -- and it must cost the picture, never the legibility.
    QLinearGradient band (box.topLeft(), box.topRight());
    band.setColorAt (0.0, band_left);
    band.setColorAt (1.0, band_right);
    painter.fillRect (box, band);

    // DECODED ONCE for the life of the process. A QImage, not a QPixmap: a function-local static
    // is destroyed after QApplication has gone and a QPixmap is a handle into the platform
    // integration, which is the same trap the caches at the head of this file are leaked to
    // avoid. A QImage owns nothing but memory and is safe to let run to static destruction.
    static QImage const source (QString::fromLatin1 (BACKDROP));

    if (!source.isNull())
    {
      QSize const device ( std::max (1, qRound (logical.width() * ratio))
                         , std::max (1, qRound (logical.height() * ratio))
                         );

      // COVER, not stretch. KeepAspectRatioByExpanding scales until BOTH dimensions are at least
      // the target's, so the aspect ratio is never distorted, and the overflow is then cropped
      // from the centre. At 1440 logical pixels wide on a ratio-2 display the target is 2880x288
      // and the source is 2880x480: the expanding scale is exactly 1.0, Qt returns the image
      // unchanged, and the only operation left is a 2880x288 crop out of the middle. That is the
      // 1:1 case the asset was cut for. Every other width resamples once, with
      // Qt::SmoothTransformation, into the device grid rather than into logical pixels -- which
      // is the whole point of doing the fit at device resolution and stamping the ratio back on
      // afterwards. Scaling at logical size and letting drawPixmap magnify would throw away
      // three quarters of the pixels on this display.
      QImage const covered
        (source.scaled (device, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));

      QPixmap art
        ( QPixmap::fromImage
            ( covered.copy
                ( QRect ( (covered.width() - device.width()) / 2
                        , (covered.height() - device.height()) / 2
                        , device.width()
                        , device.height()
                        )
                )
            )
        );
      art.setDevicePixelRatio (ratio);

      painter.drawPixmap (QPointF (0.0, 0.0), art);

      // The veil is the band gradient again, at VEIL_ALPHA. Using the band's own two colours
      // rather than a flat black keeps the left-to-right darkening the theme asks for reading
      // through the picture instead of fighting it, and keeps the colour decision in the theme.
      QLinearGradient veil (box.topLeft(), box.topRight());
      veil.setColorAt (0.0, withAlpha (band_left, VEIL_ALPHA));
      veil.setColorAt (1.0, withAlpha (band_right, VEIL_ALPHA));
      painter.fillRect (box, veil);

      if (scrim_fade > scrim_hold && scrim_fade > 0.0)
      {
        // Stated in LOGICAL PIXELS from the left edge and not as a fraction of the width, so the
        // wordmark keeps exactly as much darkness as it needs whatever the window is resized to.
        // A fraction would thin the scrim out from under the wordmark the moment the window was
        // widened, which is precisely when the artwork gets busier.
        QLinearGradient scrim (QPointF (0.0, 0.0), QPointF (scrim_fade, 0.0));
        scrim.setColorAt (0.0, withAlpha (scrim_ink, SCRIM_ALPHA));
        scrim.setColorAt (std::clamp (scrim_hold / scrim_fade, 0.0, 1.0),
                          withAlpha (scrim_ink, SCRIM_ALPHA));
        scrim.setColorAt (1.0, withAlpha (scrim_ink, 0.0));

        painter.fillRect
          (QRectF (0.0, 0.0, std::min (scrim_fade, box.width()), box.height()), scrim);
      }
    }

    QLinearGradient rule (box.bottomLeft(), box.bottomRight());
    rule.setColorAt (0.00, withAlpha (rule_ink, 1.00));
    rule.setColorAt (0.28, withAlpha (rule_ink, 0.55));
    rule.setColorAt (0.62, withAlpha (rule_ink, 0.18));
    rule.setColorAt (1.00, withAlpha (rule_ink, 0.00));

    painter.fillRect
      (QRectF (box.left(), box.bottom() - RULE_HEIGHT, box.width(), RULE_HEIGHT), rule);

    painter.end();

    return target;
  }

  QPixmap circularIcon (QIcon const& icon, int extent, QColor const& ring, qreal ratio)
  {
    if (extent <= 0 || ratio <= 0.0)
    {
      return QPixmap();
    }

    QPixmap target (newLayer (QSize (extent, extent), ratio));

    QPainter painter (&target);
    painter.setRenderHint (QPainter::Antialiasing, true);
    painter.setRenderHint (QPainter::SmoothPixmapTransform, true);

    QPainterPath clip;
    clip.addEllipse (QRectF (0.0, 0.0, extent, extent));
    painter.setClipPath (clip);

    if (!icon.isNull())
    {
      // QIcon::pixmap takes a LOGICAL size and returns a pixmap whose device pixel ratio is 1, so
      // asking for `extent` on a 2x screen yields half the pixels the circle has to fill. Ask for
      // the DEVICE size and draw it into the logical rect, which is 1:1 on this painter.
      //
      // The source rect is the pixmap's OWN rect rather than the size that was asked for: QIcon
      // hands back the best it has and is under no obligation to match the request, so naming the
      // requested size here would sample outside a smaller bitmap.
      QPixmap const source
        (icon.pixmap (QSize (qRound (extent * ratio), qRound (extent * ratio))));

      if (!source.isNull())
      {
        painter.drawPixmap
          (QRectF (0.0, 0.0, extent, extent), source, QRectF (source.rect()));
      }
    }

    painter.setClipping (false);
    painter.setBrush (Qt::NoBrush);
    painter.setPen (QPen (ring, 1.0));
    painter.drawEllipse (QRectF (0.5, 0.5, extent - 1.0, extent - 1.0));

    painter.end();

    return target;
  }

  QPixmap actionTile (char32_t glyph, QColor const& tile, QColor const& ink, qreal ratio)
  {
    //! 10 of the tile's 40, i.e. a quarter of the side. The sheet's control tier is 6 on a 28px
    //! box, 0.214 of the side; this is 0.250, deliberately rounder, because the tile is a plate
    //! the glyph sits on rather than a control the pointer aims at.
    constexpr qreal TILE_RADIUS = 10.0;

    if (ratio <= 0.0)
    {
      return QPixmap();
    }

    using Key = std::tuple<char32_t, QRgb, QRgb, int>;

    auto& cache (pixmapCache<Key>());

    Key const key (glyph, tile.rgba(), ink.rgba(), ratioKey (ratio));

    auto const cached (cache.find (key));

    if (cached != cache.end())
    {
      return cached->second;
    }

    QPixmap target (newLayer (QSize (TILE_EXTENT + TILE_GAP, TILE_EXTENT), ratio));

    QPainter painter (&target);
    painter.setRenderHint (QPainter::Antialiasing, true);
    painter.setPen (Qt::NoPen);
    painter.setBrush (tile);
    painter.drawRoundedRect
      (QRectF (0.0, 0.0, TILE_EXTENT, TILE_EXTENT), TILE_RADIUS, TILE_RADIUS);

    // ThemeIcons and IconSet are members of the Noggit::Ui NAMESPACE, not of the FontAwesome
    // struct -- that struct (FontAwesome.hpp:29) holds only the Icons enum. Spelling them
    // FontAwesome::ThemeIcons does not compile, so they are qualified from the namespace.
    QPixmap const mark
      ( Noggit::Ui::ThemeIcons::pixmap
          ( Noggit::Ui::IconSet::FontAwesome
          , glyph
          , QIcon::Off
          , QSize (GLYPH_EXTENT, GLYPH_EXTENT)
          , ratio
          , ink
          )
      );

    if (!mark.isNull())
    {
      QSizeF const drawn (QSizeF (mark.size()) / mark.devicePixelRatio());

      painter.drawPixmap
        ( QPointF ( (TILE_EXTENT - drawn.width()) / 2.0
                  , (TILE_EXTENT - drawn.height()) / 2.0
                  )
        , mark
        );
    }

    painter.end();

    cache.emplace (key, target);

    return target;
  }

  QPixmap calendarGlyph (int extent, QColor const& ink, qreal ratio)
  {
    if (extent < 8 || ratio <= 0.0)
    {
      return QPixmap();
    }

    using Key = std::tuple<int, QRgb, int>;

    auto& cache (pixmapCache<Key>());

    Key const key (extent, ink.rgba(), ratioKey (ratio));

    auto const cached (cache.find (key));

    if (cached != cache.end())
    {
      return cached->second;
    }

    QPixmap target (newLayer (QSize (extent, extent), ratio));

    qreal const side (extent);

    QPainter painter (&target);
    painter.setRenderHint (QPainter::Antialiasing, true);

    // A body with a filled header band and two hangers. Drawn from proportions of `extent` so it
    // stays a calendar at any size the card ever asks for, rather than from pixel offsets tuned
    // to one.
    QRectF const body (0.5, side * 0.18 + 0.5, side - 1.0, side * 0.82 - 1.0);

    QPainterPath body_path;
    body_path.addRoundedRect (body, side * 0.12, side * 0.12);

    painter.setBrush (Qt::NoBrush);
    painter.setPen (QPen (ink, 1.0));
    painter.drawPath (body_path);

    // Clipped to the body, or the square header band puts a pixel of ink outside each of the two
    // rounded top corners and the mark stops reading as one shape.
    painter.setClipPath (body_path);
    painter.setPen (Qt::NoPen);
    painter.setBrush (ink);
    painter.drawRect
      (QRectF (body.left(), body.top(), body.width(), std::max (2.0, side * 0.18)));
    painter.setClipping (false);

    painter.setBrush (Qt::NoBrush);
    painter.setPen (QPen (ink, 1.0));
    painter.drawLine (QPointF (side * 0.30, 0.5), QPointF (side * 0.30, side * 0.20));
    painter.drawLine (QPointF (side * 0.70, 0.5), QPointF (side * 0.70, side * 0.20));

    painter.end();

    cache.emplace (key, target);

    return target;
  }

  QPixmap projectArtwork ( QString const& project_directory
                         , QSize const& logical
                         , QColor const& fill
                         , QColor const& ink
                         , QColor const& ring
                         , qreal ratio
                         )
  {
    //! The card artwork is a CONTAINER in the design system's radius tiers, one step below the
    //! card's own 12px because it is nested inside it.
    constexpr qreal ART_RADIUS = 8.0;

    //! The procedural field is sampled finer than the banner's because the tile is an eighth of
    //! the width: 3 logical pixels per cell over a 104px tile is 35 columns, which is enough
    //! closed loops to be recognisably different from the next project's.
    constexpr int ART_CONTOUR_STEP = 3;
    constexpr qreal ART_ALPHA_LOW = 0.05;
    constexpr qreal ART_ALPHA_HIGH = 0.13;

    if (logical.isEmpty() || ratio <= 0.0)
    {
      return QPixmap();
    }

    using Key = std::tuple<QString, int, int, int>;

    auto& cache (pixmapCache<Key>());

    Key const key (project_directory, logical.width(), logical.height(), ratioKey (ratio));

    auto const cached (cache.find (key));

    if (cached != cache.end())
    {
      return cached->second;
    }

    QPixmap target (newLayer (logical, ratio));

    QRectF const box (0.0, 0.0, logical.width(), logical.height());

    QPainter painter (&target);
    painter.setRenderHint (QPainter::Antialiasing, true);
    painter.setRenderHint (QPainter::SmoothPixmapTransform, true);

    QPainterPath clip;
    clip.addRoundedRect (box, ART_RADIUS, ART_RADIUS);
    painter.setClipPath (clip);

    QPixmap sidecar;

    for (QString const& name : {QStringLiteral ("project_art.png")
                               , QStringLiteral ("project_art.jpg")
                               , QStringLiteral ("project_art.jpeg")})
    {
      QFileInfo const candidate (QDir (project_directory).filePath (name));

      if (candidate.isFile() && sidecar.load (candidate.absoluteFilePath()))
      {
        break;
      }
    }

    if (!sidecar.isNull())
    {
      // Expanding rather than KeepAspectRatio: the slot is a fixed shape in the card and a
      // letterboxed picture would show the card fill through the bars, which reads as a broken
      // image. Cropping to fill is what every other thumbnail in the application does.
      QPixmap scaled
        ( sidecar.scaled ( QSize (qRound (box.width() * ratio), qRound (box.height() * ratio))
                         , Qt::KeepAspectRatioByExpanding
                         , Qt::SmoothTransformation
                         )
        );

      // QPixmap::scaled does NOT carry the device pixel ratio across -- it goes through
      // QPixmap::transformed, which builds a fresh pixmap and leaves the ratio at 1. Without this
      // line the bitmap was scaled to device resolution and then drawn as though every device
      // pixel were a logical one, i.e. at twice the size on this display.
      scaled.setDevicePixelRatio (ratio);

      QSizeF const drawn (QSizeF (scaled.size()) / ratio);

      painter.drawPixmap
        ( QPointF ((box.width() - drawn.width()) / 2.0, (box.height() - drawn.height()) / 2.0)
        , scaled
        );
    }
    else
    {
      painter.fillRect (box, fill);

      paintContourField ( painter, box.size(), pathSeed (project_directory), ink
                        , ART_ALPHA_LOW, ART_ALPHA_HIGH, ART_CONTOUR_STEP
                        );
    }

    painter.setClipping (false);
    painter.setBrush (Qt::NoBrush);
    painter.setPen (QPen (ring, 1.0));
    painter.drawRoundedRect
      ( QRectF (0.5, 0.5, box.width() - 1.0, box.height() - 1.0)
      , ART_RADIUS - 0.5
      , ART_RADIUS - 0.5
      );

    painter.end();

    cache.emplace (key, target);

    return target;
  }
}
