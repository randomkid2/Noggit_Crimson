// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/AutoTextureDialog.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/Alphamap.hpp>
#include <noggit/Brush.h>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/texture_set.hpp>
#include <opengl/context.hpp>
#include <opengl/scoped.hpp>

#include <QtCore/QElapsedTimer>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QCursor>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace Noggit::Ui;

namespace
{
  namespace Collector = Noggit::TerrainRuleCollector;

  constexpr int UNITS_PER_CHUNK = Collector::CHUNK_INNER_SIDE * Collector::CHUNK_INNER_SIDE;

  // An MCNK holds at most four texture layers. Not a tunable; it is the width of the MCLY array.
  constexpr std::size_t MAX_CHUNK_LAYERS = 4;

  // The MCAL planes behind those layers: one fewer, because layer 0 is whatever the other three
  // leave over. MAX_ALPHAMAPS is an int (Alphamap.hpp:13) and every use here indexes with a
  // std::size_t.
  constexpr std::size_t ALPHA_PLANES = static_cast<std::size_t>(MAX_ALPHAMAPS);

  // Hardness just short of 1, and the reason is arithmetic rather than aesthetic: Brush::getValue
  // computes `1 - (dist - iradius) / oradius` (Brush.cpp:43), and hardness 1 makes oradius exactly
  // 0, so a texel landing precisely on the radius divides zero by zero and writes NaN into an
  // alpha map. At 0.98 the falloff band is 2% of the radius -- under a tenth of a texel at the
  // sizes used here -- so every texel that is touched is still touched at full strength.
  constexpr float BRUSH_HARDNESS = 0.98f;

  // Radius of the per-unit brush, in world units.
  //
  // A unit is 8x8 alphamap texels, so half a unit across is UNITSIZE/2. That is NOT enough: the
  // brush measures the shortest distance to each texel's SQUARE, and the corner texel of a unit
  // sits 3*TEXDETAILSIZE*sqrt(2) = 0.53*UNITSIZE from the unit centre, so a radius of half a unit
  // leaves the four corners of every unit unpainted -- speckle, in whatever was there before.
  // Rounded up to 0.56 to clear that with margin.
  //
  // The cost is about one texel of bleed into the neighbouring unit on each side, which no
  // circular brush can avoid and which does not matter: neighbours that agree on a texture
  // overwrite each other with the same value, and at a boundary the seam moves by half a metre.
  constexpr float UNIT_BRUSH_RADIUS = UNITSIZE * 0.56f;

  // Covers every texel of one chunk with room to spare -- the corner texel is 22.8 units from the
  // centre against a chunk 33.3 across -- and, because the paint is issued on the chunk directly
  // rather than through World::paintTexture, reaches nothing outside it.
  constexpr float CHUNK_BRUSH_RADIUS = CHUNKSIZE;

  // Exact text of the one validate() complaint that is advisory rather than fatal
  // (TerrainRules.cpp:449). A rule set that paints only cliffs is a legitimate thing to want and
  // necessarily has no catch-all; every other complaint means a rule cannot work at all. Matched
  // by value because the header states the wording is stable enough to assert on.
  constexpr char const* NO_CATCH_ALL_PROBLEM
    = "no catch-all rule: points matching nothing are left untouched";

  // Endpoint equality that counts two NaNs as the same endpoint.
  //
  // A NaN endpoint is a real state of a TerrainRange -- it constrains, and it matches nothing
  // (TerrainRules.cpp:54) -- and `a != b` is TRUE for a NaN against itself, so a plain comparison
  // would report a rule nobody touched as edited every single time it was looked at.
  bool sameEndpoint(float lhs, float rhs)
  {
    return (std::isnan(lhs) && std::isnan(rhs)) || lhs == rhs;
  }

  bool sameRange(Noggit::TerrainRange const& lhs, Noggit::TerrainRange const& rhs)
  {
    return sameEndpoint(lhs.min, rhs.min) && sameEndpoint(lhs.max, rhs.max);
  }

  // Every field the dialog can write. Spelled out rather than defaulted on TerrainRule, because
  // TerrainRule lives in the module that has to stay STL-only and testable, and an operator==
  // added there would be one more thing for the rule evaluator's tests to keep honest.
  bool sameRule(Noggit::TerrainRule const& lhs, Noggit::TerrainRule const& rhs)
  {
    return lhs.texture == rhs.texture
        && sameRange(lhs.slope, rhs.slope)
        && sameRange(lhs.height, rhs.height)
        && lhs.strength == rhs.strength
        && lhs.priority == rhs.priority
        && lhs.enabled == rhs.enabled;
  }

  // Everything about a chunk's texturing that this tool can move: which layers exist, in which
  // order, and the committed alpha behind them.
  //
  // Taken before the first edit to a chunk and again after the last one, because "did this paint
  // change anything" cannot be answered any other way. TextureSet::paintTexture reports true for a
  // texel it rewrote with the value it already held (texture_set.cpp:955 sets `changed` for every
  // texel inside the radius, whether or not the alpha moved), and its other early return reports
  // true for a chunk it did not touch at all (texture_set.cpp:832, `return nTextures == 1`). So the
  // paint's own answer cannot be used to decide whether the tile is now unsaved.
  struct ChunkTextureFingerprint
  {
    std::vector<std::string> layers;
    std::array<std::array<std::uint8_t, 64 * 64>, ALPHA_PLANES> alpha{};
    // An absent plane and an all-zero plane are different states of the chunk, and the difference
    // survives a save, so it is not safe to fold them together.
    std::array<bool, ALPHA_PLANES> alpha_present{};
  };

  bool operator== (ChunkTextureFingerprint const& lhs, ChunkTextureFingerprint const& rhs)
  {
    return lhs.layers == rhs.layers
        && lhs.alpha_present == rhs.alpha_present
        && lhs.alpha == rhs.alpha;
  }

  void captureTextureFingerprint(TextureSet* texture_set, ChunkTextureFingerprint& out)
  {
    out.layers.clear();

    for (std::size_t layer = 0; layer < texture_set->num(); ++layer)
    {
      out.layers.push_back(texture_set->filename(layer));
    }

    auto const& maps = *texture_set->getAlphamaps();

    for (std::size_t i = 0; i < ALPHA_PLANES; ++i)
    {
      out.alpha_present[i] = maps[i] != nullptr;

      if (maps[i])
      {
        std::memcpy(out.alpha[i].data(), maps[i]->getAlpha(), out.alpha[i].size());
      }
      else
      {
        out.alpha[i].fill(0);
      }
    }
  }

  QString shortTextureName(QString const& path)
  {
    return path.section('\\', -1).section('/', -1);
  }

