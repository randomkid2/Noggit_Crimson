// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/ErosionToolSettings.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/ErosionKernel.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <noggit/ui/FontNoggit.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>

#include <QtGui/QIcon>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <cstddef>
#include <string_view>

namespace
{
  // How far apart the kernel samples the terrain, in world units.
  //
  // UNITSIZE, and NOT UNITSIZE/2, because the sampler does not interpolate. World::GetVertex ends
  // in MapChunk::GetVertex (MapChunk.cpp:396-410), which SNAPS to the nearest vertex: rows are
  // UNITSIZE/2 apart in z, but within one row the vertices are a full UNITSIZE apart in x
  // (MapChunk.cpp:90, xpos = i * UNITSIZE). Half-unit sampling therefore catches the inner rows in
  // z and nothing whatever in x -- two lattice columns land on the same terrain column and return
  // the same height, so the sampled surface is a stair whose x differences alternate 0, d, 0, d.
  // The non-zero ones report twice the real tangent, which makes a 30-degree slope look like 50 to
  // a 45-degree repose setting and terraces terrain the tool documents as a fixed point. The
  // half-unit x offset of the odd rows (MapChunk.cpp:93) staggers which column is duplicated from
  // row to row, which manufactures z-edges on terrain with no z-slope at all.
  //
  // At UNITSIZE consecutive lattice columns are consecutive terrain columns and consecutive
  // lattice rows are two terrain rows apart, so every sample is a distinct vertex of one parity
  // and no edge length is misreported. The interleaved rows are then reached only through the
  // interpolated delta rather than sampled directly; that is the smooth half of the surface and
  // losing it costs nothing the erosion was measuring. It is also four times fewer cells.
  //
  // MapChunk::blurTerrain avoids the same trap by sampling the brick pattern instead of a square
  // lattice (MapChunk.cpp:1139-1142). That option is open here too, but it would need an
  // interpolating height query on World, which does not exist yet.
  constexpr float EROSION_CELL_SIZE = UNITSIZE;

  // How far past the brush radius a vertex can still be moved.
  //
  // The correction is applied by bilinear interpolation (ErosionCollector::applyToVertices ->
  // ErosionDeltaField::correctionAt), so a vertex reads the four lattice cells around it. A cell
  // only carries a non-zero delta if it is inside the brush -- under GateFlow its influence is
  // zero at and beyond the radius, under FadeResult the fade multiplies it away -- so the furthest
  // a moved vertex can be from the centre is one cell diagonally beyond the last cell inside the
  // disc: radius + sqrt(2) * cell_size. See the for_all_chunks_in_range call in erode().
  constexpr float EROSION_APPLY_MARGIN = EROSION_CELL_SIZE * 1.41421356237309504880f;

  // The brush is capped well below the 1000 the other terrain brushes allow.
  //
  // Erosion is not a per-vertex filter: it relaxes a whole lattice, and the lattice grows with the
  // SQUARE of the radius while the iteration count multiplies on top. At 200 yards and a cell of
  // UNITSIZE the lattice is 99x99, so a default 8-iteration tick is around 300k edge visits; at
  // 1000 it would be 483x483, 24 times the cells, every frame, for a stroke the user is holding
  // down. MAX_LATTICE_HALF_EXTENT would still refuse to allocate long before anything overflowed,
  // but refusing at the slider is friendlier than a brush that silently stops working past some
  // radius.
  constexpr double MAX_BRUSH_RADIUS = 200.0;

  QString describeRun(Noggit::ErosionStats const& stats)
  {
    if (!stats.ok())
    {
      std::string_view const text = Noggit::erosionStatusText(stats.status);

      return QString("no run: %1").arg(QString::fromUtf8(text.data()
                                                        , static_cast<int>(text.size())));
    }

    if (stats.iterations_run == 0)
    {
      // Not a failure. A surface already at or below the angle of repose is the kernel's fixed
      // point, so a converged patch re-eroded moves nothing and says so -- that is the idempotence
      // the header promises, visible in the panel.
      return QString("at rest (nothing above %1 unstable edges)")
        .arg(stats.unstable_edges_before);
    }

    return QString("slope %1 -> %2 tan | unstable %3 -> %4 | moved %5 | drift %6")
      .arg(stats.max_slope_before, 0, 'f', 2)
      .arg(stats.max_slope_after, 0, 'f', 2)
      .arg(stats.unstable_edges_before)
      .arg(stats.unstable_edges_after)
      .arg(stats.material_moved, 0, 'g', 3)
      .arg(stats.conservationError(), 0, 'g', 3);
  }
}

