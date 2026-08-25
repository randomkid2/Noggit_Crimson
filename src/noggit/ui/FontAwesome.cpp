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

#include <noggit/ui/FontAwesome.hpp>

#include <QtCore/QDir>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QColor>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetrics>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

// Third caller of these, after ApplicationEntry.cpp (which defines them) and SettingsPanel.cpp.
// The note at their definition says a header is the shape to reach for once a third appears;
// this file cannot create one, so the declaration is repeated once more and the debt is real.
// Resolving the theme directory any other way is what must not happen: it is anchored to the
// executable, not to the working directory, and a second copy of that rule would drift.
namespace Noggit::Application
{
  QString defaultThemeName();
  QString themesDirectory();
}

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

  // One row per icon the application can ask for, in either set.
  //
  // `name` is what a theme ships artwork under and is the enumerator's own spelling, so the two
  // can never drift: FontAwesome::pen is "pen.png", FontNoggit::TOOL_RAISE_LOWER is
  // "tool_raise_lower.png". `label` is the last resort and is deliberately terse -- it has to
  // fit inside a 22x22 button.
  struct IconFallback
  {
    char const* name;
    char const* label;
    QStyle::StandardPixmap standard_pixmap;
    bool has_standard_pixmap;
  };

  // Covers every FontAwesome::Icons value the application actually instantiates. Anything
  // else falls back to a generic marker; add an entry here when a new icon starts being used.
  std::unordered_map<char32_t, IconFallback> const& fontAwesomeIcons()
  {
    static std::unordered_map<char32_t, IconFallback> const fallbacks
      { {0xf100, {"angledoubleleft",  "<<",  QStyle::SP_MediaSkipBackward,   true }}
      , {0xf101, {"angledoubleright", ">>",  QStyle::SP_MediaSkipForward,    true }}
      , {0xf02e, {"bookmark",         "BM",  QStyle::SP_CustomBase,          false}}
      , {0xf0d7, {"caretdown",        "v",   QStyle::SP_ArrowDown,           true }}
      , {0xf0da, {"caretright",       ">",   QStyle::SP_ArrowRight,          true }}
      , {0xf00c, {"check",            "OK",  QStyle::SP_DialogApplyButton,   true }}
      , {0xf078, {"chevrondown",      "v",   QStyle::SP_ArrowDown,           true }}
      , {0xf077, {"chevronup",        "^",   QStyle::SP_ArrowUp,             true }}
      , {0xf328, {"clipboard",        "CLP", QStyle::SP_CustomBase,          false}}
      , {0xf0c2, {"cloud",            "CLD", QStyle::SP_CustomBase,          false}}
      , {0xf013, {"cog",              "CFG", QStyle::SP_CustomBase,          false}}
      // edit and pen (below) are the ON and OFF states of ExtendedSlider's tablet button, which
      // every Radius / Inner Radius / Speed row carries -- fifteen instances across six tools.
      // With no entry here fallbackLabel() fell through to the raw codepoint, so each of them
      // rendered the literal text "f044"/"f304" inside a 22x22 box. No standard pixmap resembles
      // a pen, and the two labels are deliberately different: they are one button's two states,
      // so giving them the same text would hide that state from a user without the font.
      , {0xf044, {"edit",             "EDT", QStyle::SP_CustomBase,          false}}
      , {0xf12d, {"eraser",           "ERS", QStyle::SP_CustomBase,          false}}
      , {0xf06e, {"eye",              "EYE", QStyle::SP_CustomBase,          false}}
      , {0xf070, {"eyeslash",         "HID", QStyle::SP_CustomBase,          false}}
      , {0xf15b, {"file",             "FIL", QStyle::SP_FileIcon,            true }}
      , {0xf1c6, {"filearchive",      "ZIP", QStyle::SP_CustomBase,          false}}
      , {0xf07c, {"folderopen",       "DIR", QStyle::SP_DirOpenIcon,         true }}
      , {0xf11b, {"gamepad",          "PAD", QStyle::SP_CustomBase,          false}}
      , {0xf129, {"info",             "i",   QStyle::SP_MessageBoxInformation, true}}
      , {0xf0eb, {"lightbulb",        "LGT", QStyle::SP_CustomBase,          false}}
      , {0xf279, {"map",              "MAP", QStyle::SP_CustomBase,          false}}
      , {0xf068, {"minus",            "-",   QStyle::SP_CustomBase,          false}}
      , {0xf6ff, {"networkwired",     "NET", QStyle::SP_DriveNetIcon,        true }}
      , {0xf53f, {"palette",          "PAL", QStyle::SP_CustomBase,          false}}
      , {0xf04c, {"pause",            "II",  QStyle::SP_MediaPause,          true }}
      , {0xf304, {"pen",              "PEN", QStyle::SP_CustomBase,          false}}
      , {0xf04b, {"play",             ">",   QStyle::SP_MediaPlay,           true }}
      , {0xf067, {"plus",             "+",   QStyle::SP_CustomBase,          false}}
      , {0xf01e, {"redo",             "RDO", QStyle::SP_ArrowForward,        true }}
      , {0xf70c, {"running",          "RUN", QStyle::SP_CustomBase,          false}}
      , {0xf0c7, {"save",             "SAV", QStyle::SP_DialogSaveButton,    true }}
      , {0xf233, {"server",           "SRV", QStyle::SP_DriveNetIcon,        true }}
      , {0xf005, {"star",             "*",   QStyle::SP_CustomBase,          false}}
      , {0xf04d, {"stop",             "[]",  QStyle::SP_MediaStop,           true }}
      , {0xf185, {"sun",              "SUN", QStyle::SP_CustomBase,          false}}
      , {0xf00d, {"times",            "X",   QStyle::SP_DialogCloseButton,   true }}
      , {0xf1f8, {"trash",            "DEL", QStyle::SP_TrashIcon,           true }}
      , {0xf2ed, {"trashalt",         "DEL", QStyle::SP_TrashIcon,           true }}
      , {0xf0e2, {"undo",             "UND", QStyle::SP_ArrowBack,           true }}
      , {0xf028, {"volumeup",         "VOL", QStyle::SP_MediaVolume,         true }}
      , {0xf410, {"windowclose",      "X",   QStyle::SP_TitleBarCloseButton, true }}
      , {0xf2d0, {"windowmaximize",   "[]",  QStyle::SP_TitleBarMaxButton,   true }}
      , {0xf2d1, {"windowminimize",   "_",   QStyle::SP_TitleBarMinButton,   true }}
      , {0xf0ad, {"wrench",           "TLS", QStyle::SP_CustomBase,          false}}
      };

    return fallbacks;
  }

  // The Noggit icon font's own set. Its glyph font IS shipped (resources/resources.qrc), so
  // unlike the table above this one is not normally reached for a label -- it is here so that
  // theme artwork can be found by name, and so that the one case where the font legitimately
  // cannot answer, a codepoint the shipped font has no glyph for, still lands on something
  // readable instead of an empty .notdef box.
  //
  // The ~60 lowercase keybinding hints (lmb, shift, a-z, f1-f12) are deliberately absent: they
  // are a working feature of the shipped font, are used in tooltips and the Help window rather
  // than on buttons, and are explicitly out of scope for the icon redesign.
  std::unordered_map<char32_t, IconFallback> const& noggitIcons()
  {
    static std::unordered_map<char32_t, IconFallback> const fallbacks
      { {0xF89C, {"tool_raise_lower",         "RSE", QStyle::SP_CustomBase, false}}
      , {0xF89D, {"tool_flatten_blur",        "FLT", QStyle::SP_CustomBase, false}}
      , {0xF89E, {"tool_texture_paint",       "TEX", QStyle::SP_CustomBase, false}}
      , {0xF89F, {"tool_hole_cutter",         "HOL", QStyle::SP_CustomBase, false}}
      , {0xF8A0, {"tool_area_designator",     "ARE", QStyle::SP_CustomBase, false}}
      , {0xF8A1, {"tool_impass_designator",   "IMP", QStyle::SP_CustomBase, false}}
      , {0xF8A2, {"tool_water_editor",        "WTR", QStyle::SP_CustomBase, false}}
      , {0xF8A3, {"tool_vertex_paint",        "VTX", QStyle::SP_CustomBase, false}}
      , {0xF8A4, {"tool_object_editor",       "OBJ", QStyle::SP_CustomBase, false}}
      , {0xF8A5, {"tool_minimap_editor",      "MMP", QStyle::SP_CustomBase, false}}
      , {0xF8A6, {"tool_stamp",               "STP", QStyle::SP_CustomBase, false}}
      , {0xf8a7, {"settings",                 "CFG", QStyle::SP_CustomBase, false}}
      , {0xf8a8, {"favorite",                 "FAV", QStyle::SP_CustomBase, false}}
      , {0xf8a9, {"visibility_wmo",           "WMO", QStyle::SP_CustomBase, false}}
      , {0xf8aa, {"visibility_wmo_doodads",   "WMD", QStyle::SP_CustomBase, false}}
      , {0xf8ab, {"visibility_doodads",       "DDD", QStyle::SP_CustomBase, false}}
      , {0xf8ac, {"visibility_with_box",      "BOX", QStyle::SP_CustomBase, false}}
      , {0xf8ad, {"visibility_unused",        "UNU", QStyle::SP_CustomBase, false}}
      , {0xf8ae, {"visibility_terrain",       "TER", QStyle::SP_CustomBase, false}}
      , {0xf8af, {"visibility_lines",         "LIN", QStyle::SP_CustomBase, false}}
      , {0xf8b0, {"visibility_wireframe",     "WIR", QStyle::SP_CustomBase, false}}
      , {0xf8b1, {"visibility_contours",      "CON", QStyle::SP_CustomBase, false}}
      , {0xf8b2, {"visibility_fog",           "FOG", QStyle::SP_CustomBase, false}}
      , {0xf8b3, {"visibility_water",         "WAT", QStyle::SP_CustomBase, false}}
      , {0xf8b4, {"visibility_groundeffects", "GFX", QStyle::SP_CustomBase, false}}
      , {0xf8b6, {"visibility_hidden_models", "HDN", QStyle::SP_CustomBase, false}}
      , {0xf8b7, {"visibility_hole_lines",    "HLN", QStyle::SP_CustomBase, false}}
      // VISIBILITY_ANIMATION and VISIBILITY_ANIMATION_2 are the SAME value; one row serves both.
      , {0xf8b8, {"visibility_animation",     "ANI", QStyle::SP_CustomBase, false}}
      , {0xf8ba, {"visibility_light",         "LGT", QStyle::SP_CustomBase, false}}
      , {0xf8bb, {"info",                     "i",   QStyle::SP_MessageBoxInformation, true}}
      , {0xf8bc, {"visibility_particles",     "PTC", QStyle::SP_CustomBase, false}}
      , {0xf8bd, {"time_normal",              "TIM", QStyle::SP_CustomBase, false}}
      , {0xf8be, {"time_pause",               "II",  QStyle::SP_MediaPause, true }}
      , {0xf8bf, {"time_speed",               ">>",  QStyle::SP_MediaSeekForward, true}}
      , {0xf8c0, {"camera_turn",              "CAM", QStyle::SP_CustomBase, false}}
      , {0xf8c1, {"camera_speed_slower",      "SLO", QStyle::SP_CustomBase, false}}
      , {0xf8c2, {"camera_speed_faster",      "FST", QStyle::SP_CustomBase, false}}
      , {0xf8c3, {"texture_palette",          "PAL", QStyle::SP_CustomBase, false}}
      , {0xf8c4, {"texture_palette_favorite", "FAV", QStyle::SP_CustomBase, false}}
      , {0xf8c5, {"mouse_invert",             "INV", QStyle::SP_CustomBase, false}}
      , {0xf8c6, {"ui_toggle",                "EXT", QStyle::SP_CustomBase, false}}
      , {0xf8c7, {"view_mode_2d",             "2D",  QStyle::SP_CustomBase, false}}
      , {0xf8c8, {"view_axis",                "AXS", QStyle::SP_CustomBase, false}}
      , {0xf8c9, {"gizmo_translate",          "MOV", QStyle::SP_CustomBase, false}}
      , {0xf8ca, {"gizmo_rotate",             "ROT", QStyle::SP_CustomBase, false}}
      , {0xf8cb, {"gizmo_scale",              "SCL", QStyle::SP_CustomBase, false}}
      , {0xf8cc, {"gizmo_visibility",         "GIZ", QStyle::SP_CustomBase, false}}
      , {0xf8cd, {"gizmo_global",             "GLB", QStyle::SP_CustomBase, false}}
      , {0xf8ce, {"gizmo_local",              "LOC", QStyle::SP_CustomBase, false}}
      , {0xf8cf, {"gizmo_visibility_all",     "ALL", QStyle::SP_CustomBase, false}}
      , {0xf8d0, {"tool_light",               "LIT", QStyle::SP_CustomBase, false}}
      , {0xf8d1, {"visibility_vertex_painter","VCL", QStyle::SP_CustomBase, false}}
      , {0xf8d2, {"visibility_climb",         "CLM", QStyle::SP_CustomBase, false}}
      , {0xf8d3, {"visibility_baked_shadows", "SHD", QStyle::SP_CustomBase, false}}
      , {0xf8d4, {"doodad",                   "DDD", QStyle::SP_CustomBase, false}}
      , {0xf8d5, {"character_radius",         "RAD", QStyle::SP_CustomBase, false}}
      , {0xf8d6, {"sun",                      "SUN", QStyle::SP_CustomBase, false}}
      , {0xf8d7, {"character",                "CHR", QStyle::SP_CustomBase, false}}
      , {0xf8d8, {"view_mode_game",           "PAD", QStyle::SP_CustomBase, false}}
      , {0xf8d9, {"sound",                    "SND", QStyle::SP_MediaVolume, true}}
      , {0xf8da, {"cube",                     "CUB", QStyle::SP_CustomBase, false}}
      , {0xf8db, {"pointer",                  "PTR", QStyle::SP_CustomBase, false}}
      , {0xf8dc, {"creature",                 "CRE", QStyle::SP_CustomBase, false}}
      , {0xf8dd, {"camera",                   "CAM", QStyle::SP_CustomBase, false}}
      , {0xf8de, {"window",                   "WIN", QStyle::SP_CustomBase, false}}
      , {0xf8df, {"visibility_flight_bounds", "FLY", QStyle::SP_CustomBase, false}}
      , {0xf8e0, {"area_trigger",             "TRG", QStyle::SP_CustomBase, false}}
      , {0xf8e1, {"area_trigger_sphere",      "SPH", QStyle::SP_CustomBase, false}}
      // The three below have no glyph in the shipped font -- see the comment on their
      // enumerators in FontNoggit.hpp. Until drawn artwork exists they resolve to these labels,
      // which is still three tools apart instead of three identical "i" buttons.
      , {0xf8e2, {"tool_scripting",           "SCR", QStyle::SP_CustomBase, false}}
      , {0xf8e3, {"tool_chunk",               "CHK", QStyle::SP_CustomBase, false}}
      , {0xf8e4, {"tool_erosion",             "ERO", QStyle::SP_CustomBase, false}}
      };

    return fallbacks;
  }

  std::unordered_map<char32_t, IconFallback> const& iconFallbacks (Noggit::Ui::IconSet set)
  {
    return set == Noggit::Ui::IconSet::Noggit ? noggitIcons() : fontAwesomeIcons();
  }

  IconFallback const* iconEntry (Noggit::Ui::IconSet set, char32_t codepoint)
  {
    auto const& table (iconFallbacks (set));
    auto const entry (table.find (codepoint));

    return entry == table.end() ? nullptr : &entry->second;
  }

  // ---- theme artwork ---------------------------------------------------------------------

  // Resolved once. A theme switch made from the settings panel restyles the application but
  // does not re-read this: the alternative is a QSettings construction on every icon paint --
  // ~35 of those per repaint of the tool strip and the view toolbar -- to answer a question
  // whose answer changes at most a handful of times in a session. Artwork therefore follows a
  // theme change at the next start. Noted rather than hidden.
  QString const& themeIconsDirectory()
  {
    static QString const resolved
      ( []
        {
          QSettings settings;

          QString const theme
            ( settings.value ("theme", Noggit::Application::defaultThemeName()).toString()
            );

          if (theme.isEmpty())
          {
            return QString();
          }

          QString const path
            ( QDir (Noggit::Application::themesDirectory())
                .absoluteFilePath (theme + "/icons")
            );

          return QDir (path).exists() ? path : QString();
        }()
      );

    return resolved;
  }

  // <icons dir>/manifest.json, the codepoint-to-file-stem map the icon set is generated with.
  //
  // Reading it rather than trusting the built-in table matters, because the two do not agree
  // and the manifest is right: it collapses aliases. FontAwesome::lightbulb 0xf0eb and
  // FontNoggit::TOOL_LIGHT are ONE drawing, so the manifest points 0xf0eb at "tool_light" and
  // there is no lightbulb.png to find; the same holds for gamepad/view_mode_game,
  // trashalt/trash and windowclose/times. Guessing the file name from the enumerator would
  // silently miss four icons and nobody would know which four.
  //
  // The built-in table is still consulted afterwards: the icon set carries names the manifest
  // has no codepoint for (the three tools that shared FontNoggit::INFO, the nine falloff
  // curves), and those resolve by name alone.
  struct IconManifest
  {
    std::map<char32_t, QString> awesome;
    std::map<char32_t, QString> noggit;
  };

  IconManifest const& iconManifest()
  {
    static IconManifest const manifest
      ( []
        {
          IconManifest parsed;

          QString const& directory (themeIconsDirectory());

          if (directory.isEmpty())
          {
            return parsed;
          }

          QFile file (directory + "/manifest.json");

          if (!file.open (QFile::ReadOnly))
          {
            return parsed;
          }

          QJsonObject const root
            (QJsonDocument::fromJson (file.readAll()).object());

          // "icons" carries a "font" per entry and is therefore the only source that can tell
          // the two overlapping codepoint spaces apart. "by_codepoint" is flat and is used only
          // for whatever "icons" did not describe, which is why it is read second and does not
          // overwrite.
          QJsonObject const icons (root.value ("icons").toObject());

          for (QString const& key : icons.keys())
          {
            QJsonObject const icon (icons.value (key).toObject());
            QString const codepoint_text (icon.value ("codepoint").toString());

            if (codepoint_text.isEmpty())
            {
              continue;
            }

            bool converted (false);
            char32_t const codepoint
              (codepoint_text.mid (2).toUInt (&converted, 16));

            if (!converted || !codepoint)
            {
              continue;
            }

            // An alias entry names the drawing it shares; the drawing is what has a file.
            QString const alias (icon.value ("alias_of").toString());
            QString const name (alias.isEmpty() ? key : alias);

            if (icon.value ("font").toString() == QLatin1String ("noggit"))
            {
              parsed.noggit.emplace (codepoint, name);
            }
            else
            {
              parsed.awesome.emplace (codepoint, name);
            }
          }

          QJsonObject const by_codepoint (root.value ("by_codepoint").toObject());

          for (QString const& key : by_codepoint.keys())
          {
            bool converted (false);
            char32_t const codepoint (key.mid (2).toUInt (&converted, 16));

            if (!converted || !codepoint)
            {
              continue;
            }

            QString const name (by_codepoint.value (key).toString());

            // Which set an entry belongs to is not recorded here. The two enums only overlap
            // above 0xf840, and every Noggit icon is at 0xf89c or beyond, so that boundary
            // separates them for everything either enum actually instantiates. Entries that
            // "icons" already placed are untouched -- emplace does not overwrite.
            (codepoint >= 0xf89c ? parsed.noggit : parsed.awesome).emplace (codepoint, name);
          }

          return parsed;
        }()
      );

    return manifest;
  }

  // Manifest first, built-in table second. Either may answer; only both failing is a miss.
  QString themeIconName (Noggit::Ui::IconSet set, char32_t codepoint)
  {
    IconManifest const& manifest (iconManifest());

    std::map<char32_t, QString> const& table
      (set == Noggit::Ui::IconSet::Noggit ? manifest.noggit : manifest.awesome);

    auto const named (table.find (codepoint));

    if (named != table.end())
    {
      return named->second;
    }

    IconFallback const* const entry (iconEntry (set, codepoint));

    return entry ? QString::fromLatin1 (entry->name) : QString();
  }

  struct ThemeIconSource
  {
    QImage image;

    // True when every visible pixel is a shade of grey, i.e. the file is an alpha mask that
    // carries no colour of its own. Only those are tinted with the pen colour; a theme that
    // ships genuinely coloured artwork gets it drawn as authored rather than flattened.
    bool monochrome;
  };

  bool isMonochrome (QImage const& image)
  {
    for (int y (0); y < image.height(); ++y)
    {
      QRgb const* line (reinterpret_cast<QRgb const*> (image.constScanLine (y)));

      for (int x (0); x < image.width(); ++x)
      {
        QRgb const pixel (line[x]);

        if (qAlpha (pixel) == 0)
        {
          continue;
        }

        if (qRed (pixel) != qGreen (pixel) || qGreen (pixel) != qBlue (pixel))
        {
          return false;
        }
      }
    }

    return true;
  }

  // One decode per name and scale for the life of the process. A miss is cached as a null
  // image, so a codepoint the theme has no artwork for costs one map lookup per paint and never
  // a stat() -- which matters, because a miss is the common case for every codepoint the icon
  // set has not reached yet.
  ThemeIconSource const& themeIconSource ( Noggit::Ui::IconSet set
                                         , char32_t codepoint
                                         , bool checked
                                         , bool prefer_double
                                         )
  {
    static std::map<std::tuple<int, char32_t, bool, bool>, ThemeIconSource> sources;

    auto const key
      (std::make_tuple (static_cast<int> (set), codepoint, checked, prefer_double));
    auto const cached (sources.find (key));

    if (cached != sources.end())
    {
      return cached->second;
    }

    ThemeIconSource source {QImage(), false};

    QString const name (themeIconName (set, codepoint));
    QString const& directory (themeIconsDirectory());

    if (!name.isEmpty() && !directory.isEmpty())
    {
      // The set ships a 24 px and a 48 px raster of every icon. Taking the larger one only
      // where it is actually needed is the point: 48 px downscaled to a 24 px button smears
      // the 2-unit stroke the whole icon language is built on, and 24 px upscaled to a 48 px
      // device rect does the same in the other direction.
      //
      // The candidates are tried in order and the FIRST that exists wins, so the checked
      // variant is optional per icon: only the star ships one today.
      QStringList candidates;

      if (checked)
      {
        if (prefer_double)
        {
          candidates << directory + "/" + name + "_filled@2x.png";
        }

        candidates << directory + "/" + name + "_filled.png";
      }

      if (prefer_double)
      {
        candidates << directory + "/" + name + "@2x.png";
      }

      candidates << directory + "/" + name + ".png";

      // PNG only. Qt's SVG image format plugin is not deployed with this application -- Qt5Svg
      // is never linked by CMakeLists.txt -- so the .svg beside each of these would load as a
      // null image with no warning at all. The .svg files are the editable source of the
      // artwork, not something to read at runtime.
      for (QString const& path : candidates)
      {
        if (!QFileInfo::exists (path))
        {
          continue;
        }

        QImage loaded (path);

        if (loaded.isNull())
        {
          continue;
        }

        source.image = loaded.convertToFormat (QImage::Format_ARGB32_Premultiplied);
        source.monochrome = isMonochrome (source.image);
        break;
      }
    }

    return sources.emplace (key, std::move (source)).first->second;
  }
}

