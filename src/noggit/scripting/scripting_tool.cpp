// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/scripting/script_context.hpp>
#include <noggit/scripting/script_exception.hpp>
#include <noggit/scripting/script_profiles.hpp>
#include <noggit/scripting/script_settings.hpp>
#include <noggit/scripting/scripting_tool.hpp>
#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/Camera.hpp>
#include <noggit/Log.h>
#include <noggit/tool_enums.hpp>
#include <noggit/World.h>
#include <noggit/MapView.h>

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QSlider>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QCheckBox>

#include <cmath>

#define CUR_PROFILE_PATH "__cur_profile"

namespace Noggit
{
  namespace Scripting
  {
    // TEMP: remove when exceptions are working
    namespace
    {
      std::string cur_exception = "";

      // Gap between two sendBrushEvent calls that can only mean the caller skipped the ticks
      // where the button was up. MapView drives onTick from the render loop, so honoured calls
      // are one frame apart; no human releases and re-presses inside a quarter second, and only
      // a hitch longer than that could produce a false positive. Used solely by the fallback in
      // sendBrushEvent, which disarms itself as soon as the caller is seen ticking while idle.
      constexpr std::chrono::milliseconds SWALLOWED_STROKE_GAP {250};
    }
    void set_cur_exception(std::string const& exception)
    {
      cur_exception = exception;
    }

    void scripting_tool::doReload()
    {
      get_settings()->clear();
      clearLog();
      int old_selection = -1;
      try
      {
        std::string old_name = get_context() == nullptr ? "" : get_context()->get_selected_name();
        _script_context = std::make_unique<script_context>(this);
        for(int i=0;i<get_context()->get_scripts().size(); ++i)
        {
          if(get_context()->get_scripts()[i]->get_name() == old_name)
          {
            old_selection = i;
            break;
          }
        }
        
        // default to 0 if there are entries
        if(get_context()->get_scripts().size()>0 && old_selection < 0)
        {
          old_selection = 0;
        }
      }
      catch (std::exception const& e)
      {
        addLog("[error]: " + std::string(e.what()));
        resetLogScroll();
        return;
      }

      _selection->clear();

      for(auto& script : get_context()->get_scripts())
      {
        _selection->addItem(script->get_name().c_str());
      }

      if (old_selection >= 0)
      {
        _selection->setCurrentIndex(old_selection);
        change_script(old_selection);
      }
    }

    void scripting_tool::change_script(int selection)
    {
      std::lock_guard<std::mutex> const lock (_script_change_mutex);

      clearDescription();
      get_settings()->clear();

      auto sn = _script_context->get_scripts()[selection]->get_name();

      get_profiles()->clear();

      auto json = get_settings()->get_raw_json();

      if (json->contains(sn))
      {
        std::vector<std::string> items;
        for (auto& v : (*json)[sn].items())
        {
          if (v.key() != CUR_PROFILE_PATH)
          {
            items.push_back(v.key());
          }
        }

        std::sort(items.begin(), items.end(), [](auto a, auto b) {
          if (a == "Default")
            return true;
          if (b == "Default")
            return false;
          return a < b;
        });

        for (auto& item : items)
        {
          get_profiles()->add_profile(item);
        }
      }

      if (get_profiles()->profile_count() == 0)
      {
        get_profiles()->add_profile("Default");
      }

      int next_profile = 0;
      auto cur_script = get_context()->get_scripts()[selection]->get_name();
      if (json->contains(cur_script))
      {
        if ((*json)[cur_script].contains(CUR_PROFILE_PATH))
        {
          auto str = (*json)[cur_script][CUR_PROFILE_PATH].get<std::string>();
          for (int i = 0; i < get_profiles()->profile_count(); ++i)
          {
            if (get_profiles()->get_profile(i) == str)
            {
              next_profile = i;
              break;
            }
          }
        }
      }

      get_profiles()->select_profile(next_profile);

      try {
        get_context()->select_script(selection);
      } catch(script_exception const& err)
      {
        addLog(err.what());
      }
      get_settings()->initialize();
    }

