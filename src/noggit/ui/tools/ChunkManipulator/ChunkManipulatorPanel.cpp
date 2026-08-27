// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ChunkManipulatorPanel.hpp"
#include "ChunkClipboard.hpp"
#include "ChunkPack.hpp"

#include <noggit/MapView.h>
#include <noggit/ui/DesignTokens.hpp>
#include <noggit/ui/FontNoggit.hpp>
#include <noggit/ui/tools/ToolPanel/ToolWidgetStyle.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>

#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

using namespace Noggit::Ui::Tools::ChunkManipulator;
namespace Style = Noggit::Ui::Tools::ToolPanelStyle;
namespace Design = Noggit::Ui::Design;

namespace
{
  //! The thirteen copy classes, in the order they appear in the panel.
  //!
  //! One table rather than thirteen hand-written check boxes and thirteen hand-written if
  //! statements, so that a box and the flag it owns cannot come apart -- which is exactly how
  //! the previous revision's ChunkCopyFlags lost AREA_ID: the enumerator existed and no field
  //! and no code behind it did.
  struct CopyFlagEntry
  {
    ChunkCopyFlags flag;
    char const* label;
    char const* tooltip;
  };

  constexpr std::array<CopyFlagEntry, 13> COPY_FLAGS
  {{
    { ChunkCopyFlags::TERRAIN, "Terrain"
    , "The 145 vertex heights. Vertex X and Z are the destination's own grid." }
    , { ChunkCopyFlags::TEXTURES, "Textures"
    , "The layer list, by file name. Without Alphamaps this re-textures a chunk and leaves its "
      "blend alone." }
    , { ChunkCopyFlags::ALPHAMAPS, "Alphamaps"
    , "The blend weights. Without Textures this copies a blend pattern onto whatever the "
      "destination already uses." }
    , { ChunkCopyFlags::LIQUID, "Liquid"
    , "Water, lava and slime layers, rebuilt against the destination chunk." }
    , { ChunkCopyFlags::MODELS, "M2 models"
    , "Doodads whose origin stands on a copied chunk, by file name and transform." }
    , { ChunkCopyFlags::WMOS, "WMOs"
    , "World objects whose origin stands on a copied chunk, by file name and transform." }
    , { ChunkCopyFlags::VERTEX_COLORS, "Vertex colors"
    , "MCCV tinting, together with both of the flags that decide whether it is written at all." }
    , { ChunkCopyFlags::SHADOWS, "Shadows"
    , "The 64x64 baked shadow map." }
    , { ChunkCopyFlags::HOLES, "Holes"
    , "The 4x4 hole mask." }
    , { ChunkCopyFlags::FLAGS, "Chunk flags"
    , "The MCNK header flags. The vertex-colour bit is never taken on its own -- see the tool "
      "notes." }
    , { ChunkCopyFlags::AREA_ID, "Area ID"
    , "The zone this chunk belongs to." }
    , { ChunkCopyFlags::GROUND_EFFECT_IDS, "Ground-effect IDs"
    , "The MCLY per-layer ground-effect ids, and the texture layer flags stored beside them." }
    , { ChunkCopyFlags::GROUND_EFFECT_EXCLUSION, "Ground-effect exclusion"
    , "The 8x8 mask of units where detail doodads are suppressed." }
  }};

  //! How often the shared cross-window clipboard is checked, in milliseconds.
  //!
  //! A poll rather than QFileSystemWatcher alone, because the pack is published by writing a
  //! sibling and renaming it into place, and a watcher loses its subscription when the path it
  //! watches is replaced rather than modified. The watcher is still installed as the fast path;
  //! this is what makes the feature work regardless of which of the two fires. The cost when
  //! nothing has changed is one stat() every two seconds.
  constexpr int SHARED_CLIPBOARD_POLL_MS = 2000;
}