namespace Noggit
{
  namespace Ui
  {

    QString ThemeIcons::name (IconSet set, char32_t codepoint)
    {
      return themeIconName (set, codepoint);
    }

    QPixmap ThemeIcons::pixmap ( IconSet set
                               , char32_t codepoint
                               , QIcon::State state
                               , QSize const& size
                               , qreal device_pixel_ratio
                               , QColor const& pen_color
                               )
    {
      if (size.isEmpty())
      {
        return QPixmap();
      }

      // The colour is part of the key because the same artwork is tinted differently for
      // checked, unchecked and hovered; the ratio is, because AA_EnableHighDpiScaling means
      // `size` is logical and the bitmap has to be built at device resolution or it is blurred.
      // Quantising the ratio keeps a fractional scale factor from making the key unbounded.
      int const ratio_key (std::max (1, qRound (device_pixel_ratio * 100.0)));

      using Key = std::tuple<int, char32_t, bool, int, int, int, QRgb>;

      static std::map<Key, QPixmap> cache;

      bool const checked (state == QIcon::On);

      Key const key
        ( static_cast<int> (set)
        , codepoint
        , checked
        , size.width()
        , size.height()
        , ratio_key
        , pen_color.rgba()
        );

      auto const cached (cache.find (key));

      if (cached != cache.end())
      {
        return cached->second;
      }

      qreal const ratio (ratio_key / 100.0);

      // The 48 px raster earns its place only once the rect it has to fill is bigger than the
      // 24 px one -- which is the 24 px toolbar button on a 2x display, and not the 16 px
      // QPushButton icon on a 1x one.
      bool const prefer_double
        (std::max (size.width(), size.height()) * ratio > 24.0);

      ThemeIconSource const& source
        (themeIconSource (set, codepoint, checked, prefer_double));

      QPixmap result;

      if (!source.image.isNull())
      {
        QImage scaled
          ( source.image.scaled ( QSize ( qRound (size.width() * ratio)
                                        , qRound (size.height() * ratio)
                                        )
                                , Qt::KeepAspectRatio
                                , Qt::SmoothTransformation
                                )
          );

        if (source.monochrome)
        {
          // SourceIn keeps the mask's alpha and replaces every colour channel, which is what
          // makes one grey file serve all of the icon states.
          QPainter tint (&scaled);
          tint.setCompositionMode (QPainter::CompositionMode_SourceIn);
          tint.fillRect (scaled.rect(), pen_color);
        }

        result = QPixmap::fromImage (scaled);
        result.setDevicePixelRatio (ratio);
      }

      cache.emplace (key, result);

      return result;
    }

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

