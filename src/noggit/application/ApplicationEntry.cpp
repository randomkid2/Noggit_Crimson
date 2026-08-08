// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/errorHandling.h>
#include <noggit/ui/FontAwesome.hpp>
#include <noggit/ui/windows/projectSelection/NoggitProjectSelectionWindow.hpp>
#include <noggit/Log.h>

#include <qcommandlineoption.h>
#include <qcommandlineparser.h>
#include <QStyleFactory>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>

namespace Noggit::Application
{
  // Startup and the settings panel are the only two callers, and they must not drift apart:
  // before this existed the panel's theme combo owned the one copy of the loading code, so the
  // application was styled only as a side effect of a signal that does not always fire.
  // Declared here and repeated in SettingsPanel.cpp rather than given a header of its own,
  // which is the shape to reach for if a third caller ever appears.
  QString defaultThemeName();
  QString themesDirectory();
  void applyTheme (QString const& theme_name);
}

namespace
{
  // The project's design tokens. themes/CrimsonSlate/theme.qss is built on the same set and the
  // two have to be changed together, or a dialog the stylesheet does not reach will sit at a
  // different value from the panel behind it. The stylesheet dresses the widgets Qt Style
  // Sheets can select; this palette is what everything else is drawn from
  // -- native dialogs, QMessageBox, menu drop shadows, combo box popup frames, drag-and-drop
  // indicators, the text cursor, item view branch guides. Fusion's stock palette is light, so
  // without this half the application stays light no matter which theme is loaded.
  //
  // ok/warn/bad (#4FB477 / #E0A33E / #E0574B) are deliberately absent: QPalette has no role
  // that means "this went well", so they exist only in the stylesheet.
  QColor const TOKEN_BG_SUNKEN   (0x16, 0x18, 0x1D);
  QColor const TOKEN_BG_BASE     (0x1B, 0x1E, 0x24);
  QColor const TOKEN_BG_RAISED   (0x22, 0x26, 0x2E);
  QColor const TOKEN_BG_OVERLAY  (0x2A, 0x2F, 0x38);
  QColor const TOKEN_STROKE      (0x34, 0x3A, 0x45);
  QColor const TOKEN_STROKE_SOFT (0x26, 0x2B, 0x33);
  QColor const TOKEN_TEXT_HI     (0xED, 0xF0, 0xF4);
  QColor const TOKEN_TEXT        (0xC6, 0xCC, 0xD6);
  QColor const TOKEN_TEXT_DIM    (0x8A, 0x93, 0xA0);
  QColor const TOKEN_TEXT_OFF    (0x59, 0x60, 0x6B);
  QColor const TOKEN_ACCENT      (0xE8, 0x54, 0x3F);
  QColor const TOKEN_ACCENT_DIM  (0xB3, 0x3F, 0x2E);

  char const* const DEFAULT_THEME_NAME = "CrimsonSlate";
  char const* const SYSTEM_THEME_NAME = "System";

  // 12px is what Qt's 9pt Windows default resolves to at 96 dpi, so this unifies the family
  // without resizing anything. High-dpi scaling still applies on top: under
  // AA_EnableHighDpiScaling a pixel size is a logical pixel, not a device pixel.
  int const INTERFACE_FONT_PIXEL_SIZE = 12;