ChunkManipulatorPanel::ChunkManipulatorPanel (MapView* map_view, QWidget* parent)
  : QWidget (parent)
  , _map_view (map_view)
  , _clipboard (new ChunkClipboard (map_view, this))
{
  // The dock's shared shell: zero margins, S3 between sections, a 250px floor. ToolWidgetStyle
  // listed this panel as one of the four tool widgets it had not reached; it has now.
  auto* const column (Style::toolColumn (this));

  column->addWidget
    (Style::keybindRow (this, FontNoggit::shift, FontNoggit::lmb, tr ("Select chunks")));
  column->addWidget
    (Style::keybindRow (this, FontNoggit::ctrl, FontNoggit::lmb, tr ("Deselect chunks")));
  column->addWidget
    (Style::keybindRow (this, FontNoggit::alt, FontNoggit::lmb_drag, tr ("Size the brush")));

  buildSelectionSection (column);
  buildCopySection (column);
  buildTransformSection (column);
  buildPasteSection (column);
  buildClipboardSection (column);

  connect ( _clipboard, &ChunkClipboard::selectionChanged
          , this, [this] (std::set<SelectedChunkIndex> const&) { refreshStatus(); }
          );
  connect (_clipboard, &ChunkClipboard::selectionCleared, this, [this] { refreshStatus(); });
  connect (_clipboard, &ChunkClipboard::clipboardChanged, this, [this] { refreshStatus(); });

  connect ( _clipboard, &ChunkClipboard::pasted
          , this
          , [this] (ChunkPasteReport const& report)
            {
              QString text (tr ("Pasted: %1 chunks, %2 objects added, %3 removed")
                              .arg (report.chunks)
                              .arg (report.objects_added)
                              .arg (report.objects_removed));

              if (report.chunks_skipped)
              {
                text += tr (" (%1 chunks fell outside the map)").arg (report.chunks_skipped);
              }

              setMessage (text, report.chunks_skipped ? Design::WARN : Design::OK);
            }
          );

  // ---- the cross-window clipboard -------------------------------------------------------------
  _shared_watcher = new QFileSystemWatcher (this);

  auto const shared_path
    (QString::fromStdWString (sharedClipboardPackPath().wstring()));

  auto const rearm
    ( [this, shared_path]
      {
        if (_shared_watcher->files().isEmpty() && QFileInfo::exists (shared_path))
        {
          _shared_watcher->addPath (shared_path);
        }
      }
    );

  rearm();

  connect ( _shared_watcher, &QFileSystemWatcher::fileChanged
          , this, [this, rearm] { _clipboard->adoptSharedClipboard(); rearm(); }
          );

  auto* const poll (new QTimer (this));
  connect ( poll, &QTimer::timeout
          , this, [this, rearm] { _clipboard->adoptSharedClipboard(); rearm(); }
          );
  poll->start (SHARED_CLIPBOARD_POLL_MS);

  refreshStatus();
}

ChunkClipboard* ChunkManipulatorPanel::clipboard() const
{
  return _clipboard;
}

// =================================================================================================
// Sections
// =================================================================================================

void ChunkManipulatorPanel::buildSelectionSection (QBoxLayout* column)
{
  auto* const section (Style::toolSection (column, tr ("Selection")));
  auto* const layout (Style::sectionColumn (section));

  _radius_slider = new UiCommon::ExtendedSlider (section);
  _radius_slider->setPrefix (tr ("Size:"));
  _radius_slider->setRange (0.0, 500.0);
  // One slider step per yard. ExtendedSlider's constructor gives the slider 0..100
  // (ExtendedSlider.cpp:109) whatever the spin box's range is, so at the default a drag would
  // move the radius five yards at a time -- a chunk is 33.33 yards across, so that is a sixth of
  // a chunk per pixel of travel. Every other tool in the dock leaves the default because their
  // ranges are 0..1 or 0..1000 where 1% is a sensible step; a chunk radius is not.
  _radius_slider->setSliderRange (0, 500);
  _radius_slider->setDecimals (2);
  _radius_slider->setValue (15.0);
  layout->addWidget (_radius_slider);

  auto* const row (new QWidget (section));
  auto* const row_layout (new QHBoxLayout (row));
  row_layout->setContentsMargins (0, 0, 0, 0);
  row_layout->setSpacing (Design::S1);

  // THE EYEDROPPER. Checkable rather than momentary: it changes what the next click MEANS, and
  // a mode the user is in has to be visible while they are in it.
  _eyedropper = new QToolButton (row);
  _eyedropper->setCheckable (true);
  _eyedropper->setIcon (FontNoggitIcon (FontNoggit::POINTER));
  _eyedropper->setToolTip (tr ("Eyedropper: the next click selects exactly the chunk under the "
                               "cursor instead of a radius."));
  row_layout->addWidget (_eyedropper);

  auto* const clear (new QPushButton (tr ("Clear selection"), row));
  clear->setToolTip (tr ("Shortcut: X"));
  connect (clear, &QPushButton::clicked, this, [this] { doClearSelection(); });
  row_layout->addWidget (clear, 1);

  layout->addWidget (row);

  _selection_status = new QLabel (section);
  _selection_status->setWordWrap (true);
  layout->addWidget (_selection_status);
}

