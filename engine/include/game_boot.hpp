#pragma once

#include "jvm.hpp"

namespace inv {

// PATH-TO-WORLD (stock PE, VA-backed):
//   Engine_boot@0x58C700 → ResourceEngine_Init → Engine_InitState@0x427980
//     (LoadGameInit "Config") → FMV_Boot_PlayPath_DirectShow@0x55C470 →
//   Engine_MainLoop@0x428960 prologue: LoadGameInit("GameInit")@0x53A5E0 →
//   while: Input/sim/Jvm_PumpFrame → Splash→MainMenu (GameLogic) →
//   CMD_NEW(=50) → Garage → Hit the Street → Valocity (world).
// Host --boot / --game stand in for MainLoop script path only.

// Host Init → GameLogic mid-boot → Splash → MainMenu → CMD_NEW → Garage.
// Console / scripted CAS path (--boot).
int game_boot_run(Jvm& jvm, const char* game_root, const char* player_name,
                  bool wait_enter = true);

// Phase 2.126 — interactive --game: window + MainMenu live loop.
// auto_new: smoke fires CMD_NEW after a few frames (no human input).
// max_frames: 0 = run until quit; >0 = capped (smoke / --no-wait).
int game_interactive_run(Jvm& jvm, const char* game_root,
                         const char* player_name, bool auto_new,
                         int32_t max_frames);

}  // namespace inv