  QPalette darkPalette()
  {
    QPalette palette;

    // The two-argument setColor() writes Active, Inactive and Disabled at once; the Disabled
    // overrides below then narrow the group that needs to differ.
    palette.setColor (QPalette::Window, TOKEN_BG_BASE);
    palette.setColor (QPalette::WindowText, TOKEN_TEXT);
    palette.setColor (QPalette::Base, TOKEN_BG_SUNKEN);
    palette.setColor (QPalette::AlternateBase, TOKEN_BG_RAISED);
    palette.setColor (QPalette::ToolTipBase, TOKEN_BG_OVERLAY);
    palette.setColor (QPalette::ToolTipText, TOKEN_TEXT_HI);
    palette.setColor (QPalette::Text, TOKEN_TEXT);
    palette.setColor (QPalette::Button, TOKEN_BG_RAISED);
    palette.setColor (QPalette::ButtonText, TOKEN_TEXT);
    palette.setColor (QPalette::BrightText, TOKEN_TEXT_HI);
    palette.setColor (QPalette::Link, TOKEN_ACCENT);
    palette.setColor (QPalette::LinkVisited, TOKEN_ACCENT_DIM);
    palette.setColor (QPalette::Highlight, TOKEN_ACCENT);

    // Dark on the accent, not white: measured against #E8543F, #16181D gives 4.85:1 where
    // #EDF0F4 gives only 3.19:1. Selected rows have to stay readable, so the darker one wins.
    palette.setColor (QPalette::HighlightedText, TOKEN_BG_SUNKEN);
    palette.setColor (QPalette::PlaceholderText, TOKEN_TEXT_DIM);

    // The 3-D shade ramp. Qt expects Light > Midlight > Button > Mid > Dark > Shadow, and
    // leaves every role the caller does not set at its light-palette default -- which is how a
    // "dark" palette ends up with pale bevels around frames, headers and sunken panels.
    palette.setColor (QPalette::Light, TOKEN_STROKE);
    palette.setColor (QPalette::Midlight, TOKEN_BG_OVERLAY);
    palette.setColor (QPalette::Mid, TOKEN_STROKE_SOFT);
    palette.setColor (QPalette::Dark, TOKEN_BG_BASE);
    palette.setColor (QPalette::Shadow, TOKEN_BG_SUNKEN);

    // Disabled is the group that decides whether a greyed-out control is readable or invisible.
    palette.setColor (QPalette::Disabled, QPalette::WindowText, TOKEN_TEXT_OFF);
    palette.setColor (QPalette::Disabled, QPalette::Text, TOKEN_TEXT_OFF);
    palette.setColor (QPalette::Disabled, QPalette::ButtonText, TOKEN_TEXT_OFF);
    palette.setColor (QPalette::Disabled, QPalette::Highlight, TOKEN_BG_OVERLAY);
    palette.setColor (QPalette::Disabled, QPalette::HighlightedText, TOKEN_TEXT_DIM);

    return palette;
  }