    scripting_tool::scripting_tool(
        QWidget* parent
      , MapView* view
      , QSettings* noggit_settings
      )
      : QWidget(parent)
      , _cur_profile ("Default")
      , _view(view)
      , _noggit_settings(noggit_settings)
    {
      auto layout(new QVBoxLayout(this));
      layout->setContentsMargins(0, 1, 0, 1);
      _selection = new QComboBox();
      layout->addWidget(_selection);

      _reload_button = new QPushButton("Reload Scripts", this);
      layout->addWidget(_reload_button);
      connect(_reload_button, &QPushButton::released, this, [this]() {
        doReload();
      });

      _profiles = new script_profiles(this);
      layout->addWidget(_profiles);

      _settings = new script_settings(this);
      _settings->load_json();
      layout->addWidget(_settings);

      _description = new QLabel(this);
      layout->addWidget(_description);

      _log = new QPlainTextEdit(this);
      _log->setFont (QFontDatabase::systemFont (QFontDatabase::FixedFont));
      _log->setReadOnly(true);
      layout->addWidget(_log);

      connect(_selection
             , QOverload<int>::of(&QComboBox::activated)
             , this
             , [this](auto index) 
             {
               clearLog();
               change_script(index);
             });

      doReload();
    }

    scripting_tool::~scripting_tool()
    {
      get_settings()->save_json();
    }

    void scripting_tool::dispatchBrushEdges( bool left
                                           , bool right
                                           , std::shared_ptr<script_brush_event> const& evt
                                           )
    {
      int const sel = get_context()->get_selection();
      if (sel < 0)
      {
        return;
      }

      auto brush = get_context()->get_scripts()[sel];

      if (left)
      {
        if (!_last_left) brush->on_left_click.call_if_exists("brush_event", evt);
        else brush->on_left_hold.call_if_exists("brush_event", evt);
      }
      else if (_last_left)
      {
        brush->on_left_release.call_if_exists("(brush_event)", evt);
      }
      // Adopted here, not by the caller, so that two transitions can be replayed back to back.
      _last_left = left;

      if (right)
      {
        if (!_last_right) brush->on_right_click.call_if_exists("brush_event", evt);
        else brush->on_right_hold.call_if_exists("brush_event", evt);
      }
      else if (_last_right)
      {
        brush->on_right_release.call_if_exists("(brush_event)", evt);
      }
      _last_right = right;
    }

