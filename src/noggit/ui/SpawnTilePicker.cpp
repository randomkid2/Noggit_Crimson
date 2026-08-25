// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/SpawnTilePicker.hpp>

#include <noggit/ui/DesignTokens.hpp>
#include <noggit/World.h>

#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>

#include <algorithm>

using namespace Noggit::Ui;

namespace
{
  // Where a tile's marker sits inside its cell, in pixels, and the smallest cell at which each
  // marker is still worth drawing.
  //
  // The base class paints the red selection outline inset by one pixel (minimap_widget.cpp:262),
  // so the "loaded" outline is inset by two to sit inside it rather than on top of it. Below a
  // five pixel cell there is no room for a second outline at all, at which point it is skipped
  // rather than drawn as a solid blob that would read as a different state entirely.
  constexpr int SELECTION_INSET = 1;
  constexpr int LOADED_INSET = 2;
  constexpr int MIN_CELL_FOR_LOADED_OUTLINE = 5;

  // Spawn count at which the density tint stops getting darker. Picked so an ordinary populated
  // tile is clearly tinted and a city tile is not indistinguishable from it.
  constexpr std::uint32_t DENSITY_SATURATION_COUNT = 300;
  constexpr int DENSITY_MIN_ALPHA = 45;
  constexpr int DENSITY_MAX_ALPHA = 195;

  // Every colour on this grid, and all four are now design-system tokens rather than the
  // hand-mixed values they were. The meanings did not move; the hexes did.
  //
  //   density tint       was (0, 190, 255)   -> INFO   -- neutral quantity, not a status
  //   loaded AND drawn   was (60, 230, 90)   -> OK     -- what you asked for is on screen
  //   loaded, NOT drawn  was (255, 170, 40)  -> WARN   -- cached but nothing renders it
  //   selection outline  was (255, 40, 40)   -> ACCENT -- "the thing you are acting on"
  //
  // The selection is the one that mattered. It was a near-pure red, which under the previous
  // scheme was also the accent, the error colour and the destructive colour -- so the outline
  // around the tiles you had deliberately chosen looked exactly like a warning about them.
  // ACCENT against BG_VOID is 8.77:1 where that red managed 5.13:1, so it is both unambiguous
  // and brighter than what it replaces.
  //
  // The alpha stays where it was on all three markers: it is what lets the density tint show
  // through, and it is not a colour decision.
  //
  // Two hues rather than two alphas of the same one for the loaded pair, because the grid is
  // already tinted by density and a brightness difference over a variable tint is not a
  // difference anybody can read. OK and WARN are 106.5 and 15.8 degrees off the accent
  // respectively, and WARN is separated from the accent by FORM here -- it is a filled dot or a
  // small inset outline, never the selection's full-cell rectangle.
  constexpr int LOADED_ALPHA = 235;

  QColor loadedDrawnColour()
  {
    QColor colour (Noggit::Ui::Design::color (Noggit::Ui::Design::OK));
    colour.setAlpha (LOADED_ALPHA);
    return colour;
  }

  QColor loadedNotDrawnColour()
  {
    QColor colour (Noggit::Ui::Design::color (Noggit::Ui::Design::WARN));
    colour.setAlpha (LOADED_ALPHA);
    return colour;
  }

  QColor densityColour (int alpha)
  {
    QColor colour (Noggit::Ui::Design::color (Noggit::Ui::Design::INFO));
    colour.setAlpha (alpha);
    return colour;
  }

  // Buffer index for an ADT tile. The one place `64 * adt_x + adt_z` is written in this file, so
  // the base class's convention (minimap_widget.cpp:49, :60, :259) is stated once.
  std::size_t bufferIndex(int adt_x, int adt_z)
  {
    return static_cast<std::size_t>(64 * adt_x + adt_z);
  }
}