  QString describeRange(Noggit::TerrainRange const& range, QString const& unit)
  {
    if (!range.bounded())
    {
      return "any";
    }

    if (!range.boundedBelow())
    {
      return QString("<= %1%2").arg(range.max, 0, 'f', 1).arg(unit);
    }

    if (!range.boundedAbove())
    {
      return QString(">= %1%2").arg(range.min, 0, 'f', 1).arg(unit);
    }

    return QString("%1%3 to %2%3").arg(range.min, 0, 'f', 1).arg(range.max, 0, 'f', 1).arg(unit);
  }

  Noggit::TerrainRange readRange( QCheckBox const* min_bounded
                                , QDoubleSpinBox const* min_spin
                                , QCheckBox const* max_bounded
                                , QDoubleSpinBox const* max_spin
                                )
  {
    // Default-constructed, i.e. infinite in both directions; an unticked box leaves its endpoint
    // alone rather than substituting the spin box's value.
    Noggit::TerrainRange range;

    if (min_bounded->isChecked())
    {
      range.min = static_cast<float>(min_spin->value());
    }

    if (max_bounded->isChecked())
    {
      range.max = static_cast<float>(max_spin->value());
    }

    return range;
  }

  void showRange( Noggit::TerrainRange const& range
                , QCheckBox* min_bounded
                , QDoubleSpinBox* min_spin
                , QCheckBox* max_bounded
                , QDoubleSpinBox* max_spin
                )
  {
    min_bounded->setChecked(range.boundedBelow());
    min_spin->setEnabled(range.boundedBelow());

    // isfinite rather than boundedBelow alone: a NaN endpoint is a CONSTRAINT as far as
    // TerrainRange is concerned (TerrainRules.cpp:54), and feeding it to setValue would clamp it
    // to the spin box's minimum and quietly turn an invalid rule validate() would have caught into
    // a valid one that means something else.
    if (range.boundedBelow() && std::isfinite(range.min))
    {
      min_spin->setValue(range.min);
    }

    max_bounded->setChecked(range.boundedAbove());
    max_spin->setEnabled(range.boundedAbove());

    if (range.boundedAbove() && std::isfinite(range.max))
    {
      max_spin->setValue(range.max);
    }
  }

  // World position of the inner vertex of one unit.
  //
  // The inner vertex IS the unit centre, so this is a lookup rather than a derivation -- and it is
  // the same vertex TerrainRuleCollector::sampleChunkUnit read the slope from, which is what keeps
  // the decision and the paint at the same place.
  glm::vec3 unitCentre(MapChunk* chunk, int unit_row, int unit_col)
  {
    return chunk->mVertices[Collector::chunkInnerVertexIndex(unit_row, unit_col)];
  }
}

AutoTextureDialog::AutoTextureDialog(MapView* map_view, QWidget* parent)
  : QDialog(parent)
  , _map_view(map_view)
{
  setWindowTitle("Automatic Texturing by Slope and Height");
  setMinimumSize(920, 640);

  auto outer (new QHBoxLayout(this));

  // --- left: the rule list ---------------------------------------------------------------
  auto left (new QVBoxLayout());

  left->addWidget(new QLabel("Rules", this));

  _rule_list = new QListWidget(this);
  _rule_list->setToolTip
    ("Precedence is decided by rule content, not by this order: higher priority first, then the\n"
     "rule that constrains more endpoints, then higher strength. \"Snow above 400\" therefore\n"
     "beats a catch-all \"grass\" without a priority being set on either.");
  left->addWidget(_rule_list, 1);

  auto edit_buttons (new QHBoxLayout());
  auto add_button (new QPushButton("Add", this));
  auto duplicate_button (new QPushButton("Duplicate", this));
  auto remove_button (new QPushButton("Remove", this));
  edit_buttons->addWidget(add_button);
  edit_buttons->addWidget(duplicate_button);
  edit_buttons->addWidget(remove_button);
  left->addLayout(edit_buttons);

  auto order_buttons (new QHBoxLayout());
  auto up_button (new QPushButton("Move up", this));
  auto down_button (new QPushButton("Move down", this));
  // List order is the LAST precedence key and only separates rules that already agree on priority,
  // specificity, strength and texture -- which therefore paint the same thing. It is here so the
  // list can be arranged to read well, and saying so stops the next person reordering rules in the
  // hope of fixing a precedence problem.
  up_button->setToolTip("Reorders the list. Precedence is decided by rule content, not by order.");
  down_button->setToolTip(up_button->toolTip());
  order_buttons->addWidget(up_button);
  order_buttons->addWidget(down_button);
  left->addLayout(order_buttons);

  outer->addLayout(left, 1);

  // --- right: the selected rule, the scope, the preview ----------------------------------
  auto right (new QVBoxLayout());

  _rule_box = buildRuleBox();
  right->addWidget(_rule_box);

  right->addWidget(buildScopeBox());

  auto preview_row (new QHBoxLayout());
  auto preview_button (new QPushButton("Preview", this));
  preview_button->setToolTip
    ("Evaluates the rules over the scope without changing anything, and reports what each rule\n"
     "would claim. Apply stays disabled until this has been run against the current rules.");
  preview_row->addWidget(preview_button);
  preview_row->addStretch(1);
  right->addLayout(preview_row);

  _report = new QPlainTextEdit(this);
  _report->setReadOnly(true);
  _report->setLineWrapMode(QPlainTextEdit::NoWrap);
  _report->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  _report->setPlainText("Add rules, choose a scope, then press Preview.");
  right->addWidget(_report, 1);

  _status = new QLabel(this);
  _status->setWordWrap(true);
  right->addWidget(_status);

  auto apply_row (new QHBoxLayout());
  apply_row->addStretch(1);

  _apply_button = new QPushButton("Apply to scope", this);
  _apply_button->setEnabled(false);
  _apply_button->setToolTip("Run a preview first. One apply is one undo step.");
  apply_row->addWidget(_apply_button);
  right->addLayout(apply_row);

  outer->addLayout(right, 2);

  connect(add_button, &QPushButton::clicked, this, &AutoTextureDialog::onAddRule);
  connect(duplicate_button, &QPushButton::clicked, this, &AutoTextureDialog::onDuplicateRule);
  connect(remove_button, &QPushButton::clicked, this, &AutoTextureDialog::onRemoveRule);
  connect(up_button, &QPushButton::clicked, this, [this] { moveSelectedRule(-1); });
  connect(down_button, &QPushButton::clicked, this, [this] { moveSelectedRule(1); });
  connect(preview_button, &QPushButton::clicked, this, &AutoTextureDialog::onPreview);
  connect(_apply_button, &QPushButton::clicked, this, &AutoTextureDialog::onApply);

  connect(_rule_list, &QListWidget::currentRowChanged, this, [this] (int) { showSelectedRule(); });

  // Every widget exists from here on, so the write-back path is safe to arm.
  _updating_ui = false;

  refreshTextureList();
  rebuildRuleList();
  showSelectedRule();
  invalidatePreview();
}

