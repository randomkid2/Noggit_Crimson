// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <noggit/scripting/script_context.hpp>
#include <noggit/scripting/script_brush.hpp>
#include <noggit/tool_enums.hpp>
#include <math/trig.hpp>
#include <QtWidgets/QWidget>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QGridLayout>
#include <QSettings>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>

class World;
class MapView;

namespace Noggit
{
  class Camera;
  namespace Scripting
  {
    class script_context;
    class script_settings;
    class script_profiles;
    class scripting_tool : public QWidget
    {
    public:
      scripting_tool(QWidget* parent
                    , MapView* view
                    , QSettings * noggit_settings
                    );
      ~scripting_tool();

      void addDescription(std::string const& text);
      void clearDescription();
      void addLog(std::string const& text);
      void resetLogScroll();
      void clearLog();
      void doReload();

      // MUST be called once per tick, INCLUDING ticks where no mouse button is held.
      //
      // This drives an edge detector: on_left_click, on_left_release and every right-button
      // callback are derived from the transition between the previous button state and the
      // current one, so a caller that only reaches this while the left button is down starves
      // it -- on_left_click fires once per session and release/right-button events never fire
      // at all. Calling it unconditionally is the contract; see the fallback in
      // scripting_tool.cpp for what happens when a caller does not honour it.
      //
      // It is cheap on an idle tick (no script call, no event object, no undo action) and it
      // opens its own undo action only when a callback is actually about to run, so the caller
      // does not have to gate it to avoid empty undo steps.
      void sendBrushEvent(glm::vec3 const& pos,float dt);

      MapView* get_view();
      script_context* get_context();
      script_settings* get_settings();
      script_profiles* get_profiles();
      QSettings* get_noggit_settings();

    private:
      std::mutex _script_change_mutex;
      std::string _cur_profile;

      bool _last_left = false;
      bool _last_right = false;

      // True once sendBrushEvent has been reached on a tick with neither button held, which only
      // a caller that ticks it unconditionally can produce. Disarms the swallowed-stroke
      // fallback in sendBrushEvent.
      bool _pumped_while_idle = false;
      std::chrono::steady_clock::time_point _last_call_time = std::chrono::steady_clock::now();

    private:
      QComboBox* _selection;
      QPushButton* _reload_button;

      QLabel* _description;
      QPlainTextEdit* _log;

      script_settings* _settings;
      script_profiles* _profiles;
    private:
      std::unique_ptr<script_context> _script_context = nullptr;
      MapView* _view;
      QSettings * _noggit_settings;
      void change_script(int script_index);

      // Fires the callbacks implied by moving from the last seen button state to (left, right)
      // and then adopts it. Separate from sendBrushEvent so a stroke boundary the caller
      // swallowed can be replayed as an extra transition.
      void dispatchBrushEdges(bool left
                             , bool right
                             , std::shared_ptr<script_brush_event> const& evt
                             );
    };

    // TEMP: remove when exceptions are working
    void set_cur_exception(std::string const& exception);
  } // namespace Scripting
} // namespace Noggit
