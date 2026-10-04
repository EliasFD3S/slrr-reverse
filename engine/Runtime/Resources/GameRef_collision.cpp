#include "host_objects.hpp"
#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "jvm.hpp"
#include "tree_interp.hpp"
#include "render_d3d9.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"
#include "Resources.h"
#include "System.h"
#include "GameRef.h"
#include "GameRef_internal.hpp"
#include "../Parts/Body/Chassis.h"
#include "../Parts/world_state.hpp"
#include "Resources_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace inv {

void java_util_resource_GameRef_setActiveCollision(InvObject* self) {
  // PE @ 0x0047DF80 size 0x71: Unbox this. Native.ptr (dword_62E008)==0 →
  // Mighty ERROR ("!" @ 0x612F30 + "Mighty ERROR" @ 0x612F34 via
  // CRT_strcat_n_thunk / Engine_ErrorLogPrintf, Engine_ErrorLogBuf cap 0x100).
  // Else GameRef_queueActiveCollision @ 0x00498810 size 0x82:
  // inner=*(handle+0xC); inner==0 → return. *(inner+0x54)&0x10000000 already
  // queued → return. else OR 0x10000000 (same dword as getFlags @ 0x0047DF40),
  // Engine_malloc(0x1C) node (vtbl off_5F09B4), ResHandle_Rebind(node+0xC,
  // inner) (188 xrefs, not ported), splice Engine_SimCallbackPending
  // sentinel @ 0x643740 / last @ 0x643748. 2 code xrefs (JNI + sub_42D6A0
  // HUD path). List/node not on host.
  // Host Soft collide gate: GameRefState.flags bit + ResState.flags mirror
  // (inner+0x54) + ResState.collide (physics_integrate pairs Phase 2.25).
  // Soft arcade_collide_queued. !self = handle 0 (silent; PE logs Mighty).
  // Host Soft asleep: PE does NOT clear asleep here (wake = voidEvent
  // "wakeup" @ 0x459ED1 → physics_set_asleep 0). Valocity ensure_car_physics
  // @ GameRef.cpp arms drive via this native after garage "suspend"; clear
  // asleep so arcade_motion_allowed / physics_drive early-out can pass.
  if (!self) return;
  resref_ensure(self);

  const uint32_t flags54 =
      static_cast<uint32_t>(java_util_resource_GameRef_getFlags(self));
  if (!arcade_collide_queued(flags54)) {
    // PE queue: *(inner+0x54) |= 0x10000000 (idempotent bit gate above).
    java_util_resource_GameRef_setFlags(
        self, static_cast<int32_t>(kArcadeCollideQueuedBit));
    {
      std::lock_guard<std::mutex> lock(g_mu);
      R(self).flags |= static_cast<int32_t>(kArcadeCollideQueuedBit);
    }
  }

  physics_set_collide_active(self, 1);
  physics_set_asleep(self, 0);
}

}  // namespace inv