SpawnTilePicker::SpawnTilePicker(QWidget* parent)
  : minimap_widget(parent)
  , _selection(TILE_BUFFER_SIZE, 0)
{
  // Bound after _selection is sized, never before: the base dereferences the pointer on the next
  // paint and indexes it with at().
  use_selection(&_selection);

  // The base paints the selection from inside its `if (draw_boundaries())` branch
  // (minimap_widget.cpp:205-271), so with boundaries off a picked tile is simply invisible. The
  // member is uninitialised in the base constructor, which is a second reason to set it here.
  draw_boundaries(true);
  draw_skies(false);

  // 64 cells across, so a cell is width/64. Below 256 the cells are three pixels and the overlay
  // markers stop being distinguishable from the grid lines.
  setMinimumSize(QSize(256, 256));

  // Connected after the base's own handlers, which Qt invokes in connection order -- so by the
  // time these run the buffer already holds the new state and a listener can read it directly.
  connect(this, &minimap_widget::tile_clicked, this, [this] (QPoint const&)
    {
      emit selectionChanged();
    });

  connect(this, &minimap_widget::reset_selection, this, [this]
    {
      // update() as well as the signal: the base memsets the buffer from its own handler but
      // mousePressEvent returns straight after emitting for a right click (minimap_widget.cpp:
      // 398-404), so nothing repaints and the cleared tiles stay drawn until something else
      // happens to trigger a paint.
      update();
      emit selectionChanged();
    });
}

std::vector<::TileIndex> SpawnTilePicker::selectedAdtTiles() const
{
  // adt_x / adt_z, not x / y: ADT-filename order throughout. See the frame note on the class.
  std::vector<::TileIndex> tiles;

  for (int adt_x = 0; adt_x < 64; ++adt_x)
  {
    for (int adt_z = 0; adt_z < 64; ++adt_z)
    {
      if (_selection[bufferIndex(adt_x, adt_z)])
      {
        tiles.emplace_back(static_cast<std::size_t>(adt_x), static_cast<std::size_t>(adt_z));
      }
    }
  }

  return tiles;
}

std::size_t SpawnTilePicker::selectedCount() const
{
  return static_cast<std::size_t>
    (std::count_if(_selection.begin(), _selection.end(), [] (char c) { return c != 0; }));
}

void SpawnTilePicker::clearSelection()
{
  std::fill(_selection.begin(), _selection.end(), 0);
  update();
  emit selectionChanged();
}

void SpawnTilePicker::selectLoadedTiles()
{
  auto const* current_world = world();

  if (!current_world)
  {
    return;
  }

  for (int adt_x = 0; adt_x < 64; ++adt_x)
  {
    for (int adt_z = 0; adt_z < 64; ++adt_z)
    {
      // ::TileIndex(x, z), the same order the base class indexes and paints in.
      ::TileIndex const adt_tile
        (static_cast<std::size_t>(adt_x), static_cast<std::size_t>(adt_z));

      if (current_world->mapIndex.tileLoaded(adt_tile))
      {
        _selection[bufferIndex(adt_x, adt_z)] = 1;
      }
    }
  }

  update();
  emit selectionChanged();
}

void SpawnTilePicker::setSpawnCounts(std::vector<std::uint32_t> counts)
{
  // A short buffer would be read out of range while painting. Anything that is not exactly the
  // grid is treated as "no scan" rather than padded, because a partially filled overlay reads as
  // "those tiles are empty", which is the one wrong answer this marker exists to avoid.
  _spawn_counts = counts.size() == TILE_BUFFER_SIZE ? std::move(counts)
                                                    : std::vector<std::uint32_t>();
  update();
}

std::uint64_t SpawnTilePicker::scannedSpawnsInSelection() const
{
  if (_spawn_counts.empty())
  {
    return 0;
  }

  std::uint64_t total = 0;

  for (std::size_t index = 0; index < TILE_BUFFER_SIZE; ++index)
  {
    if (_selection[index])
    {
      total += _spawn_counts[index];
    }
  }

  return total;
}

void SpawnTilePicker::setLoadedTiles(std::vector<TileLoadState> states)
{
  // Same rule as setSpawnCounts: a buffer that is not exactly the grid is dropped rather than
  // padded, because the padding would be drawn as a state and read as a fact.
  _loaded = states.size() == TILE_BUFFER_SIZE ? std::move(states)
                                              : std::vector<TileLoadState>();
  update();
}

