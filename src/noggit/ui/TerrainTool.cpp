// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/ui/TerrainTool.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/MapView.h>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/tools/UiCommon/expanderwidget.h>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>
#include <noggit/ui/tools/UiCommon/ImageMaskSelector.hpp>
#include <noggit/World.h>

#include <QtWidgets/QButtonGroup>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/QDial>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>

#define _USE_MATH_DEFINES
#include <math.h>

#include <vector>

namespace Noggit
{
  namespace Ui
  {
    TerrainTool::TerrainTool(MapView* map_view, QWidget* parent, bool stamp)
      : QWidget(parent)
      , _edit_type (eTerrainType_Linear)
      , _vertex_angle (0.0f)
      , _vertex_orientation (0.0f)
      , _cursor_pos(nullptr)
      , _vertex_mode(eVertexMode_Center)
      , _map_view(map_view)
    {
      setMinimumWidth(250);
      // setMaximumWidth(250);
      auto layout (new QVBoxLayout (this));
      layout->setAlignment(Qt::AlignTop);
      // One gutter for the whole tool, stated rather than inherited from whatever the style's
      // default happens to be, and the same figures the texturing tool now uses. The top inset
      // is smaller than the sides because the first thing in the panel is a QGroupBox, and the
      // theme already reserves 20px above every group box frame for its title.
      layout->setContentsMargins(9, 4, 9, 9);
      layout->setSpacing(6);

      // The brush type picker. It used to be nine QRadioButtons in a 2-column grid -- five rows
      // of indicator-plus-word, ~134px of the panel, and the widest single block in the dock.
      //
      // It is now a 3x3 segmented control of CHECKABLE QPushButtons in the same exclusive
      // QButtonGroup. Nothing about the group changed: the same nine ids (the eTerrainType
      // values), the same idClicked signal, the same exclusivity, and nextType() still reaches
      // them through _type_button_group->button(id)->toggle(), which is QAbstractButton API and
      // indifferent to the concrete class. An exclusive QButtonGroup refuses to uncheck its
      // checked member on a second click for push buttons exactly as it does for radios, so the
      // click behaviour is identical too.
      //
      // Why push buttons and not restyled radios: the theme's own token notes say accent AS A
      // FILL means "one checked thing out of a set", and QPushButton:checked is the rule that
      // already implements that mark. Reusing it means this control needs no colour of its own
      // and follows the palette wherever it goes; hiding a radio indicator would instead have
      // meant hand-rolling a checked colour here and letting it drift from the sheet. It also
      // gets centred text for free -- QSS text-align applies to QPushButton and not to
      // QRadioButton, so restyled radios would have had their labels jammed against the left
      // edge of each chip.
      //
      // Why words and not falloff icons: two of the nine are not falloff curves at all. Vertex
      // is a selection mode and Script hands the brush to the scripting tool, and neither has a
      // curve to draw. A row of nine glyphs would have had to invent two of them.
      //
      // The only user-visible difference is keyboard traversal: a radio group is one tab stop
      // with arrow keys inside it, nine push buttons are nine tab stops. Every button is still
      // reachable and still activates with Space, and the tool's own shortcut (nextType) is
      // untouched.
      _type_button_group = new QButtonGroup (this);

      auto make_type_button
        ( [this] (char const* text) -> QPushButton*
          {
            auto* button (new QPushButton (text, this));
            button->setCheckable (true);
            button->setAutoDefault (false);
            button->setSizePolicy (QSizePolicy::Preferred, QSizePolicy::Fixed);
            return button;
          }
        );

      QPushButton* radio_flat = make_type_button ("Flat");
      QPushButton* radio_linear = make_type_button ("Linear");
      QPushButton* radio_smooth = make_type_button ("Smooth");
      QPushButton* radio_polynomial = make_type_button ("Polynomial");
      QPushButton* radio_trigo = make_type_button ("Trigonom");
      QPushButton* radio_quadra = make_type_button ("Quadratic");
      QPushButton* radio_gauss = make_type_button ("Gaussian");

      QPushButton* radio_vertex = nullptr;
      if (!stamp)
        radio_vertex = make_type_button ("Vertex");

      QPushButton* radio_script = make_type_button ("Script");

      _type_button_group->addButton (radio_flat, (int)eTerrainType_Flat);
      _type_button_group->addButton (radio_linear, (int)eTerrainType_Linear);
      _type_button_group->addButton (radio_smooth, (int)eTerrainType_Smooth);
      _type_button_group->addButton (radio_polynomial, (int)eTerrainType_Polynom);
      _type_button_group->addButton (radio_trigo, (int)eTerrainType_Trigo);
      _type_button_group->addButton (radio_quadra, (int)eTerrainType_Quadra);
      _type_button_group->addButton (radio_gauss, (int)eTerrainType_Gaussian);

      if (!stamp)
        _type_button_group->addButton (radio_vertex, (int)eTerrainType_Vertex);

      _type_button_group->addButton (radio_script, (int)eTerrainType_Script);

      radio_linear->toggle();

      QGroupBox* terrain_type_group (new QGroupBox ("Type", this));
      terrain_type_group->setObjectName ("terrainTypeSegments");
      // The sheet's QPushButton padding is 4px 10px, sized for a standalone button with room
      // around it. Nine of them three-abreast in a 250px dock is a different problem: at 10px
      // side padding "Polynomial" alone asks for ~82px and the row overflows to ~254px, which
      // is wider than the panel and would put a horizontal scroll bar under every terrain tool.
      // 5px brings the widest chip to ~70px and the row to ~218px, inside the ~232px the dock
      // has after its own gutters. Padding and font size only -- every colour, border, radius
      // and state in this control still comes from the application sheet, so a palette change
      // upstream still reaches it.
      terrain_type_group->setStyleSheet
        ( "QGroupBox#terrainTypeSegments QPushButton {"
          "  padding: 4px 5px;"
          "  font-size: 11px;"
          "  min-width: 0px;"
          "}"
        );

      QGridLayout* terrain_type_layout (new QGridLayout (terrain_type_group));
      terrain_type_layout->setContentsMargins (0, 0, 0, 0);
      terrain_type_layout->setHorizontalSpacing (4);
      terrain_type_layout->setVerticalSpacing (4);

      // Filled left to right, three per row, so the grid stays rectangular whether or not the
      // stamp build drops the Vertex entry (9 buttons -> 3/3/3, 8 -> 3/3/2).
      {
        std::vector<QPushButton*> type_buttons
          {radio_flat, radio_linear, radio_smooth, radio_polynomial, radio_trigo, radio_quadra,
           radio_gauss};

        if (!stamp)
          type_buttons.push_back (radio_vertex);

        type_buttons.push_back (radio_script);

        int const columns (3);

        for (std::size_t i (0); i < type_buttons.size(); ++i)
        {
          terrain_type_layout->addWidget
            (type_buttons[i], static_cast<int> (i) / columns, static_cast<int> (i) % columns);
        }

        for (int c (0); c < columns; ++c)
        {
          terrain_type_layout->setColumnStretch (c, 1);
        }
      }

      layout->addWidget(terrain_type_group);

      _radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _radius_slider->setRange (0, 1000);
      _radius_slider->setPrefix("Radius:");
      _radius_slider->setDecimals(2);
      _radius_slider->setValue(15);

      _inner_radius_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _inner_radius_slider->setRange (0.0, 1.0);
      _inner_radius_slider->setPrefix("Inner Radius:");
      _inner_radius_slider->setDecimals(2);
      _inner_radius_slider->setSingleStep(0.05f);
      _inner_radius_slider->setValue(0);

      QGroupBox* settings_group(new QGroupBox ("Settings", this));
      auto settings_layout (new QVBoxLayout (settings_group));
      // The theme already pads the inside of every QGroupBox (12px top / 14px bottom, on top of
      // the 20px margin that clears the title). The 0,12,0,12 that used to be here added a
      // second inset inside the first, so each of this tool's group boxes spent ~24px of the
      // panel on nothing. Zero it and let the sheet own the gutter; that keeps every tool's
      // group boxes consistent with each other instead of only this one being roomier.
      settings_layout->setContentsMargins(0, 0, 0, 0);
      // Each ExtendedSlider is now a tight two-line block (value row, then a 16px track), so the
      // gap BETWEEN blocks is what tells the eye where one setting ends and the next begins.
      // The style default of 6px was less than the 2px+track inside a row plus the row's own
      // ascender, which is why three stacked sliders read as one undifferentiated column of
      // controls. 8px is larger than any gap inside a row and smaller than the group rule above.
      settings_layout->setSpacing(8);

      _speed_slider = new Noggit::Ui::Tools::UiCommon::ExtendedSlider(this);
      _speed_slider->setPrefix("Speed:");
      _speed_slider->setRange (0, 10 * 100);
      _speed_slider->setSingleStep (1);
      _speed_slider->setValue(2);

      _snap_m2_objects_chkbox = new QCheckBox("Snap M2 objects", this);
      _snap_m2_objects_chkbox->setChecked(true);

      _snap_wmo_objects_chkbox = new QCheckBox("Snap WMO objects", this);
      _snap_wmo_objects_chkbox->setChecked(true);

      settings_layout->addWidget(_radius_slider);
      settings_layout->addWidget(_inner_radius_slider);
      settings_layout->addWidget(_speed_slider);
      settings_layout->addWidget(_snap_m2_objects_chkbox);
      settings_layout->addWidget(_snap_wmo_objects_chkbox);

      layout->addWidget(settings_group);

      _image_mask_group = new Noggit::Ui::Tools::ImageMaskSelector(map_view, this);
      _mask_image = _image_mask_group->getPixmap()->toImage();
      // layout->addWidget(_image_mask_group);
      _image_mask_group->setBrushModeVisible(!stamp);

      auto* customBrushBox = new ExpanderWidget(this);
      customBrushBox->setExpanderTitle("Custom Brush");
      customBrushBox->addPage(_image_mask_group);
      customBrushBox->setExpanded(false);
      layout->addWidget(customBrushBox);

      _vertex_type_group = new QGroupBox ("Vertex edit", this);
      _vertex_type_group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
      QVBoxLayout* vertex_layout (new QVBoxLayout (_vertex_type_group));
      vertex_layout->setContentsMargins (0, 0, 0, 0);
      vertex_layout->setSpacing (6);

      // Same treatment as the type grid above, for the same reason: this is a two-way exclusive
      // choice, and having it drawn as a pair of radio dots directly under a segmented control
      // that means the same thing is the sort of inconsistency the panel was being criticised
      // for. Ids, group, exclusivity and idClicked are unchanged.
      _vertex_button_group = new QButtonGroup (this);
      QPushButton* radio_mouse = new QPushButton ("Cursor", _vertex_type_group);
      QPushButton* radio_center = new QPushButton ("Selection center", _vertex_type_group);

      for (auto* button : {radio_mouse, radio_center})
      {
        button->setCheckable (true);
        button->setAutoDefault (false);
        button->setSizePolicy (QSizePolicy::Preferred, QSizePolicy::Fixed);
      }

      radio_mouse->setToolTip ("Orient vertices using the cursor pos as reference");
      radio_center->setToolTip ("Orient vertices using the selection center as reference");

      _vertex_button_group->addButton (radio_mouse, (int)eVertexMode_Mouse);
      _vertex_button_group->addButton (radio_center, (int)eVertexMode_Center);

      radio_center->toggle();

      QHBoxLayout* vertex_type_layout (new QHBoxLayout);
      vertex_type_layout->setContentsMargins (0, 0, 0, 0);
      vertex_type_layout->setSpacing (4);
      vertex_type_layout->addWidget (radio_mouse);
      vertex_type_layout->addWidget (radio_center);
      vertex_layout->addItem (vertex_type_layout);

      QHBoxLayout* vertex_angle_layout (new QHBoxLayout);
      vertex_angle_layout->addWidget (_orientation_dial = new QDial (_vertex_type_group));
      _orientation_dial->setRange(0, 360);
      _orientation_dial->setWrapping(true);
      _orientation_dial->setSliderPosition(_vertex_orientation._ - 90); // to get ingame orientation
      _orientation_dial->setToolTip("Orientation");
      _orientation_dial->setSingleStep(10);

      vertex_angle_layout->addWidget (_angle_slider = new QSlider (_vertex_type_group));
      _angle_slider->setRange(-89, 89);
      _angle_slider->setSliderPosition(_vertex_angle._);
      _angle_slider->setToolTip("Angle");

      vertex_layout->addItem (vertex_angle_layout);

      layout->addWidget(_vertex_type_group);
      _vertex_type_group->hide();

      connect ( _type_button_group, qOverload<int> (&QButtonGroup::idClicked)
              , [&] (int id)
                {
                  _edit_type = static_cast<eTerrainType> (id);
                  updateVertexGroup();
                }
              );


      connect ( _vertex_button_group, qOverload<int> (&QButtonGroup::idClicked)
              , [&] (int id)
                {
                  _vertex_mode = id;
                }
              );

      connect ( _angle_slider, &QSlider::valueChanged
              , [this] (int v)
                  {
                    if (NOGGIT_CUR_ACTION)
                    {
                      setAngle(v);
                    }
                    else
                    {
                      NOGGIT_ACTION_MGR->beginAction(_map_view);
                      setAngle(v);
                      NOGGIT_ACTION_MGR->endAction();
                    }

                  }
                );

      connect ( _orientation_dial, &QDial::valueChanged
              , [this] (int v)
                  {
                    if (NOGGIT_CUR_ACTION)
                    {
                      setOrientation(v + 90.0f);
                    }
                    else
                    {
                      NOGGIT_ACTION_MGR->beginAction(_map_view);
                      setOrientation(v + 90.0f);
                      NOGGIT_ACTION_MGR->endAction();
                    }

                  }
                );

      connect (_image_mask_group, &Noggit::Ui::Tools::ImageMaskSelector::rotationUpdated, this, &TerrainTool::updateMaskImage);
      connect (_radius_slider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged, this, &TerrainTool::updateMaskImage);
      connect(_image_mask_group, &Noggit::Ui::Tools::ImageMaskSelector::pixmapUpdated, this, &TerrainTool::updateMaskImage);


    }