void ChunkManipulatorPanel::buildCopySection (QBoxLayout* column)
{
  auto* const section (Style::toolSection (column, tr ("Copy")));
  auto* const layout (Style::sectionColumn (section));

  for (std::size_t i (0); i < COPY_FLAGS.size(); ++i)
  {
    auto* const box (new QCheckBox (tr (COPY_FLAGS[i].label), section));
    box->setToolTip (tr (COPY_FLAGS[i].tooltip));
    box->setChecked (hasFlag (_clipboard->copyFlags(), COPY_FLAGS[i].flag));
    connect (box, &QCheckBox::toggled, this, [this] { pushCopyFlags(); });
    layout->addWidget (box);
    _copy_boxes[i] = box;
  }
}

void ChunkManipulatorPanel::buildTransformSection (QBoxLayout* column)
{
  auto* const section (Style::segmentedSection (column, tr ("Transform")));
  auto* const grid (Style::segmentGrid (section));

  struct TransformEntry
  {
    char const* label;
    int quarter_turns;
    ChunkGridOp op;
  };

  // Rotation is expressed as repeated quarter turns of the ONE derived permutation rather than
  // as three separate index maps, so 180 and 270 cannot disagree with 90 about which way round
  // the block goes -- or about the yaw every carried model gets.
  static constexpr std::array<TransformEntry, 5> ENTRIES
  {{
    {"Rotate 90", 1, ChunkGridOp::ROTATE_90}
    , {"Rotate 180", 2, ChunkGridOp::ROTATE_90}
    , {"Rotate 270", 3, ChunkGridOp::ROTATE_90}
    , {"Mirror H", 1, ChunkGridOp::MIRROR_X}
    , {"Mirror V", 1, ChunkGridOp::MIRROR_Z}
  }};

  for (int i (0); i < static_cast<int> (ENTRIES.size()); ++i)
  {
    auto* const button (Style::segmentButton (section, tr (ENTRIES[i].label)));
    // Not checkable: these are actions applied to the clipboard, not a state the panel holds.
    // Two "Rotate 90" presses leave the block at 180, and the block itself is the record of
    // that -- a checked chip would be a second, redundant one that could disagree.
    button->setCheckable (false);

    int const turns (ENTRIES[i].quarter_turns);
    ChunkGridOp const op (ENTRIES[i].op);

    connect ( button, &QPushButton::clicked, this
            , [this, turns, op] { for (int t (0); t < turns; ++t) { applyTransform (op); } }
            );

    grid->addWidget (button, i / 3, i % 3);
  }
}