namespace Noggit
{
  namespace Ui
  {
    ErosionToolSettings::ErosionToolSettings(QWidget* parent)
      : QWidget(parent)
    {
      setMinimumWidth(250);

      auto layout(new QVBoxLayout(this));

      // Keybind hint, in the shape flatten_blur_tool uses for the same purpose.
      auto keybind_layout(new QHBoxLayout);
      keybind_layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
      keybind_layout->setContentsMargins(0, 0, 0, 0);
      keybind_layout->setSpacing(2);

      auto keybind_shift(new QLabel(this));
      keybind_shift->setPixmap(QIcon(FontNoggitIcon(FontNoggit::shift)).pixmap(20, 20));
      keybind_layout->addWidget(keybind_shift);

      keybind_layout->addWidget(new QLabel("+"));

      auto keybind_lmb(new QLabel(this));
      keybind_lmb->setPixmap(QIcon(FontNoggitIcon(FontNoggit::lmb)).pixmap(20, 20));
      keybind_layout->addWidget(keybind_lmb);

      keybind_layout->addWidget(new QLabel("Erode Terrain"));

      auto keybind_container(new QWidget(this));
      keybind_container->setLayout(keybind_layout);
      keybind_container->setFixedHeight(25);
      layout->addWidget(keybind_container);

      auto brush_group(new QGroupBox("Brush", this));
      brush_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      auto brush_layout(new QVBoxLayout(brush_group));

      _radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _radius_slider->setPrefix("Radius:");
      _radius_slider->setRange(0.0, MAX_BRUSH_RADIUS);
      _radius_slider->setDecimals(2);
      _radius_slider->setValue(15.0);
      brush_layout->addWidget(_radius_slider);

      _inner_radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _inner_radius_slider->setPrefix("Inner Radius:");
      _inner_radius_slider->setRange(0.0, 1.0);
      _inner_radius_slider->setDecimals(2);
      _inner_radius_slider->setSingleStep(0.05);
      // Not 0, unlike the raise/lower brush. A hard-edged erosion disc leaves a rim: gated flow
      // piles material just inside the boundary and the fade cuts the correction off in one step.
      // Both artefacts spread out into something invisible as soon as there is a falloff band.
      _inner_radius_slider->setValue(0.6);
      brush_layout->addWidget(_inner_radius_slider);

      layout->addWidget(brush_group);

      auto erosion_group(new QGroupBox("Erosion", this));
      erosion_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      auto erosion_layout(new QVBoxLayout(erosion_group));

      _repose_angle_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _repose_angle_slider->setPrefix("Angle of Repose:");
      _repose_angle_slider->setRange(0.0, MAX_REPOSE_ANGLE_DEGREES);
      _repose_angle_slider->setDecimals(1);
      _repose_angle_slider->setValue(45.0);
      erosion_layout->addWidget(_repose_angle_slider);

      _strength_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _strength_slider->setPrefix("Strength:");
      // The kernel clamps to MAX_STABLE_STRENGTH anyway; letting the slider go past it would only
      // offer the user a setting that does nothing.
      _strength_slider->setRange(0.0, MAX_STABLE_STRENGTH);
      _strength_slider->setDecimals(2);
      _strength_slider->setSingleStep(0.05);
      _strength_slider->setValue(MAX_STABLE_STRENGTH);
      erosion_layout->addWidget(_strength_slider);

      auto parameter_layout(new QFormLayout);

      _iterations_spin = new QSpinBox(this);
      _iterations_spin->setRange(1, 256);
      _iterations_spin->setValue(8);
      _iterations_spin->setToolTip("Relaxation passes per tick. More converges sooner, not"
                                   " further -- the fixed point is the same.");
      parameter_layout->addRow("Iterations:", _iterations_spin);

      _edge_mode_combo = new QComboBox(this);
      // The mode is carried in the item's DATA, never inferred from its row. Reading an enum back
      // out of a combo box by index is the same defect as reading a selection out of a list widget
      // by currentRow(): it survives exactly until someone reorders or filters the entries.
      _edge_mode_combo->addItem("Conserve material (gate)"
                               , static_cast<int>(ErosionEdgeMode::GateFlow));
      _edge_mode_combo->addItem("Clean edge (fade)"
                               , static_cast<int>(ErosionEdgeMode::FadeResult));
      _edge_mode_combo->setToolTip("Gate keeps every grain on the map and can leave a rim at the"
                                   " brush edge. Fade keeps the edge clean and does not deposit"
                                   " what it scaled away.");
      parameter_layout->addRow("Edge:", _edge_mode_combo);

      erosion_layout->addLayout(parameter_layout);
      layout->addWidget(erosion_group);

      // The panel reports what the last tick measured rather than asserting that erosion happened.
      // Conservation in particular is a claim the kernel makes about itself; this is where it is
      // checkable without a debugger.
      _evidence_label = new QLabel("no stroke yet", this);
      _evidence_label->setWordWrap(true);
      _evidence_label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      layout->addWidget(_evidence_label);
    }