QGroupBox* AutoTextureDialog::buildRuleBox()
{
  auto box (new QGroupBox("Selected rule", this));
  auto form (new QFormLayout(box));

  auto texture_row (new QHBoxLayout());

  // Editable, unlike the ground effect editor's equivalent, and that difference is the point of
  // this tool: terrainTexturesInScope reports what is ALREADY painted, but the reason to write a
  // rule set is usually to introduce a texture that is not on the terrain yet -- there is no snow
  // to find before the snow rule has run. The list is the convenient case; typing a tileset path
  // is the necessary one.
  _texture = new QComboBox(box);
  _texture->setEditable(true);
  _texture->setInsertPolicy(QComboBox::NoInsert);
  _texture->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  _texture->setToolTip
    ("Textures found on the terrain in the chosen scope. Type or paste a tileset path to use one\n"
     "that is not painted anywhere yet.");
  texture_row->addWidget(_texture, 1);

  auto rescan (new QPushButton("Rescan", box));
  rescan->setToolTip("Re-read the terrain, after painting textures or loading more tiles.");
  texture_row->addWidget(rescan);

  form->addRow("Texture", texture_row);

  // Returns the row layout and fills in the two widget pointers. `bounded` and `spin` are
  // out-parameters of THIS call, so the connection below captures the pointer values rather than
  // the references -- a reference to the caller's member would be to a variable this lambda does
  // not own and the slot would read it long after the lambda is gone.
  auto const make_bound_row
    = [this, box] ( QCheckBox*& bounded
                  , QDoubleSpinBox*& spin
                  , QString const& label
                  , QString const& suffix
                  )
      {
        auto row (new QHBoxLayout());

        bounded = new QCheckBox(label, box);
        row->addWidget(bounded);

        spin = new QDoubleSpinBox(box);
        spin->setDecimals(1);
        spin->setSuffix(suffix);
        spin->setEnabled(false);
        row->addWidget(spin, 1);

        QDoubleSpinBox* const spin_widget = spin;

        connect( bounded, &QCheckBox::toggled, this
               , [this, spin_widget] (bool on)
                 {
                   spin_widget->setEnabled(on);
                   applyEditsToSelectedRule();
                 }
               );

        connect( spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this
               , [this] (double) { applyEditsToSelectedRule(); }
               );

        return row;
      };

  auto slope_row (new QHBoxLayout());
  slope_row->addLayout(make_bound_row(_slope_min_bounded, _slope_min, "min", " deg"));
  slope_row->addLayout(make_bound_row(_slope_max_bounded, _slope_max, "max", " deg"));
  // Steepness from horizontal cannot leave [0, 90] (slopeDegreesFromNormal), and validate() would
  // report a range outside it as a unit mistake -- so the spin boxes refuse to produce one.
  _slope_min->setRange(Noggit::TERRAIN_SLOPE_MIN_DEGREES, Noggit::TERRAIN_SLOPE_MAX_DEGREES);
  _slope_max->setRange(Noggit::TERRAIN_SLOPE_MIN_DEGREES, Noggit::TERRAIN_SLOPE_MAX_DEGREES);
  _slope_max->setValue(Noggit::TERRAIN_SLOPE_MAX_DEGREES);
  form->addRow("Slope", slope_row);

  auto height_row (new QHBoxLayout());
  height_row->addLayout(make_bound_row(_height_min_bounded, _height_min, "min", ""));
  height_row->addLayout(make_bound_row(_height_max_bounded, _height_max, "max", ""));
  // World Y. Blizzard terrain runs to roughly +/-2000; the wider range costs nothing and keeps a
  // custom map from hitting an arbitrary wall.
  _height_min->setRange(-20000.0, 20000.0);
  _height_max->setRange(-20000.0, 20000.0);
  form->addRow("Height", height_row);

  _strength = new QSpinBox(box);
  _strength->setRange(0, Noggit::TERRAIN_ALPHA_MAX);
  _strength->setValue(Noggit::TERRAIN_ALPHA_MAX);
  _strength->setToolTip
    ("Alpha written where this rule wins, on the same 0-255 scale as the Texturing tool's brush\n"
     "level. 255 replaces whatever was there; lower values blend with it.");
  form->addRow("Strength", _strength);

  _priority = new QSpinBox(box);
  _priority->setRange(-999, 999);
  _priority->setToolTip
    ("Highest priority wins first. Leave it at 0 unless two rules genuinely need an order that\n"
     "specificity does not already give them.");
  form->addRow("Priority", _priority);

  _enabled = new QCheckBox("Rule is active", box);
  _enabled->setChecked(true);
  _enabled->setToolTip("A disabled rule keeps its numbers but never matches and never wins.");
  form->addRow("", _enabled);

  connect(rescan, &QPushButton::clicked, this, [this] { refreshTextureList(); });

  connect( _texture, &QComboBox::currentTextChanged, this
         , [this] (QString const&) { applyEditsToSelectedRule(); });
  connect( _strength, qOverload<int>(&QSpinBox::valueChanged), this
         , [this] (int) { applyEditsToSelectedRule(); });
  connect( _priority, qOverload<int>(&QSpinBox::valueChanged), this
         , [this] (int) { applyEditsToSelectedRule(); });
  connect(_enabled, &QCheckBox::toggled, this, [this] (bool) { applyEditsToSelectedRule(); });

  return box;
}

QGroupBox* AutoTextureDialog::buildScopeBox()
{
  auto box (new QGroupBox("Scope", this));
  auto layout (new QVBoxLayout(box));

  _scope = new QComboBox(box);
  _scope->addItem("This ADT tile", 0);
  _scope->addItem("All loaded tiles", 1);
  layout->addWidget(_scope);

  layout->addWidget
    ( new QLabel("Rules are evaluated once per 8x8 chunk unit -- 64 decisions per chunk, 16384\n"
                 "per tile. Terrain that no rule claims is left exactly as it is.", box));

  // Rescanned on a scope change for the same reason the ground effect editor does it: "all loaded
  // tiles" can hold textures this tile does not, and offering one that is not in scope is how the
  // confusing "nothing changed" result gets back in.
  connect( _scope, qOverload<int>(&QComboBox::currentIndexChanged), this
         , [this] (int)
           {
             refreshTextureList();
             invalidatePreview();
           }
         );

  return box;
}

