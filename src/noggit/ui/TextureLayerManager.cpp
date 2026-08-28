// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/TextureLayerManager.hpp>

#include <noggit/MapView.h>
#include <noggit/TileIndex.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/ui/WaitCursor.hpp>
#include <opengl/context.hpp>
#include <opengl/scoped.hpp>

#include <ClientData.hpp>

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

namespace Noggit::Ui
{
  namespace
  {
    // The longest palette Prepare Area can be asked for, and it is not a tunable: it is the number
    // of MCLY entries in an MCNK.
    constexpr int MAX_PALETTE_ENTRIES = 4;

    QString plural(std::size_t count, char const* singular, char const* many)
    {
      return QString("%1 %2").arg(count).arg(count == 1 ? singular : many);
    }
  }

  TextureLayerManager::TextureLayerManager(MapView* map_view, QWidget* parent)
    : QDialog(parent)
    , _map_view(map_view)
  {
    setWindowTitle("Texture Layers");
    setWindowFlags(Qt::Tool);

    auto* layout = new QVBoxLayout(this);

    layout->addWidget(buildScopeGroup());
    layout->addWidget(buildReplacementGroup());
    layout->addWidget(buildPrepareGroup());
    layout->addWidget(buildHygieneGroup());

    _status = new QLabel("Ready.", this);
    _status->setWordWrap(true);
    layout->addWidget(_status);

    layout->addStretch(1);
  }

  QGroupBox* TextureLayerManager::buildScopeGroup()
  {
    auto* group = new QGroupBox("Scope", this);
    auto* form = new QFormLayout(group);

    _scope_combo = new QComboBox(group);
    // Order matters: the index is cast straight to LayerOpScope, whose enumerators are 0, 1, 2 in
    // this order.
    _scope_combo->addItem("Brush radius");
    _scope_combo->addItem("Chunk under the cursor");
    _scope_combo->addItem("ADT tile under the cursor");
    form->addRow("Apply to:", _scope_combo);

    _radius_spin = new QDoubleSpinBox(group);
    _radius_spin->setRange(1.0, 1000.0);
    _radius_spin->setDecimals(1);
    _radius_spin->setValue(15.0);
    form->addRow("Radius:", _radius_spin);

    auto* note = new QLabel
      ( "Everything here works at the terrain cursor -- the position the brush would paint at, not "
        "the camera. Each button is one undo step for every chunk it touches."
      , group
      );
    note->setWordWrap(true);
    form->addRow(note);

    connect(_scope_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] (int index)
      {
        _radius_spin->setEnabled(index == static_cast<int>(Noggit::LayerOpScope::Brush));
      });

