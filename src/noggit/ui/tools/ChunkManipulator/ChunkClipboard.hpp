// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_CHUNKCLIPBOARD_HPP
#define NOGGIT_CHUNKCLIPBOARD_HPP

#include <noggit/ui/tools/ChunkManipulator/ChunkData.hpp>
#include <noggit/ui/tools/ChunkManipulator/ChunkGridTransform.hpp>

#include <QtCore/QObject>

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

class MapView;
class World;

namespace Noggit::Ui::Tools::ChunkManipulator
{
  //! The selection, the copied block, and the paste. Owned by ChunkManipulatorPanel.
  //!
  //! WHAT CHANGED HERE AND WHY IT MATTERS. The previous revision of this class compiled and was
  //! never instantiated: nothing in the tree constructed a ChunkClipboard, copySelected built a
  //! ChunkCache on the stack and never appended it to _cached_chunks, and pasteSelection had an
  //! empty body. It is now owned by the panel, it stores what it copies, and it can put it back.
  class ChunkClipboard : public QObject
  {
    Q_OBJECT

  public:
    explicit ChunkClipboard (MapView* map_view, QObject* parent = nullptr);

    // ---- selection -------------------------------------------------------------------------

    //! Add or remove every loaded chunk within `radius` of `cursor_pos`.
    void selectRange (glm::vec3 const& cursor_pos, float radius, ChunkSelectionMode mode);

    //! Add or remove the single chunk under `pos`. This is the eyedropper.
    void selectChunk (glm::vec3 const& pos, ChunkSelectionMode mode);

    void selectChunk (SelectedChunkIndex const& index, ChunkSelectionMode mode);

    //! Drop the selection. Emits selectionCleared() -- which the previous revision declared and
    //! never emitted anywhere, so "Clear selection" would have left every listener showing a
    //! selection that no longer existed.
    void clearSelection();

    [[nodiscard]]
    std::set<SelectedChunkIndex> const& selectedChunks() const;

    // ---- clipboard -------------------------------------------------------------------------

    //! Capture the selection, with `pivot_pos` as the origin every relative offset is measured
    //! from. Returns the number of chunks actually captured.
    unsigned copySelected (glm::vec3 const& pivot_pos);

    void clearClipboard();

    [[nodiscard]]
    bool hasClipboard() const;

    [[nodiscard]]
    std::size_t clipboardChunkCount() const;

    [[nodiscard]]
    std::string const& clipboardMapName() const;

    //! True when the clipboard came from a pack rather than from this window's own copy -- an
    //! imported file, or the shared cross-window clipboard another editor wrote.
    [[nodiscard]]
    bool clipboardFromAnotherWindow() const;

    // ---- transforms ------------------------------------------------------------------------

    //! Rotate or mirror the whole clipboard in place, terrain and objects together.
    //!
    //! Applied to the CLIPBOARD, not to the map, so the same code serves an in-memory copy and
    //! an imported pack and the result can be inspected before it is pasted anywhere.
    void applyGridOp (ChunkGridOp op);

    // ---- paste -----------------------------------------------------------------------------

    //! Write the clipboard to the map with `pos` as the destination pivot.
    //!
    //! One undo step for the whole operation, terrain and objects included. Returns what it did,
    //! for the status line.
    ChunkPasteReport pasteSelection (glm::vec3 const& pos);

    // ---- parameters ------------------------------------------------------------------------

    [[nodiscard]] ChunkCopyFlags copyFlags() const;
    void setCopyFlags (ChunkCopyFlags flags);

    [[nodiscard]] ChunkPasteFlags pasteFlags() const;
    void setPasteFlags (ChunkPasteFlags flags);

    [[nodiscard]] ChunkHeightMode heightMode() const;
    void setHeightMode (ChunkHeightMode mode);

    [[nodiscard]] float heightOffset() const;
    void setHeightOffset (float offset);

    // ---- packs -----------------------------------------------------------------------------

    //! Write the clipboard to a pack file. Throws ChunkPackError.
    void exportPack (std::filesystem::path const& path) const;

    //! Replace the clipboard from a pack file. Throws ChunkPackError, and on a throw the
    //! clipboard is left exactly as it was -- the decode completes into a local before anything
    //! here is touched.
    void importPack (std::filesystem::path const& path);

    //! Pick up the shared cross-window pack, if another editor process wrote it since we last
    //! did. Returns true when the clipboard changed. Never throws: a half-written or foreign
    //! file simply means there is nothing to adopt yet.
    bool adoptSharedClipboard();

  signals:
    void selectionChanged (std::set<Noggit::Ui::Tools::ChunkManipulator::SelectedChunkIndex> const& selected_chunks);
    void selectionCleared();
    //! The clipboard's contents changed: a copy, an import, a transform, or a clear.
    void clipboardChanged();
    void pasted (Noggit::Ui::Tools::ChunkManipulator::ChunkPasteReport const& report);

  private:
    //! The chunk grid coordinate a paste of the current clipboard would put `rel` at.
    [[nodiscard]]
    bool resolvePasteOrigin (glm::vec3 const& pos, int& base_global_x, int& base_global_z) const;

    //! Best-effort publish of the clipboard for other editor processes. Failures are swallowed
    //! deliberately: a temp directory that cannot be written to must not make copying fail.
    void publishSharedClipboard();

    MapView* _map_view;
    World* _world;

    std::set<SelectedChunkIndex> _selected_chunks;
    std::vector<CachedChunk> _cached_chunks;

    //! Where the copy pivot was, so a same-map paste can be put back exactly.
    glm::vec3 _clipboard_pivot {0.0f, 0.0f, 0.0f};
    SelectedChunkIndex _clipboard_pivot_chunk;
    //! Ground height under the pivot at copy time -- the reference for ChunkHeightMode::DESTINATION_ELEVATION.
    float _clipboard_pivot_height = 0.0f;
    std::string _clipboard_map_name;
    std::uint32_t _clipboard_map_id = 0;
    ChunkCopyFlags _clipboard_flags = ChunkCopyFlags::NONE;
    bool _clipboard_from_another_window = false;

    //! Our own last write to the shared pack, so adoptSharedClipboard can tell somebody else's
    //! copy from the echo of our own.
    std::filesystem::file_time_type _shared_clipboard_stamp {};
    bool _shared_clipboard_stamp_valid = false;

    ChunkCopyFlags _copy_flags = ChunkCopyFlags::ALL;
    ChunkPasteFlags _paste_flags = ChunkPasteFlags::REPLACE_DESTINATION | ChunkPasteFlags::SEW_SEAMS;
    ChunkHeightMode _height_mode = ChunkHeightMode::SOURCE_ELEVATION;
    float _height_offset = 0.0f;
  };
}

#endif // NOGGIT_CHUNKCLIPBOARD_HPP