void ChunkManipulatorPanel::buildPasteSection (QBoxLayout* column)
{
  auto* const section (Style::toolSection (column, tr ("Paste")));
  auto* const layout (Style::sectionColumn (section));

  auto const add_box
    ( [&] (QCheckBox*& target, QString const& label, QString const& tooltip, bool checked)
      {
        target = new QCheckBox (label, section);
        target->setToolTip (tooltip);
        target->setChecked (checked);
        connect (target, &QCheckBox::toggled, this, [this] { pushPasteFlags(); });
        layout->addWidget (target);
      }
    );

  add_box ( _paste_replace, tr ("Replace destination")
          , tr ("A copied class that turned out to be EMPTY at the source clears the destination "
                "rather than leaving it alone. In practice this is what decides whether pasting "
                "dry ground removes the water that was there.")
          , true
          );

  add_box ( _paste_at_source, tr ("Paste at source ADT location")
          , tr ("Ignore the cursor and put every chunk back at the map coordinates it was copied "
                "from.")
          , false
          );

  add_box ( _paste_replace_objects, tr ("Models: replace destination objects")
          , tr ("Delete the models and WMOs standing on the destination chunks before adding the "
                "copied ones.")
          , false
          );

  add_box ( _paste_sew_seams, tr ("Sew terrain seams automatically")
          , tr ("Weld the pasted block's outer height edge to the terrain it lands against. "
                "Heights only -- there is no alphamap, liquid or shadow seam fixing anywhere in "
                "this editor.")
          , true
          );

  _height_mode = new QComboBox (section);
  _height_mode->addItem (tr ("Height: absolute (source elevation)"));
  _height_mode->addItem (tr ("Height: relative (destination elevation)"));
  _height_mode->setToolTip (tr ("Absolute reproduces the source exactly. Relative keeps the shape "
                                "and re-bases it onto the ground under the cursor, which is what "
                                "makes a hillside copied from a valley usable on a plateau."));
  connect ( _height_mode, QOverload<int>::of (&QComboBox::currentIndexChanged), this
          , [this] (int index)
            {
              _clipboard->setHeightMode (index == 1 ? ChunkHeightMode::DESTINATION_ELEVATION
                                                    : ChunkHeightMode::SOURCE_ELEVATION);
            }
          );
  layout->addWidget (_height_mode);

  _height_offset_slider = new UiCommon::ExtendedSlider (section);
  _height_offset_slider->setPrefix (tr ("Height offset:"));
  // The first ExtendedSlider in the tree with a NEGATIVE minimum. sliderToSpin and spinToSlider
  // carry a spin-minimum offset term for exactly this case (ExtendedSlider.cpp:179-210) and
  // their own note says it is "here for the next caller that sets a non-zero minimum, not for
  // any that exists" -- this is that caller, and the term is what makes 0 land in the middle of
  // the track rather than at its left end.
  _height_offset_slider->setRange (-500.0, 500.0);
  // 1000 steps over a 1000-unit span: one slider step per unit of height.
  _height_offset_slider->setSliderRange (0, 1000);
  // Tablet pressure ADDS a fraction of the full range to value() while a stylus is down
  // (ExtendedSlider.cpp:244-248). That is meaningful for a brush strength and meaningless for a
  // paste offset, where it would silently move a pasted block by up to 1000 units.
  _height_offset_slider->setTabletSupportEnabled (false);
  _height_offset_slider->setDecimals (2);
  _height_offset_slider->setValue (0.0);
  _height_offset_slider->setToolTip (tr ("Added to every pasted height, and to every carried "
                                         "model. Shift + mouse wheel changes it."));
  connect ( _height_offset_slider, &UiCommon::ExtendedSlider::valueChanged
          , this, [this] (double value) { _clipboard->setHeightOffset (static_cast<float> (value)); }
          );
  layout->addWidget (_height_offset_slider);

  pushPasteFlags();
}