    void TerrainTool::updateMaskImage()
    {
      QPixmap* pixmap = _image_mask_group->getPixmap();
      QTransform matrix;
      matrix.rotateRadians(_image_mask_group->getRotation() / 360.0f * 2.0f * M_PI);
      _mask_image = pixmap->toImage().transformed(matrix, Qt::SmoothTransformation);

      emit _map_view->trySetBrushTexture(&_mask_image, this);
    }

    void TerrainTool::changeTerrain
      (World* world, glm::vec3 const& pos, float dt)
    {

      float radius =  static_cast<float>(_radius_slider->value());
      if(_edit_type != eTerrainType_Vertex)
      {
        if (_image_mask_group->isEnabled())
        {
          // store the ground height diff at center of all objects hit before editing it
          std::vector<std::pair<SceneObject*, float>> objects_ground_distance = world->getObjectsGroundDistance(pos, radius
              , _snap_wmo_objects_chkbox->isChecked(), _snap_m2_objects_chkbox->isChecked());

          world->stamp(pos, dt * _speed_slider->value(), &_mask_image, radius,
                       _inner_radius_slider->value(),  _edit_type, _image_mask_group->getBrushMode());

          // re apply the ground height diff to the objects
          for (auto pair : objects_ground_distance)
          {
              auto obj = pair.first;
              auto new_ground_height = world->get_ground_height(obj->pos).y;
              world->set_model_pos(obj, glm::vec3(obj->pos.x, new_ground_height + pair.second, obj->pos.z));
          }
        }
        else
        {
          world->changeTerrain(pos, dt * _speed_slider->value(), radius, _edit_type, _inner_radius_slider->value());

          world->changeObjectsWithTerrain(pos, dt * _speed_slider->value(), radius, _edit_type, _inner_radius_slider->value()
              , _snap_wmo_objects_chkbox->isChecked(), _snap_m2_objects_chkbox->isChecked());
        }
      }
      else
      {
        // < 0 ==> control is pressed
        if (dt >= 0.0f)
        {
          world->selectVertices(pos,  radius);
        }
        else
        {
          if (world->deselectVertices(pos,  radius))
          {
            _vertex_angle = math::degrees (0.0f);
            _vertex_orientation = math::degrees (0.0f);
            world->clearVertexSelection();
          }
        }
      }
    }

