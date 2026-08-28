// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/AutoTextureDialog.hpp>
#include <noggit/ui/DesignTokens.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/terrain/TerrainRulePainter.hpp>
#include <noggit/terrain/TerrainRuleStore.hpp>
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
#include <cmath>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace Noggit::Ui;

namespace
{
  namespace Collector = Noggit::TerrainRuleCollector;

  // An MCNK holds at most four texture layers. Not a tunable; it is the width of the MCLY array.
  constexpr std::size_t MAX_CHUNK_LAYERS = 4;

  // The brush geometry, the alphamap fingerprint and the per-chunk paint that used to live here
  // are now TerrainRulePainter, because Live Auto Texture paints the same rules onto the same
  // chunks at the end of a sculpting stroke and the two must not be able to disagree. See the note
  // above the painter's construction in onApply.

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
  // THE MONOSPACE RANK, from the design system's type scale: MONO_FAMILY at FONT_MONO with the
  // regular weight. This was QFontDatabase::systemFont(FixedFont), which on Windows resolves to
  // Courier New at the system's POINT size -- a different family and a different size from the
  // Lua log and the SQL changeset preview, so the editor's three monospaced readouts were three
  // different typefaces. StyleHint Monospace is the fallback chain for a machine without
  // Consolas: it is not redistributed with this repository (see ATTRIBUTION.md) and must be
  // resolved by name from the system font database, exactly as the interface family is.
  {
    QFont report_font (QString::fromLatin1(Design::MONO_FAMILY));
    report_font.setStyleHint(QFont::Monospace, QFont::PreferMatch);
    report_font.setPixelSize(Design::FONT_MONO);
    report_font.setWeight(Design::WEIGHT_REGULAR);
    _report->setFont(report_font);
  }
  _report->setPlainText("Add rules, choose a scope, then press Preview.");
  right->addWidget(_report, 1);

  _status = new QLabel(this);
  _status->setWordWrap(true);
  right->addWidget(_status);

  // LIVE AUTO TEXTURE, the opt-in surface.
  //
  // It lives HERE, in the window where the rules are authored and previewed, and nowhere else. The
  // feature repaints chunks as a side effect of a sculpting gesture, which overwrites hand-painted
  // alpha; the only person who should be able to arm that is one who has already written a rule
  // set, run a preview and read the unclaimed count. A toolbar button or a menu entry would let it
  // be switched on by somebody who has never seen what these rules would do.
  //
  // Unchecked at every application start. TerrainRuleStore deliberately does not persist the flag;
  // see the note above that class.
  _live_auto = new QCheckBox("Live: retexture terrain as I sculpt", this);
  _live_auto->setChecked(Noggit::TerrainRuleStore::instance()->liveAutoEnabled());
  _live_auto->setToolTip
    ("When on, the chunks a Raise/Lower or Flatten/Blur stroke moves are retextured against these\n"
     "rules the moment the stroke ends, together with one ring of neighbouring chunks so the\n"
     "result does not stop at a chunk border. The paint joins the stroke's own undo step, so one\n"
     "Ctrl+Z reverts the shape and the texture together.\n"
     "\n"
     "This OVERWRITES painting already on those chunks. It is off every time Noggit starts.");
  right->addWidget(_live_auto);

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

  connect( _live_auto, &QCheckBox::toggled, this
         , [this] (bool on)
           {
             Noggit::TerrainRuleStore::instance()->setLiveAutoEnabled(on);

             // Said out loud rather than left to the tooltip. The one thing a user has to
             // understand before arming this is that it writes over painting they did by hand, and
             // a tooltip is only read by someone already unsure.
             _status->setText
               ( on
               ? "Live retexturing is ON. Terrain strokes will repaint the chunks they move, plus "
                 "one ring of neighbours, using these rules. That overwrites painting already "
                 "there; one Ctrl+Z reverts the shape and the paint together."
               : "Live retexturing is off."
               );
           }
         );

  // The rules are the store's, not this dialog's. Seeded from it here so a rule set survives the
  // window being closed and the application being restarted, and pushed back to it on every edit
  // (see publishRules) so the live stroke path paints the rules the user is actually looking at
  // rather than a copy taken when the dialog opened.
  _rules = Noggit::TerrainRuleStore::instance()->rules();

  // Every widget exists from here on, so the write-back path is safe to arm.
  _updating_ui = false;

  refreshTextureList();
  rebuildRuleList();
  showSelectedRule();
  invalidatePreview();
}

