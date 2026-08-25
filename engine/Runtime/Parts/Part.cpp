// Split from natives_generated_world.cpp — Part.cpp
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

// PE @ 0x00469340 — IDA Part_getWear. Handle 0 / failed phys walk → 1.0
// (no Mighty ERROR). Java unit = remaining condition 0..1 (1=new).
// PE stores consumed wear at *(inner+0x4C)+0xB8; returns 1.0 - that.
// Host TREE "wear" is the Java-facing value (setWear writes it directly).

float java_game_parts_Part_getWear(InvObject* self) {
  if (!self) return 1.f;
  // Pristine default until first setWear (PE: no Native.ptr / no phys block).
  if (tree_field_get_int(self, "wear_set") == 0) return 1.f;
  return tree_field_get_float(self, "wear");
}

float java_game_parts_Part_setWear(InvObject* self, float value) {
  // PE @ 0x00469420 size 0xD3. Same phys walk as getWear; success →
  // *(payload+0x4C)+0xB8 = 1.0−value; fail walk → no-op store; always
  // returns value (no clamp). Host TREE "wear" = Java-facing value.
  if (!self) return value;
  tree_field_set_int(self, "wear_set", 1);
  tree_field_set_float(self, "wear", value);
  return value;
}

// PE @ 0x00469500 size 0xcf — IDA Part_setMaxWear. Handle 0 / failed
// phys walk → silent ret (no Mighty ERROR; contrast getWear/getTear
// fail → 1.0). No clamp. PE stores 1.0/value at *(inner+0x4C)+0xBC
// (flt_5F08F0 bytes 00 00 80 3F). Host TREE "max_wear" is the
// Java-facing lifetime budget (kmToMaxWear = km*1000; writes it
// directly). sub_5447D0 310 xrefs — not renamed.

void java_game_parts_Part_setMaxWear(InvObject* self, float value) {
  // PE @ 0x00469500 size 0xcf. Unbox this+F. Handle 0 / inner 0 /
  // sub_5447D0 sign / vtbl+0xC==0 → silent ret. NO Mighty (unlike
  // getWear @ 0x00469340 / getTear @ 0x004695D0 fail → 1.0).
  // Success: fstp 1.0/value at *(inner+0x4C)+0xBC. No clamp.
  // Host: Java kmToMaxWear budget in TREE max_wear (not 1/x).
  if (!self) return;
  tree_field_set_float(self, "max_wear", value);
}

// PE @ 0x004695D0 — IDA java_game_parts_Part_getTear. Handle 0 / failed
// phys walk → 1.0 (var_4 = 0x3F800000, no Mighty ERROR). Java unit =
// remaining 0..1 (1=new). PE stores tear at *(inner+0x4C)+0xB4 and
// returns that (NOT 1.0-that; wear is +0xB8 inverted). Host TREE "tear"
// is the Java-facing value (setTear writes it directly).

float java_game_parts_Part_getTear(InvObject* self) {
  if (!self) return 1.f;
  if (tree_field_get_int(self, "tear_set") == 0) return 1.f;
  return tree_field_get_float(self, "tear");
}

float java_game_parts_Part_setTear(InvObject* self, float value) {
  // PE @ 0x004696A0 size 0xcd. Same phys walk as setWear @ 0x00469420;
  // success → *(payload+0x4C)+0xB4 = value (NOT 1.0−value; wear is
  // +0xB8 inverted). Fail walk → no-op store; always returns value
  // (no clamp). Host TREE "tear" = Java-facing value. sub_5447D0 /
  // dword_62E008 not renamed.
  if (!self) return value;
  tree_field_set_int(self, "tear_set", 1);
  tree_field_set_float(self, "tear", value);
  return value;
}

// PE @ 0x00469AB0 size 0xc7 — IDA java_game_parts_Part_getTexture.
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner=*(handle+0xC)==0
// / sub_5447D0 sign / vtbl+0xC==0 → return 0 (xor ebp,ebp). NO Mighty
// ERROR (unlike ResourceRef.id @ 0x0047D290). [inner+0x4C]!=INSTANCE_GAME=1
// → vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → 0. payload=vtbl+0xC(1.0f). second=*(payload+0x44);
// block=*(payload+0x4C). second 0 → 0. Same INSTANCE_GAME check on
// second. sub_5447D0(second, 0x20000000, 0.0, 0.0) sign / vtbl+0xC==0
// → 0. Success: dword [block+0x84] (int_convert 132). Twin of getMesh @
// 0x00469CC0 (+0x94 mesh). Same payload+0x4C block as getCar @
// 0x004690A0 / getWear @ 0x00469340. Java ()I → texture resource ID
// (Part.save). No Native.ptr / vtable / sub_5447D0 on host. Host TREE
// "part_texture" (setTexture writes it). sub_5447D0 / dword_62E008 not
// renamed.

