// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/TerrainMaskDialog.hpp>

// This is the translation unit that INSTANTIATES the bake templates, so it carries the includes
// their bodies need. TerrainMaskBaker.hpp and TextureLayerAlphaProbe.hpp deliberately include none
// of these themselves -- that is what keeps the pure half of the mask code linkable on a bare
// machine -- and the cost of that choice is paid exactly here.
//
// Alphamap.hpp and texture_set.hpp are the two that are easy to miss: MapChunk.h only
// forward-declares TextureSet (MapChunk.h:93), so without them the failure appears inside
// TextureLayerAlphaProbe.hpp as "use of undefined type" with nothing pointing back to this list.
#include <noggit/Alphamap.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/terrain/TerrainMaskBaker.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>
#include <noggit/texture_set.hpp>

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <algorithm>
#include <string>
#include <vector>

using namespace Noggit;

namespace
{
  // The order the kind combo lists filters in, and the index the stacked property pages use. One
  // array so the two cannot disagree.
  MaskFilterKind const KIND_ORDER[] =
    { MaskFilterKind::Slope
    , MaskFilterKind::Height
    , MaskFilterKind::Curvature
    , MaskFilterKind::LayerAlpha
    , MaskFilterKind::AreaId
    , MaskFilterKind::Noise
    , MaskFilterKind::Constant
    };

  constexpr int KIND_COUNT = static_cast<int>(sizeof(KIND_ORDER) / sizeof(KIND_ORDER[0]));

  char const* kindLabel(MaskFilterKind kind)
  {
    switch (kind)
    {
      case MaskFilterKind::Slope:      return "Slope";
      case MaskFilterKind::Height:     return "Height";
      case MaskFilterKind::Curvature:  return "Curvature";
      case MaskFilterKind::LayerAlpha: return "Texture layer alpha";
      case MaskFilterKind::AreaId:     return "Area id";
      case MaskFilterKind::Noise:      return "Noise";
      case MaskFilterKind::Constant:   return "Constant";
    }

    return "Slope";
  }

  // The units the range spin boxes are measured in. Shown next to them because the three axes
  // differ by four orders of magnitude and a feather of "5" means something completely different
  // on each.
  char const* kindUnits(MaskFilterKind kind)
  {
    switch (kind)
    {
      case MaskFilterKind::Slope:      return "degrees from horizontal, 0 to 90";
      case MaskFilterKind::Height:     return "yards of world height";
      case MaskFilterKind::Curvature:  return "1 / yards; POSITIVE is concave (hollows, drainage)";
      case MaskFilterKind::LayerAlpha: return "0 to 1; leave unbounded to use the alpha directly";
      case MaskFilterKind::Noise:      return "0 to 1; leave unbounded to use the noise directly";
      default:                         return "";
    }
  }

  int kindIndex(MaskFilterKind kind)
  {
    for (int i = 0; i < KIND_COUNT; ++i)
    {
      if (KIND_ORDER[i] == kind)
      {
        return i;
      }
    }

    return 0;
  }

  MaskCombine const COMBINE_ORDER[] =
    { MaskCombine::Replace
    , MaskCombine::Add
    , MaskCombine::Subtract
    , MaskCombine::Multiply
    , MaskCombine::Min
    , MaskCombine::Max
    };

  constexpr int COMBINE_COUNT = static_cast<int>(sizeof(COMBINE_ORDER) / sizeof(COMBINE_ORDER[0]));

  char const* combineLabel(MaskCombine op)
  {
    switch (op)
    {
      case MaskCombine::Replace:  return "Replace";
      case MaskCombine::Add:      return "Add";
      case MaskCombine::Subtract: return "Subtract";
      // The two that behave like AND and OR without eroding a soft edge; see the note on
      // MaskCombine.
      case MaskCombine::Multiply: return "Multiply (AND)";
      case MaskCombine::Min:      return "Min (soft AND)";
      case MaskCombine::Max:      return "Max (soft OR)";
    }

    return "Replace";
  }

  int combineIndex(MaskCombine op)
  {
    for (int i = 0; i < COMBINE_COUNT; ++i)
    {
      if (COMBINE_ORDER[i] == op)
      {
        return i;
      }
    }

    return 0;
  }

