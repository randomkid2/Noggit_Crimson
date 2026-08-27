// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_CHUNKMANIPULATORPANEL_HPP
#define NOGGIT_CHUNKMANIPULATORPANEL_HPP

#include <noggit/ui/tools/ChunkManipulator/ChunkData.hpp>
#include <noggit/ui/tools/ChunkManipulator/ChunkGridTransform.hpp>

#include <QtWidgets/QWidget>

#include <array>
#include <cstddef>

class MapView;
class QBoxLayout;
class QCheckBox;
class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QToolButton;

namespace Noggit::Ui::Tools::UiCommon
{
  class ExtendedSlider;
}

namespace Noggit::Ui::Tools::ChunkManipulator
{
  class ChunkClipboard;

  //! The chunk manipulator's dock panel: selection size, the thirteen copy classes, the paste
  //! options, the transforms, the clipboard buttons and the two status lines.
  //!
  //! Owns the ChunkClipboard. Before this revision the panel's constructor body was empty and
  //! nothing in the tree constructed a ChunkClipboard at all, so the tool opened a blank panel
  //! over a clipboard that did not exist.
  class ChunkManipulatorPanel : public QWidget
  {
    Q_OBJECT

  public:
    explicit ChunkManipulatorPanel (MapView* map_view, QWidget* parent = nullptr);

    [[nodiscard]]
    ChunkClipboard* clipboard() const;

    [[nodiscard]]
    float selectionRadius() const;

    void changeSelectionRadius (float change);

    //! True while the eyedropper is armed: a click picks exactly one chunk instead of a radius.
    [[nodiscard]]
    bool eyedropperActive() const;

    void setEyedropperActive (bool active);

    //! Nudge the paste height offset, for Shift + wheel.
    void changeHeightOffset (float change);

    // Driven by the tool's hotkeys as well as by the buttons, so both routes go through one
    // implementation and cannot drift.
    void doCopy();
    void doPaste();
    void doClearSelection();
    void doClearClipboard();
    void applyTransform (ChunkGridOp op);

  private:
    void buildSelectionSection (QBoxLayout* column);
    void buildCopySection (QBoxLayout* column);
    void buildPasteSection (QBoxLayout* column);
    void buildTransformSection (QBoxLayout* column);
    void buildClipboardSection (QBoxLayout* column);

    void pushCopyFlags();
    void pushPasteFlags();
    void refreshStatus();
    void setMessage (QString const& text, char const* color_token);

    void exportPack();
    void importPack();

    MapView* _map_view;
    ChunkClipboard* _clipboard;

    UiCommon::ExtendedSlider* _radius_slider = nullptr;
    UiCommon::ExtendedSlider* _height_offset_slider = nullptr;
    QComboBox* _height_mode = nullptr;

    //! Indexed by the bit position of the ChunkCopyFlags value each box owns, so the map from
    //! box to flag is a lookup rather than thirteen hand-written if statements.
    static constexpr std::size_t COPY_FLAG_COUNT = 13;
    std::array<QCheckBox*, COPY_FLAG_COUNT> _copy_boxes {};

    QCheckBox* _paste_replace = nullptr;
    QCheckBox* _paste_at_source = nullptr;
    QCheckBox* _paste_replace_objects = nullptr;
    QCheckBox* _paste_sew_seams = nullptr;

    QToolButton* _eyedropper = nullptr;

    QLabel* _selection_status = nullptr;
    QLabel* _clipboard_status = nullptr;
    QLabel* _message = nullptr;

    //! Watches the shared cross-window pack so that a copy in another editor process shows up
    //! here without the user asking for it. This is the "copied in another window" path.
    QFileSystemWatcher* _shared_watcher = nullptr;
  };
}

#endif // NOGGIT_CHUNKMANIPULATORPANEL_HPP