int32_t java_game_parts_Part_getTexture(InvObject* self) {
  // PE @ 0x00469AB0 size 0xc7. Phys walk → [*(payload+0x4C)+0x84].
  // Host stand-in: TREE part_texture until native phys objects exist.
  return self ? tree_field_get_int(self, "part_texture") : 0;
}

int32_t java_game_parts_Part_setTexture(InvObject* self, int32_t ID) {
  if (!self) return 0;
  const int32_t prev = tree_field_get_int(self, "part_texture");
  tree_field_set_int(self, "part_texture", ID);
  return prev;
}

// PE @ 0x00469CC0 size 0xc7 — IDA java_game_parts_Part_getMesh.
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner=*(handle+0xC)==0
// / sub_5447D0 sign / vtbl+0xC==0 → return 0 (xor ebp,ebp). NO Mighty
// ERROR (unlike ResourceRef.id @ 0x0047D290). [inner+0x4C]!=INSTANCE_GAME=1
// → vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → 0. payload=vtbl+0xC(1.0f). mesh_obj=*(payload+0x44);
// block=*(payload+0x4C). mesh_obj 0 → 0. Same INSTANCE_GAME check on
// mesh_obj. sub_5447D0(mesh_obj, 0x20000000, 0.0, 0.0) sign / vtbl+0xC==0
// → 0. Success: dword [block+0x94] (int_convert 148). Same payload+0x4C
// pointer as getCar @ 0x004690A0 / getWear @ 0x00469340. Java ()I →
// ResourceRef(mshID) (Part.save). No Native.ptr / vtable / sub_5447D0 on
// host. Host TREE "part_mesh" (setMesh writes it). sub_5447D0 /
// dword_62E008 not renamed.

int32_t java_game_parts_Part_getMesh(InvObject* self) {
  // PE @ 0x00469CC0 size 0xc7. Phys walk → [*(payload+0x4C)+0x94].
  // Host stand-in: TREE part_mesh until native phys/mesh objects exist.
  return self ? tree_field_get_int(self, "part_mesh") : 0;
}

int32_t java_game_parts_Part_setMesh(InvObject* self, int32_t ID) {
  if (!self) return 0;
  const int32_t prev = tree_field_get_int(self, "part_mesh");
  tree_field_set_int(self, "part_mesh", ID);
  return prev;
}

// PE @ 0x00469ED0 size 0xc7 — IDA java_game_parts_Part_getRenderType.
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner=*(handle+0xC)==0
// / sub_5447D0 sign / vtbl+0xC==0 → return 0 (xor ebp,ebp). NO Mighty
// ERROR (unlike ResourceRef.id @ 0x0047D290). [inner+0x4C]!=INSTANCE_GAME=1
// → vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → 0. payload=vtbl+0xC(1.0f). second=*(payload+0x44);
// block=*(payload+0x4C). second 0 → 0. Same INSTANCE_GAME check on
// second. sub_5447D0(second, 0x20000000, 0.0, 0.0) sign / vtbl+0xC==0
// → 0. Success: dword [block+0xA4] (int_convert 164). Twin of getTexture @
// 0x00469AB0 (+0x84 texture) / getMesh @ 0x00469CC0 (+0x94 mesh). Same
// payload+0x4C block as getCar @ 0x004690A0 / getWear @ 0x00469340. Java
// ()I → render-type resource ID (tyre LOD scripts setRenderType). No
// Native.ptr / vtable / sub_5447D0 on host. Host TREE "part_render_type"
// (setRenderType writes it). sub_5447D0 / dword_62E008 not renamed.

int32_t java_game_parts_Part_getRenderType(InvObject* self) {
  // PE @ 0x00469ED0 size 0xc7. Phys walk → [*(payload+0x4C)+0xA4].
  // Host stand-in: TREE part_render_type until native phys objects exist.
  return self ? tree_field_get_int(self, "part_render_type") : 0;
}