  // Human-readable byte count. Masks are measured in megabytes and a raw byte figure is unreadable
  // at that scale.
  QString formatBytes(std::size_t bytes)
  {
    if (bytes < 1024u)
    {
      return QString("%1 B").arg(bytes);
    }

    if (bytes < 1024u * 1024u)
    {
      return QString("%1 KiB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }

    return QString("%1 MiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
  }

  std::string areaIdsToText(std::vector<int> const& ids)
  {
    std::string text;

    for (std::size_t i = 0; i < ids.size(); ++i)
    {
      if (i > 0)
      {
        text += ", ";
      }

      text += std::to_string(ids[i]);
    }

    return text;
  }

  std::vector<int> areaIdsFromText(QString const& text)
  {
    std::vector<int> ids;

    // Split by hand rather than with QString::split and a separator class. Both spellings of that
    // call are deprecated in the Qt this project builds against -- QRegExp is superseded by
    // QRegularExpression, and QString::SkipEmptyParts by Qt::SkipEmptyParts -- and a warning-free
    // build is worth more here than the two lines saved, on a field whose grammar is "integers with
    // punctuation between them".
    QString token;

    auto flush = [&token, &ids] ()
    {
      if (token.isEmpty())
      {
        return;
      }

      bool ok = false;
      int const value = token.toInt(&ok);

      // Unparseable text is dropped rather than rejecting the whole field: the user is typing, and
      // every intermediate state of "12, 3" is briefly invalid.
      if (ok)
      {
        ids.push_back(value);
      }

      token.clear();
    };

    for (QChar const c : text)
    {
      if (c.isDigit() || c == QChar('-'))
      {
        token.append(c);
        continue;
      }

      flush();
    }

    flush();

    return ids;
  }
}

namespace Noggit::Ui
{
  TerrainMaskDialog::TerrainMaskDialog(MapView* map_view, QWidget* parent)
    : QDialog(parent)
    , _map_view(map_view)
  {
    setWindowTitle("Terrain Masks");
    setMinimumWidth(760);

    auto* const root = new QVBoxLayout(this);

    auto* const columns = new QHBoxLayout();
    columns->addWidget(buildMaskListGroup(), 1);

    auto* const right = new QVBoxLayout();
    right->addWidget(buildStackGroup(), 1);
    right->addWidget(buildLayerPropertiesGroup(), 0);
    columns->addLayout(right, 2);

    root->addLayout(columns, 1);
    root->addWidget(buildBakeGroup(), 0);

    // Loaded from the project on open rather than held across map changes, because a mask describes
    // one project's terrain and means nothing against another's.
    TerrainMaskStore::instance()->load(projectPath());

    refreshMaskList(TerrainMaskStore::instance()->activeName());
  }

  QGroupBox* TerrainMaskDialog::buildMaskListGroup()
  {
    auto* const group = new QGroupBox("Masks", this);
    auto* const layout = new QVBoxLayout(group);

    _mask_list = new QListWidget(group);
    layout->addWidget(_mask_list, 1);

    auto* const buttons = new QHBoxLayout();
    _new_button = new QPushButton("New", group);
    _duplicate_button = new QPushButton("Duplicate", group);
    buttons->addWidget(_new_button);
    buttons->addWidget(_duplicate_button);
    layout->addLayout(buttons);

    auto* const buttons2 = new QHBoxLayout();
    _rename_button = new QPushButton("Rename", group);
    _delete_button = new QPushButton("Delete", group);
    buttons2->addWidget(_rename_button);
    buttons2->addWidget(_delete_button);
    layout->addLayout(buttons2);

    _clip_enabled = new QCheckBox("Clip brushes to this mask", group);
    _clip_enabled->setToolTip
      ( "While this is ticked, every terrain and texture brush is scaled by the selected mask.\n"
        "It is switched off every time the editor starts."
      );
    layout->addWidget(_clip_enabled);

    connect(_new_button, &QPushButton::clicked, this, &TerrainMaskDialog::onNewMask);
    connect(_duplicate_button, &QPushButton::clicked, this, &TerrainMaskDialog::onDuplicateMask);
    connect(_rename_button, &QPushButton::clicked, this, &TerrainMaskDialog::onRenameMask);
    connect(_delete_button, &QPushButton::clicked, this, &TerrainMaskDialog::onDeleteMask);
    connect(_mask_list, &QListWidget::currentRowChanged, this, [this] (int) { onMaskSelectionChanged(); });
    connect(_clip_enabled, &QCheckBox::toggled, this, &TerrainMaskDialog::onClipToggled);

    return group;
  }

  QGroupBox* TerrainMaskDialog::buildStackGroup()
  {
    auto* const group = new QGroupBox("Filter stack", this);
    auto* const layout = new QVBoxLayout(group);

    _layer_list = new QListWidget(group);
    layout->addWidget(_layer_list, 1);

    auto* const row = new QHBoxLayout();

    _add_kind = new QComboBox(group);

    for (int i = 0; i < KIND_COUNT; ++i)
    {
      _add_kind->addItem(kindLabel(KIND_ORDER[i]));
    }

    _add_button = new QPushButton("Add", group);
    _remove_button = new QPushButton("Remove", group);
    _up_button = new QPushButton("Up", group);
    _down_button = new QPushButton("Down", group);

    row->addWidget(_add_kind, 1);
    row->addWidget(_add_button);
    row->addWidget(_remove_button);
    row->addWidget(_up_button);
    row->addWidget(_down_button);
    layout->addLayout(row);

    connect(_add_button, &QPushButton::clicked, this, &TerrainMaskDialog::onAddLayer);
    connect(_remove_button, &QPushButton::clicked, this, &TerrainMaskDialog::onRemoveLayer);
    connect(_up_button, &QPushButton::clicked, this, [this] { onMoveLayer(-1); });
    connect(_down_button, &QPushButton::clicked, this, [this] { onMoveLayer(1); });
    connect(_layer_list, &QListWidget::currentRowChanged, this, [this] (int) { onLayerSelectionChanged(); });

    return group;
  }

  QGroupBox* TerrainMaskDialog::buildLayerPropertiesGroup()
  {
    auto* const group = new QGroupBox("Selected layer", this);
    auto* const form = new QFormLayout(group);

    _layer_enabled = new QCheckBox("Enabled", group);
    _layer_invert = new QCheckBox("Invert this layer", group);

    auto* const flags = new QHBoxLayout();
    flags->addWidget(_layer_enabled);
    flags->addWidget(_layer_invert);
    form->addRow(flags);

    _combine = new QComboBox(group);

    for (int i = 0; i < COMBINE_COUNT; ++i)
    {
      _combine->addItem(combineLabel(COMBINE_ORDER[i]));
    }

    _combine->setToolTip
      ( "How this layer folds into the layers above it.\n"
        "The first enabled layer always replaces, whatever this says."
      );
    form->addRow("Combine with", _combine);

    _opacity = new QDoubleSpinBox(group);
    _opacity->setRange(0.0, 1.0);
    _opacity->setSingleStep(0.05);
    _opacity->setDecimals(2);
    form->addRow("Opacity", _opacity);

    // --- Range ---

    auto* const low_row = new QHBoxLayout();
    _low_bounded = new QCheckBox("At least", group);
    _range_low = new QDoubleSpinBox(group);
    // Wide enough for world height in yards and fine enough for curvature in reciprocal yards. Four
    // decimals because a broad hill is 0.006 /yd and three would round it to 0.006 with no room to
    // tune.
    _range_low->setRange(-100000.0, 100000.0);
    _range_low->setDecimals(4);
    low_row->addWidget(_low_bounded);
    low_row->addWidget(_range_low, 1);
    form->addRow(low_row);

    auto* const high_row = new QHBoxLayout();
    _high_bounded = new QCheckBox("At most", group);
    _range_high = new QDoubleSpinBox(group);
    _range_high->setRange(-100000.0, 100000.0);
    _range_high->setDecimals(4);
    high_row->addWidget(_high_bounded);
    high_row->addWidget(_range_high, 1);
    form->addRow(high_row);

    _range_feather = new QDoubleSpinBox(group);
    _range_feather->setRange(0.0, 100000.0);
    _range_feather->setDecimals(4);
    _range_feather->setToolTip
      ("Width of the soft shoulder outside the range, in the same units as the range itself.");
    form->addRow("Feather", _range_feather);

    _range_units = new QLabel(group);
    _range_units->setWordWrap(true);
    form->addRow(_range_units);

    // --- Kind-specific pages ---

    _kind_pages = new QStackedWidget(group);

    // Slope and Height need nothing beyond the range, so they share an empty page. Two separate
    // empty pages would still need indices that track KIND_ORDER, and one is enough.
    for (int i = 0; i < 2; ++i)
    {
      _kind_pages->addWidget(new QWidget(_kind_pages));
    }

    {
      auto* const page = new QWidget(_kind_pages);
      auto* const page_form = new QFormLayout(page);

      _curvature_step = new QSpinBox(page);
      _curvature_step->setRange(1, 16);
      _curvature_step->setToolTip
        ( "The SCALE curvature is measured at, in vertex steps of 4.167 yards.\n"
          "Small values pick up gullies; large values pick up whole valleys and ignore the gullies.\n"
          "Larger is also more precise -- a second difference loses accuracy at short spacing."
        );
      page_form->addRow("Scale (vertex steps)", _curvature_step);

      _curvature_scale = new QLabel(page);
      _curvature_scale->setWordWrap(true);
      page_form->addRow(_curvature_scale);

      _kind_pages->addWidget(page);
    }

    {
      auto* const page = new QWidget(_kind_pages);
      auto* const page_form = new QFormLayout(page);
      _texture = new QLineEdit(page);
      _texture->setToolTip("Exact texture path as the chunk stores it.");
      page_form->addRow("Texture", _texture);
      _kind_pages->addWidget(page);
    }

    {
      auto* const page = new QWidget(_kind_pages);
      auto* const page_form = new QFormLayout(page);
      _area_ids = new QLineEdit(page);
      _area_ids->setToolTip("Comma-separated area ids. Empty matches nothing.");
      page_form->addRow("Area ids", _area_ids);
      _kind_pages->addWidget(page);
    }

    {
      auto* const page = new QWidget(_kind_pages);
      auto* const page_form = new QFormLayout(page);

      _noise_wavelength = new QDoubleSpinBox(page);
      _noise_wavelength->setRange(0.5, 100000.0);
      _noise_wavelength->setDecimals(1);
      _noise_wavelength->setToolTip("Size of the largest patches, in yards.");
      page_form->addRow("Wavelength (yards)", _noise_wavelength);

      _noise_octaves = new QSpinBox(page);
      // Capped at 8 to match maskValueNoise, which stops there because octave 8 at the default
      // wavelength is already finer than one mask texel and can only alias.
      _noise_octaves->setRange(1, 8);
      page_form->addRow("Octaves", _noise_octaves);

      _noise_gain = new QDoubleSpinBox(page);
      _noise_gain->setRange(0.0, 1.0);
      _noise_gain->setSingleStep(0.05);
      _noise_gain->setDecimals(2);
      page_form->addRow("Gain", _noise_gain);

      _noise_seed = new QSpinBox(page);
      // Signed int range, not the full uint32 the field can hold: QSpinBox is int-based, and a
      // seed is an arbitrary label rather than a quantity, so half the space is no loss.
      _noise_seed->setRange(0, 2147483647);
      page_form->addRow("Seed", _noise_seed);

      _kind_pages->addWidget(page);
    }

    {
      auto* const page = new QWidget(_kind_pages);
      auto* const page_form = new QFormLayout(page);
      _constant = new QDoubleSpinBox(page);
      _constant->setRange(0.0, 1.0);
      _constant->setSingleStep(0.05);
      _constant->setDecimals(2);
      page_form->addRow("Value", _constant);
      _kind_pages->addWidget(page);
    }

    form->addRow(_kind_pages);

    // Every property widget writes back through one slot. See _loading_widgets for why the
    // model-to-widget direction has to suppress it.
    connect(_combine, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged)
           , this, [this] (int) { onLayerEdited(); });
    connect(_opacity, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged)
           , this, [this] (double) { onLayerEdited(); });
    connect(_layer_enabled, &QCheckBox::toggled, this, [this] (bool) { onLayerEdited(); });
    connect(_layer_invert, &QCheckBox::toggled, this, [this] (bool) { onLayerEdited(); });
    connect(_low_bounded, &QCheckBox::toggled, this, [this] (bool) { onLayerEdited(); });
    connect(_high_bounded, &QCheckBox::toggled, this, [this] (bool) { onLayerEdited(); });
    connect(_range_low, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged)
           , this, [this] (double) { onLayerEdited(); });
    connect(_range_high, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged)
           , this, [this] (double) { onLayerEdited(); });
    connect(_range_feather, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged)
           , this, [this] (double) { onLayerEdited(); });
    connect(_curvature_step, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged)
           , this, [this] (int) { onLayerEdited(); });
    connect(_noise_wavelength, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged)
           , this, [this] (double) { onLayerEdited(); });
    connect(_noise_octaves, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged)
           , this, [this] (int) { onLayerEdited(); });
    connect(_noise_gain, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged)
           , this, [this] (double) { onLayerEdited(); });
    connect(_noise_seed, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged)
           , this, [this] (int) { onLayerEdited(); });
    connect(_constant, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged)
           , this, [this] (double) { onLayerEdited(); });
    connect(_texture, &QLineEdit::textChanged, this, [this] (QString const&) { onLayerEdited(); });
    connect(_area_ids, &QLineEdit::textChanged, this, [this] (QString const&) { onLayerEdited(); });

    return group;
  }

  QGroupBox* TerrainMaskDialog::buildBakeGroup()
  {
    auto* const group = new QGroupBox("Bake and persistence", this);
    auto* const layout = new QVBoxLayout(group);

    auto* const row = new QHBoxLayout();

    _bake_button = new QPushButton("Bake loaded tiles", group);
    _bake_button->setToolTip
      ( "Evaluates the stack over every tile currently loaded and folds the painted layer on top.\n"
        "Tiles that are not baked do not clip anything -- an unbaked tile fails open."
      );

    _clear_bake_button = new QPushButton("Clear bake", group);
    _save_button = new QPushButton("Save to project", group);
    _reload_button = new QPushButton("Reload from project", group);

    row->addWidget(_bake_button);
    row->addWidget(_clear_bake_button);
    row->addStretch(1);

    auto* const paint_label = new QLabel("Paint folds with", group);
    _paint_combine = new QComboBox(group);

    for (int i = 0; i < COMBINE_COUNT; ++i)
    {
      _paint_combine->addItem(combineLabel(COMBINE_ORDER[i]));
    }

    _paint_combine->setToolTip
      ( "How hand-painted adjustments fold onto the derived result during a bake.\n"
        "Max lets the brush add to the mask; Min lets it carve out of it."
      );

    row->addWidget(paint_label);
    row->addWidget(_paint_combine);
    row->addWidget(_save_button);
    row->addWidget(_reload_button);
    layout->addLayout(row);

    _status = new QLabel(group);
    _status->setWordWrap(true);
    layout->addWidget(_status);

    _memory = new QLabel(group);
    _memory->setWordWrap(true);
    layout->addWidget(_memory);

    _problems = new QPlainTextEdit(group);
    _problems->setReadOnly(true);
    _problems->setMaximumHeight(90);
    layout->addWidget(_problems);

    auto* const caveat = new QLabel
      ( "Masks are editor-side only: nothing here is written into ADT data, and mask edits are not "
        "on the undo stack."
      , group
      );
    caveat->setWordWrap(true);
    layout->addWidget(caveat);

    connect(_bake_button, &QPushButton::clicked, this, &TerrainMaskDialog::onBakeLoadedTiles);
    connect(_clear_bake_button, &QPushButton::clicked, this, &TerrainMaskDialog::onClearBake);
    connect(_save_button, &QPushButton::clicked, this, &TerrainMaskDialog::onSave);
    connect(_reload_button, &QPushButton::clicked, this, &TerrainMaskDialog::onReload);
    connect(_paint_combine, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged)
           , this, [this] (int index)
      {
        if (_loading_widgets)
        {
          return;
        }

        NamedTerrainMask* const mask = selectedMask();

        if (mask && index >= 0 && index < COMBINE_COUNT)
        {
          mask->paint_combine = COMBINE_ORDER[index];
        }
      });

    return group;
  }

  std::string TerrainMaskDialog::projectPath() const
  {
    // Safe here specifically: this dialog is reachable only from a MapView, and a MapView cannot
    // exist without an open project. TerrainMaskStore itself takes the path as an argument rather
    // than calling this, precisely so it does not inherit that precondition.
    auto* const project = Project::CurrentProject::get();
    return project ? project->ProjectPath : std::string();
  }

  NamedTerrainMask* TerrainMaskDialog::selectedMask()
  {
    QListWidgetItem const* const item = _mask_list->currentItem();

    if (!item)
    {
      return nullptr;
    }

    // Looked up by name every time. TerrainMaskStore keeps masks in a std::vector, so any create or
    // remove reallocates and a cached pointer would dangle.
    return TerrainMaskStore::instance()->find(item->text().toStdString());
  }

  int TerrainMaskDialog::selectedLayerRow() const
  {
    return _layer_list->currentRow();
  }

  void TerrainMaskDialog::refreshMaskList(std::string const& select)
  {
    _loading_widgets = true;

    _mask_list->clear();

    int select_row = -1;
    std::vector<std::string> const names = TerrainMaskStore::instance()->names();

    for (std::size_t i = 0; i < names.size(); ++i)
    {
      _mask_list->addItem(QString::fromStdString(names[i]));

      if (names[i] == select)
      {
        select_row = static_cast<int>(i);
      }
    }

    if (select_row < 0 && !names.empty())
    {
      select_row = 0;
    }

    _mask_list->setCurrentRow(select_row);

    _clip_enabled->setChecked(TerrainMaskStore::instance()->clippingEnabled());

    _loading_widgets = false;

    onMaskSelectionChanged();
  }

  void TerrainMaskDialog::refreshLayerList(int select_row)
  {
    bool const was_loading = _loading_widgets;
    _loading_widgets = true;

    _layer_list->clear();

    NamedTerrainMask const* const mask = const_cast<TerrainMaskDialog*>(this)->selectedMask();

    if (mask)
    {
      std::vector<MaskFilterLayer> const& layers = mask->stack.layers();

      for (std::size_t i = 0; i < layers.size(); ++i)
      {
        MaskFilterLayer const& layer = layers[i];

        // The row text is the whole summary of the layer, because a stack is read top to bottom and
        // having to click each row to find out what it does defeats the point of a stack.
        QString text = QString("%1. %2").arg(i + 1).arg(kindLabel(layer.kind));

        if (i > 0)
        {
          text += QString(" [%1]").arg(combineLabel(layer.combine));
        }

        if (layer.invert)
        {
          text += " (inverted)";
        }

        if (!layer.enabled)
        {
          text += "  - disabled";
        }

        _layer_list->addItem(text);
      }

      if (select_row >= 0 && select_row < static_cast<int>(layers.size()))
      {
        _layer_list->setCurrentRow(select_row);
      }
      else if (!layers.empty())
      {
        _layer_list->setCurrentRow(0);
      }
    }

    _loading_widgets = was_loading;

    loadLayerIntoWidgets();
  }

  void TerrainMaskDialog::loadLayerIntoWidgets()
  {
    _loading_widgets = true;

    NamedTerrainMask* const mask = selectedMask();
    int const row = selectedLayerRow();

    bool const have_layer = mask && row >= 0 && row < static_cast<int>(mask->stack.layers().size());

    // Disabled rather than hidden when nothing is selected: a panel that vanishes reflows the whole
    // dialog every time the selection changes.
    _combine->setEnabled(have_layer);
    _opacity->setEnabled(have_layer);
    _layer_enabled->setEnabled(have_layer);
    _layer_invert->setEnabled(have_layer);
    _low_bounded->setEnabled(have_layer);
    _high_bounded->setEnabled(have_layer);
    _range_low->setEnabled(have_layer);
    _range_high->setEnabled(have_layer);
    _range_feather->setEnabled(have_layer);
    _kind_pages->setEnabled(have_layer);

    if (have_layer)
    {
      MaskFilterLayer const& layer = mask->stack.layers()[static_cast<std::size_t>(row)];

      _combine->setCurrentIndex(combineIndex(layer.combine));
      _opacity->setValue(static_cast<double>(layer.opacity));
      _layer_enabled->setChecked(layer.enabled);
      _layer_invert->setChecked(layer.invert);

      bool const low_bounded = layer.range.low > -1.0e29f;
      bool const high_bounded = layer.range.high < 1.0e29f;

      _low_bounded->setChecked(low_bounded);
      _high_bounded->setChecked(high_bounded);
      _range_low->setValue(low_bounded ? static_cast<double>(layer.range.low) : 0.0);
      _range_high->setValue(high_bounded ? static_cast<double>(layer.range.high) : 0.0);
      _range_feather->setValue(static_cast<double>(layer.range.feather));
      _range_low->setEnabled(low_bounded);
      _range_high->setEnabled(high_bounded);

      _range_units->setText(QString::fromLatin1(kindUnits(layer.kind)));

      _curvature_step->setValue(layer.curvature_step);
      _curvature_scale->setText
        ( QString("Measured over %1 yards.")
          .arg(static_cast<double>(layer.curvature_step) * static_cast<double>(MASK_UNIT_SIZE), 0, 'f', 2)
        );

      _noise_wavelength->setValue(static_cast<double>(layer.noise_wavelength));
      _noise_octaves->setValue(layer.noise_octaves);
      _noise_gain->setValue(static_cast<double>(layer.noise_gain));
      _noise_seed->setValue(static_cast<int>(layer.noise_seed & 0x7FFFFFFFu));
      _texture->setText(QString::fromStdString(layer.texture));
      _area_ids->setText(QString::fromStdString(areaIdsToText(layer.area_ids)));
      _constant->setValue(static_cast<double>(layer.constant));

      _kind_pages->setCurrentIndex(kindIndex(layer.kind));
    }

    if (mask)
    {
      _paint_combine->setCurrentIndex(combineIndex(mask->paint_combine));
    }

    _loading_widgets = false;

    refreshStatus();
  }

  void TerrainMaskDialog::commitWidgetsToLayer()
  {
    if (_loading_widgets)
    {
      return;
    }

    NamedTerrainMask* const mask = selectedMask();
    int const row = selectedLayerRow();

    if (!mask || row < 0 || row >= static_cast<int>(mask->stack.layers().size()))
    {
      return;
    }

    MaskFilterLayer& layer = mask->stack.layers()[static_cast<std::size_t>(row)];

    int const combine_row = _combine->currentIndex();
    layer.combine = (combine_row >= 0 && combine_row < COMBINE_COUNT)
                  ? COMBINE_ORDER[combine_row]
                  : MaskCombine::Replace;

    layer.opacity = static_cast<float>(_opacity->value());
    layer.enabled = _layer_enabled->isChecked();
    layer.invert = _layer_invert->isChecked();

    // An unticked bound is stored as the infinity TerrainRange-style sentinel, not as the spin
    // box's number. This is the same decision TerrainRuleStore::writeRange makes, for the same
    // reason: "no lower bound" and "at least zero" are different filters and must not be able to
    // turn into one another.
    layer.range.low = _low_bounded->isChecked()
                    ? static_cast<float>(_range_low->value())
                    : -1.0e30f;
    layer.range.high = _high_bounded->isChecked()
                     ? static_cast<float>(_range_high->value())
                     : 1.0e30f;
    layer.range.feather = static_cast<float>(_range_feather->value());

    layer.curvature_step = _curvature_step->value();
    layer.noise_wavelength = static_cast<float>(_noise_wavelength->value());
    layer.noise_octaves = _noise_octaves->value();
    layer.noise_gain = static_cast<float>(_noise_gain->value());
    layer.noise_seed = static_cast<std::uint32_t>(_noise_seed->value());
    layer.texture = _texture->text().toStdString();
    layer.area_ids = areaIdsFromText(_area_ids->text());
    layer.constant = static_cast<float>(_constant->value());

    _range_low->setEnabled(_low_bounded->isChecked());
    _range_high->setEnabled(_high_bounded->isChecked());
  }

  void TerrainMaskDialog::refreshStatus()
  {
    NamedTerrainMask const* const mask = selectedMask();

    if (!mask)
    {
      _status->setText("No mask selected.");
      _memory->clear();
      _problems->clear();
      return;
    }

    std::size_t loaded_tiles = 0;
    std::size_t baked_tiles = 0;

    if (_map_view && _map_view->getWorld())
    {
      for (MapTile* tile : _map_view->getWorld()->mapIndex.loaded_tiles())
      {
        if (!tile)
        {
          continue;
        }

        ++loaded_tiles;

        if (mask->tileIsBaked(static_cast<int>(tile->index.x), static_cast<int>(tile->index.z)))
        {
          ++baked_tiles;
        }
      }
    }

    // The unbaked count is the headline, because an unbaked tile FAILS OPEN -- it clips nothing --
    // and that is invisible from the viewport. A user seeing a brush apply where the mask should
    // have stopped it needs this number to be the first thing they read.
    _status->setText
      ( QString("%1 of %2 loaded tiles baked.%3")
        .arg(baked_tiles)
        .arg(loaded_tiles)
        .arg( baked_tiles < loaded_tiles
            ? QString("  Unbaked tiles are NOT clipped.")
            : QString()
            )
      );

    _memory->setText
      ( QString("Composited %1 (%2 chunks, %3 dense).  Painted %4.  Budget %5.")
        .arg(formatBytes(mask->compositedBytes()))
        .arg(mask->composited.chunkCount())
        .arg(mask->composited.denseChunkCount())
        .arg(formatBytes(mask->paintBytes()))
        .arg(formatBytes(TerrainMaskStore::instance()->budgetBytes()))
      );

    QString problems;

    for (std::string const& problem : mask->stack.validate())
    {
      problems += QString::fromStdString(problem) + "\n";
    }

    _problems->setPlainText(problems);
  }

  // --- Mask list actions ---

  void TerrainMaskDialog::onNewMask()
  {
    bool ok = false;
    QString const name = QInputDialog::getText
      (this, "New mask", "Name", QLineEdit::Normal, "New mask", &ok);

    if (!ok || name.isEmpty())
    {
      return;
    }

    if (!TerrainMaskStore::instance()->create(name.toStdString()))
    {
      QMessageBox::warning(this, "New mask", "A mask with that name already exists.");
      return;
    }

    refreshMaskList(name.toStdString());
  }

  void TerrainMaskDialog::onDuplicateMask()
  {
    NamedTerrainMask const* const source = selectedMask();

    if (!source)
    {
      return;
    }

    bool ok = false;
    QString const name = QInputDialog::getText
      ( this, "Duplicate mask", "Name", QLineEdit::Normal
      , QString::fromStdString(source->name) + " copy", &ok
      );

    if (!ok || name.isEmpty())
    {
      return;
    }

    // The stack and the paint layer are copied; the composited field deliberately is not, so the
    // duplicate starts unbaked rather than carrying a field that belongs to the original's bake.
    MaskFilterStack const stack_copy = source->stack;
    TerrainMask const paint_copy = source->paint;
    MaskCombine const paint_combine = source->paint_combine;

    NamedTerrainMask* const created = TerrainMaskStore::instance()->create(name.toStdString());

    if (!created)
    {
      QMessageBox::warning(this, "Duplicate mask", "A mask with that name already exists.");
      return;
    }

    created->stack = stack_copy;
    created->paint = paint_copy;
    created->paint_combine = paint_combine;

    refreshMaskList(name.toStdString());
  }

  void TerrainMaskDialog::onRenameMask()
  {
    NamedTerrainMask const* const mask = selectedMask();

    if (!mask)
    {
      return;
    }

    std::string const old_name = mask->name;

    bool ok = false;
    QString const name = QInputDialog::getText
      (this, "Rename mask", "Name", QLineEdit::Normal, QString::fromStdString(old_name), &ok);

    if (!ok || name.isEmpty() || name.toStdString() == old_name)
    {
      return;
    }

    if (!TerrainMaskStore::instance()->rename(old_name, name.toStdString()))
    {
      QMessageBox::warning(this, "Rename mask", "A mask with that name already exists.");
      return;
    }

    refreshMaskList(name.toStdString());
  }

  void TerrainMaskDialog::onDeleteMask()
  {
    NamedTerrainMask const* const mask = selectedMask();

    if (!mask)
    {
      return;
    }

    std::string const name = mask->name;

    if (QMessageBox::question
          ( this, "Delete mask"
          , QString("Delete '%1'? The painted layer cannot be recovered.").arg(QString::fromStdString(name))
          ) != QMessageBox::Yes)
    {
      return;
    }

    TerrainMaskStore::instance()->remove(name);
    refreshMaskList(std::string());
  }

  void TerrainMaskDialog::onMaskSelectionChanged()
  {
    QListWidgetItem const* const item = _mask_list->currentItem();

    // Selecting a mask in the list makes it the ACTIVE one, i.e. the one the brushes are clipped
    // by. One selection rather than two -- a separate "active" marker distinct from the list
    // selection is the kind of state that leads to editing one mask while a different one silently
    // clips the brush.
    TerrainMaskStore::instance()->setActive(item ? item->text().toStdString() : std::string());

    refreshLayerList(0);
  }

  void TerrainMaskDialog::onClipToggled(bool enabled)
  {
    if (_loading_widgets)
    {
      return;
    }

    TerrainMaskStore::instance()->setClippingEnabled(enabled);
    refreshStatus();
  }

  // --- Layer actions ---

  void TerrainMaskDialog::onAddLayer()
  {
    NamedTerrainMask* const mask = selectedMask();

    if (!mask)
    {
      return;
    }

    int const index = _add_kind->currentIndex();

    MaskFilterLayer layer;
    layer.kind = (index >= 0 && index < KIND_COUNT) ? KIND_ORDER[index] : MaskFilterKind::Slope;

    // A new layer on a non-empty stack defaults to Min rather than Replace. Replace would silently
    // discard everything beneath it, which looks like the previous layers stopped working; Min is
    // the intersection, which is what "add another condition" almost always means.
    layer.combine = mask->stack.layers().empty() ? MaskCombine::Replace : MaskCombine::Min;

    mask->stack.layers().push_back(layer);

    refreshLayerList(static_cast<int>(mask->stack.layers().size()) - 1);
  }

  void TerrainMaskDialog::onRemoveLayer()
  {
    NamedTerrainMask* const mask = selectedMask();
    int const row = selectedLayerRow();

    if (!mask || row < 0 || row >= static_cast<int>(mask->stack.layers().size()))
    {
      return;
    }

    mask->stack.layers().erase(mask->stack.layers().begin() + row);

    refreshLayerList(row > 0 ? row - 1 : 0);
  }

  void TerrainMaskDialog::onMoveLayer(int delta)
  {
    NamedTerrainMask* const mask = selectedMask();
    int const row = selectedLayerRow();
    int const target = row + delta;

    if ( !mask || row < 0
      || row >= static_cast<int>(mask->stack.layers().size())
      || target < 0
      || target >= static_cast<int>(mask->stack.layers().size())
       )
    {
      return;
    }

    std::swap(mask->stack.layers()[static_cast<std::size_t>(row)]
             , mask->stack.layers()[static_cast<std::size_t>(target)]);

    refreshLayerList(target);
  }

  void TerrainMaskDialog::onLayerSelectionChanged()
  {
    loadLayerIntoWidgets();
  }

  void TerrainMaskDialog::onLayerEdited()
  {
    if (_loading_widgets)
    {
      return;
    }

    commitWidgetsToLayer();

    // The row text carries the kind, combinator and flags, so an edit has to redraw it. The
    // selection is preserved, and refreshLayerList re-enters loadLayerIntoWidgets under
    // _loading_widgets, so this does not recurse.
    int const row = selectedLayerRow();
    refreshLayerList(row);
  }

  // --- Bake and persistence ---

  void TerrainMaskDialog::onBakeLoadedTiles()
  {
    NamedTerrainMask* const mask = selectedMask();

    if (!mask || !_map_view || !_map_view->getWorld())
    {
      return;
    }

    TerrainMaskBaker::BakeResult const result
      = TerrainMaskBaker::bakeLoadedTiles<World, MapTile, MapChunk>(*_map_view->getWorld(), *mask);

    if (result.refused)
    {
      QMessageBox::warning
        (this, "Bake", QString("Nothing was baked: %1").arg(QString::fromStdString(result.reason)));
      return;
    }

    std::size_t const dropped = TerrainMaskStore::instance()->enforceBudget();

    if (dropped > 0)
    {
      _status->setText
        ( QString("Dropped the baked field of %1 inactive mask(s) to stay inside the memory budget.")
          .arg(dropped)
        );
    }

    refreshStatus();
  }

  void TerrainMaskDialog::onClearBake()
  {
    NamedTerrainMask* const mask = selectedMask();

    if (!mask)
    {
      return;
    }

    // The paint layer is untouched -- it is the half that cannot be recomputed.
    mask->invalidateBake();
    refreshStatus();
  }

  void TerrainMaskDialog::onSave()
  {
    if (!TerrainMaskStore::instance()->save(projectPath()))
    {
      QMessageBox::warning
        ( this, "Save masks"
        , QString::fromStdString(TerrainMaskStore::instance()->lastError())
        );
      return;
    }

    _status->setText("Saved.");
  }

  void TerrainMaskDialog::onReload()
  {
    if (QMessageBox::question
          ( this, "Reload masks"
          , "Discard unsaved mask changes and reload from the project?"
          ) != QMessageBox::Yes)
    {
      return;
    }

    if (!TerrainMaskStore::instance()->load(projectPath()))
    {
      QMessageBox::warning
        ( this, "Reload masks"
        , QString::fromStdString(TerrainMaskStore::instance()->lastError())
        );
    }

    refreshMaskList(TerrainMaskStore::instance()->activeName());
  }
}