void AutoTextureDialog::refreshTextureList()
{
  auto const layers_by_texture (_map_view->terrainTexturesInScope(allLoadedTiles()));

  // Preserved verbatim, including a path the user typed that is in no list: a rescan after
  // painting must not silently retarget a rule.
  QString const previous (_texture->currentText());

  // clear() drives the editable combo's text to empty and setCurrentText below puts it back, and
  // between those two the selected rule would be written a rule with no texture -- which
  // validate() then reports. Held down rather than blocking signals on the combo, because the
  // guard is the same one showSelectedRule uses and one mechanism is easier to reason about.
  bool const was_updating = _updating_ui;
  _updating_ui = true;

  _texture->clear();

  for (auto const& entry : layers_by_texture)
  {
    QString const full (QString::fromStdString(entry.first));

    // The item TEXT is the full path, not the pretty short name, because the combo is editable:
    // currentText() then IS the rule's texture whether it was picked or typed, with no label to
    // translate back into a path. The layer count goes in the tooltip instead.
    _texture->addItem(full);
    _texture->setItemData
      ( _texture->count() - 1
      , QString("%1 layer%2 in scope").arg(entry.second).arg(entry.second == 1 ? "" : "s")
      , Qt::ToolTipRole
      );
  }

  std::string const selected (_map_view->selectedTexturePath());

  if (!selected.empty() && _texture->findText(QString::fromStdString(selected)) < 0)
  {
    _texture->addItem(QString::fromStdString(selected));
    _texture->setItemData(_texture->count() - 1, "Selected in the Texturing tool", Qt::ToolTipRole);
  }

  _texture->setCurrentText(previous);

  _updating_ui = was_updating;

  if (_texture->count() == 0)
  {
    _status->setText
      ("No terrain textures found in that scope. Type a tileset path, or load a tile.");
  }
}

bool AutoTextureDialog::allLoadedTiles() const
{
  return _scope->currentData().toInt() == 1;
}

Noggit::TerrainRuleSet AutoTextureDialog::buildRuleSet() const
{
  return Noggit::TerrainRuleSet(_rules);
}

int AutoTextureDialog::selectedRuleIndex() const
{
  auto const* item = _rule_list->currentItem();

  if (!item)
  {
    return -1;
  }

  bool ok = false;
  int const index = item->data(Qt::UserRole).toInt(&ok);

  return ok ? index : -1;
}

QString AutoTextureDialog::describeRule(std::size_t index) const
{
  Noggit::TerrainRule const& rule = _rules[index];

  QString const texture (QString::fromStdString(rule.texture));

  return QString("%1%2  slope %3, height %4  [a%5 p%6]")
           .arg(rule.enabled ? QString() : QString("(off) "))
           .arg(texture.isEmpty() ? QString("(no texture)") : shortTextureName(texture))
           .arg(describeRange(rule.slope, " deg"))
           .arg(describeRange(rule.height, QString()))
           .arg(rule.strength)
           .arg(rule.priority);
}

void AutoTextureDialog::rebuildRuleList()
{
  int const previously_selected = selectedRuleIndex();

  // Blocked so the clear/refill does not fire currentRowChanged for every intermediate state,
  // which would run showSelectedRule against a half-built list.
  QSignalBlocker const blocker (_rule_list);

  _rule_list->clear();

  for (std::size_t i = 0; i < _rules.size(); ++i)
  {
    auto* item = new QListWidgetItem(describeRule(i), _rule_list);
    item->setData(Qt::UserRole, static_cast<int>(i));

    if (static_cast<int>(i) == previously_selected)
    {
      _rule_list->setCurrentItem(item);
    }
  }

  if (_rule_list->currentItem() == nullptr && _rule_list->count() > 0)
  {
    _rule_list->setCurrentRow(0);
  }
}

void AutoTextureDialog::showSelectedRule()
{
  int const index = selectedRuleIndex();
  bool const have_rule = index >= 0 && static_cast<std::size_t>(index) < _rules.size();

  _rule_box->setEnabled(have_rule);

  if (!have_rule)
  {
    return;
  }

  Noggit::TerrainRule const& rule = _rules[static_cast<std::size_t>(index)];

  bool const was_updating = _updating_ui;
  _updating_ui = true;

  _texture->setCurrentText(QString::fromStdString(rule.texture));
  showRange(rule.slope, _slope_min_bounded, _slope_min, _slope_max_bounded, _slope_max);
  showRange(rule.height, _height_min_bounded, _height_min, _height_max_bounded, _height_max);
  _strength->setValue(rule.strength);
  _priority->setValue(rule.priority);
  _enabled->setChecked(rule.enabled);

  _updating_ui = was_updating;
}

bool AutoTextureDialog::commitSelectedRuleEdits()
{
  if (_updating_ui)
  {
    return false;
  }

  int const index = selectedRuleIndex();

  if (index < 0 || static_cast<std::size_t>(index) >= _rules.size())
  {
    return false;
  }

  Noggit::TerrainRule& rule = _rules[static_cast<std::size_t>(index)];

  Noggit::TerrainRule edited (rule);

  edited.texture = _texture->currentText().trimmed().toStdString();
  edited.slope = readRange(_slope_min_bounded, _slope_min, _slope_max_bounded, _slope_max);
  edited.height = readRange(_height_min_bounded, _height_min, _height_max_bounded, _height_max);
  edited.strength = static_cast<std::uint8_t>(_strength->value());
  edited.priority = _priority->value();
  edited.enabled = _enabled->isChecked();

  if (sameRule(edited, rule))
  {
    return false;
  }

  rule = std::move(edited);

  if (auto* item = _rule_list->currentItem())
  {
    item->setText(describeRule(static_cast<std::size_t>(index)));
  }

  return true;
}

void AutoTextureDialog::applyEditsToSelectedRule()
{
  if (_updating_ui)
  {
    return;
  }

  commitSelectedRuleEdits();

  // Unconditional, NOT gated on the return value. This is the path every editing signal takes, and
  // the direction the two possible mistakes fail in is not symmetric: invalidating a preview that
  // was still good costs one press of Preview, while keeping one that no longer describes the
  // rules is the whole reason the preview gate exists.
  invalidatePreview();
}

void AutoTextureDialog::onAddRule()
{
  Noggit::TerrainRule rule;

  // Default-constructed ranges are unbounded, so a new rule is the catch-all every set wants at
  // least one of. Narrowing it is then a deliberate act; widening one that started narrow is the
  // step people forget, and forgetting it is exactly what leaves terrain unclaimed.
  rule.texture = _texture->currentText().trimmed().toStdString();

  _rules.push_back(rule);

  rebuildRuleList();
  _rule_list->setCurrentRow(static_cast<int>(_rules.size()) - 1);
  showSelectedRule();
  invalidatePreview();
}

void AutoTextureDialog::onDuplicateRule()
{
  int const index = selectedRuleIndex();

  if (index < 0 || static_cast<std::size_t>(index) >= _rules.size())
  {
    return;
  }

  _rules.push_back(_rules[static_cast<std::size_t>(index)]);

  rebuildRuleList();
  _rule_list->setCurrentRow(static_cast<int>(_rules.size()) - 1);
  showSelectedRule();
  invalidatePreview();
}

