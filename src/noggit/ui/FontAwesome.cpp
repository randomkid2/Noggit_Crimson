// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// -----------------------------------------------------------------------------------------
// The Font Awesome font file is NOT distributed with this repository.
//
// Upstream shipped resources/font_awesome.otf, which is "Font Awesome 5 Pro Regular" 5.14.0 --
// a paid product whose licence forbids redistributing the font file and specifically forbids
// committing it to a public repository. This fork therefore removed the file and the
// ":/fonts/font_awesome.otf" Qt resource entry. Nothing here downloads it either.
//
// TO RESTORE THE ICONS: put a Font Awesome desktop font file in a "fonts" folder next to
// noggit.exe -- that is  <install dir>/fonts/  -- under any of the names listed in
// FONT_FILE_CANDIDATES below (for example "fa-solid-900.ttf" from the free
// "Font Awesome Free ... Desktop" download, or your own licensed
// "Font Awesome 5 Pro-Regular-400.otf"). A Font Awesome family installed system-wide is
// picked up as well. Font Awesome Free is SIL OFL 1.1 licensed and may be installed freely,
// but it does not contain every glyph the Pro set does, so a few of the codepoints in
// FontAwesome::Icons will stay blank even with Free installed.
//
// WITHOUT ANY OF THAT the application still runs: FontAwesomeIconEngine paints a standard Qt
// icon where one fits the meaning, and a short text label otherwise. Buttons stay labelled;
// they just are not pretty. This is a real, visible degradation and is not hidden.
// -----------------------------------------------------------------------------------------

#include <QtCore/QDir>
#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>

#include <noggit/ui/FontAwesome.hpp>

#include <algorithm>
#include <unordered_map>

namespace
{
  // File names looked for in <exe dir>/fonts, <exe dir> and ./fonts, in this order.
  QStringList const FONT_FILE_CANDIDATES
    { "font_awesome.otf"
    , "fa-solid-900.ttf"
    , "fa-regular-400.ttf"
    , "fa-brands-400.ttf"
    , "Font Awesome 6 Free-Solid-900.otf"
    , "Font Awesome 6 Free-Regular-400.otf"
    , "Font Awesome 5 Free-Solid-900.otf"
    , "Font Awesome 5 Free-Regular-400.otf"
    , "Font Awesome 5 Pro-Regular-400.otf"
    , "Font Awesome 5 Pro-Solid-900.otf"
    };

  QStringList fontSearchDirectories()
  {
    QStringList directories;

    QString const app_dir (QCoreApplication::applicationDirPath());

    if (!app_dir.isEmpty())
    {
      directories << app_dir + "/fonts" << app_dir;
    }

    directories << QDir::currentPath() + "/fonts" << QDir::currentPath();

    return directories;
  }

  QString familyFromFile()
  {
    for (QString const& directory : fontSearchDirectories())
    {
      for (QString const& file_name : FONT_FILE_CANDIDATES)
      {
        QString const path (directory + "/" + file_name);

        if (!QFileInfo::exists (path))
        {
          continue;
        }

        int const id (QFontDatabase::addApplicationFont (path));

        if (id == -1)
        {
          continue;
        }

        QStringList const families (QFontDatabase::applicationFontFamilies (id));

        if (!families.isEmpty())
        {
          return families.at (0);
        }
      }
    }

    return QString();
  }

  QString familyFromSystem()
  {
    QStringList const installed (QFontDatabase().families());

    QString first_match;

    for (QString const& family : installed)
    {
      if (!family.startsWith ("Font Awesome", Qt::CaseInsensitive))
      {
        continue;
      }

      // Prefer the solid face: it carries the largest part of the icon set.
      if (family.contains ("Solid", Qt::CaseInsensitive))
      {
        return family;
      }

      if (first_match.isEmpty())
      {
        first_match = family;
      }
    }

    return first_match;
  }

  struct IconFallback
  {
    char const* label;
    QStyle::StandardPixmap standard_pixmap;
    bool has_standard_pixmap;
  };

