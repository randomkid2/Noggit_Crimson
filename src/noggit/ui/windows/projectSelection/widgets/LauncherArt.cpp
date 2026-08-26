// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/widgets/LauncherArt.hpp>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLine>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPoint>
#include <QRadialGradient>
#include <QRect>
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
                        , QColor const& ink
                        , QColor const& rule_ink
                        )
  {
    // The fixed seed. "NOGG" in ASCII, so the number is readable rather than magic.
    constexpr std::uint32_t BANNER_SEED = 0x4E4F4747u;

    //! 4 logical pixels per marching-squares cell. At 1180 wide that is 295 columns and 36 rows,
    //! i.e. 10,620 cells evaluated once and marched nine times. The pass is cached and only runs
    //! again when the banner is resized or moves to a screen with a different pixel ratio.
    constexpr int CONTOUR_STEP = 4;

    //! The contour ink ramps from 4.5% to 9% alpha across the nine levels. At the top of that
    //! range brand.crimson over band.left composites to #2A1616, 1.084:1 above the band; at the
    //! bottom it is #201412, 1.036:1. Texture, and nowhere near a boundary anything could be
    //! mistaken for -- the design system's own smallest deliberate surface step is 1.104:1.
    constexpr qreal CONTOUR_ALPHA_LOW = 0.045;
    constexpr qreal CONTOUR_ALPHA_HIGH = 0.09;

    //! How much of the contour layer is masked away at the dark end. The band already darkens
    //! left to right; fading the texture in the SAME direction is what stops the two gradients
    //! reading as mud, and it is why the wordmark end of the banner is the busy end.
    constexpr qreal CONTOUR_RIGHT_FADE = 0.72;

    //! Six seeded radial glows, additive, peaking at 10% alpha. "Particles" in the mockup are
    //! static here on purpose: a 60 Hz repaint on a launcher to animate decoration costs a core
    //! for something nobody looks at twice.
    constexpr int GLOW_COUNT = 6;
    constexpr qreal GLOW_MIN_RADIUS = 40.0;
    constexpr qreal GLOW_MAX_RADIUS = 120.0;
    constexpr qreal GLOW_PEAK_ALPHA = 0.10;

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

    QLinearGradient band (box.topLeft(), box.topRight());
    band.setColorAt (0.0, band_left);
    band.setColorAt (1.0, band_right);
    painter.fillRect (box, band);

    Lcg glow_rng (BANNER_SEED ^ 0x9E3779B9u);

    painter.setCompositionMode (QPainter::CompositionMode_Plus);

    for (int i (0); i < GLOW_COUNT; ++i)
    {
      QPointF const centre (glow_rng.nextF() * box.width(), glow_rng.nextF() * box.height());
      qreal const radius
        (GLOW_MIN_RADIUS + glow_rng.nextF() * (GLOW_MAX_RADIUS - GLOW_MIN_RADIUS));

      QRadialGradient glow (centre, radius);
      glow.setColorAt (0.0, withAlpha (ink, GLOW_PEAK_ALPHA));
      glow.setColorAt (1.0, withAlpha (ink, 0.0));

      painter.fillRect
        ( QRectF (centre.x() - radius, centre.y() - radius, radius * 2.0, radius * 2.0)
            .intersected (box)
        , glow
        );
    }

    painter.setCompositionMode (QPainter::CompositionMode_SourceOver);

    // The contours go on their OWN layer so the left-to-right fade can be applied to the whole
    // field in one DestinationIn pass. Fading them per line segment would mean a pen change per
    // segment, and fading them by stepping the level alpha would fade them by HEIGHT instead of
    // by position, which is not what the band does.
    QPixmap contours (newLayer (logical, ratio));

    {
      QPainter layer (&contours);
      layer.setRenderHint (QPainter::Antialiasing, true);

      paintContourField ( layer, box.size(), BANNER_SEED, ink
                        , CONTOUR_ALPHA_LOW, CONTOUR_ALPHA_HIGH, CONTOUR_STEP
                        );

      QLinearGradient fade (box.topLeft(), box.topRight());
      fade.setColorAt (0.0, QColor (0, 0, 0, 255));
      fade.setColorAt (1.0, QColor (0, 0, 0, qRound (255.0 * (1.0 - CONTOUR_RIGHT_FADE))));

      layer.setCompositionMode (QPainter::CompositionMode_DestinationIn);
      layer.fillRect (box, fade);
    }

    painter.drawPixmap (QPointF (0.0, 0.0), contours);

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