    void scripting_tool::sendBrushEvent(glm::vec3 const& pos, float dt)
    {
      bool const new_left = get_view()->leftMouse;
      bool const new_right = get_view()->rightMouse;

      auto const now = std::chrono::steady_clock::now();
      auto const since_last_call = now - _last_call_time;
      _last_call_time = now;

      // A call with neither button down is only reachable from a caller that ticks this
      // unconditionally, and it is also the only call on which a release edge can be observed.
      // Seeing one proves the contract in the header is being honoured.
      if (!new_left && !new_right)
      {
        _pumped_while_idle = true;
      }

      // Idle and nothing pending: no script call, no undo action, not even the event object.
      // This is what makes "call me every tick" cheap enough for the caller to honour.
      if (!new_left && !new_right && !_last_left && !_last_right)
      {
        return;
      }

      // doReload() leaves the context null if the very first load throws (doReload, above), and
      // this now runs on every tick rather than only mid-stroke.
      if (!get_context())
      {
        return;
      }

      if (get_context()->get_selection() < 0)
      {
        // No script to receive anything. State is still adopted, so a script selected mid-hold
        // does not see a click edge for a button that was already down.
        _last_left = new_left;
        _last_right = new_right;
        return;
      }

      // Script callbacks edit the world, and a world edit with no action running dereferences a
      // null NOGGIT_CUR_ACTION for terrain (World.cpp:1270). The release edge in particular
      // lands on the tick AFTER MapView closed the caller's LMB-modal action
      // (MapView.cpp:4197), so there is provably none open for it. beginAction returns the
      // running action untouched when there is one (ActionManager.cpp:64-65), so a caller that
      // opens its own keeps ownership and nothing below fires.
      bool const opened_action = (NOGGIT_CUR_ACTION == nullptr);
      if (opened_action)
      {
        int modality = ActionModalityControllers::eNONE;
        if (new_left)  modality |= ActionModalityControllers::eLMB;
        if (new_right) modality |= ActionModalityControllers::eRMB;

        // Objects and terrain both: a script is not restricted to one kind of edit, so the
        // action has to record whatever it touches.
        NOGGIT_ACTION_MGR->beginAction(get_view()
          , ActionFlags::eOBJECTS_ADDED
          | ActionFlags::eOBJECTS_REMOVED
          | ActionFlags::eCHUNKS_TERRAIN
          , modality);
      }

      auto evt = std::make_shared<script_brush_event>(
          get_settings()
        , pos
        , dt
      );

      try
      {
        // Fallback for a caller that only reaches this while a button is held: the up ticks
        // never arrive, so the detector sees one endless hold. A gap far longer than a frame is
        // the only in-band evidence that the stroke ended; replaying the missing all-up
        // transition fires the release and turns the next press back into a click. It cannot
        // recover right-button events, which such a caller never reaches at all.
        if (!_pumped_while_idle && since_last_call > SWALLOWED_STROKE_GAP)
        {
          dispatchBrushEdges(false, false, evt);
        }

        dispatchBrushEdges(new_left, new_right, evt);
      }
      catch (std::exception const& e)
      {
        doReload();
        // TEMP: remove when exceptions are working
        if(std::string(e.what()).find("C++ exception") == 0 && cur_exception.size()>0)
        {
          addLog("[error]: "+std::string(cur_exception));
          cur_exception = "";
        }
        else
        {
          addLog(("[error]: " + std::string(e.what())));
        }
        resetLogScroll();
      }

      // Outside the try on purpose: a throwing callback must not leave the detector on a stale
      // state and re-fire the same click on every following tick.
      _last_left = new_left;
      _last_right = new_right;

      // An action opened for a tick with no button held carries no modality controller, so
      // endActionOnModalityMismatch can never close it (it returns early on eNONE,
      // ActionManager.cpp:128-129) and undo() would assert with it still running. While a button
      // IS held it is deliberately left open: MapView closes it on release, which is what makes
      // a whole stroke one undo step instead of one per tick.
      if (opened_action && !new_left && !new_right && NOGGIT_CUR_ACTION)
      {
        NOGGIT_ACTION_MGR->endAction();
      }
    }

    void scripting_tool::addDescription(std::string const& stext)
    {
      _description->setText(_description->text() 
                           + "\n" 
                           + QString::fromStdString (stext)
                           );
    }

    void scripting_tool::addLog(std::string const& text)
    {

      LogDebug << "[script window]: " << text << "\n";
      _log->appendPlainText (QString::fromStdString (text));
      _log->verticalScrollBar()->setValue(_log->verticalScrollBar()->maximum());
    }

    script_context* scripting_tool::get_context()
    {
      return _script_context.get();
    }

    MapView* scripting_tool::get_view()
    {
      return _view;
    }

    void scripting_tool::resetLogScroll()
    {
      _log->verticalScrollBar()->setValue(0);
    }

    void scripting_tool::clearLog()
    {
      _log->clear();
    }

    void scripting_tool::clearDescription()
    {
      _description->clear();
    }

    script_settings* scripting_tool::get_settings()
    {
      return _settings;
    }

    script_profiles* scripting_tool::get_profiles()
    {
      return _profiles;
    }

    QSettings* scripting_tool::get_noggit_settings()
    {
      return _noggit_settings;
    }
  } // namespace Scripting
} // namespace Noggit
