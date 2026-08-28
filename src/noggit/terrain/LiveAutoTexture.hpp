// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_LIVEAUTOTEXTURE_HPP
#define NOGGIT_LIVEAUTOTEXTURE_HPP

#include <noggit/terrain/TerrainRulePainter.hpp>

#include <cstddef>

class MapView;

namespace Noggit
{
  // LIVE AUTO TEXTURE: at the end of a terrain sculpting stroke, the chunks the stroke moved
  // retexture themselves against the rules the user authored in Automatic Texturing, so shaping a
  // cliff paints it as it goes.
  //
  // The idea is taken from Noggit Gold (github.com/AbyssalRealm/Noggit_Gold), another GPL-3.0 fork
  // of the same upstream, where it is called "Live Auto". Their implementation was not read and no
  // code is shared; this is an independent build on top of TerrainRules, TerrainRulePainter and the
  // ActionManager, all of which this fork already owns. The credit is here because it is decent
  // form, not because an idea carries a licence obligation.
  //
  // THE FOUR PROPERTIES THAT MAKE THIS SAFE RATHER THAN DESTRUCTIVE:
  //
  //   1. OPT-IN, OFF AT EVERY START. TerrainRuleStore::liveAutoEnabled is false until somebody
  //      ticks the box in the Automatic Texturing dialog, and it is never written to QSettings.
  //      Retexturing a chunk overwrites hand-painted alpha, and an hour of blended shoreline is not
  //      something to put behind a setting that quietly survives a restart.
  //
  //   2. ONCE PER STROKE, NOT ONCE PER TICK. See runIfStrokeEnding.
  //
  //   3. INSIDE THE STROKE'S OWN UNDO STEP. Nothing here opens or closes an action. The painter
  //      registers its before-images on the action the stroke is already holding open, which adds
  //      ActionFlags::eCHUNKS_TEXTURE to it (Action.cpp:699) so Action::finish snapshots the
  //      texture post-state alongside the terrain one. One Ctrl+Z then reverts the shape and the
  //      paint together. Landing the paint in its OWN action would leave the user able to undo the
  //      paint but keep the new terrain, then undo again and get the old terrain with the new
  //      paint -- a state that never existed and looks exactly like data corruption.
  //
  //   4. THE STROKE'S OWN CHUNKS, PLUS ONE RING. Never the whole tile. See gatherStrokeChunks in
  //      the implementation for how the ring is found and what happens when it leaves the loaded
  //      area.
  namespace LiveAutoTexture
  {
    // THE STROKE-END HOOK. Call from MapView::tick with the modality word that is about to be
    // handed to ActionManager::endActionOnModalityMismatch, and BEFORE that call.
    //
    // WHY HERE AND NOWHERE ELSE. A sculpting stroke is one action spanning many ticks: the tools
    // call beginAction inside onTick on every frame the mouse is held, and beginAction returns the
    // action that is already running rather than opening a second one (ActionManager.cpp:64). So
    // there is no per-stroke callback to hang this on -- but there is exactly one place where such
    // an action is closed, and it is ActionManager::endActionOnModalityMismatch, reached from
    // MapView.cpp:5300 and from nowhere else in the tree. The frame after the user lets go of
    // Shift or the left button, the modality word no longer contains the controllers the action was
    // opened with, and the action is finished. That instant is the stroke end.
    //
    // BEFORE, not after, and that ordering is not cosmetic: endActionOnModalityMismatch calls
    // Action::finish(), which is where the redo snapshot of every registered chunk is taken
    // (Action.cpp:425). A texture change registered after finish() would have a before-image and no
    // after-image, so redo would put the terrain back and leave the paint behind.
    //
    // Doing this per tick instead would re-evaluate every rule over every affected chunk dozens of
    // times a second and repaint 4096 alphamap texels per chunk per call (texture_set.cpp:847
    // walks the whole 64x64 grid whatever the brush radius is), for a result that is thrown away by
    // the next tick. Only the final geometry decides the final texture.
    //
    // Returns the number of chunks the pass actually changed; 0 whenever the switch is off, the
    // action is not a terrain edit, the stroke is not ending, or nothing needed repainting.
    std::size_t runIfStrokeEnding(MapView* map_view, unsigned action_modality);

    // The same pass, run unconditionally against whatever action is open right now.
    //
    // For the terrain edits that are NOT drags: the Flatten hotkey brackets its own beginAction and
    // endAction inside a single call (RaiseLowerTool.cpp:28-39), so its action never passes through
    // a modality mismatch and runIfStrokeEnding would never see it. Call this between the edit and
    // endAction().
    std::size_t runNow(MapView* map_view);
  }
}

#endif // NOGGIT_LIVEAUTOTEXTURE_HPP
