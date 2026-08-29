// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/TerrainMaskPreview.hpp>

#include <noggit/MapView.h>
#include <noggit/terrain/TerrainMask.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>
#include <noggit/ui/DesignTokens.hpp>

#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtGui/QPen>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
  // Sample grid edge. 192 x 192 = 36'864 samples, each one unordered_map lookup and four texel
  // reads, at most fifteen refreshes a second -- 552'960 field samples a second.
  //
  // FOR SCALE, because this is not free: a radius-15 stroke covers pi * 225 = 706.9 square yards,
  // which at MASK_TEXEL_SIZE^2 = 0.2713 square yards per texel is about 2'606 texels a tick, or
  // 156'360 a second while the button is held. So the loupe costs roughly 3.5 times a stroke in
  // flight. It is affordable because it is BOUNDED at both ends: the timer stops the moment the
  // panel is hidden (hideEvent), and refreshIfChanged skips the whole pass when neither the
  // cursor nor the mask has moved, which is every frame the user is not painting.
  //
  // Kept below the widget's own 224 px so the image is scaled UP rather than down: an upscale
  // with a smooth transform reads as a soft mask edge, which is what a feathered mask actually
  // is, whereas a downscale would alias the 0.52 yd texel grid into moire.
  constexpr int SAMPLE_RESOLUTION = 192;

  // Below this the chunk grid is denser than the pixels drawing it and turns the loupe into a
  // sheet of lines. 33.33 yd per chunk over a 256 yd window is 7.7 chunks across a 224 px
  // square, so a line every 29 px; over the 533.33 yd whole-tile window it would be a line every
  // 14 px, which is where it stops helping.
  constexpr float CHUNK_GRID_MAX_WINDOW_YARDS = 300.0f;

  // Distance the cursor must move before the loupe redraws, in yards. One sample of the loupe at
  // the tightest window is 64 / 192 = 0.333 yd, so anything under that cannot change a pixel.
  constexpr float CURSOR_MOVE_EPSILON = 0.25f;
}

namespace Noggit::Ui
{
  QColor maskIdentityColor(std::string const& name)
  {
    if (name.empty())
    {
      return Design::color(Design::TEXT_OFF);
    }

    // FNV-1a over the name. Any stable hash would do; this one is four lines and has no
    // dependency, and the only property required of it is that two different names rarely land
    // within a few degrees of each other.
    std::uint32_t hash = 2166136261u;

    for (char const character : name)
    {
      hash ^= static_cast<std::uint8_t>(character);
      hash *= 16777619u;
    }

    // Saturation and value are fixed rather than hashed. A hashed value would eventually produce
    // a mask whose colour is nearly the panel background, and the whole point of the swatch is
    // that it is legible at a glance.
    int const hue = static_cast<int>(hash % 360u);

    return QColor::fromHsv(hue, 200, 235);
  }

  TerrainMaskPreview::TerrainMaskPreview(MapView* map_view, QWidget* parent)
    : QWidget(parent)
    , _map_view(map_view)
    , _timer(new QTimer(this))
  {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumSize(160, 160);
    setToolTip("The active mask around the cursor, seen from above. Dark is masked out; the"
               " mask's own colour is masked in. The two rings are the brush's outer radius and"
               " its hard core.");

    // 15 Hz. The loupe is a readout, not an animation: a slower rate is visibly laggy under a
    // moving cursor and a faster one buys nothing, because the terrain cursor itself is only
    // recomputed once per frame.
    _timer->setInterval(66);
    connect(_timer, &QTimer::timeout, this, [this] { refreshIfChanged(); });

    _field = QImage(SAMPLE_RESOLUTION, SAMPLE_RESOLUTION, QImage::Format_RGB32);
    _field.fill(Design::color(Design::BG_VOID));
  }

  void TerrainMaskPreview::setWindowYards(float yards)
  {
    if (yards > 0.0f && yards != _window_yards)
    {
      _window_yards = yards;
      markDirty();
    }
  }