int32_t java_game_parts_Part_setRenderType(InvObject* self, int32_t ID) {
  if (!self) return 0;
  const int32_t prev = tree_field_get_int(self, "part_render_type");
  tree_field_set_int(self, "part_render_type", ID);
  return prev;
}

InvObject* java_game_parts_Part_install_OK(InvObject* self, InvObject* dest, int32_t slot, InvObject* part, int32_t slot2, InvObject* pos) {
  (void)pos;
  InvObject* parent = dest ? dest : self;
  if (!parent) return nullptr;

  int32_t ps = slot;
  int32_t cs = slot2 > 0 ? slot2 : 1;
  InvObject* child = part;

  // Phase 2.140: slot==0 → CFG auto-match for `self` onto `dest`.
  // Explicit slots keep Valocity/probe semantics (install `part` child).
  if (slot <= 0) {
    if (!self || !dest) return nullptr;
    if (!part_find_cfg_install(dest, self, &ps, &cs)) return nullptr;
    child = self;
    parent = dest;
    if (part) {
      java_util_resource_ResourceRef_set_1(part, dest);
      tree_field_set_obj(part, "script_instance", dest);
    }
  } else if (!child) {
    return nullptr;
  }

  if (!part_install(parent, ps, child, cs)) return nullptr;
  // Stock returns int[2] = {parentSlot, childSlot}; host: length-2 vector.
  InvObject* out = tree_vector_new();
  InvObject* a = gameref_new();
  InvObject* b = gameref_new();
  tree_field_set_int(a, "value", ps);
  tree_field_set_int(b, "value", cs);
  tree_vector_add(out, a);
  tree_vector_add(out, b);
  return out;
}

// PE @ 0x0046A5A0 size 0xD9 — IDA java_game.parts.Part.flap(I)I.
// Unbox this + mode (I) via JVM_UnboxArg @ 0x0045D910. Native.ptr
// (dword_62E008) via JVM_vm_get_int_field @ 0x0042AB50. Handle 0 /
// inner=*(handle+0xC)==0 / [inner+0x4C]!=INSTANCE_GAME=1 → vtbl+0x14(1.0f
// 0x3F800000) / sub_5447D0(inner, 0xA0000000, 0, 0) sign / vtbl+0xC==0 →
// return -1 (or ebp,ebp=0xFFFFFFFF). NO Mighty ERROR. Two-level walk like
// getCar @ 0x004690A0 / getMesh @ 0x00469CC0: payload=vtbl+0xC(1.0f);
// second=*(payload+0x44); block=*(payload+0x4C); same INSTANCE_GAME +
// sub_5447D0(second, 0x20000000) / vtbl+0xC on second. Success:
// Part_flapApply(obj, block, mode) @ 0x00474B00 (size 0x29C). Inner: slot list head
// from *[obj+0x115C] (4444) gated by [list+4]; count [obj+0x1150] (4432);
// flags base [block+0x78] (120) step 0x68 (104); match hood slot via
// [node+0x48] (72) + flag byte [flags+0x14] bit0; flap phys [esi+0x88]
// (136). Encode read: bit0 set→1 else 2; bit1(0x2)→|4; bit2(0x4)→|8.
// mode 0 (Java getFlap): return encoding (miss loop → 0, mode ignored).
// mode 1 (Java toggleFlap): bit0 set → call 0x43F280, [+0x88]|=2, return
// local; bit0 clear → [+0x88]=([+0x88]&~2)|4, return encoding. modes
// 2–4: jump table @ 0x474D9C (dec mode; ja if >4). Java only 0/1.
// Host TREE "flap_state" = Java 0/1 (bit0 stand-in; same field as
// part_flap_toggle / Mechanic flap_toggle command); no Native.ptr /
// Part_flapApply / sub_5447D0. dword_62E008 / sub_5447D0 not renamed.

int32_t java_game_parts_Part_flap(InvObject* self, int32_t mode) {
  // PE @ 0x0046A5A0 size 0xD9. Fail walk → eax=-1; Part_flapApply miss → 0.
  if (!self) return -1;
  int32_t st = tree_field_get_int(self, "flap_state");
  if (mode == 1) {
    st ^= 1;
    tree_field_set_int(self, "flap_state", st);
  }
  return st;
}