void AutoTextureDialog::onRemoveRule()
{
  int const index = selectedRuleIndex();

  if (index < 0 || static_cast<std::size_t>(index) >= _rules.size())
  {
    return;
  }

  _rules.erase(_rules.begin() + index);

  rebuildRuleList();
  showSelectedRule();
  invalidatePreview();
}

void AutoTextureDialog::moveSelectedRule(int offset)
{
  int const index = selectedRuleIndex();
  int const target = index + offset;

  if ( index < 0 || static_cast<std::size_t>(index) >= _rules.size()
    || target < 0 || static_cast<std::size_t>(target) >= _rules.size()
     )
  {
    return;
  }

  std::swap(_rules[static_cast<std::size_t>(index)], _rules[static_cast<std::size_t>(target)]);

  rebuildRuleList();
  _rule_list->setCurrentRow(target);
  showSelectedRule();
  invalidatePreview();
}

void AutoTextureDialog::invalidatePreview()
{
  _preview = PreviewResult{};
  _preview_fresh = false;
  _apply_button->setEnabled(false);
}

std::vector<TileIndex> AutoTextureDialog::resolveScope() const
{
  std::vector<TileIndex> tiles;

  if (allLoadedTiles())
  {
    for (MapTile* tile : _map_view->getWorld()->mapIndex.loaded_tiles())
    {
      if (tile)
      {
        tiles.push_back(tile->index);
      }
    }

    // Sorted so that two resolutions of the same scope compare equal. loaded_tiles() walks the
    // 64x64 grid in a fixed order today, but the comparison in onApply is a safety check and
    // making it depend on that would be relying on the wrong thing.
    std::sort(tiles.begin(), tiles.end());
  }
  else
  {
    tiles.emplace_back(_map_view->cameraPosition());
  }

  return tiles;
}

void AutoTextureDialog::forEachChunkInScope
  ( std::vector<TileIndex> const& tiles
  , std::function<void(MapTile*, MapChunk*)> const& visit
  ) const
{
  World* world = _map_view->getWorld();

  auto const walk_tile = [&visit] (MapTile* tile)
  {
    // finishedLoading() even though loaded_tiles() already filters on it (map_index.cpp:29),
    // because getTile below does not: reading a half-parsed tile's chunk array is a data race.
    if (!tile || !tile->finishedLoading())
    {
      return;
    }

    for (int z = 0; z < 16; ++z)
    {
      for (int x = 0; x < 16; ++x)
      {
        if (MapChunk* chunk = tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(z)))
        {
          visit(tile, chunk);
        }
      }
    }
  };

  // getTile is a lookup and never triggers a load (map_index.cpp:614), so a tile that has been
  // unloaded since the scope was resolved comes back null and is skipped rather than dragged back
  // into memory to be painted.
  for (TileIndex const& index : tiles)
  {
    walk_tile(world->mapIndex.getTile(index));
  }
}

void AutoTextureDialog::onPreview()
{
  // Committed without invalidating: flushing the widgets is what makes the preview describe what
  // the user is looking at, and the preview this call would throw away is the one being built two
  // lines further down.
  commitSelectedRuleEdits();

  if (_rules.empty())
  {
    _report->setPlainText("No rules. Press Add to create one.");
    _status->setText("Nothing to preview.");
    invalidatePreview();
    return;
  }

  Noggit::TerrainRuleSet const rules (buildRuleSet());
  std::vector<std::string> const problems (rules.validate());

  PreviewResult result;
  result.units_per_rule.assign(rules.size(), 0);

  // Resolved ONCE, here, and carried on the result. Everything from this point on -- the walk
  // below, the report it produces, and the apply the user authorises from that report -- refers to
  // these tiles and not to wherever the camera has drifted to since.
  result.tiles = resolveScope();

  std::unordered_set<MapTile*> tiles;

  QApplication::setOverrideCursor(Qt::WaitCursor);

  QElapsedTimer timer;
  timer.start();

  forEachChunkInScope
    ( result.tiles
    , [&] (MapTile* tile, MapChunk* chunk)
      {
        tiles.insert(tile);
        ++result.chunks_scanned;

        // Distinct winners for THIS chunk, against the four MCLY slots. A rule set naming six
        // textures is fine as long as no single chunk needs more than four of them, and that is
        // not something the global tally can answer -- which is why TerrainRuleCoverage's own
        // documentation says the per-region count is what the layer decision needs.
        std::vector<std::string_view> winners;

        Collector::collectChunkUnits
          ( chunk
          , rules
          , [&] ( MapChunk*
                , int
                , int
                , Noggit::TerrainSample const&
                , Noggit::TerrainRuleResult const& unit_result
                )
            {
              result.coverage.addSample(unit_result);

              if (!unit_result.matched)
              {
                return;
              }

              if (unit_result.rule_index < result.units_per_rule.size())
              {
                ++result.units_per_rule[unit_result.rule_index];
              }

              if (std::find(winners.begin(), winners.end(), unit_result.texture) == winners.end())
              {
                winners.push_back(unit_result.texture);
              }
            }
          );

        if (winners.size() > MAX_CHUNK_LAYERS)
        {
          ++result.chunks_over_layer_limit;
          return;
        }

        if (!chunk->texture_set)
        {
          return;
        }

        // Layers already on the chunk that no rule claims still occupy MCLY slots. TextureSet
        // drops the ones painting nothing (eraseUnusedTextures, texture_set.cpp:294), so this is
        // "may need eviction" rather than "will fail" -- but a chunk in this state is where a
        // texture silently fails to be added, and the user should hear about it before, not after.
        std::size_t distinct = winners.size();

        for (std::size_t layer = 0; layer < chunk->texture_set->num(); ++layer)
        {
          std::string_view const existing (chunk->texture_set->filename(layer));

          if (std::find(winners.begin(), winners.end(), existing) == winners.end())
          {
            ++distinct;
          }
        }

        if (distinct > MAX_CHUNK_LAYERS)
        {
          ++result.chunks_needing_eviction;
        }
      }
    );

  qint64 const elapsed_ms = timer.elapsed();

  QApplication::restoreOverrideCursor();

  result.tiles_scanned = tiles.size();
  _preview = std::move(result);

  // --- the report ------------------------------------------------------------------------
  QStringList lines;

  if (_preview.chunks_scanned == 0)
  {
    lines << "No loaded terrain in that scope. Is the tile loaded?";
  }
  else
  {
    lines << QString("Scope: %1 tile(s), %2 chunk(s), %3 unit(s) evaluated in %4 ms.")
               .arg(_preview.tiles_scanned)
               .arg(_preview.chunks_scanned)
               .arg(_preview.coverage.sampleCount())
               .arg(elapsed_ms);
    lines << QString();
    lines << "Per rule:";

    for (std::size_t i = 0; i < _rules.size(); ++i)
    {
      std::size_t const claimed
        (i < _preview.units_per_rule.size() ? _preview.units_per_rule[i] : 0);

      lines << QString("  %1 %2 %3 unit(s)%4")
                 .arg(i, 3)
                 .arg(describeRule(i), -62)
                 .arg(claimed, 9)
                 .arg(claimed == 0 && _rules[i].enabled ? "   <- claims nothing" : "");
    }

    lines << QString();
    lines << "Per texture:";

    for (auto const& entry : _preview.coverage.entries())
    {
      lines << QString("  %1 %2 unit(s), average alpha %3, peak %4")
                 .arg(QString::fromStdString(entry.texture), -62)
                 .arg(entry.sample_count, 9)
                 .arg(entry.averageAlpha(), 0, 'f', 1)
                 .arg(entry.max_alpha);
    }

    lines << QString();

    std::size_t const unmatched = _preview.coverage.unmatchedCount();

    if (unmatched == 0)
    {
      lines << "Every unit in scope is claimed by a rule.";
    }
    else
    {
      double const share
        ( 100.0 * static_cast<double>(unmatched)
        / static_cast<double>(std::max<std::size_t>(1, _preview.coverage.sampleCount()))
        );

      lines << QString("UNCLAIMED: %1 unit(s), %2% of the scope, match no rule.")
                 .arg(unmatched).arg(share, 0, 'f', 1);
      lines << "Those units keep whatever is painted on them now. That is not a preview";
      lines << "artefact -- it is what Apply will leave behind.";
    }

    if (_preview.chunks_over_layer_limit > 0)
    {
      lines << QString();
      lines << QString("%1 chunk(s) need more than %2 textures and cannot be fully painted.")
                 .arg(_preview.chunks_over_layer_limit).arg(MAX_CHUNK_LAYERS);
    }

    if (_preview.chunks_needing_eviction > 0)
    {
      lines << QString();
      lines << QString("%1 chunk(s) would exceed %2 layers once the existing ones are counted.")
                 .arg(_preview.chunks_needing_eviction).arg(MAX_CHUNK_LAYERS);

      // Read rather than assumed, because eraseUnusedTextures is switched off wholesale by this
      // setting (texture_set.cpp:301-306) and the two answers are opposite: with it on, a layer
      // painting nothing is dropped and the paint lands; with it off, nothing is dropped and every
      // unit on that chunk is refused. Promising the first while the second happens is exactly the
      // kind of silent hole this report exists to prevent.
      lines << ( QSettings().value("cleanup_unused_textures", true).toBool()
               ? "Layers painting nothing are dropped there to make room; if every layer is in "
                 "use, that paint is refused."
               : "\"cleanup_unused_textures\" is off in the settings, so no layer can be dropped "
                 "and every unit on those chunks will be refused."
               );
    }
  }

  bool blocked = false;

  if (!problems.empty())
  {
    lines << QString();
    lines << "Problems reported by the rule set:";

    for (std::string const& problem : problems)
    {
      bool const advisory = problem == NO_CATCH_ALL_PROBLEM;
      blocked = blocked || !advisory;

      lines << QString("  %1 %2")
                 .arg(advisory ? "note:        " : "BLOCKS APPLY:")
                 .arg(QString::fromStdString(problem));
    }
  }

  _report->setPlainText(lines.join("\n"));

  // Fresh even when the set is unusable: the report is what a preview is FOR, and Apply is gated
  // separately on `blocked` below.
  _preview_fresh = _preview.chunks_scanned > 0;

  _apply_button->setEnabled(_preview_fresh && !blocked);

  if (blocked)
  {
    _status->setText("Fix the problems listed above before applying.");
  }
  else if (!_preview_fresh)
  {
    _status->setText("Nothing in scope.");
  }
  else
  {
    _status->setText(QString::fromStdString(_preview.coverage.summary()));
  }
}