    void TerrainTool::moveVertices (World* world, float dt)
    {
      world->moveVertices(dt * _speed_slider->value());
    }

    void TerrainTool::flattenVertices (World* world)
    {
      if (_edit_type == eTerrainType_Vertex)
      {
        world->flattenVertices (world->vertexCenter().y);
      }
    }

    void TerrainTool::nextType()
    {
      _edit_type = static_cast<eTerrainType> ((static_cast<int> (_edit_type) + 1) % eTerrainType_Count);
      _type_button_group->button (_edit_type)->toggle();
      updateVertexGroup();
    }

    void TerrainTool::setRadius(float radius)
    {
      _radius_slider->setValue(radius);
    }

    void TerrainTool::setInnerRadius(float radius)
    {
      _inner_radius_slider->setValue(radius);
    }

    void TerrainTool::changeRadius(float change)
    {
      setRadius (_radius_slider->value() + change);
    }

    void TerrainTool::changeInnerRadius(float change)
    {
      _inner_radius_slider->setValue(_inner_radius_slider->value() + change);
    }

    void TerrainTool::changeSpeed(float change)
    {
      _speed_slider->setValue(_speed_slider->value() + change);
    }

    void TerrainTool::setSpeed(float speed)
    {
      _speed_slider->setValue(speed);
    }

