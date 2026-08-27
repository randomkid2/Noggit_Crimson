// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/UidFixWindow.hpp>

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>


namespace Noggit
{
  namespace Ui
  {
    UidFixWindow::UidFixWindow (glm::vec3 pos
                                   , math::degrees camera_pitch
                                   , math::degrees camera_yaw
                                   )
      : QDialog (nullptr)
    {
      setWindowIcon (QIcon (":/icon"));
      auto layout (new QFormLayout (this));


      auto label1 = new QLabel ("<big><p>In order to avoid issues with duplicating/missing "
        "models and possibly with model culling/collision "
        "unique ID of all objects on a map, or to fix that and "
        "also fix all existing models on the map.</p>"
        "<b>This is only required once.</b></big><br><br>"
        "<h2>For unedited blizzard maps</h2>"
        "<p>This is the fastest method but it will only prevent "
        "new issues from occuring and not fix the current ones.</p>"
        , this
      );
      label1->setWordWrap(true);
      layout->addWidget(label1);
      
      auto get_max (new QPushButton("Get max UID", this));
      layout->addWidget(get_max);

      auto label2 = new QLabel("<hr><h2>Recommended for edited/custom maps</h2>"
        "<p>Takes more time than the max uid method but "
        "it will fix any model duplication, collision and culling issue "
        "while making sure no models have the same id. "
        "It will fail if any model is missing or could not be loaded "
        "to avoid any collision and culling issue.</p>"
        , this
      );
      label2->setWordWrap(true);
      layout->addWidget(label2);

      auto fix_all (new QPushButton("Fix all UIDs", this));
      layout->addWidget(fix_all);

      auto label3 = new QLabel("<hr><font color=red><h2>/!\\ NOT RECOMMENDED</h2>"
        "<h3>USE AT YOUR OWN RISKS</h3></font>"
        "Same as Fix all UIDs, but it ignores model loading errors instead of stopping. "
        "Any model that is <b>missing or badly ported</b> will keep a broken UID, which leads to "
        "collision and culling problems in the client. Only use this if a normal fix will not "
        "complete and you understand that the result may need repairing by hand."
        , this
      );
      label3->setWordWrap(true);
      layout->addWidget(label3);

      auto fix_all_ignore_errors
        (new QPushButton("Fix all UIDs, ignoring model errors", this));
      layout->addWidget(fix_all_ignore_errors);      

      connect ( get_max, &QPushButton::clicked
              , [=]
                {
                  hide();
                  emit fix_uid(pos, camera_pitch, camera_yaw, uid_fix_mode::max_uid);
                  deleteLater();
                }
              );

      connect ( fix_all, &QPushButton::clicked
              , [=]
                {
                  hide();
                  emit fix_uid(pos, camera_pitch, camera_yaw, uid_fix_mode::fix_all_fail_on_model_loading_error);
                  deleteLater();
                }
              );

      connect ( fix_all_ignore_errors, &QPushButton::clicked
              , [=]
                {
                  hide();
                  emit fix_uid(pos, camera_pitch, camera_yaw, uid_fix_mode::fix_all_ignore_errors);
                  deleteLater();
                }
              );
    }
  }
}