    void ErosionToolSettings::erode(World* world, glm::vec3 const& cursor_pos)
    {
      float const radius = brushRadius();

      if (radius <= 0.0f)
      {
        return;
      }

      float const inner_radius = radius * innerRadiusRatio();

      ErosionSettings settings;
      settings.repose_angle_degrees = static_cast<float>(_repose_angle_slider->value());
      settings.strength = static_cast<float>(_strength_slider->value());
      settings.iterations = _iterations_spin->value();
      settings.cell_size = EROSION_CELL_SIZE;

      auto const lattice = latticeCovering(cursor_pos.x, cursor_pos.z, radius, settings.cell_size);

      // World.h stays out of ErosionKernel.hpp, so the wrapper lives here. GetVertex answering
      // false means "no terrain at this world position" -- an unloaded tile or a hole -- and
      // sampleLattice turns that into influence 0 rather than a height of zero, which is why a
      // brush straddling the edge of the loaded region does not carve a cliff there.
      auto sample = [world](float x, float z, float& out) -> bool
      {
        glm::vec3 vertex;

        if (!world->GetVertex(x, z, &vertex))
        {
          return false;
        }

        out = vertex.y;

        return true;
      };

      auto const edge_mode = static_cast<ErosionEdgeMode>(_edge_mode_combo->currentData().toInt());

      auto const run = ErosionCollector::erodeRegion( lattice
                                                    , settings
                                                    , sample
                                                    , cursor_pos.x
                                                    , cursor_pos.z
                                                    , radius
                                                    , inner_radius
                                                    , edge_mode
                                                    );

      _evidence_label->setText(describeRun(run.stats));

      // Nothing moved: either the run was refused or the surface is already at rest. Returning
      // before for_all_chunks_in_range matters, because that helper marks every tile it touches
      // changed through mapIndex.setChanged, and a brush held over settled terrain would otherwise
      // keep flagging tiles dirty forever.
      if (!run.stats.ok() || run.stats.iterations_run == 0 || run.field.empty())
      {
        return;
      }

      // radius + EROSION_APPLY_MARGIN, not radius. Every other terrain brush may walk the bare
      // radius because it moves only vertices with dist < radius (MapChunk::changeTerrain,
      // MapChunk::blurTerrain), so a moved vertex proves its chunk is in range. Erosion applies an
      // INTERPOLATED delta, which reaches up to one lattice cell past the disc, and the chunk walk
      // tests the distance to the chunk SQUARE (misc::getShortestDist, MapTile.cpp:497). Walking
      // the bare radius therefore lets a vertex on a chunk seam move in the chunk that was visited
      // and stay put in the neighbour that was not -- and both chunks keep their own copy of that
      // shared vertex, so the result is a tear that survives the save. Invisible at the default
      // 0.6 inner ratio, where the rim correction is second-order small; a hardened brush (the
      // slider goes to 1.0, and applyRadialInfluence documents a hard edge as legal) makes it a
      // vertical crack. Chunks that gain nothing return moved == 0, so the extra ones neither mark
      // their tile changed nor recalculate normals; the cost is a few more undo snapshots, plus
      // the odd adjacent ADT being loaded (MapIndex::tiles_in_range loads what it yields) -- which
      // is the whole point, because that tile holds the other copy of the seam vertex.
      world->for_all_chunks_in_range
        ( cursor_pos, radius + EROSION_APPLY_MARGIN
        , [&](MapChunk* chunk)
          {
            // Before the first vertex moves: the action stores a copy of mVertices the first time
            // it sees a chunk and ignores every later call for it (Action.cpp:655-664), which is
            // what makes a whole stroke -- many ticks, one action -- a single undo step.
            NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);

            std::size_t const moved = ErosionCollector::applyToVertices( run.field
                                                                       , chunk->mVertices
                                                                       , mapbufsize
                                                                       );

            if (moved == 0)
            {
              return false;
            }

            // No recalcExtents here, deliberately: updateVerticesData recomputes vmin.y/vmax.y
            // itself when this flag is set (MapChunk.cpp:643-658), and the other terrain brushes
            // rely on exactly that.
            chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);

            return true;
          }
        , [world](MapChunk* chunk)
          {
            world->recalc_norms(chunk);
          }
        );
    }

    void ErosionToolSettings::changeRadius(float change)
    {
      _radius_slider->setValue(_radius_slider->value() + change);
    }

    void ErosionToolSettings::changeInnerRadius(float change)
    {
      _inner_radius_slider->setValue(_inner_radius_slider->value() + change);
    }

    float ErosionToolSettings::brushRadius() const
    {
      return static_cast<float>(_radius_slider->value());
    }

    float ErosionToolSettings::innerRadiusRatio() const
    {
      return static_cast<float>(_inner_radius_slider->value());
    }

    QSize ErosionToolSettings::sizeHint() const
    {
      return QSize(250, height());
    }
  }
}