void AutoTextureDialog::onApply()
{
  // Flushes a half-typed edit, and invalidates ONLY if that edit really moved a rule. The obvious
  // spelling -- applyEditsToSelectedRule() -- cannot work here: its last act is invalidatePreview(),
  // which clears _preview_fresh, and the guard below then refuses. Apply would be unreachable, on
  // every press, for every rule set.
  if (commitSelectedRuleEdits())
  {
    invalidatePreview();
  }

  if (!_preview_fresh)
  {
    _status->setText("Run a preview first -- the rules changed since the last one.");
    return;
  }

  // The scope is walked from _preview.tiles, not resolved again; this compares the two only to
  // notice that they have parted company. "This ADT tile" is the tile the CAMERA is over, this
  // dialog is modeless, and the camera can cross a tile boundary while the report is being read --
  // after which every number in it, including the unmatched-unit count that raises the confirmation
  // below, describes a tile that is no longer the one Apply would paint.
  if (resolveScope() != _preview.tiles)
  {
    invalidatePreview();
    _status->setText
      ("The scope moved since the preview: the camera is over a different tile, or the set of "
       "loaded tiles has changed. Nothing was painted. Run Preview again.");
    return;
  }

  Noggit::TerrainRuleSet const rules (buildRuleSet());

  // Re-validated here rather than trusting the button state. The rule set is rebuilt from _rules
  // on every call, and a refusal that lives only in setEnabled() is one stray signal away from
  // being no refusal at all.
  for (std::string const& problem : rules.validate())
  {
    if (problem == NO_CATCH_ALL_PROBLEM)
    {
      continue;
    }

    _status->setText(QString("Refused: %1").arg(QString::fromStdString(problem)));

    QMessageBox::warning
      ( this
      , "Automatic Texturing"
      , QString("This rule set cannot be applied.\n\n%1").arg(QString::fromStdString(problem))
      );
    return;
  }

  std::size_t const unmatched = _preview.coverage.unmatchedCount();

  if (unmatched > 0)
  {
    QMessageBox::StandardButton const answer = QMessageBox::question
      ( this
      , "Automatic Texturing"
      , QString("%1 unit(s) in scope match no rule and will keep whatever texture they have "
                "now.\n\nApply anyway?").arg(unmatched)
      , QMessageBox::Yes | QMessageBox::No
      , QMessageBox::No
      );

    if (answer != QMessageBox::Yes)
    {
      return;
    }
  }

  World* world = _map_view->getWorld();

  // Adding a layer constructs a scoped_blp_texture_reference, which loads and uploads a BLP.
  // Action::undo makes the context current before doing the same thing (Action.cpp:55-56); this
  // path has the same requirement.
  _map_view->context()->makeCurrent(_map_view->context()->surface());
  OpenGL::context::scoped_setter const context_setter (::gl, _map_view->context());

  std::map<std::string, scoped_blp_texture_reference> texture_refs;

  try
  {
    // One reference per distinct texture rather than one per paint call: a tile-sized scope is
    // tens of thousands of calls and each construction is a lookup in the async object map.
    for (std::string const& path : rules.distinctTextures())
    {
      if (path.empty())
      {
        continue;
      }

      texture_refs.emplace(path, scoped_blp_texture_reference(path, _map_view->getRenderContext()));
    }
  }
  catch (std::exception const& e)
  {
    LogError << "Auto-texturing could not load a texture: " << e.what() << std::endl;

    QMessageBox::critical
      ( this
      , "Automatic Texturing"
      , QString("A texture named by the rules could not be loaded.\n\n%1").arg(e.what())
      );
    return;
  }

  Brush unit_brush;
  unit_brush.init();
  unit_brush.setHardness(BRUSH_HARDNESS);
  unit_brush.setRadius(UNIT_BRUSH_RADIUS);

  Brush chunk_brush;
  chunk_brush.init();
  chunk_brush.setHardness(BRUSH_HARDNESS);
  chunk_brush.setRadius(CHUNK_BRUSH_RADIUS);

  std::size_t chunks_painted = 0;
  std::size_t chunks_uniform = 0;
  // Chunks the run brushed over and left byte-identical. Counted separately from chunks_painted
  // because the difference is the whole question of whether the tile is now unsaved.
  std::size_t chunks_unchanged = 0;
  std::size_t units_painted = 0;
  std::size_t units_refused = 0;
  std::size_t units_unchanged = 0;

  // Reused across chunks rather than declared inside the walk: 3 KiB of alpha each, and a tile is
  // 256 chunks.
  ChunkTextureFingerprint before;
  ChunkTextureFingerprint after;

  QApplication::setOverrideCursor(Qt::WaitCursor);

  QElapsedTimer timer;
  timer.start();

  // ONE action for the whole run. beginAction/endAction bracket the entire scope rather than each
  // chunk, so undo takes the map back to before the run in a single press;
  // registerChunkTextureChange is what accumulates the per-chunk before-state inside it, and it
  // already ignores a chunk it has seen (Action.cpp:671-675).
  NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);

  try
  {
    forEachChunkInScope
      ( _preview.tiles
      , [&] (MapTile* tile, MapChunk* chunk)
        {
          TextureSet* texture_set = chunk->texture_set.get();

          if (!texture_set)
          {
            return;
          }

          std::array<Noggit::TerrainRuleResult, UNITS_PER_CHUNK> decisions{};

          Collector::collectChunkUnits
            ( chunk
            , rules
            , [&decisions] ( MapChunk*
                           , int unit_row
                           , int unit_col
                           , Noggit::TerrainSample const&
                           , Noggit::TerrainRuleResult const& unit_result
                           )
              {
                decisions[unit_row * Collector::CHUNK_INNER_SIDE + unit_col] = unit_result;
              }
            );

          // A chunk every rule agrees on is one brush pass instead of 64. Flat ground is most of
          // most maps, so this is the difference between a tile taking a moment and taking most of
          // a second, and it costs one comparison per unit to find out.
          bool uniform = decisions[0].matched;

          for (auto const& decision : decisions)
          {
            if ( !decision.matched
              || decision.texture != decisions[0].texture
              || decision.alpha != decisions[0].alpha
               )
            {
              uniform = false;
              break;
            }
          }

          // Registered on the first operation that will really touch the chunk -- a paint, or the
          // layer eviction resolve() may have to do to make room -- so a chunk the rules do not
          // move does not put a copy of its three alpha maps into the undo step. The before-image
          // is taken at the same instant, which is the last moment at which it is still the
          // chunk's original state.
          bool touched = false;

          // Set once this chunk has proved it has no layer to spare; see resolve().
          bool eviction_failed = false;

          auto const ensure_registered = [&]
          {
            if (touched)
            {
              return;
            }

            captureTextureFingerprint(texture_set, before);
            NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
            touched = true;
          };

          // A chunk that is already nothing but the winning texture has nothing to paint, and
          // paintTexture agrees -- it returns early on nTextures == 1 (texture_set.cpp:830), with
          // no alpha map in existence for the strength to be written into. Caught here so the
          // commonest no-op costs nothing at all; the general case is caught after the fact by
          // comparing the two fingerprints, because a chunk with two or more layers can be
          // repainted with exactly the values it already holds and neither paintTexture nor
          // apply_alpha_changes will say so.
          auto const already_done = [&] (Noggit::TerrainRuleResult const& decision)
          {
            return texture_set->num() == 1
                && std::string_view(texture_set->filename(0)) == decision.texture;
          };

          // The texture reference to paint a decision with, or null when this chunk cannot take
          // it. Not an error: four MCLY slots is a hard limit and the preview already counted the
          // chunks that hit it. Re-asked per unit because a paint can free or fill a slot.
          auto const resolve = [&] (Noggit::TerrainRuleResult const& decision)
            -> scoped_blp_texture_reference*
          {
            auto const found = texture_refs.find(std::string(decision.texture));

            if (found == texture_refs.end())
            {
              return nullptr;
            }

            if (texture_set->canPaintTexture(found->second))
            {
              return &found->second;
            }

            // canPaintTexture answers "already here, or is there a spare slot" and stops there
            // (texture_set.cpp:271-286). The paint does not stop there: get_texture_index_or_add
            // drops layers that paint nothing before it gives up (texture_set.cpp:396). Mirroring
            // that is what makes the preview's promise -- "layers painting nothing are dropped
            // there to make room" -- true. Without it the preview counts the chunk under
            // chunks_needing_eviction, says the unused layer will go, and then every unit on that
            // chunk is refused with no way to find it afterwards.
            //
            // Asked once per chunk, not once per unit. A chunk whose four layers are all in use
            // fails this for every one of its 64 units, and each failed attempt still walks every
            // texel of every layer (texture_set.cpp:308-346). Nothing between two units can turn a
            // failure into a success either: the only thing that frees a slot is paintTexture's
            // own trailing eraseUnusedTextures, and after that canPaintTexture answers true above
            // without ever reaching here.
            if (eviction_failed)
            {
              return nullptr;
            }

            // Registered first: eraseUnusedTextures rewrites the layer list in place, so the undo
            // entry has to exist before it runs.
            ensure_registered();

            if (!texture_set->eraseUnusedTextures())
            {
              eviction_failed = true;
              return nullptr;
            }

            return texture_set->canPaintTexture(found->second) ? &found->second : nullptr;
          };

          // The one place a texture layer weight is written, and paintTexture owns all of it:
          // adding or evicting the layer (get_texture_index_or_add), creating the in-flight float
          // alpha maps, and rescaling the other layers so the texel still sums to 255. Strength is
          // on the same 0-255 scale as the Texturing tool's brush level (texturing_tool.cpp:816),
          // and pressure is 1 because a rule is an assignment rather than a stroke that builds up.
          auto const paint = [&] ( Noggit::TerrainRuleResult const& decision
                                 , scoped_blp_texture_reference& texture
                                 , glm::vec3 const& position
                                 , Brush& brush
                                 )
          {
            chunk->paintTexture
              (position, &brush, static_cast<float>(decision.alpha), 1.0f, texture);
          };

          // Held per chunk and only added to the run's totals once the chunk is known to have
          // really moved. A chunk that ends where it started contributes to units_unchanged
          // instead, so the closing message never claims work that did not happen.
          std::size_t chunk_units_painted = 0;
          bool one_brush_pass = false;

          if (uniform)
          {
            if (already_done(decisions[0]))
            {
              units_unchanged += UNITS_PER_CHUNK;
              return;
            }

            if (scoped_blp_texture_reference* texture = resolve(decisions[0]))
            {
              glm::vec3 const centre
                (chunk->xbase + CHUNKSIZE * 0.5f, chunk->ybase, chunk->zbase + CHUNKSIZE * 0.5f);

              ensure_registered();
              paint(decisions[0], *texture, centre, chunk_brush);

              chunk_units_painted += UNITS_PER_CHUNK;
              one_brush_pass = true;
            }
            else
            {
              units_refused += UNITS_PER_CHUNK;
            }
          }
          else
          {
            for (int unit_row = 0; unit_row < Collector::CHUNK_INNER_SIDE; ++unit_row)
            {
              for (int unit_col = 0; unit_col < Collector::CHUNK_INNER_SIDE; ++unit_col)
              {
                auto const& decision
                  = decisions[unit_row * Collector::CHUNK_INNER_SIDE + unit_col];

                // Unmatched units are the ones the preview counted as unclaimed. Leaving them
                // untouched is the documented behaviour, not an oversight.
                if (!decision.matched)
                {
                  continue;
                }

                if (already_done(decision))
                {
                  ++units_unchanged;
                  continue;
                }

                scoped_blp_texture_reference* texture = resolve(decision);

                if (!texture)
                {
                  ++units_refused;
                  continue;
                }

                ensure_registered();

                paint(decision, *texture, unitCentre(chunk, unit_row, unit_col), unit_brush);

                ++chunk_units_painted;
              }
            }
          }

          if (!touched)
          {
            return;
          }

          // Committed per chunk rather than left for the save path. An uncommitted stroke keeps a
          // tmp_edit_alpha_values alive -- 4 planes of 4096 floats, 64 KiB -- and the Action
          // caches a copy of it for both the before and the after state, so deferring across a
          // tile would cost about 48 MiB for nothing. apply_alpha_changes also raises the ALPHAMAP
          // update flag and the lod-map dirty bit (texture_set.cpp:1676).
          texture_set->apply_alpha_changes();

          // The after-image, and the only trustworthy answer to "did this chunk actually move".
          // Re-applying a rule set that has already been applied repaints every multi-layer chunk
          // with the values it is already holding: paintTexture reports that as a change, and
          // taking its word for it marks the tile unsaved and tells the user thousands of units
          // were painted when the file on disk would come out identical.
          captureTextureFingerprint(texture_set, after);

          if (after == before)
          {
            units_unchanged += chunk_units_painted;
            ++chunks_unchanged;
            return;
          }

          units_painted += chunk_units_painted;
          chunks_uniform += one_brush_pass ? 1 : 0;
          ++chunks_painted;

          // registerChunkUpdate is a render-refresh flag and cannot mean "unsaved" -- every chunk
          // that loads sets the same bits. The save path tests MapTile::changed instead
          // (map_index.cpp:555), so without this the ADT is skipped by saveChanged and the whole
          // run is lost on unload.
          world->mapIndex.setChanged(tile);
        }
      );
  }
  catch (std::exception const& e)
  {
    // The action has to be closed on this path too. An action left open swallows every later edit
    // into itself, so the editor would keep working and undo would then revert an arbitrary amount
    // of unrelated work.
    NOGGIT_ACTION_MGR->endAction();
    QApplication::restoreOverrideCursor();

    LogError << "Auto-texturing failed partway through: " << e.what() << std::endl;

    QMessageBox::critical
      ( this
      , "Automatic Texturing"
      , QString("The run failed partway through and was stopped.\n\n%1\n\nPress undo to revert "
                "what it had already painted.").arg(e.what())
      );

    invalidatePreview();
    return;
  }

  NOGGIT_ACTION_MGR->endAction();

  qint64 const elapsed_ms = timer.elapsed();

  QApplication::restoreOverrideCursor();

  if (chunks_painted == 0)
  {
    QString message
      ( units_unchanged > 0
      ? QString("Nothing to do: the %1 unit(s) the rules claimed already carry exactly what the "
                "rules ask for. No tile was marked changed.").arg(units_unchanged)
      : QString("Nothing changed. Either no rule claimed anything in scope, or every chunk that "
                "would have been painted already holds four textures that are all in use.")
      );

    // Said out loud rather than left to be discovered. The undo entry cannot be avoided -- a
    // chunk's before-image has to be cached before it is painted, and whether the paint moved
    // anything is only knowable afterwards -- so the honest thing is to warn that the next undo
    // press will look broken.
    if (chunks_unchanged > 0)
    {
      message += QString(" An undo step was still recorded for the %1 chunk(s) the run brushed "
                         "over; undoing it will appear to do nothing, because nothing changed.")
                   .arg(chunks_unchanged);
    }

    _status->setText(message);
    Log << message.toStdString() << std::endl;

    // Nothing was painted, but the terrain may still have been touched (a layer painting nothing
    // can have been dropped), and the run has consumed the preview either way.
    invalidatePreview();
    return;
  }

  QString message
    ( QString("Painted %1 unit(s) across %2 chunk(s) in %3 ms; %4 chunk(s) were a single texture "
              "and took one brush pass. One undo press reverts the whole run.")
        .arg(units_painted).arg(chunks_painted).arg(elapsed_ms).arg(chunks_uniform));

  if (units_unchanged > 0)
  {
    message += QString(" %1 unit(s) already carried the texture the rules ask for.")
                 .arg(units_unchanged);
  }

  if (chunks_unchanged > 0)
  {
    message += QString(" %1 chunk(s) came out byte-identical and were not marked changed.")
                 .arg(chunks_unchanged);
  }

  if (units_refused > 0)
  {
    message += QString(" %1 unit(s) were refused: their chunk holds four textures and every one "
                       "of them is in use.").arg(units_refused);
  }

  message += " Save the ADTs to keep it.";

  _status->setText(message);
  Log << message.toStdString() << std::endl;

  // The terrain just changed under the preview, so the numbers on screen no longer describe it.
  invalidatePreview();
  _report->appendPlainText("\n--- applied ---\n" + message);
}