// PE @ 0x004690A0 size 0x7c — IDA java_game_parts_Part_getCar.
// Registered Natives_Register_PartDyno @ 0x0046B530 xref 0x0046B7FF.
// Callees: JVM_UnboxArg @ 0x0045D910, JVM_vm_get_int_field @ 0x0042AB50,
// sub_5447D0 @ 0x005447D0. Unbox this. Native.ptr (dword_62E008). Handle 0
// / inner=*(handle+0xC)==0 / sub_5447D0 sign / vtbl+0xC==0 → return 0
// (xor edi,edi). NO Mighty ERROR (unlike ResourceRef.id @ 0x0047D290).
// [inner+0x4C]!=INSTANCE_GAME=1 → vtbl+0x14(1.0f 0x3F800000).
// sub_5447D0(inner, 0xA0000000, 0.0, 0.0) test 0x80000000 → 0. Success:
// dword [*(vtbl+0xC(1.0f)+0x4C)+0xC8] (int_convert 200). Same payload+0x4C
// pointer as getWear @ 0x00469340 v5 (wear float +0xB8). Java ()I → new
// GameRef(carID) (Part.addPart). No Native.ptr / vtable / sub_5447D0 on
// host. Host TREE: part_car_root (part_parent walk) then ResourceRef.id on
// install-graph root. dword_62E008 / sub_5447D0 not renamed.

int32_t java_game_parts_Part_getCar(InvObject* self) {
  // PE @ 0x004690A0 size 0x7c. Phys walk → [*(payload+0x4C)+0xC8].
  if (!self) return 0;
  InvObject* root = part_car_root(self);
  return root ? java_util_resource_ResourceRef_id(root) : 0;
}

// PE @ 0x004691F0 size 0x96 — IDA java_game_parts_Part_getWheelID.
// Unbox this via JVM_UnboxArg @ 0x0045D910. Native.ptr (dword_62E008)
// via JVM_vm_get_int_field @ 0x0042AB50. Handle 0 / inner=*(handle+0xC)==0
// → return -1 (edi=0xFFFFFFFF). NO Mighty ERROR. [inner+0x4C]!=INSTANCE_GAME=1
// → vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → -1. payload=vtbl+0xC(1.0f); block=*(payload+0x4C).
// slot=[block+0xD0] (int_convert 208). slot>100 (0x64) && slot<=400 (0x190):
// return (slot-101)%10 (idiv 10, edx). Else -1. Single-level phys walk
// (not two-level getCar @ 0x004690A0). Java ()I → Chassis.getWheel(id).
// 1 xref data: Natives_Register_PartDyno @ 0x46B81E. No Native.ptr / vtable /
// sub_5447D0 on host. Host part_wheel_id: part_parent walk to hop under
// car root; chassis slots 101..104 → 0..3; tyre inherits via rim.
// dword_62E008 / sub_5447D0 not renamed.

int32_t java_game_parts_Part_getWheelID(InvObject* self) {
  // PE @ 0x004691F0 size 0x96. Phys → [*(payload+0x4C)+0xD0]; (slot-101)%10.
  if (!self) return -1;
  return part_wheel_id(self);
}

void java_game_parts_Part_disableSlot(InvObject* self, int32_t slotID,
                                      int32_t status) {
  // status 1 = lock (Chassis suspension chain), 0 = unlock.
  part_disable_slot(self, slotID, status);
}

InvObject* java_game_parts_Part_getSlotDamage(InvObject* self,
                                              int32_t slotIndex) {
  if (!self || slotIndex < 0) return string_new("");
  auto it = g_slot_dmg.find(self);
  if (it == g_slot_dmg.end()) return string_new("");
  auto jt = it->second.find(slotIndex);
  if (jt == it->second.end() || jt->second.empty()) return string_new("");
  return string_new(jt->second.c_str());
}

// PE @ 0x00468C50 size 0x193 — IDA java_game_parts_Part_setSlotDamage.
// Unbox this + slotIndex (I) + data (Ljava/lang/String;) via JVM_UnboxArg
// @ 0x0045D910. Native.ptr (dword_62E008) via JVM_vm_get_int_field @
// 0x0042AB50. Same two-level walk as getSlotDamage @ 0x00468AB0 /
// getSlots @ 0x004684A0: handle→inner+0xC, INSTANCE_GAME / vtbl+0x14(1.0f)
// / sub_5447D0(inner, 0xA0000000) sign / vtbl+0xC; second=*(payload+0x44);
// edi=*(payload+0x4C); sub_5447D0(second, 0x20000000) / vtbl+0xC;
// obj=*(payload+0xC). slotIndex>=0 && slotIndex<[obj+0x1150] (4432) else
// silent ret. flags=*(edi+0x78)+slotIndex*0x68 (104). String len>1:
// Util_Sscanf "%f,%f,%f,%f,%f,%f" → pos [flags+0x2C..+0x34] (44..52),
// Ypr_toMatrix @ +0x38 (56), [flags+0x14]|=0x20. len<=1: [flags+0x14]&=~0x20.
// Fail walk → silent ret (no Mighty ERROR). Java (ILjava/lang/String;)V.
// Host g_slot_dmg[slotIndex] = formatted 6-float CSV (getSlotDamage twin);
// erase when clear. No Native.ptr / vtable / sub_5447D0 on host.
// dword_62E008 / sub_5447D0 not renamed.