void AutoTextureDialog::publishRules() const
{
  // A no-op when nothing moved: TerrainRuleStore::setRules compares the lists before it writes
  // QSettings, and this is called from every path that can touch a rule -- including the ones that
  // turn out not to have. There is nothing to notify; the store is not a QObject and has no change
  // signal, deliberately (TerrainRuleStore.hpp).
  Noggit::TerrainRuleStore::instance()->setRules(_rules);
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

  // Published on the one path that knows a rule actually moved. The live stroke path reads the
  // store, so a rule edited here and not published would be previewed one way and painted another.
  publishRules();

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

  publishRules();

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

  publishRules();

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

  publishRules();

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

  publishRules();

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

  // Adding a layer constructs a scoped_blp_texture_reference, which loads and uploads a BLP.
  // Action::undo makes the context current before doing the same thing (Action.cpp:55-56); this
  // path has the same requirement.
  _map_view->context()->makeCurrent(_map_view->context()->surface());
  OpenGL::context::scoped_setter const context_setter (::gl, _map_view->context());

  // The paint itself is TerrainRulePainter, and this dialog no longer owns a line of it.
  //
  // THE REASON IS LIVE AUTO TEXTURE. The same rules are now painted from a second place -- the end
  // of a terrain sculpting stroke, see LiveAutoTexture.hpp -- and two implementations of "turn a
  // TerrainRuleSet into alpha on a chunk" would drift the first time either was touched. The drift
  // would surface as "the live pass paints something different from the preview I approved", which
  // is not falsifiable from outside the editor. One implementation, called from two places.
  //
  // What stays here is what only this dialog can do: the preview gate, the pinned scope, the layer
  // report, and the one action that brackets the whole run.
  Noggit::TerrainRulePainter painter (_map_view, rules);

  {
    std::string texture_error;

    if (!painter.prepareTextures(texture_error))
    {
      LogError << "Auto-texturing could not load a texture: " << texture_error << std::endl;

      QMessageBox::critical
        ( this
        , "Automatic Texturing"
        , QString("A texture named by the rules could not be loaded.\n\n%1")
            .arg(QString::fromStdString(texture_error))
        );
      return;
    }
  }

  QApplication::setOverrideCursor(Qt::WaitCursor);

  QElapsedTimer timer;
  timer.start();

  // ONE action for the whole run. beginAction/endAction bracket the entire scope rather than each
  // chunk, so undo takes the map back to before the run in a single press;
  // registerChunkTextureChange is what accumulates the per-chunk before-state inside it, and it
  // already ignores a chunk it has seen (Action.cpp:701-705).
  NOGGIT_ACTION_MGR->beginAction(_map_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);

  try
  {
    forEachChunkInScope
      ( _preview.tiles
      , [&painter] (MapTile* tile, MapChunk* chunk) { painter.paintChunk(tile, chunk); }
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

  // Counted by the painter, one chunk at a time, and restated here unaltered. Whether a chunk
  // belongs under chunks_painted or under chunks_unchanged is decided by its before/after alphamap
  // fingerprint rather than by how many paint calls this dialog issued, and that distinction is
  // the whole question of whether the tile is now unsaved.
  Noggit::TerrainPaintStats const& stats = painter.stats();

  QApplication::restoreOverrideCursor();

  if (stats.chunks_painted == 0)
  {
    QString message
      ( stats.units_unchanged > 0
      ? QString("Nothing to do: the %1 unit(s) the rules claimed already carry exactly what the "
                "rules ask for. No tile was marked changed.").arg(stats.units_unchanged)
      : QString("Nothing changed. Either no rule claimed anything in scope, or every chunk that "
                "would have been painted already holds four textures that are all in use.")
      );

    // Said out loud rather than left to be discovered. The undo entry cannot be avoided -- a
    // chunk's before-image has to be cached before it is painted, and whether the paint moved
    // anything is only knowable afterwards -- so the honest thing is to warn that the next undo
    // press will look broken.
    if (stats.chunks_unchanged > 0)
    {
      message += QString(" An undo step was still recorded for the %1 chunk(s) the run brushed "
                         "over; undoing it will appear to do nothing, because nothing changed.")
                   .arg(stats.chunks_unchanged);
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
        .arg(stats.units_painted).arg(stats.chunks_painted).arg(elapsed_ms).arg(stats.chunks_uniform));

  if (stats.units_unchanged > 0)
  {
    message += QString(" %1 unit(s) already carried the texture the rules ask for.")
                 .arg(stats.units_unchanged);
  }

  if (stats.chunks_unchanged > 0)
  {
    message += QString(" %1 chunk(s) came out byte-identical and were not marked changed.")
                 .arg(stats.chunks_unchanged);
  }

  if (stats.units_refused > 0)
  {
    message += QString(" %1 unit(s) were refused: their chunk holds four textures and every one "
                       "of them is in use.").arg(stats.units_refused);
  }

  message += " Save the ADTs to keep it.";

  _status->setText(message);
  Log << message.toStdString() << std::endl;

  // The terrain just changed under the preview, so the numbers on screen no longer describe it.
  invalidatePreview();
  _report->appendPlainText("\n--- applied ---\n" + message);
}
