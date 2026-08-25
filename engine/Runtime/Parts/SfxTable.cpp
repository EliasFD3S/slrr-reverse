// Split from natives_generated_world.cpp — SfxTable.cpp
#include "natives.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "render_d3d9.hpp"
#include "tree_interp.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "world_state.hpp"

namespace inv {

void java_game_parts_SfxTable_finalize(InvObject* self) {
  if (!self) return;
  g_sfxtables.erase(self);
}

int32_t java_game_parts_SfxTable_getItems(InvObject* self) {
  // PE @ 0x00442310 size 0x31 (49). IDA java_game_parts_SfxTable_getItems.
  // Callees: JVM_UnboxArg @ 0x0045D910 (sig ()I dest0),
  // JVM_vm_get_int_field (dword_62E008 / Native.ptr) @ 0x0042AB50.
  // Xref: Natives_Register_Partial data @ 0x00442A52. Handle==0 →
  // xor eax,eax ret 0 (NO Mighty). Else mov eax,[eax+0x240] (576)
  // item count; retn. clear @ 0x00442350 zeros +0x240; addItem @
  // 0x004423E0 incs same dword (jge 16 only there). NO cmp 16 here.
  // Host: !self / map miss ≈ handle==0 → 0; vector.size() ≈ [+0x240].
  if (!self) return 0;
  auto it = g_sfxtables.find(self);
  if (it == g_sfxtables.end()) return 0;
  return static_cast<int32_t>(it->second.size());
}

void java_game_parts_SfxTable_clear(InvObject* self) {
  // PE @ 0x00442350 size 0x88 (136). IDA java_game_parts_SfxTable_clear.
  // Callees: JVM_UnboxArg @ 0x0045D910 (sig ()V dest0),
  // JVM_vm_get_int_field (dword_62E008 / Native.ptr) @ 0x0042AB50.
  // Xref: Natives_Register_Partial data @ 0x00442A71. Handle==0 → jz ret
  // (NO Mighty). Loop edi=0..[handle+0x240]-1 (count at +576, slot base
  // handle+4, stride 0x24): ecx=slot; esi=[ecx+8] (sfx @ handle+0x0C for
  // slot0). esi!=0 → unlink sfx+0x48 intrusive list via [ecx-4]/[ecx]/[ecx+4],
  // then zero [ecx-4],[ecx],[ecx+4],[ecx+8]; else only [ecx+4]=0. Floats
  // +0x10..+0x20 (pitch..vmax) left stale until next addItem overwrites.
  // End: [handle+0x240]=0. NO cmp 16 (addItem @ 0x004423E0 only).
  // Host: !self / map miss ≈ handle==0; vector.clear() ≈ count=0 (drops
  // slots); sfx+0x48 intrusive list not mirrored.
  if (!self) return;
  auto it = g_sfxtables.find(self);
  if (it == g_sfxtables.end()) return;
  it->second.clear();
}

void java_game_parts_SfxTable_addItem(InvObject* self, InvObject* sfx,
                                     float pitch, float pmin, float pmax,
                                     float vmin, float vmax) {
  // PE @ 0x004423E0 size 0x120 (288). Unbox this+ResourceRef+FFFFF
  // (sig (Ljava.util.resource.ResourceRef;FFFFF)V dest0..6) via
  // JVM_UnboxArg @ 0x0045D910. Native.ptr = JVM_vm_get_int_field
  // (dword_62E008 — not renamed) @ 0x0042AB50. Handle==0 → jz ret
  // (NO Mighty). Count=[handle+0x240] (576); cmp 10h / jge no-op.
  // Else slot=handle+count*0x24 (36; lea edx+edx*8,*4), then
  // inc count into +0x240. esi=[ResourceRef+0x0C]; if slot+0x0C!=esi:
  // unlink old (sfx+0x48 list via slot+0/4) if old!=0; if esi!=0 link
  // slot as new head at esi+0x48, slot+0x0C=esi, slot+0x08=[esi+0x50]
  // (80); else zero slot+0/4/8/C. Same-sfx → skip rebind. Then FPU:
  // flt_5F08F0 (1.0 @ 0x005F08F0) fdiv pitch → fstp slot+0x10; then
  // +0x14..+0x20 = pmin,pmax,vmin,vmax (fstp order). Cap 16 = here
  // only (getItems/clear no clamp). Host: !self ≈ handle==0;
  // vector.size()>=16 ≈ jge; operator[] seeds map (Chassis.getSfxTable);
  // stores Java ResourceRef*; intrusive list not mirrored; pitch=1/pitch.
  if (!self) return;
  auto& rows = g_sfxtables[self];
  if (rows.size() >= 16) return;
  SfxItem item;
  item.sfx = sfx;
  item.pitch = 1.f / pitch;
  item.pmin = pmin;
  item.pmax = pmax;
  item.vmin = vmin;
  item.vmax = vmax;
  rows.push_back(item);
}

}  // namespace inv