  // Covers every FontAwesome::Icons value the application actually instantiates. Anything
  // else falls back to a generic marker; add an entry here when a new icon starts being used.
  std::unordered_map<char32_t, IconFallback> const& iconFallbacks()
  {
    static std::unordered_map<char32_t, IconFallback> const fallbacks
      { {0xf100, {"<<",   QStyle::SP_MediaSkipBackward,   true }}  // angledoubleleft
      , {0xf101, {">>",   QStyle::SP_MediaSkipForward,    true }}  // angledoubleright
      , {0xf02e, {"BM",   QStyle::SP_CustomBase,          false}}  // bookmark
      , {0xf0d7, {"v",    QStyle::SP_ArrowDown,           true }}  // caretdown
      , {0xf0da, {">",    QStyle::SP_ArrowRight,          true }}  // caretright
      , {0xf00c, {"OK",   QStyle::SP_DialogApplyButton,   true }}  // check
      , {0xf078, {"v",    QStyle::SP_ArrowDown,           true }}  // chevrondown
      , {0xf077, {"^",    QStyle::SP_ArrowUp,             true }}  // chevronup
      , {0xf328, {"CLP",  QStyle::SP_CustomBase,          false}}  // clipboard
      , {0xf0c2, {"CLD",  QStyle::SP_CustomBase,          false}}  // cloud
      , {0xf013, {"CFG",  QStyle::SP_CustomBase,          false}}  // cog
      , {0xf06e, {"EYE",  QStyle::SP_CustomBase,          false}}  // eye
      , {0xf070, {"HID",  QStyle::SP_CustomBase,          false}}  // eyeslash
      , {0xf15b, {"FIL",  QStyle::SP_FileIcon,            true }}  // file
      , {0xf1c6, {"ZIP",  QStyle::SP_CustomBase,          false}}  // filearchive
      , {0xf07c, {"DIR",  QStyle::SP_DirOpenIcon,         true }}  // folderopen
      , {0xf11b, {"PAD",  QStyle::SP_CustomBase,          false}}  // gamepad
      , {0xf129, {"i",    QStyle::SP_MessageBoxInformation, true}} // info
      , {0xf0eb, {"LGT",  QStyle::SP_CustomBase,          false}}  // lightbulb
      , {0xf279, {"MAP",  QStyle::SP_CustomBase,          false}}  // map
      , {0xf068, {"-",    QStyle::SP_CustomBase,          false}}  // minus
      , {0xf53f, {"PAL",  QStyle::SP_CustomBase,          false}}  // palette
      , {0xf04c, {"II",   QStyle::SP_MediaPause,          true }}  // pause
      , {0xf04b, {">",    QStyle::SP_MediaPlay,           true }}  // play
      , {0xf067, {"+",    QStyle::SP_CustomBase,          false}}  // plus
      , {0xf01e, {"RDO",  QStyle::SP_ArrowForward,        true }}  // redo
      , {0xf70c, {"RUN",  QStyle::SP_CustomBase,          false}}  // running
      , {0xf0c7, {"SAV",  QStyle::SP_DialogSaveButton,    true }}  // save
      , {0xf233, {"SRV",  QStyle::SP_DriveNetIcon,        true }}  // server
      , {0xf005, {"*",    QStyle::SP_CustomBase,          false}}  // star
      , {0xf04d, {"[]",   QStyle::SP_MediaStop,           true }}  // stop
      , {0xf185, {"SUN",  QStyle::SP_CustomBase,          false}}  // sun
      , {0xf00d, {"X",    QStyle::SP_DialogCloseButton,   true }}  // times
      , {0xf1f8, {"DEL",  QStyle::SP_TrashIcon,           true }}  // trash
      , {0xf2ed, {"DEL",  QStyle::SP_TrashIcon,           true }}  // trashalt
      , {0xf0e2, {"UND",  QStyle::SP_ArrowBack,           true }}  // undo
      , {0xf028, {"VOL",  QStyle::SP_MediaVolume,         true }}  // volumeup
      , {0xf410, {"X",    QStyle::SP_TitleBarCloseButton, true }}  // windowclose
      , {0xf2d0, {"[]",   QStyle::SP_TitleBarMaxButton,   true }}  // windowmaximize
      , {0xf2d1, {"_",    QStyle::SP_TitleBarMinButton,   true }}  // windowminimize
      , {0xf0ad, {"TLS",  QStyle::SP_CustomBase,          false}}  // wrench
      };

    return fallbacks;
  }
}

namespace Noggit
{
  namespace Ui
  {

    QString FontAwesomeFont::family()
    {
      static QString const resolved
        ( []
          {
            QString from_file (familyFromFile());

            return from_file.isEmpty() ? familyFromSystem() : from_file;
          }()
        );

      return resolved;
    }

    bool FontAwesomeFont::isAvailable()
    {
      return !family().isEmpty();
    }

    QIcon FontAwesomeFont::fallbackIcon (char32_t codepoint)
    {
      auto const entry (iconFallbacks().find (codepoint));

      if (entry == iconFallbacks().end() || !entry->second.has_standard_pixmap)
      {
        return QIcon();
      }

      QStyle* style (QApplication::style());

      if (!style)
      {
        return QIcon();
      }

      QIcon icon (style->standardIcon (entry->second.standard_pixmap));

      // Some styles return an icon that carries no pixmap at all; treat that as "no icon"
      // so the caller draws the text label rather than nothing.
      return icon.availableSizes().isEmpty() ? QIcon() : icon;
    }