void java_game_parts_Part_setSlotDamage(InvObject* self, int32_t slotIndex,
                                        InvObject* data) {
  // PE @ 0x00468C50 size 0x193. Phys flags walk; len>1 → 6-float blob.
  if (!self || slotIndex < 0) return;
  if (slotIndex >= part_slot_count(self)) return;

  const char* s = data ? string_cstr(data) : "";
  const size_t len = s ? std::strlen(s) : 0;
  if (len <= 1) {
    auto it = g_slot_dmg.find(self);
    if (it != g_slot_dmg.end()) it->second.erase(slotIndex);
    return;
  }

  float px = 0.f, py = 0.f, pz = 0.f, oy = 0.f, op = 0.f, or_ = 0.f;
  std::sscanf(s, "%f,%f,%f,%f,%f,%f", &px, &py, &pz, &oy, &op, &or_);
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", px, py, pz,
                oy, op, or_);
  g_slot_dmg[self][slotIndex] = buf;
}

void java_game_parts_Part_setSlotPos(InvObject* self, int32_t slotID, InvObject* pos, InvObject* ypr) {
  part_set_slot_pos(self, slotID, pos, ypr);
  // Phase 2.65: chassis wheel slots keep WheelRef state in sync.
  if (slotID >= 101 && slotID <= 104) {
    InvObject* rim = part_on_slot(self, slotID);
    if (!rim) return;
    auto& w = WR(rim);
    if (pos) {
      vec3_get(pos, &w.px, &w.py, &w.pz);
      w.has_pos = true;
    }
    if (ypr) {
      ypr_get(ypr, &w.oy, &w.op, &w.or_);
      w.has_ypr = true;
    }
  }
}

// PE @ 0x004684A0 size 0xc4 — IDA java_game_parts_Part_getSlots.
// Unbox this (JVM_UnboxArg @ 0x0045D910). Native.ptr via
// JVM_vm_get_int_field @ 0x0042AB50 (dword_62E008). ebx=0 fail sentinel
// (xor ebx,ebx; mov eax,ebx). Handle 0 / inner=*(handle+0xC)==0 → 0.
// NO Mighty ERROR. [inner+0x4C]!=INSTANCE_GAME=1 → vtbl+0x14(1.0f
// 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0) test 0x80000000
// → 0. payload=vtbl+0xC(1.0f)==0 → 0. second=*(payload+0x44)==0 → 0.
// Same INSTANCE_GAME + sub_5447D0(second, 0x20000000, 0.0, 0.0) /
// vtbl+0xC. Success: dword [*(*(vtbl+0xC(1.0f))+0xC)+0x1150]
// (int_convert 4432). Two-level walk like getMesh @ 0x00469CC0.
// Xref: Natives_Register_PartDyno @ 0x0046B536. Java ()I → max slot
// index+1 (Part.java). Host TREE: part_slot_count (part_slots size).
// No Native.ptr / vtable / sub_5447D0 on host. dword_62E008 /
// sub_5447D0 not renamed (race109 TREE gate, 100+ xrefs).

int32_t java_game_parts_Part_getSlots(InvObject* self) {
  // PE @ 0x004684A0 size 0xc4. Phys → [*(*(obj+0xC)+0x1150)]; fail ebx=0.
  if (!self) return 0;
  return part_slot_count(self);
}

