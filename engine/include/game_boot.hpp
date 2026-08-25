#pragma once

#include "jvm.hpp"

namespace inv {

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