    return group;
  }

  QGroupBox* TextureLayerManager::buildReplacementGroup()
  {
    auto* group = new QGroupBox("Layer replacement", this);
    auto* form = new QFormLayout(group);

    _slot_spin = new QSpinBox(group);
    _slot_spin->setRange(0, MAX_PALETTE_ENTRIES - 1);
    form->addRow("Slot:", _slot_spin);

    _replacement_texture = new QLineEdit(group);
    _replacement_texture->setReadOnly(true);
    _replacement_texture->setPlaceholderText("no texture chosen");
    form->addRow("Texture:", _replacement_texture);

    auto* use_selected = new QPushButton("Use the selected texture", group);
    form->addRow(use_selected);

    _alpha_handling = new QComboBox(group);
    // Same rule as the scope combo: the index is the LayerAlphaHandling value.
    _alpha_handling->addItem("Keep this layer's alpha (straight substitution)");
    _alpha_handling->addItem("Reset this layer's alpha (load it in blank)");
    form->addRow("Alpha:", _alpha_handling);

    auto* warning = new QLabel
      ( "Slot 0 has no stored alpha of its own -- it is 255 minus the layers above it -- so "
        "resetting slot 0 clears every overlay and leaves the chunk showing only its base texture."
      , group
      );
    warning->setWordWrap(true);
    form->addRow(warning);

    auto* apply = new QPushButton("Replace layer", group);
    form->addRow(apply);

    connect(use_selected, &QPushButton::clicked, this, [this]
      {
        std::string const path = selectedTexturePath();

        if (path.empty())
        {
          _status->setText("No texture is selected in the texturing tool.");
          return;
        }

        _replacement_texture->setText(QString::fromStdString(path));
      });

    connect(apply, &QPushButton::clicked, this, [this]
      {
        std::string const path = _replacement_texture->text().toStdString();
        std::size_t const slot = static_cast<std::size_t>(_slot_spin->value());
        auto const handling
          = static_cast<Noggit::LayerAlphaHandling>(_alpha_handling->currentIndex());

        runOperation([this, path, slot, handling]
          {
            return Noggit::TextureLayerOps::replaceLayer
              (_map_view, currentScope(), slot, path, handling);
          });
      });

    return group;
  }

  QGroupBox* TextureLayerManager::buildPrepareGroup()
  {
    auto* group = new QGroupBox("Prepare area", this);
    auto* box = new QVBoxLayout(group);

    _clear_overlays = new QCheckBox("Clear overlays back to the base texture", group);
    box->addWidget(_clear_overlays);

    box->addWidget(new QLabel("Palette to guarantee room for (max 4):", group));

    _palette = new QListWidget(group);
    _palette->setMaximumHeight(90);
    box->addWidget(_palette);

    auto* palette_buttons = new QHBoxLayout();
    auto* add = new QPushButton("Add selected texture", group);
    auto* remove = new QPushButton("Remove", group);
    palette_buttons->addWidget(add);
    palette_buttons->addWidget(remove);
    box->addLayout(palette_buttons);

    _evict_to_fit = new QCheckBox("Evict the least visible layer when a texture will not fit", group);
    box->addWidget(_evict_to_fit);

    auto* note = new QLabel
      ( "With eviction off this pass cannot destroy a layer: it only adds to chunks that had a "
        "free slot, and counts the rest as refused. Added layers start at zero alpha, so nothing "
        "changes on screen until you paint them -- and an unpainted one is dropped again by the "
        "next stroke's own cleanup, so treat the palette as a check that it fits rather than a "
        "permanent reservation. Clearing the overlays is permanent."
      , group
      );
    note->setWordWrap(true);
    box->addWidget(note);

    auto* apply = new QPushButton("Prepare area", group);
    box->addWidget(apply);

    connect(add, &QPushButton::clicked, this, [this]
      {
        if (_palette->count() >= MAX_PALETTE_ENTRIES)
        {
          _status->setText("A chunk holds four texture layers; the palette is full.");
          return;
        }

        std::string const path = selectedTexturePath();

        if (path.empty())
        {
          _status->setText("No texture is selected in the texturing tool.");
          return;
        }

        QString const entry = QString::fromStdString(path);

        // Both sides of this comparison are already unix-normalised by selectedTexturePath, so a
        // texture cannot get into the palette twice under two spellings and waste one of the four
        // slots the pass is trying to reserve.
        for (int row = 0; row < _palette->count(); ++row)
        {
          if (_palette->item(row)->text() == entry)
          {
            _status->setText("That texture is already in the palette.");
            return;
          }
        }

        _palette->addItem(entry);
      });

    connect(remove, &QPushButton::clicked, this, [this]
      {
        delete _palette->takeItem(_palette->currentRow());
      });

    connect(apply, &QPushButton::clicked, this, [this]
      {
        Noggit::PrepareAreaRequest request;
        request.clear_overlays = _clear_overlays->isChecked();
        request.evict_to_fit = _evict_to_fit->isChecked();

        for (int row = 0; row < _palette->count(); ++row)
        {
          request.palette.push_back(_palette->item(row)->text().toStdString());
        }

        runOperation([this, request]
          {
            return Noggit::TextureLayerOps::prepareArea(_map_view, currentScope(), request);
          });
      });

    return group;
  }

  QGroupBox* TextureLayerManager::buildHygieneGroup()
  {
    auto* group = new QGroupBox("Layer hygiene", this);
    auto* box = new QVBoxLayout(group);

    _purge_duplicates = new QCheckBox("Texture duplicates", group);
    _purge_duplicates->setChecked(true);
    _purge_duplicates->setToolTip
      ( "Two slots holding the same BLP draw identically and cost two of the chunk's four layers. "
        "The duplicate's alpha is folded into the layer that survives, so nothing disappears."
      );
    box->addWidget(_purge_duplicates);

    _purge_threshold = new QCheckBox("Textures below threshold", group);
    _purge_threshold->setToolTip
      ( "Removes a layer whose strongest alpha anywhere on its chunk is at most this value. At 0 "
        "that is exactly the layers that are invisible everywhere."
      );
    box->addWidget(_purge_threshold);

    auto* threshold_row = new QHBoxLayout();
    threshold_row->addWidget(new QLabel("Threshold (0-255):", group));
    _threshold_spin = new QSpinBox(group);
    _threshold_spin->setRange(0, 255);
    _threshold_spin->setValue(0);
    _threshold_spin->setEnabled(false);
    threshold_row->addWidget(_threshold_spin);
    threshold_row->addStretch(1);
    box->addLayout(threshold_row);

    auto* note = new QLabel
      ( "A chunk is never emptied: if every layer is under the threshold, the one covering the "
        "most ground is kept."
      , group
      );
    note->setWordWrap(true);
    box->addWidget(note);

    auto* apply = new QPushButton("Clear layers", group);
    box->addWidget(apply);

    connect(_purge_threshold, &QCheckBox::toggled, _threshold_spin, &QSpinBox::setEnabled);

    connect(apply, &QPushButton::clicked, this, [this]
      {
        bool const duplicates = _purge_duplicates->isChecked();
        bool const threshold = _purge_threshold->isChecked();

        if (!duplicates && !threshold)
        {
          _status->setText("Nothing ticked.");
          return;
        }

        auto const value = static_cast<std::uint8_t>(_threshold_spin->value());

        runOperation([this, duplicates, threshold, value]
          {
            Noggit::LayerOpResult combined;

            // DUPLICATES FIRST, and the order is load bearing. Merging two slots holding the same
            // BLP adds their alpha together, so a pair that each peak at 3 becomes one layer
            // peaking at 6 -- which a threshold of 4 would have deleted outright if it had run
            // first, losing paint the merge would have kept.
            //
            // Two runs means two undo steps when both boxes are ticked. That is the one place this
            // window does not manage a single step, and it is deliberate: the two are separate
            // decisions with separate consequences, and being able to undo the threshold purge
            // while keeping the merge is worth more than the tidiness.
            if (duplicates)
            {
              combined = Noggit::TextureLayerOps::purgeDuplicates(_map_view, currentScope());

              if (!combined.error.empty())
              {
                return combined;
              }
            }

            if (threshold)
            {
              Noggit::LayerOpResult const second
                = Noggit::TextureLayerOps::purgeBelowThreshold(_map_view, currentScope(), value);

              if (!duplicates)
              {
                return second;
              }

              combined.chunks_changed += second.chunks_changed;
              combined.layers_removed += second.layers_removed;
              combined.chunks_refused += second.chunks_refused;
              combined.error = second.error;

              // chunks_visited is NOT summed: the two passes walk the same chunks, and adding
              // them would report a tile as 512 chunks.
              combined.chunks_visited = std::max(combined.chunks_visited, second.chunks_visited);
            }

            return combined;
          });
      });

    return group;
  }

  Noggit::LayerOpScopeRequest TextureLayerManager::currentScope() const
  {
    Noggit::LayerOpScopeRequest scope;

    scope.scope = static_cast<Noggit::LayerOpScope>(_scope_combo->currentIndex());
    scope.position = _map_view->cursorPosition();
    scope.radius = static_cast<float>(_radius_spin->value());

    return scope;
  }

  std::string TextureLayerManager::selectedTexturePath() const
  {
    auto const texture = Noggit::Ui::selected_texture::get();

    if (!texture)
    {
      return {};
    }

    // Normalised here, once, so every path this window hands to TextureLayerOps -- the palette,
    // the replacement texture, the nominated eviction target -- is on the same footing as the
    // filenames TextureSet stores and can be compared without normalising per chunk.
    return BlizzardArchive::ClientData::normalizeFilenameUnix((*texture)->file_key().filepath());
  }

  void TextureLayerManager::runOperation(std::function<Noggit::LayerOpResult()> const& operation)
  {
    if (!_map_view)
    {
      _status->setText("No map view.");
      return;
    }

    Noggit::Ui::WaitCursor const busy;

    // Adding or replacing a layer constructs a scoped_blp_texture_reference, which loads and
    // uploads a BLP, so the context has to be current before anything runs.
    _map_view->context()->makeCurrent(_map_view->context()->surface());
    OpenGL::context::scoped_setter const _(::gl, _map_view->context());

    reportResult(operation());
  }

  void TextureLayerManager::reportResult(Noggit::LayerOpResult const& result)
  {
    if (!result.error.empty())
    {
      _status->setText(QString::fromStdString(result.error));
      return;
    }

    TileIndex const tile(_map_view->cursorPosition());

    QStringList parts;
    parts << QString("%1 of %2 changed")
               .arg(plural(result.chunks_changed, "chunk", "chunks"))
               .arg(result.chunks_visited);

    if (result.layers_removed)
    {
      parts << QString("%1 removed").arg(plural(result.layers_removed, "layer", "layers"));
    }

    if (result.layers_replaced)
    {
      parts << QString("%1 replaced").arg(plural(result.layers_replaced, "layer", "layers"));
    }

    if (result.layers_added)
    {
      parts << QString("%1 added").arg(plural(result.layers_added, "layer", "layers"));
    }

    if (result.chunks_refused)
    {
      parts << QString("%1 refused").arg(plural(result.chunks_refused, "chunk", "chunks"));
    }

    // The tile is named because this window is modeless and the terrain cursor moves while it is
    // open: "0 chunks changed" reads as a broken button until you can see it ran somewhere else.
    _status->setText
      (QString("%1. At tile %2, %3.").arg(parts.join(", ")).arg(tile.x).arg(tile.z));
  }
}
