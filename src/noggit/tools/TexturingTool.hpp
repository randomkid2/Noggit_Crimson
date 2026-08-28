// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Tool.hpp>
#include <noggit/BoolToggleProperty.hpp>

class QDockWidget;

namespace Noggit
{
    namespace Ui
    {
        class texturing_tool;
        struct tileset_chooser;
        class texture_picker;
        class texture_palette_small;
        class TextureLayerManager;
    }

    class TexturingTool final : public Tool
    {
    public:
        TexturingTool(MapView* mapView);
        ~TexturingTool();

        [[nodiscard]]
        char const* name() const override;

        [[nodiscard]]
        editing_mode editingMode() const override;

        [[nodiscard]]
        Ui::FontNoggit::Icons icon() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;

        void registerMenuItems(QMenu* menu) override;

        [[nodiscard]]
        ToolDrawParameters drawParameters() const override;

        void onSelected() override;

        void onDeselected() override;

        void onTick(float deltaTime, TickParameters const& params) override;

        void onMousePress(MousePressParameters const& params) override;

        void onMouseMove(MouseMoveParameters const& params) override;

        void onMouseWheel(MouseWheelParameters const& params) override;

        void hidePopups() override;

        // Path of the texture currently selected in the texturing UI, or empty when there is none.
        //
        // Exposed so the ground effect editor can apply a set to "the texture you are working
        // with" without being handed the texturing widget, which would tie a library editor to a
        // brush tool for the sake of one string.
        [[nodiscard]]
        std::string selectedTexturePath() const;

    private:
        Ui::texturing_tool* _texturingTool = nullptr;
        QDockWidget* _textureBrowserDock = nullptr;
        Ui::tileset_chooser* _textureBrowser = nullptr;
        Ui::texture_picker* _texturePicker = nullptr;
        Ui::texture_palette_small* _texturePalette = nullptr;
        QDockWidget* _texturePaletteDock = nullptr;
        QDockWidget* _texturePickerDock = nullptr;

        // The layer budget window: layer replacement, prepare area, and the duplicate and
        // threshold purges. Built on first use and kept, so the palette the user assembled is
        // still there the next time they open it.
        //
        // NOT deleted in the destructor and NOT given WA_DeleteOnClose, unlike the docks above:
        // it is parented to the MapView, which is the only owner it needs. The docks carry both a
        // delete here and a deleteLater on the view's destruction, which is a race this does not
        // need to join.
        Ui::TextureLayerManager* _textureLayerManager = nullptr;
        bool _texturePickerNeedUpdate = false;
        Noggit::BoolToggleProperty _show_texture_browser_window = { false };
        Noggit::BoolToggleProperty _show_texture_palette_window = { false };

        void randomizeTexturingRotation();

        void setupTextureBrowser(MapView* mv);
        void setupTexturePalette(MapView* mv);
        void setupTexturePicker(MapView* mv);

        void showTextureLayerManager();
    };
}