  void TerrainMaskPreview::setBrush(float radius, float hardness)
  {
    if (radius != _brush_radius || hardness != _brush_hardness)
    {
      _brush_radius = radius;
      _brush_hardness = hardness;
      markDirty();
    }
  }

  void TerrainMaskPreview::setShowPaintOnly(bool paint_only)
  {
    if (paint_only != _paint_only)
    {
      _paint_only = paint_only;
      markDirty();
    }
  }

  void TerrainMaskPreview::markDirty()
  {
    _dirty = true;
  }

  float TerrainMaskPreview::valueUnderCursor() const
  {
    return _value_under_cursor;
  }

  QSize TerrainMaskPreview::sizeHint() const
  {
    // Square, and 224 rather than the panel's 250 px of usable content: the loupe sits inside a
    // section, and a widget that exactly fills the content width leaves the section's card edge
    // touching it on both sides.
    return QSize(224, 224);
  }

  void TerrainMaskPreview::showEvent(QShowEvent* event)
  {
    QWidget::showEvent(event);
    markDirty();
    _timer->start();
  }

  void TerrainMaskPreview::hideEvent(QHideEvent* event)
  {
    // ToolPanel::setCurrentTool hides every tool widget but the active one, so switching away
    // from the mask tool stops this timer and the loupe costs nothing at all while another tool
    // is in use.
    _timer->stop();
    QWidget::hideEvent(event);
  }

  void TerrainMaskPreview::refreshIfChanged()
  {
    glm::vec3 const cursor = _map_view->cursorPosition();

    bool const moved
      = std::fabs(cursor.x - _last_cursor.x) > CURSOR_MOVE_EPSILON
     || std::fabs(cursor.z - _last_cursor.z) > CURSOR_MOVE_EPSILON;

    if (!moved && !_dirty)
    {
      return;
    }

    _last_cursor = cursor;
    _dirty = false;

    update();
  }

  void TerrainMaskPreview::paintEvent(QPaintEvent* event)
  {
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    int const side = std::min(width(), height());
    QRect const box((width() - side) / 2, (height() - side) / 2, side, side);

    painter.fillRect(box, Design::color(Design::BG_VOID));

    NamedTerrainMask const* const mask = TerrainMaskStore::instance()->active();

    if (!mask)
    {
      painter.setPen(Design::color(Design::TEXT_OFF));
      painter.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, tr("No mask selected"));
      painter.setPen(Design::color(Design::STROKE));
      painter.drawRect(box.adjusted(0, 0, -1, -1));
      _value_under_cursor = -1.0f;
      return;
    }

    TerrainMask const& field = _paint_only ? mask->paint : mask->composited;

    glm::vec3 const cursor = _map_view->cursorPosition();

    float const half = _window_yards * 0.5f;
    float const origin_x = cursor.x - half;
    float const origin_z = cursor.z - half;
    float const yards_per_sample = _window_yards / static_cast<float>(SAMPLE_RESOLUTION);

    QColor const identity = maskIdentityColor(mask->name);
    QColor const empty = Design::color(Design::BG_VOID);

    for (int row = 0; row < SAMPLE_RESOLUTION; ++row)
    {
      // Screen Y downward is world Z; see the orientation note in the header.
      float const world_z = origin_z + (static_cast<float>(row) + 0.5f) * yards_per_sample;

      auto* const scanline = reinterpret_cast<QRgb*>(_field.scanLine(row));

      for (int column = 0; column < SAMPLE_RESOLUTION; ++column)
      {
        float const world_x = origin_x + (static_cast<float>(column) + 0.5f) * yards_per_sample;

        float const value = field.valueAt(world_x, world_z);

        // Lerped toward the mask's identity colour rather than to white, so two masks open in
        // two sessions do not look like the same picture. The floor is the panel's void colour
        // rather than pure black for the same reason every other well in this theme uses it.
        int const red = static_cast<int>(empty.red() + (identity.red() - empty.red()) * value);
        int const green = static_cast<int>(empty.green() + (identity.green() - empty.green()) * value);
        int const blue = static_cast<int>(empty.blue() + (identity.blue() - empty.blue()) * value);

        scanline[column] = qRgb(red, green, blue);
      }
    }