    bool FontAwesomeFont::hasGlyph (QString const& font_family, char32_t codepoint)
    {
      if (font_family.isEmpty())
      {
        return false;
      }

      // QFontMetrics is not free and this is asked once per icon per repaint, so the answer is
      // memoised. Both families are resolved once for the life of the process, so keying on the
      // family string cannot grow without bound.
      static std::map<std::pair<QString, char32_t>, bool> known;

      auto const key (std::make_pair (font_family, codepoint));
      auto const cached (known.find (key));

      if (cached != known.end())
      {
        return cached->second;
      }

      bool const present (QFontMetrics (QFont (font_family)).inFont (QChar (codepoint)));

      known.emplace (key, present);

      return present;
    }

    bool FontAwesomeFont::hasFallback (IconSet set, char32_t codepoint)
    {
      return iconEntry (set, codepoint) != nullptr;
    }

    QIcon FontAwesomeFont::fallbackIcon (IconSet set, char32_t codepoint)
    {
      IconFallback const* const entry (iconEntry (set, codepoint));

      if (!entry || !entry->has_standard_pixmap)
      {
        return QIcon();
      }

      QStyle* style (QApplication::style());

      if (!style)
      {
        return QIcon();
      }

      QIcon icon (style->standardIcon (entry->standard_pixmap));

      // Some styles return an icon that carries no pixmap at all; treat that as "no icon"
      // so the caller draws the text label rather than nothing.
      return icon.availableSizes().isEmpty() ? QIcon() : icon;
    }