void ChunkManipulatorPanel::buildClipboardSection (QBoxLayout* column)
{
  auto* const section (Style::toolSection (column, tr ("Clipboard")));
  auto* const layout (Style::sectionColumn (section));

  auto* const actions (new QWidget (section));
  auto* const actions_layout (new QHBoxLayout (actions));
  actions_layout->setContentsMargins (0, 0, 0, 0);
  actions_layout->setSpacing (Design::S1);

  auto* const copy (new QPushButton (tr ("Copy"), actions));
  copy->setToolTip (tr ("Capture the selection. Shortcut: C"));
  connect (copy, &QPushButton::clicked, this, [this] { doCopy(); });
  actions_layout->addWidget (copy, 1);

  auto* const paste (new QPushButton (tr ("Paste"), actions));
  paste->setToolTip (tr ("Shortcut: V"));
  connect (paste, &QPushButton::clicked, this, [this] { doPaste(); });
  actions_layout->addWidget (paste, 1);

  layout->addWidget (actions);

  auto* const clear (new QPushButton (tr ("Clear clipboard"), section));
  connect (clear, &QPushButton::clicked, this, [this] { doClearClipboard(); });
  layout->addWidget (clear);

  auto* const packs (new QWidget (section));
  auto* const packs_layout (new QHBoxLayout (packs));
  packs_layout->setContentsMargins (0, 0, 0, 0);
  packs_layout->setSpacing (Design::S1);

  auto* const export_button (new QPushButton (tr ("Export pack..."), packs));
  export_button->setToolTip (tr ("Write the clipboard to a .ncp file that another Noggit window, "
                                 "on another map, can import."));
  connect (export_button, &QPushButton::clicked, this, [this] { exportPack(); });
  packs_layout->addWidget (export_button, 1);

  auto* const import_button (new QPushButton (tr ("Import pack..."), packs));
  connect (import_button, &QPushButton::clicked, this, [this] { importPack(); });
  packs_layout->addWidget (import_button, 1);

  layout->addWidget (packs);

  _clipboard_status = new QLabel (section);
  _clipboard_status->setWordWrap (true);
  layout->addWidget (_clipboard_status);

  _message = new QLabel (section);
  _message->setWordWrap (true);
  layout->addWidget (_message);
}

// =================================================================================================
// State
// =================================================================================================

void ChunkManipulatorPanel::pushCopyFlags()
{
  ChunkCopyFlags flags (ChunkCopyFlags::NONE);

  for (std::size_t i (0); i < COPY_FLAGS.size(); ++i)
  {
    if (_copy_boxes[i] && _copy_boxes[i]->isChecked())
    {
      flags |= COPY_FLAGS[i].flag;
    }
  }

  _clipboard->setCopyFlags (flags);
}

void ChunkManipulatorPanel::pushPasteFlags()
{
  ChunkPasteFlags flags (ChunkPasteFlags::NONE);

  if (_paste_replace && _paste_replace->isChecked())
  {
    flags |= ChunkPasteFlags::REPLACE_DESTINATION;
  }

  if (_paste_at_source && _paste_at_source->isChecked())
  {
    flags |= ChunkPasteFlags::AT_SOURCE_LOCATION;
  }

  if (_paste_replace_objects && _paste_replace_objects->isChecked())
  {
    flags |= ChunkPasteFlags::REPLACE_OBJECTS;
  }

  if (_paste_sew_seams && _paste_sew_seams->isChecked())
  {
    flags |= ChunkPasteFlags::SEW_SEAMS;
  }

  _clipboard->setPasteFlags (flags);
}

void ChunkManipulatorPanel::refreshStatus()
{
  if (_selection_status)
  {
    _selection_status->setText
      (tr ("Selection: %1 chunk(s)").arg (_clipboard->selectedChunks().size()));
  }

  if (!_clipboard_status)
  {
    return;
  }

  if (!_clipboard->hasClipboard())
  {
    _clipboard_status->setText (tr ("Clipboard: empty"));
    return;
  }

  QString text (tr ("Clipboard: %1 chunk(s)").arg (_clipboard->clipboardChunkCount()));

  if (!_clipboard->clipboardMapName().empty())
  {
    text += tr (" from '%1'").arg (QString::fromStdString (_clipboard->clipboardMapName()));
  }

  if (_clipboard->clipboardFromAnotherWindow())
  {
    text += tr (", copied in another window");
  }

  _clipboard_status->setText (text);
}

void ChunkManipulatorPanel::setMessage (QString const& text, char const* color_token)
{
  if (!_message)
  {
    return;
  }

  // A widget-level sheet, which outranks the application sheet for this label and reaches
  // nothing else. The colour comes from DesignTokens so a palette change upstream still reaches
  // it and it never needs a colour of its own.
  _message->setStyleSheet (QStringLiteral ("color: %1;").arg (QLatin1String (color_token)));
  _message->setText (text);
}

// =================================================================================================
// Actions
// =================================================================================================

float ChunkManipulatorPanel::selectionRadius() const
{
  return _radius_slider ? static_cast<float> (_radius_slider->value()) : 15.0f;
}

void ChunkManipulatorPanel::changeSelectionRadius (float change)
{
  if (_radius_slider)
  {
    _radius_slider->setValue (_radius_slider->value() + change);
  }
}