    painter.drawImage(box, _field);

    _value_under_cursor = field.valueAt(cursor.x, cursor.z);

    float const pixels_per_yard = static_cast<float>(side) / _window_yards;

    // --- The chunk grid ---
    //
    // A mask block is exactly one chunk (64 x 64 texels over MASK_CHUNK_SIZE yards), so these
    // lines are the storage grid as well as the terrain's. Seeing them is what tells a user that
    // a stroke crossed into a chunk the mask has nothing in yet.
    if (_window_yards <= CHUNK_GRID_MAX_WINDOW_YARDS)
    {
      painter.setPen(QPen(Design::color(Design::STROKE_SOFT), 1));

      float const first_chunk_x
        = std::ceil(origin_x / MASK_CHUNK_SIZE) * MASK_CHUNK_SIZE;
      float const first_chunk_z
        = std::ceil(origin_z / MASK_CHUNK_SIZE) * MASK_CHUNK_SIZE;

      for (float x = first_chunk_x; x < origin_x + _window_yards; x += MASK_CHUNK_SIZE)
      {
        int const screen_x = box.left() + static_cast<int>((x - origin_x) * pixels_per_yard);
        painter.drawLine(screen_x, box.top(), screen_x, box.bottom());
      }

      for (float z = first_chunk_z; z < origin_z + _window_yards; z += MASK_CHUNK_SIZE)
      {
        int const screen_y = box.top() + static_cast<int>((z - origin_z) * pixels_per_yard);
        painter.drawLine(box.left(), screen_y, box.right(), screen_y);
      }
    }

    // --- The brush ---
    //
    // Drawn from the same two numbers the stroke uses, so what is circled is what will change.
    // The inner ring is the hard core: inside it the stroke applies at full strength, and between
    // the rings it feathers linearly, which is the profile TerrainMask::paintCircle produces and
    // the one Brush::getValue produces for the texture brush.
    QPointF const centre(box.center());

    float const outer = _brush_radius * pixels_per_yard;
    float const inner = outer * std::clamp(_brush_hardness, 0.0f, 1.0f);

    painter.setPen(QPen(Design::color(Design::TEXT_HI), 1.5));
    painter.drawEllipse(centre, outer, outer);

    if (inner > 1.0f)
    {
      painter.setPen(QPen(Design::color(Design::TEXT_DIM), 1.0, Qt::DashLine));
      painter.drawEllipse(centre, inner, inner);
    }

    // --- The unbaked warning ---
    //
    // TerrainMaskStore::factorAt returns 1.0 -- fully unmasked -- on a tile the active mask has
    // never been baked over, and it does that deliberately, so an unbaked tile is silent rather
    // than clipping everything. Silent is the right failure for a brush and the wrong one for the
    // panel that is supposed to explain the mask, so the loupe says it out loud.
    // The bounds test comes BEFORE the cast, not after. A cursor that missed the terrain arrives
    // as NaN -- the case TerrainMaskQuery.hpp names -- and NaN fails every ordered comparison, so
    // this rejects it; casting it to int first would be undefined rather than merely wrong.
    bool const on_map = cursor.x >= 0.0f && cursor.z >= 0.0f;

    if (on_map
     && !mask->tileIsBaked( static_cast<int>(cursor.x / MASK_TILE_SIZE)
                          , static_cast<int>(cursor.z / MASK_TILE_SIZE)
                          ))
    {
      QRect const banner(box.left(), box.bottom() - 18, box.width(), 18);
      painter.fillRect(banner, Design::color(Design::FILL_WARN));
      painter.setPen(Design::color(Design::WARN));
      painter.drawText(banner, Qt::AlignCenter, tr("tile not baked - stroke will not clip yet"));
    }

    painter.setPen(Design::color(Design::STROKE));
    painter.drawRect(box.adjusted(0, 0, -1, -1));
  }
}