void SpawnTilePicker::paintEvent(QPaintEvent* paint_event)
{
  // The base paints the minimap image, the tile grid, the unsaved-ADT marker and the red
  // selection outline. Its QPainter is constructed and destroyed inside that call, so the one
  // below is sequential rather than nested.
  minimap_widget::paintEvent(paint_event);

  if (!world())
  {
    return;
  }

  // Identical geometry to minimap_widget::paintEvent:187-190, deliberately: the overlay has to
  // land on the same cells the base drew, and it is a two-line calculation rather than something
  // worth exposing from the base.
  int const smaller_side ((qMin(rect().width(), rect().height()) / 64) * 64);
  int const cell (smaller_side / 64);

  if (cell <= 0)
  {
    return;
  }

  QPainter painter (this);

  // --- spawns per tile, from the last map scan ----------------------------------------------
  // Drawn as a fill rather than a dot so density is legible at a glance; drawn first so the two
  // outlines below sit on top of it.
  if (!_spawn_counts.empty())
  {
    for (int adt_x = 0; adt_x < 64; ++adt_x)
    {
      for (int adt_z = 0; adt_z < 64; ++adt_z)
      {
        std::uint32_t const count (_spawn_counts[bufferIndex(adt_x, adt_z)]);

        if (count == 0)
        {
          continue;
        }

        int const alpha
          ( DENSITY_MIN_ALPHA
          + static_cast<int>
              ( static_cast<std::uint64_t>(std::min(count, DENSITY_SATURATION_COUNT))
              * (DENSITY_MAX_ALPHA - DENSITY_MIN_ALPHA) / DENSITY_SATURATION_COUNT
              )
          );

        painter.fillRect
          ( QRect(cell * adt_x + 1, cell * adt_z + 1, cell - 1, cell - 1)
          , densityColour(alpha)
          );
      }
    }
  }

  // --- tiles whose spawns are loaded right now ----------------------------------------------
  // Two colours, not one. Green means the spawns are in the cache AND the ADT is streamed, so
  // they are on screen; amber means they are in the cache and nothing draws them, because the
  // renderer only visits tiles it is already walking. See TileLoadState on why conflating the two
  // is the defect this exists to fix -- a solid green 8x8 block over an empty viewport.
  if (!_loaded.empty())
  {
    // An outline inside the base's selection outline where there is room for one, and a solid
    // dot where there is not. Drawing a two-pixel-inset rectangle into a four-pixel cell yields a
    // zero-width rectangle, which Qt renders as a line -- a third state the user has no way to
    // read. A panel narrow enough to hit that is unusual but not preventable from here.
    bool const outline (cell >= MIN_CELL_FOR_LOADED_OUTLINE);

    for (int adt_x = 0; adt_x < 64; ++adt_x)
    {
      for (int adt_z = 0; adt_z < 64; ++adt_z)
      {
        TileLoadState const state (_loaded[bufferIndex(adt_x, adt_z)]);

        if (state == TileLoadState::NONE)
        {
          continue;
        }

        QColor const colour
          ( state == TileLoadState::DRAWN ? loadedDrawnColour() : loadedNotDrawnColour());

        painter.setBrush(outline ? QBrush(Qt::NoBrush) : QBrush(colour));
        painter.setPen(colour);

        if (outline)
        {
          painter.drawRect
            ( QRect( cell * adt_x + LOADED_INSET
                   , cell * adt_z + LOADED_INSET
                   , cell - 2 * LOADED_INSET
                   , cell - 2 * LOADED_INSET
                   )
            );
        }
        else
        {
          painter.fillRect(QRect(cell * adt_x + 1, cell * adt_z + 1, 2, 2), colour);
        }
      }
    }
  }

  // --- the selection, again ------------------------------------------------------------------
  // The base already drew this, but the density fill above went over the top of it and the
  // selection is the one thing the user is actively manipulating. Redrawing it last costs one
  // pass over the grid and keeps it readable over a dark tint.
  painter.setBrush(Qt::NoBrush);
  painter.setPen(Noggit::Ui::Design::color(Noggit::Ui::Design::ACCENT));

  for (int adt_x = 0; adt_x < 64; ++adt_x)
  {
    for (int adt_z = 0; adt_z < 64; ++adt_z)
    {
      if (!_selection[bufferIndex(adt_x, adt_z)])
      {
        continue;
      }

      painter.drawRect
        ( QRect( cell * adt_x + SELECTION_INSET
               , cell * adt_z + SELECTION_INSET
               , cell - 2 * SELECTION_INSET
               , cell - 2 * SELECTION_INSET
               )
        );
    }
  }
}
