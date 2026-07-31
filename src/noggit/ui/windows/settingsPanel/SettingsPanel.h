// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QMainWindow>

namespace Ui
{
  class SettingsPanel;
}

class QLineEdit;
class QSettings;

namespace Noggit
{
  namespace Ui
  {
    class settings : public QMainWindow
    {
      Q_OBJECT
      QSettings* _settings;
      ::Ui::SettingsPanel* ui;

      // Built in the constructor rather than in SettingsPanel.ui: the designer form is shared
      // with upstream, and keeping this fork's field out of it avoids a merge conflict in a
      // generated-XML file every time upstream touches the panel.
      QLineEdit* _mysql_dev_schema_field = nullptr;
    public:
      settings(QWidget* parent = nullptr);
      void discard_changes();
      void save_changes();

    signals:
      void saved();
    };
  }
}
