// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QFontDatabase>
#include <QtGui/QPainter>

#include <noggit/ui/FontNoggit.hpp>

namespace
{
  // The Noggit icon font ships inside the Qt resource system (resources/resources.qrc), so
  // unlike Font Awesome it is always present. Resolving it once still matters: this used to sit
  // inside paint(), which called QFontDatabase::addApplicationFont again for every icon height
  // the engine had not seen before, and threw std::runtime_error on failure. Throwing out of a
  // paint path unwinds through Qt's own painting code, and the repaint that provoked it repeats
  // forever -- so the failure mode of a missing resource was an unrecoverable window rather than
  // an ugly one. An empty family here degrades to the base engine's text fallback instead.
  QString const& noggitFontFamily()
  {
    static QString const resolved
      ( []
        {
          int const id (QFontDatabase::addApplicationFont(":/fonts/noggit_font.ttf"));

          if (id == -1)
          {
            return QString();
          }

          QStringList const families (QFontDatabase::applicationFontFamilies(id));

          return families.isEmpty() ? QString() : families.at(0);
        }()
      );

    return resolved;
  }
}

namespace Noggit
{
  namespace Ui
  {

      // IconSet::Noggit is what keeps this engine's lookups off the Font Awesome tables. The
      // two codepoint spaces overlap -- FontNoggit::TOOL_LIGHT is 0xf8d0 and there are Font
      // Awesome names in that range too -- so without it a tool icon could resolve to another
      // set's artwork and paint a confidently wrong picture.
      FontNoggitIconEngine::FontNoggitIconEngine(const QString& text)
        : FontAwesomeIconEngine(text, IconSet::Noggit)
        , _text(text)
      {}

      FontNoggitIconEngine* FontNoggitIconEngine::clone() const
      {
        return new FontNoggitIconEngine(_text);
      }

      void FontNoggitIconEngine::paint(QPainter* painter
        , QRect const& rect
        , QIcon::Mode mode
        , QIcon::State state
      )
      {
        painter->save();
        {
          QColor const pen_color
            (iconPenColorForMode(iconProbePalette<FontNoggitButtonStyle>(), mode, state));

          painter->setPen(pen_color);

          if (mode == QIcon::Disabled)
          {
            painter->setOpacity(painter->opacity() * ICON_DISABLED_OPACITY);
          }

          // Theme artwork, then the drawn set, then the font below. Identical order to the base
          // engine and for the same reason -- see FontAwesomeIconEngine::paintArtwork.
          if (paintArtwork(painter, rect, state, pen_color))
          {
            painter->restore();
            return;
          }

          QString const& family(noggitFontFamily());

          // The second test is what the enumerators past AREA_TRIGGER_SPHERE need: they are
          // real icons with no glyph in the shipped font, and drawText on a codepoint the font
          // does not carry paints .notdef -- an empty box that reads as a broken button.
          //
          // hasFallback() is what keeps that test off the ~60 keybinding hints. They have no
          // table row on purpose, so they take the font path unconditionally and the Help
          // window's key glyphs cannot regress into "?" if inFont() ever answers wrongly.
          if ( family.isEmpty()
            || ( !FontAwesomeFont::hasGlyph(family, codepoint())
              && FontAwesomeFont::hasFallback(IconSet::Noggit, codepoint())
               )
             )
          {
            // Same degradation the Font Awesome path already takes: an outlined box with a short
            // label, so the control stays visible and hittable rather than becoming a blank gap.
            paintFallback(painter, rect, mode, state);
            painter->restore();
            return;
          }

          if (!_fonts.count(rect.height()))
          {
            QFont font(family);
            font.setPixelSize(rect.height());

            _fonts[rect.height()] = font;
          }

          painter->setFont(_fonts.at(rect.height()));

          painter->drawText
          (rect, _text, QTextOption(Qt::AlignCenter | Qt::AlignVCenter));
        }
        painter->restore();
      }

      std::map<int, QFont> FontNoggitIconEngine::_fonts = {};

      FontNoggitIcon::FontNoggitIcon(FontNoggit::Icons const& icon)
        : QIcon(new FontNoggitIconEngine(QString(QChar(icon))))
      {}

  }
}