    float TerrainTool::getSpeed() const
    {
      return _speed_slider->value();
    }

    Noggit::Ui::Tools::UiCommon::ExtendedSlider* TerrainTool::getRadiusSlider()
    {
      return _radius_slider;
    }

    Noggit::Ui::Tools::UiCommon::ExtendedSlider* TerrainTool::getInnerRadiusSlider()
    {
      return _inner_radius_slider;
    }

    Noggit::Ui::Tools::UiCommon::ExtendedSlider* TerrainTool::getSpeedSlider()
    {
      return _speed_slider;
    }

    QDial* TerrainTool::getMaskOrientationDial()
    {
      return _image_mask_group->getMaskOrientationDial();
    }

    void TerrainTool::changeOrientation (float change)
    {
      setOrientation (_vertex_orientation._ + change);
    }

    void TerrainTool::setOrientation (float orientation)
    {
      if (_edit_type == eTerrainType_Vertex)
      {
        QSignalBlocker const blocker (_orientation_dial);

        while (orientation >= 360.0f)
        {
          orientation -= 360.0f;
        }
        while (orientation < 0.0f)
        {
          orientation += 360.0f;
        }

        _vertex_orientation = math::degrees (orientation);
        _orientation_dial->setSliderPosition (_vertex_orientation._ - 90.0f);

        emit updateVertices(_vertex_mode, _vertex_angle, _vertex_orientation);
      }
    }