    QString FontAwesomeFont::fallbackLabel (IconSet set, char32_t codepoint)
    {
      IconFallback const* const entry (iconEntry (set, codepoint));

      if (entry)
      {
        return QString::fromLatin1 (entry->label);
      }

      // Deliberately NOT the codepoint. Printing it was greppable for a developer and pure
      // noise for everyone else -- "f304" is what the tablet button of every ExtendedSlider row
      // showed, fifteen times over, and it read as a rendering fault rather than as a button.
      // A codepoint with no row in either table is a table omission, not something a user can
      // act on; the tables above now cover every value the application instantiates, so this is
      // reachable only by a new icon that was added without one.
      return QString ("?");
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

    FontAwesomeIconEngine::FontAwesomeIconEngine (const QString& text, IconSet set)
      : QIconEngine()
      , _set (set)
      , _text (text)
    {}

    FontAwesomeIconEngine* FontAwesomeIconEngine::clone() const
    {
      return new FontAwesomeIconEngine (_text, _set);
    }

    char32_t FontAwesomeIconEngine::codepoint() const
    {
      return _text.isEmpty() ? 0 : _text.at (0).unicode();
    }

    // The artwork tier. Keyed by codepoint and set, cached, and allowed to be absent -- which is
    // what makes the icon set safe to land one icon at a time: a name the artwork does not cover
    // falls straight through to the glyph that was there before, and no button is ever blank at
    // any point in that process.
    bool FontAwesomeIconEngine::paintArtwork ( QPainter* painter
                                             , QRect const& rect
                                             , QIcon::State state
                                             , QColor const& pen_color
                                             )
    {
      char32_t const glyph (codepoint());

      if (!glyph || rect.isEmpty())
      {
        return false;
      }

      QPixmap const artwork
        ( ThemeIcons::pixmap
            ( _set
            , glyph
            , state
            , rect.size()
            , painter->device() ? painter->device()->devicePixelRatioF() : 1.0
            , pen_color
            )
        );

      if (!artwork.isNull())
      {
        // KeepAspectRatio means the bitmap can be narrower than the rect; centre it rather
        // than stretching, so a square icon in an oblong button is not distorted.
        QSizeF const logical (artwork.size() / artwork.devicePixelRatio());
        QRectF target (QPointF (0.0, 0.0), logical);
        target.moveCenter (QRectF (rect).center());

        painter->drawPixmap (target.topLeft(), artwork);

        return true;
      }

      return false;
    }

    void FontAwesomeIconEngine::paint ( QPainter* painter
                                         , QRect const& rect
                                         , QIcon::Mode mode
                                         , QIcon::State state
                                         )
    {
    painter->save();
      {
        QColor const pen_color
          ( iconPenColorForMode (iconProbePalette<FontAwesomeButtonStyle>(), mode, state)
          );

        painter->setPen (pen_color);

        // Genuinely disabled, as opposed to merely unchecked -- the two share a colour and
        // cannot be told apart any other way. Applied to the whole block so it reaches the
        // artwork and the text fallback alike.
        if (mode == QIcon::Disabled)
        {
          painter->setOpacity (painter->opacity() * ICON_DISABLED_OPACITY);
        }

        if (paintArtwork (painter, rect, state, pen_color))
        {
          painter->restore();
          return;
        }

        // No Font Awesome file is shipped with this repository -- see the header comment.
        // When none is installed, or when the one that is installed has no glyph at this
        // codepoint (Font Awesome Free is missing the Pro-only part of the set), the icon
        // degrades to a Qt standard icon or a text label instead of throwing, which used to
        // take the whole window down with it.
        //
        // hasFallback() gates the glyph test rather than the other way round: leaving the font
        // path costs nothing only when there is something better to arrive at.
        if ( !FontAwesomeFont::isAvailable()
          || ( !FontAwesomeFont::hasGlyph (FontAwesomeFont::family(), codepoint())
            && FontAwesomeFont::hasFallback (_set, codepoint())
             )
           )
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
      QIcon const standard (FontAwesomeFont::fallbackIcon (_set, codepoint()));

      if (!standard.isNull())
      {
        standard.paint (painter, rect, Qt::AlignCenter, mode, state);
        return;
      }

      QString const label (FontAwesomeFont::fallbackLabel (_set, codepoint()));

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

    // Every caller that asks a QIcon for a QPixmap rather than letting it paint itself arrives
    // here: the IconActions on the two secondary view bars, the map and bookmark rows on the
    // main menu, and the project-creation dialog.
    //
    // Build a bitmap of exactly `size` and leave its device pixel ratio alone. It is tempting
    // to render at qApp->devicePixelRatio() and tag the result, so that scaled displays get a
    // sharp icon instead of an upscaled one -- that was tried here and it is WRONG. Measured
    // against Qt 5.15.2 with this application's exact attributes (AA_EnableHighDpiScaling set,
    // qApp->devicePixelRatio() == 2), driving a QIconEngine through QIcon::pixmap(QSize):
    //
    //   * the engine is asked for the LOGICAL size. Requesting 22x22 from the QIcon calls this
    //     function with 22x22 -- Qt does NOT pre-multiply by the ratio.
    //   * whatever ratio the returned pixmap carries is DISCARDED. Returning a 22x22 bitmap
    //     tagged devicePixelRatio 2 yields a pixmap reported back as 22x22 at ratio 1.
    //
    // So the returned bitmap is taken at face value, and rendering at 2x produced a 44x44
    // ratio-1 pixmap -- every icon came out at exactly twice its intended size. QIcon::pixmap
    // (QSize) has no path that can carry a high-DPI pixmap out of an engine; the ratio-aware
    // overload is QIcon::pixmap(QWindow*, QSize), which only a call site has the window for.
    // Sharpening these icons therefore belongs at the call sites, not in the engine.
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