    QString FontAwesomeFont::fallbackLabel (char32_t codepoint)
    {
      auto const entry (iconFallbacks().find (codepoint));

      if (entry != iconFallbacks().end())
      {
        return QString (entry->second.label);
      }

      // Unknown glyph: show the codepoint so the button is at least distinguishable from
      // its neighbours and greppable against FontAwesome::Icons.
      return QString ("%1").arg (static_cast<uint> (codepoint), 0, 16);
    }

    QString UiFonts::interfaceFamily()
    {
      static QString const resolved
        ( []
          {
            QStringList const preferred
              { "Segoe UI"        // Windows
              , "Noto Sans"
              , "DejaVu Sans"
              , "Liberation Sans"
              , "Arial"
              };

            QStringList const installed (QFontDatabase().families());

            for (QString const& family : preferred)
            {
              if (installed.contains (family, Qt::CaseInsensitive))
              {
                return family;
              }
            }

            return QFontDatabase::systemFont (QFontDatabase::GeneralFont).family();
          }()
        );

      return resolved;
    }

    QFont UiFonts::interfaceFont (int pixel_size)
    {
      QFont font (interfaceFamily());

      if (pixel_size > 0)
      {
        font.setPixelSize (pixel_size);
      }

      return font;
    }

    FontAwesomeIconEngine::FontAwesomeIconEngine (const QString& text)
      : QIconEngine()
      ,_text (text)
    {}

    FontAwesomeIconEngine* FontAwesomeIconEngine::clone() const
    {
      return new FontAwesomeIconEngine(_text);
    }

    void FontAwesomeIconEngine::paint ( QPainter* painter
                                         , QRect const& rect
                                         , QIcon::Mode mode
                                         , QIcon::State state
                                         )
    {
    painter->save();
      {
        auto temp_btn = new FontAwesomeButtonStyle();

        temp_btn->ensurePolished();

        if (state == QIcon::On)
        {
            painter->setPen(temp_btn->palette().color(QPalette::WindowText));
        }
        else if (state == QIcon::Off)
        {
            painter->setPen(temp_btn->palette().color(QPalette::Disabled, QPalette::WindowText));
        }

        delete temp_btn;

        // No Font Awesome file is shipped with this repository -- see the header comment.
        // When none is installed the icon degrades to a Qt standard icon or a text label
        // instead of throwing, which used to take the whole window down with it.
        if (!FontAwesomeFont::isAvailable())
        {
          paintFallback (painter, rect, mode, state);
          painter->restore();
          return;
        }

        if (!_fonts.count (rect.height()))
        {
          QFont font (FontAwesomeFont::family());
          font.setPixelSize (rect.height());

          _fonts[rect.height()] = font;
        }

        painter->setFont (_fonts.at (rect.height()));

        painter->drawText
          (rect, _text, QTextOption (Qt::AlignCenter | Qt::AlignVCenter));
      }
      painter->restore();
    }

    void FontAwesomeIconEngine::paintFallback ( QPainter* painter
                                              , QRect const& rect
                                              , QIcon::Mode mode
                                              , QIcon::State state
                                              )
    {
      char32_t const codepoint (_text.isEmpty() ? 0 : _text.at (0).unicode());

      QIcon const standard (FontAwesomeFont::fallbackIcon (codepoint));

      if (!standard.isNull())
      {
        standard.paint (painter, rect, Qt::AlignCenter, mode, state);
        return;
      }

      QString const label (FontAwesomeFont::fallbackLabel (codepoint));

      // Outline first: on a system whose font database cannot render the label at all the
      // box is still drawn, so the control never becomes an invisible hit area.
      QRect const box (rect.adjusted (1, 1, -1, -1));

      if (box.isValid())
      {
        painter->setBrush (Qt::NoBrush);
        painter->drawRoundedRect (box, 2.0, 2.0);
      }

      int pixel_size (std::max (7, rect.height() * 3 / 5));
      QFont font (UiFonts::interfaceFont (pixel_size));
      font.setBold (true);

      while ( pixel_size > 6
           && QFontMetrics (font).horizontalAdvance (label) > box.width()
            )
      {
        --pixel_size;
        font.setPixelSize (pixel_size);
      }

      painter->setFont (font);
      painter->drawText (rect, Qt::AlignCenter, label);
    }

    QPixmap FontAwesomeIconEngine::pixmap ( QSize const& size
                                             , QIcon::Mode mode
                                             , QIcon::State state
                                             )
    {
      QPixmap pm (size);
      pm.fill (Qt::transparent);
      {
        QPainter p (&pm);
        paint (&p, QRect(QPoint(0, 0), size), mode, state);
      }
      return pm;
    }


    std::map<int, QFont> FontAwesomeIconEngine::_fonts = {};

    FontAwesomeIcon::FontAwesomeIcon (FontAwesome::Icons const& icon)
      : QIcon (new FontAwesomeIconEngine (QString (QChar (icon))))
    {}


  }
}
