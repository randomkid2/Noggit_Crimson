// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UI_CLICKABLELABEL_HPP
#define NOGGIT_UI_CLICKABLELABEL_HPP

#include <QtWidgets/QLabel>
#include <QMouseEvent>

namespace Noggit
{
  namespace Ui
  {
    //! A QLabel that emits on click. Seven of these are primary interactions in the texturing
    //! dock -- the 128x128 current-texture swatch, the four chunk-layer tiles in TexturePicker,
    //! and the brush-mask thumbnail shared by TerrainTool, ShaderTool and texturing_tool -- and
    //! until the pointing-hand cursor the constructor now sets, not one of them said it was
    //! clickable before it was clicked. See the note on the constructor, which also records why
    //! the hover-state property that used to live here was removed rather than completed.
    class ClickableLabel : public QLabel
    {
      Q_OBJECT

    public:
      ClickableLabel(QWidget* parent=nullptr);

    signals:
      void clicked();
      void leftClicked();
      void rightClicked();
      void middleClicked();

    protected:
      virtual void mouseReleaseEvent (QMouseEvent* event) override;
    };
  }
}

#endif // NOGGIT_UI_CLICKABLELABEL_HPP