// PE @ 0x00468570 size 0x164 — IDA java_game_parts_Part_getSlotID.
// Unbox this + slotIndex (I). Native.ptr (dword_62E008). Same two-level
// phys walk as getSlots @ 0x004684A0: handle→inner+0xC, INSTANCE_GAME /
// vtbl+0x14(1.0f) / sub_5447D0(inner, 0xA0000000) sign / vtbl+0xC;
// second=*(payload+0x44); edi=*(payload+0x4C); sub_5447D0(second,
// 0x20000000) / vtbl+0xC; obj=*(payload+0xC). Fail → ebp=0. NO Mighty
// ERROR. After walk (asm jl @ 0x46864B; Hex-Rays drops this branch):
//   slotIndex>=0: list=obj+0x1154 (4436); head=sub_429390?0:[list+8];
//     walk idx times via [node+4] (null if [node+4]==0 || *+4==0);
//     return [node+0x48] (72).
//   slotIndex<0 (Java -1): head from *[obj+0x115C] (4444) via [list+4]
//     gate; flags=[edi+0x78]+0x14 step 0x68 over count=[obj+0x1150];
//     stop when flag&1; return [node+0x48] (mate on this part).
// Java (I)I: index→slot ID; -1→parent mate slot ID (Part.java). Host TREE:
// >=0 part_slot_id_at (OOB→0); <0 mate via part_parent +
// part_slot_id_on_slot(parent, part_parent_slot). No Native.ptr / vtable /
// sub_5447D0 on host. dword_62E008 / sub_5447D0 / sub_429390 not renamed.

int32_t java_game_parts_Part_getSlotID(InvObject* self, int32_t slotIndex) {
  // PE @ 0x00468570 size 0x164. Phys → [node+0x48]; fail ebp=0.
  if (!self) return 0;
  if (slotIndex < 0) {
    // PE loc_468688: *[obj+0x115C] / flag&1 → mate slot ID on self.
    InvObject* parent = tree_field_get_obj(self, "part_parent");
    const int32_t ps = tree_field_get_int(self, "part_parent_slot");
    if (!parent || ps == 0) return 0;
    return part_slot_id_on_slot(parent, ps);
  }
  // PE loc_46864D: +0x1154 / walk [node+4] slotIndex → [node+0x48].
  const int32_t id = part_slot_id_at(self, slotIndex);
  return id < 0 ? 0 : id;
}

// PE @ 0x004686E0 size 0x10B — IDA java_game_parts_Part_getSlotIndex.
// Unbox this + slotID (I) into arg0 / var_4 (JVM_UnboxArg). Native.ptr
// (dword_62E008). Same two-level walk as getSlotID @ 0x00468570 /
// getSlots @ 0x004684A0: handle→inner+0xC, INSTANCE_GAME /
// vtbl+0x14(1.0f) / sub_5447D0(inner, 0xA0000000) sign / vtbl+0xC;
// second=*(payload+0x44); sub_5447D0(second, 0x20000000) / vtbl+0xC;
// obj=*(payload+0xC). List: ecx=*[obj+0x115C] (4444); add eax,1154h
// dead (overwritten); head=([list+4]!=0)?list:0 — same node chain as
// getSlotID idx≥0 head v7[2]=*(obj+0x1154+8), gate differs (no
// sub_429390). Loop @ 0x4687C1: cmp [node+0x48], var_4/slotID → jz
// return edx; else node=[node+4] (null if node==0 || [node+4]==0),
// ++edx; miss ebx=0. Hex-Rays falsely showed while([node+0x48]!=0).
// Index 0 and fail both return 0 (PE). No Mighty ERROR. Inverse of
// getSlotID(idx≥0) by ID. Host TREE: scan part_slot_id_at until match.
// No Native.ptr / vtable / sub_5447D0 on host. dword_62E008 /
// sub_5447D0 not renamed.

int32_t java_game_parts_Part_getSlotIndex(InvObject* self, int32_t slotID) {
  // PE @ 0x004686E0 size 0x10B. Phys: [node+0x48]==slotID → index.
  if (!self) return 0;
  const int32_t n = part_slot_count(self);
  for (int32_t i = 0; i < n; ++i) {
    if (part_slot_id_at(self, i) == slotID) return i;
  }
  return 0;
}