    void TerrainTool::setOrientRelativeTo (World* world, glm::vec3 const& pos)
    {
      if (_edit_type == eTerrainType_Vertex)
      {
        glm::vec3 const& center = world->vertexCenter();
        _vertex_orientation = math::radians (std::atan2(center.z - pos.z, center.x - pos.x));
        emit updateVertices(_vertex_mode, _vertex_angle, _vertex_orientation);
      }
    }

    float TerrainTool::brushRadius() const
    {
      return static_cast<float>(_radius_slider->value());
    }

    float TerrainTool::innerRadius() const
    {
      return static_cast<float>(_inner_radius_slider->value());
    }

    void TerrainTool::storeCursorPos(glm::vec3* cursor_pos)
    {
      _cursor_pos = cursor_pos;
    }

    Noggit::Ui::Tools::ImageMaskSelector* TerrainTool::getImageMaskSelector()
    {
      return _image_mask_group;
    }

    QImage* TerrainTool::getMaskImage()
    {
      return &_mask_image;
    }

    void TerrainTool::changeAngle (float change)
    {
      setAngle (_vertex_angle._ + change);
    }

    void TerrainTool::setAngle (float angle)
    {
      if (_edit_type == eTerrainType_Vertex)
      {
        QSignalBlocker const blocker (_angle_slider);
        _vertex_angle = math::degrees (std::max(-89.0f, std::min(89.0f, angle)));
        _angle_slider->setSliderPosition (_vertex_angle._);
        emit updateVertices(_vertex_mode, _vertex_angle, _vertex_orientation);
      }
    }

    void TerrainTool::updateVertexGroup()
    {
      _vertex_type_group->setVisible(_edit_type == eTerrainType_Vertex);
      _image_mask_group->setVisible(_edit_type != eTerrainType_Vertex && _edit_type != eTerrainType_Script);
    }

    QSize TerrainTool::sizeHint() const
    {
      return QSize(250, height());
    }

    QJsonObject TerrainTool::toJSON()
    {
      QJsonObject json;

      json["brush_action_type"] = "TERRAIN";

      json["radius"] = _radius_slider->rawValue();
      json["inner_radius"] = _inner_radius_slider->rawValue();
      json["speed"] = _speed_slider->rawValue();
      json["edit_type"] = static_cast<int>(_edit_type);

      json["mask_enabled"] = _image_mask_group->isEnabled();
      json["brush_mode"] = _image_mask_group->getBrushMode();
      json["randomize_rot"] = _image_mask_group->getRandomizeRotation();
      json["mask_rot"] = _image_mask_group->getRotation();
      json["mask_image"] = _image_mask_group->getImageMaskPath();

      return json;
    }

    void TerrainTool::fromJSON(QJsonObject const& json)
    {
      _radius_slider->setValue(json["radius"].toDouble());
      _inner_radius_slider->setValue(json["inner_radius"].toDouble());
      _speed_slider->setValue(json["speed"].toDouble());
      _edit_type = static_cast<eTerrainType>(json["edit_type"].toInt());

      _image_mask_group->setEnabled(json["mask_enabled"].toBool());
      _image_mask_group->setBrushMode(json["brush_mode"].toInt());
      _image_mask_group->setRandomizeRotation(json["randomize_rot"].toBool());
      _image_mask_group->setRotationRaw(json["mask_rot"].toInt());
      _image_mask_group->setImageMask(json["mask_image"].toString());
    }
  }
}