  // Returns false, having said why and where, whenever the stylesheet does not end up applied.
  bool loadStyleSheet (QString const& theme_name)
  {
    QString const path
      ( QDir (Noggit::Application::themesDirectory())
          .absoluteFilePath (theme_name + "/theme.qss")
      );

    QFile file (path);

    if (!file.exists())
    {
      LogError << "Theme \"" << theme_name.toStdString() << "\": no stylesheet at "
               << path.toStdString() << std::endl;
      return false;
    }

    if (!file.open (QFile::ReadOnly | QFile::Text))
    {
      LogError << "Theme \"" << theme_name.toStdString() << "\": cannot open "
               << path.toStdString() << " -- " << file.errorString().toStdString() << std::endl;
      return false;
    }

    QString style_sheet (QString::fromUtf8 (file.readAll()));

    QString application_dir (QCoreApplication::applicationDirPath());

    // The trailing separator is stripped from the *path*, which is what the original code in
    // the settings panel meant to do: it tested the whole stylesheet for a trailing slash and
    // chopped its tail instead, two characters for a one-character suffix.
    while (application_dir.endsWith ('/') || application_dir.endsWith ('\\'))
    {
      application_dir.chop (1);
    }

    style_sheet.replace ("@rpath", application_dir);

    // Qt resolves a relative url() against the process working directory rather than against
    // the stylesheet it came from, so a theme's own images vanish the moment Noggit starts
    // anywhere but in its own folder -- the same defect as the theme scan, one level down.
    // Anchor them where the sheet itself is anchored. applicationDirPath() uses forward slashes
    // on every platform, so nothing here can be read back as a regex escape; the result is
    // quoted because an installation path may contain spaces, which unquoted QSS url() does
    // not survive.
    static QRegularExpression const relative_theme_url
      (QStringLiteral (R"(url\(\s*(['"]?)themes/([^'")]+)\1\s*\))"));

    style_sheet.replace
      (relative_theme_url, QStringLiteral ("url(\"") + application_dir + "/themes/\\2\")");

    qApp->setStyleSheet (style_sheet);

    return true;
  }
}

namespace Noggit::Application
{
  QString defaultThemeName()
  {
    return QString::fromLatin1 (DEFAULT_THEME_NAME);
  }

  QString themesDirectory()
  {
    // Against the executable, never against the working directory. Launching from a shortcut,
    // a debugger or another folder used to leave the theme scan pointing somewhere with no
    // themes/ in it, and the failure was silent all the way through.
    return QDir (QCoreApplication::applicationDirPath()).absoluteFilePath ("themes");
  }

  void applyTheme (QString const& theme_name)
  {
    if (theme_name.isEmpty() || theme_name == QLatin1String (SYSTEM_THEME_NAME))
    {
      qApp->setStyleSheet (QString());
      return;
    }

    if (loadStyleSheet (theme_name))
    {
      return;
    }

    if ( theme_name != QLatin1String (DEFAULT_THEME_NAME)
      && loadStyleSheet (QString::fromLatin1 (DEFAULT_THEME_NAME))
       )
    {
      LogError << "Fell back to the \"" << DEFAULT_THEME_NAME << "\" theme." << std::endl;
      return;
    }

    qApp->setStyleSheet (QString());

    LogError << "No theme stylesheet could be loaded from "
             << themesDirectory().toStdString()
             << ". The palette is still dark, but every stylesheet rule is missing."
             << std::endl;
  }
}

QCommandLineParser* ProcessCommandLine()
{
    QCommandLineParser* parser = new QCommandLineParser();
    parser->setApplicationDescription("Help");
    parser->addHelpOption();
    parser->addVersionOption();
    parser->addOptions({
        {"disable-update", QApplication::translate("main", "Disable the check for update.")},
        {"force-changelog", QApplication::translate("main", "Force displaying the changelog popup.")}
        });

    return parser;
}

int main(int argc, char *argv[])
{
  Noggit::RegisterErrorHandlers();
  std::set_terminate(Noggit::Application::NoggitApplication::terminationHandler);

  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication q_application (argc, argv);
  q_application.setApplicationName ("Noggit");
  q_application.setOrganizationName ("Noggit");

  // Before any widget exists, so nothing is ever constructed against Fusion's light default and
  // then repainted. The organisation and application names above are what QSettings keys off,
  // so they have to come first.
  q_application.setPalette (darkPalette());
  q_application.setFont
    (Noggit::Ui::UiFonts::interfaceFont (INTERFACE_FONT_PIXEL_SIZE));

  auto parser = ProcessCommandLine();
  parser->process(q_application);

  std::vector<bool> Command;
  Command.push_back(parser->isSet("disable-update"));
  Command.push_back(parser->isSet("force-changelog"));

  auto noggit = Noggit::Application::NoggitApplication::instance();
  bool initialized = noggit->initalize(argc, argv, Command);

  if (!initialized) [[unlikely]]
  {
    return EXIT_FAILURE;
  }

  // After initalize(), because that is where InitLogging() redirects the streams into log.txt --
  // a theme that cannot be found is exactly the kind of thing someone reads the log for. Every
  // window below this line is created already styled.
  {
    QSettings settings;

    Noggit::Application::applyTheme
      ( settings.value ("theme", Noggit::Application::defaultThemeName()).toString()
      );
  }

  auto project_selection = new Noggit::Ui::Windows::NoggitProjectSelectionWindow(noggit);
  // project_selection->show();

  return q_application.exec();
}