// PE @ 0x00469970 size 0x13C — IDA java_game_parts_Part_isSlotDisabled.
// Unbox this + slotID (I). Native.ptr (dword_62E008). Same two-level walk
// as getSlotIndex @ 0x004686E0: handle→inner+0xC, INSTANCE_GAME /
// vtbl+0x14(1.0f) / sub_5447D0(inner, 0xA0000000) sign / vtbl+0xC;
// second=*(payload+0x44); edi=*(payload+0x4C); sub_5447D0(second,
// 0x20000000) / vtbl+0xC; obj=*(payload+0xC). List head from
// *[obj+0x115C] gated by [list+4] (int_convert 4444); parallel flags
// base=[edi+0x78] (120). Walk: match [node+0x48]==slotID, else
// node=[node+4] (null if [node+4]==0 || *[node+4]+4==0) and flags+=0x68
// (104). Hit: flags dword at +0x14; test al,0x50 → return 2; else
// (flags>>4)&1 (PE shr path; with disableSlot bits always 0 when 0x50
// clear). Miss / failed walk → 0 (ebx). Contrast disableSlot @
// 0x00469770 (II)V: clear flags&=~0x50 then status==1 → |0x10,
// status==2 → |0x40 (and fallthrough |0x10), status==0 leave clear.
// Java (I)I. No Native.ptr / vtable / sub_5447D0 on host. Host TREE:
// slot.disabled (disableSlot @ 0x00469770 sets flags|0x10/0x50) → 2/0.
// dword_62E008 / sub_5447D0 not renamed.

int32_t java_game_parts_Part_isSlotDisabled(InvObject* self, int32_t slotID) {
  // PE @ 0x00469970 size 0x13C. ebx=0 fail; hit flags+0x14 &0x50 → 2.
  if (!self || slotID == 0) return 0;
  return part_slot_is_disabled(self, slotID) ? 2 : 0;
}

// IDA Part_getLogo @ 0x0046A0E0 — manufacturer / logo rid field.

int32_t java_game_parts_Part_getLogo(InvObject* self) {
  if (!self) return 0;
  const int32_t m = tree_field_get_int(self, "manufacturer");
  if (m) return m;
  return tree_field_get_int(self, "logo");
}

// IDA Part_getMass @ 0x00469290 — physics mass; host mirror field.

float java_game_parts_Part_getMass(InvObject* self) {
  if (!self) return 0.f;
  const float m = tree_field_get_float(self, "mass");
  return m > 0.f ? m : 0.f;
}

// IDA Part_setSfxLoopParams @ 0x46A1B0 — store loop pitch pair, return 1.

int32_t java_game_parts_Part_setSfxLoopParams(InvObject* self, float a,
                                              float b) {
  if (!self) return 0;
  tree_field_set_float(self, "sfx_loop_a", a);
  tree_field_set_float(self, "sfx_loop_b", b);
  return 1;
}

InvObject* java_game_parts_Part_partOnSlot(InvObject* self, int32_t slotID) {
  // PE @ 0x004687F0: slotID==-1 → *[obj+0x50] (self Part); else list +0x1154.
  if (slotID == -1) return self;
  return part_on_slot(self, slotID);
}

int32_t java_game_parts_Part_slotIDOnSlot(InvObject* self, int32_t slotID) {
  return part_slot_id_on_slot(self, slotID);
}

// PE @ 0x00469120 size 0xc9 — IDA java_game_parts_Part_getCarRef.
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner=*(handle+0xC)==0
// / sub_5447D0 sign / vtbl+0xC==0 → return 0 (xor eax). NO Mighty ERROR
// (unlike ResourceRef.id @ 0x0047D290). [inner+0x4C]!=INSTANCE_GAME=1 →
// vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → 0. payload=vtbl+0xC(1.0f). Same payload+0x4C block as
// getCar @ 0x004690A0 (+0xC8 carID) / getWear @ 0x00469340: chassis
// native = dword [*(payload+0x4C)+0xCC] (int_convert 204). Null → 0.
// Second walk on that object (INSTANCE_GAME / sub_5447D0 / vtbl+0xC);
// success → dword [payload2+0x50] (int_convert 80) = Java Part* (same
// +0x50 as partOnSlot -1 @ 0x004687F0). Fail → 0. Java ()Ljava.game.
// parts.Part; → chassis/root Part (Part.getWheel). No Native.ptr /
// vtable / sub_5447D0 on host. Host TREE: part_car_root (part_parent
// walk). dword_62E008 / sub_5447D0 not renamed.

InvObject* java_game_parts_Part_getCarRef(InvObject* self) {
  // PE @ 0x00469120 size 0xc9. Phys: [*(payload+0x4C)+0xCC] → [+0x50].
  // Host stand-in: install-graph root via part_parent until native phys.
  return part_car_root(self);
}

}  // namespace inv