bool ChunkManipulatorPanel::eyedropperActive() const
{
  return _eyedropper && _eyedropper->isChecked();
}

void ChunkManipulatorPanel::setEyedropperActive (bool active)
{
  if (_eyedropper)
  {
    _eyedropper->setChecked (active);
  }
}

void ChunkManipulatorPanel::changeHeightOffset (float change)
{
  if (_height_offset_slider)
  {
    _height_offset_slider->setValue (_height_offset_slider->value() + change);
  }
}

void ChunkManipulatorPanel::doCopy()
{
  unsigned const count (_clipboard->copySelected (_map_view->cursorPosition()));

  if (count)
  {
    setMessage (tr ("Copied %1 chunk(s).").arg (count), Design::OK);
  }
  else
  {
    setMessage (tr ("Nothing to copy: select chunks with Shift + left click first."), Design::WARN);
  }
}

void ChunkManipulatorPanel::doPaste()
{
  if (!_clipboard->hasClipboard())
  {
    setMessage (tr ("The clipboard is empty."), Design::WARN);
    return;
  }

  ChunkPasteReport const report (_clipboard->pasteSelection (_map_view->cursorPosition()));

  if (!report.chunks)
  {
    setMessage (tr ("Nothing was pasted: the cursor is not over a loaded ADT, every destination "
                    "chunk falls outside the map, or another edit is still in progress."),
                Design::WARN);
  }

  _map_view->invalidate();
}

void ChunkManipulatorPanel::doClearSelection()
{
  _clipboard->clearSelection();
  setMessage (tr ("Selection cleared."), Design::TEXT_DIM);
}

void ChunkManipulatorPanel::doClearClipboard()
{
  _clipboard->clearClipboard();
  setMessage (tr ("Clipboard cleared."), Design::TEXT_DIM);
}

void ChunkManipulatorPanel::applyTransform (ChunkGridOp op)
{
  if (!_clipboard->hasClipboard())
  {
    setMessage (tr ("Nothing on the clipboard to transform."), Design::WARN);
    return;
  }

  _clipboard->applyGridOp (op);
}

// =================================================================================================
// Packs
// =================================================================================================

void ChunkManipulatorPanel::exportPack()
{
  if (!_clipboard->hasClipboard())
  {
    setMessage (tr ("The clipboard is empty; there is nothing to export."), Design::WARN);
    return;
  }

  QString const path
    ( QFileDialog::getSaveFileName
        (this, tr ("Export chunk pack"), QString(), tr ("Noggit chunk pack (*.ncp)"))
    );

  if (path.isEmpty())
  {
    return;
  }

  try
  {
    _clipboard->exportPack (std::filesystem::path (path.toStdWString()));
    setMessage (tr ("Exported %1 chunk(s).").arg (_clipboard->clipboardChunkCount()), Design::OK);
  }
  catch (ChunkPackError const& error)
  {
    QString const message (QString::fromUtf8 (error.what()));
    setMessage (message, Design::BAD);
    QMessageBox::warning (this, tr ("Export failed"), message);
  }
}

void ChunkManipulatorPanel::importPack()
{
  QString const path
    ( QFileDialog::getOpenFileName
        (this, tr ("Import chunk pack"), QString(), tr ("Noggit chunk pack (*.ncp)"))
    );

  if (path.isEmpty())
  {
    return;
  }

  try
  {
    _clipboard->importPack (std::filesystem::path (path.toStdWString()));
    setMessage (tr ("Imported %1 chunk(s).").arg (_clipboard->clipboardChunkCount()), Design::OK);
  }
  catch (ChunkPackError const& error)
  {
    // The clipboard is untouched: importPack decodes the whole file into a local and assigns
    // only on success, so a refused pack leaves whatever was already there intact. Saying so
    // matters -- the alternative reading of a failed import is "my clipboard is now half gone".
    QString const message (QString::fromUtf8 (error.what())
                           + QString (" The clipboard was left unchanged."));
    setMessage (message, Design::BAD);
    QMessageBox::warning (this, tr ("Import failed"), message);
  }
}
