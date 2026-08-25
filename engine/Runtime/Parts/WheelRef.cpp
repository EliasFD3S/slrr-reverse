// Split from natives_generated_world.cpp — WheelRef.cpp
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
namespace {

// PE Ypr_toMatrix @ 0x0054ECD0 / Ypr_fromMatrix @ 0x00551C90 (getYpr @ 0x00441FD0).
void wheelref_ypr_to_mat34(float m[12], float yaw, float pitch, float roll) {
  const float sy = std::sin(yaw), cy = std::cos(yaw);
  const float sp = std::sin(pitch), cp = std::cos(pitch);
  const float sr = std::sin(roll), cr = std::cos(roll);
  const float sr_sp = sr * sp;
  const float cr_sp = cr * sp;
  m[0] = sr_sp * sy + cr * cy;
  m[1] = cr_sp * sy - sr * cy;
  m[2] = cp * sy;
  m[4] = sr * cp;
  m[5] = cr * cp;
  m[6] = -sp;
  m[8] = sr_sp * cy - cr * sy;
  m[9] = cr_sp * cy + sr * sy;
  m[10] = cp * cy;
}

void wheelref_ypr_from_mat34(const float m[12], float& yaw, float& pitch,
                             float& roll) {
  if (m[10] == 0.f && m[2] == 0.f) {
    unsigned bits = 0x40490FDB;
    std::memcpy(&pitch, &bits, sizeof(pitch));
    roll = 0.f;
    yaw = std::atan2(m[0], -m[1]);
    return;
  }
  yaw = std::atan2(m[2], m[10]);
  roll = std::atan2(m[4], m[5]);
  const float cr = std::cos(roll);
  const float v6 = cr * m[6];
  if (!(cr > 0.f))
    pitch = std::atan2(v6, -m[5]);
  else
    pitch = std::atan2(-v6, m[5]);
}

}  // namespace

void java_game_parts_WheelRef_finalize(InvObject* self) {
  if (!self) return;
  g_wheelrefs.erase(self);
}

InvObject* java_game_parts_WheelRef_getPos(InvObject* self) {
  // PE @ 0x00441B60 size 0x1A2. Unbox this. Native.ptr (dword_62E008)==0 →
  // null. NO Mighty ERROR. Else veh=*(handle+0x22C)(556); sub_426470(veh,6,
  // lock). [lock+0x4C]!=1 → vtbl+0x14(1.0f). sub_5447D0(0x80000000,0,0)≥0 &&
  // vtbl+0xC(1.0f)→ctx. idx=(handle-*[veh+0x13E4])/692(0x2B4);
  // slot=[ctx+0xC]+200*idx. x=slot+0x1F44(8004); z=slot+0x1F4C(8012);
  // y=[handle+0x7C]-slot+0x1F48(8008)-(slot+0x38+slot+0x3C). Lock fail →
  // garbage xyz but still Engine_malloc(0x1C) Vector3 + set fields. Unlock
  // doubly-linked list. Contraste setPos@0x441D10 (writes +0x78..+0x80,
  // sub_419860) / Chassis.getWheelPos@0x43CE30 (delegates here). Callees:
  // JVM_UnboxArg, JVM_vm_get_int_field, sub_426470, sub_5447D0,
  // Engine_malloc, JVM_getClass, JVM_Instance_initialize,
  // JVM_vm_set_float_field. Host: !self=nullptr; car+wid slot base x/z +
  // WR.py(+0x7C)−slot_y (no +0x38/+0x3C); lock fail→zeros; has_pos-only /
  // attach fallback when no chassis (GAP).
  if (!self) return nullptr;

  float x = 0.f, y = 0.f, z = 0.f;
  InvObject* car = part_car_root(self);
  const int32_t wid = part_wheel_id(self);
  auto& w = WR(self);
  float slot_x = 0.f, slot_y = 0.f, slot_z = 0.f;
  const bool has_slot =
      car && wid >= 0 &&
      part_slot_get_pose(car, 101 + wid, &slot_x, &slot_y, &slot_z, nullptr,
                         nullptr, nullptr);
  if (has_slot) {
    const float wheel_y = w.has_pos ? w.py : slot_y;
    x = slot_x;
    z = slot_z;
    y = wheel_y - slot_y;  // PE also −(+0x38++0x3C); not on host
  } else if (w.has_pos) {
    x = w.px;
    y = w.py;
    z = w.pz;
  } else {
    x = tree_field_get_float(self, "attach_x") * 0.01f;
    y = tree_field_get_float(self, "attach_y") * 0.01f;
    z = tree_field_get_float(self, "attach_z") * 0.01f;
  }
  return vec3_new(x, y, z);
}

InvObject* java_game_parts_WheelRef_getYpr(InvObject* self) {
  // PE @ 0x00441FD0 size 0xBF. Unbox this. Native.ptr (dword_62E008)==0 →
  // null. NO Mighty ERROR. NO lock/sub_426470/sub_5447D0 (unlike getPos /
  // setYpr-null). Ypr_fromMatrix(local,[handle+0x90]) @ 0x00551C90; Engine_malloc
  // (0x1C); JVM_getClass java.lang.Ypr; init; set float fields y/p/r. Inverse of
  // setYpr Ypr_toMatrix @ 0x0054ECD0 (+0x90). setYpr also memcpy +0x110←+0x90;
  // getYpr reads +0x90 only. Host: !self = handle 0 → nullptr; matrix stand-in
  // from WR via Ypr_toMatrix when has_ypr else zeros (uninit +0x90). Gaps: lock
  // table path, raw +0x90/+0x110 storage, Engine_malloc fail still sets fields.
  if (!self) return nullptr;
  auto& w = WR(self);
  float m[12] = {};
  if (w.has_ypr)
    wheelref_ypr_to_mat34(m, w.oy, w.op, w.or_);
  float y = 0, p = 0, r = 0;
  wheelref_ypr_from_mat34(m, y, p, r);
  return ypr_new(y, p, r);
}

float java_game_parts_WheelRef_getDrive(InvObject* self) {
  // PE @ 0x00440DC0 size 0x35 (53). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Native.ptr via JVM_vm_get_int_field(this, dword_62E008) @ 0x0042AB50.
  // handle==0 → fld flt_5E73CC (bytes 00 00 00 00 = 0.0). NO Mighty ERROR.
  // Else fld dword [handle+0xCC] (204) — first store of setDrive @ 0x00440E00
  // (also writes +0xE0/224, unread here). NO fmul. NOT getSteer [+0xC8]/
  // getRadius [+0x60] / setWidth [+0x1D4]. Callees: JVM_UnboxArg,
  // JVM_vm_get_int_field. Host WR.drive; !self = handle 0. Gap: existing
  // WheelRefState.drive defaults 1.f (ctor native not ported).
  return self ? WR(self).drive : 0.f;
}

float java_game_parts_WheelRef_getSteer(InvObject* self) {
  // PE @ 0x00440E50 size 0x35. Unbox this. Native.ptr (dword_62E008)==0 →
  // fld flt_5E73CC (bytes 00 00 00 00 = 0.0). NO Mighty ERROR. Else
  // fld dword [handle+0xC8] (200) — primary store of setSteer @ 0x00440E90
  // (mov [handle+0xC8] @ 0x440EC0). NO fmul. NOT getDrive [+0xCC] /
  // getRadius [+0x60]. Host WR.steer; !self = handle 0.
  return self ? WR(self).steer : 0.f;
}

float java_game_parts_WheelRef_getRadius(InvObject* self) {
  // PE @ 0x00440F50 size 0x32 (50). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Native.ptr via JVM_vm_get_int_field(this, dword_62E008) @ 0x0042AB50.
  // handle==0 → fld flt_5E73CC (bytes 00 00 00 00 = 0.0). NO Mighty ERROR.
  // Else fld dword [handle+0x60] (96) — primary store of setRadius @
  // 0x00440ED0 (also fstp [+0x5C]=val+[+0x50] + thiscall sub_491F80,
  // unread here). NO fmul. NOT getSteer [+0xC8] / getDrive [+0xCC] /
  // setWidth [+0x1D4]. Metres (Tyre.SetupTyre mm→m). Xref: register table
  // Natives_Register_Partial @ 0x00442B69. Callees: JVM_UnboxArg,
  // JVM_vm_get_int_field. Host WR.radius; !self = handle 0. Gap:
  // WheelRefState.radius defaults 0.32f (ctor native not ported).
  return self ? WR(self).radius : 0.f;
}

void java_game_parts_WheelRef_setPos(InvObject* self, InvObject* val) {
  // PE @ 0x00441D10 size 0x2B3. Unbox this+Vector3. Native.ptr
  // (dword_62E008)==0 → silent ret. NO Mighty ERROR. Else:
  //   sub_426470([handle+0x22C], 6, lock); sub_419860(lock, 0x80000000,
  //   0x3F800000=1.0f, 0, 0) → ctx. Wheel idx =
  //   (handle - *[chassis+0x13E4](5092)) / 692; entry =
  //   [ctx+0xC]+200*idx+0x1F44(8004). Load xyz; Y +=
  //   [entry+0x38]+[entry+0x3C]. If Vector3!=0: add JVM float fields
  //   "x","y","z". Delta [handle+0x27C..+0x290] vs old pos; store xyz →
  //   [handle+0x78/+0x7C/+0x80] (120/124/128). Optional sub_48AEA0
  //   (chassis+0x2C) world add; thiscall sub_492030([handle+0x30], &xyz,
  //   handle+0x110). Unlock. Host WR.px/py/pz = Vector3 absolute (PE
  //   dword triple); has_pos; slot 101+wid sync (not in PE). Gaps:
  //   table-base add, lock/sub_419860, +0x27C.. deltas, sub_48AEA0,
  //   sub_492030; null Vector3 still writes table base (host early-out).
  if (!self || !val) return;
  auto& w = WR(self);
  vec3_get(val, &w.px, &w.py, &w.pz);
  w.has_pos = true;
  InvObject* car = part_car_root(self);
  const int32_t wid = part_wheel_id(self);
  if (car && wid >= 0) {
    InvObject* ypr = w.has_ypr ? ypr_new(w.oy, w.op, w.or_) : nullptr;
    part_set_slot_pos(car, 101 + wid, val, ypr);
  }
}

void java_game_parts_WheelRef_setYpr(InvObject* self, InvObject* val) {
  // PE @ 0x00442090 size 0x180. Unbox this+Ypr. Native.ptr
  // (dword_62E008)==0 → silent ret. NO Mighty ERROR. Else:
  //   If Ypr!=0: load JVM float fields "y","p","r"; thiscall
  //   Ypr_toMatrix([handle+0x90], &ypr) @ 0x0054ECD0. NO lock.
  //   Else (null Ypr): sub_426470([handle+0x22C], 6, lock);
  //   sub_5447D0(lock, 0x80000000, 0.f, 0.f) → ctx. Wheel idx =
  //   (handle - *[chassis+0x13E4](5092)) / 692; entry =
  //   [ctx+0xC]+200*idx+0x1F50(8016); Ypr_toMatrix([handle+0x90],
  //   entry); unlock. Then always: memcpy [handle+0x110] ←
  //   [handle+0x90] size 0x30 (12 dwords / 3x4). Host WR.oy/op/or_ =
  //   Ypr absolute (PE matrix at +0x90); has_ypr; slot 101+wid sync
  //   (not in PE). Gaps: Ypr_toMatrix, +0x90/+0x110, lock/sub_5447D0,
  //   null-Ypr table path; null Ypr host early-out.
  if (!self || !val) return;
  auto& w = WR(self);
  ypr_get(val, &w.oy, &w.op, &w.or_);
  w.has_ypr = true;
  InvObject* car = part_car_root(self);
  const int32_t wid = part_wheel_id(self);
  if (car && wid >= 0) {
    InvObject* pos = w.has_pos ? vec3_new(w.px, w.py, w.pz) : nullptr;
    part_set_slot_pos(car, 101 + wid, pos, val);
  }
}

void java_game_parts_WheelRef_setDrive(InvObject* self, float val) {
  // PE @ 0x00440E00 size 0x42. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0xCC] (204) and
  // [handle+0xE0] (224) = val bits; no fmul. getDrive @ 0x00440DC0 flds
  // [handle+0xCC]. NOT setRadius [handle+0x60] (96) / setWidth
  // [handle+0x1D4] (468). Java 0..1 (Chassis 0; Transmission drive_front /
  // 1-drive_front). Host WR.drive = val (PE dword at +0xCC).
  if (self) WR(self).drive = val;
}

void java_game_parts_WheelRef_setSteer(InvObject* self, float val) {
  // PE @ 0x00440E90 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0xC8] (200) = val
  // bits @ 0x440EC0; no fmul (unlike setWidth @ 0x004416C0 fmul 0.5 into
  // [handle+0x1D4]). getSteer @ 0x00440E50 flds same slot. NOT setDrive
  // [handle+0xCC]/[+0xE0]. Host WR.steer = val (PE dword). !self = handle 0.
  if (self) WR(self).steer = val;
}

void java_game_parts_WheelRef_setRadius(InvObject* self, float val) {
  // PE @ 0x00440ED0 size 0x77. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0x60] (96) = val
  // bits @ 0x440F02; no fmul (unlike setWidth @ 0x004416C0 fmul flt_5F09D0
  // bytes 00 00 00 3F = 0.5 into [handle+0x1D4]). getRadius @ 0x00440F50
  // flds same slot. NOT setSteer [handle+0xC8] / setDrive [handle+0xCC].
  // Also fstp [handle+0x5C]=val+[handle+0x50] then thiscall sub_491F80
  // ([handle+0x30], ...) — not hosted. Java metres (Tyre mm→m). Host
  // WR.radius = val (PE dword). !self = handle 0.
  if (self) WR(self).radius = val;
}

void java_game_parts_WheelRef_setWidth(InvObject* self, float val) {
  // PE @ 0x004416C0 size 0x3e. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else fld val; fmul flt_5F09D0 (bytes
  // 00 00 00 3F = 0.5); fstp dword [handle+0x1D4] (468). NOT setRadius
  // slot [handle+0x60] (96). Java metres (Tyre tyre_width/1000). Host
  // WR.width = half-width (PE dword).
  if (self) WR(self).width = val * 0.5f;
}

void java_game_parts_WheelRef_setCPatch(InvObject* self, float halfwidth, float angle,
                                       float offset) {
  // PE @ 0x00441700 size 0x68. Unbox this+FFF. Native.ptr==0 or
  // *[handle+0x30]==0 → silent ret. Else inner: [+0x84]=fabs(hw),
  // [+0x88]=sin(|angle|), [+0x8C]=offset (no fabs). Host stand-in same.
  if (!self) return;
  auto& w = WR(self);
  w.cpatch_hw = std::fabs(halfwidth);
  w.cpatch_ang = std::sin(std::fabs(angle));
  w.cpatch_off = offset;
}

void java_game_parts_WheelRef_setFriction(InvObject* self, float val) {
  // PE @ 0x00440F90 size 0xB8. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else:
  //   mov [handle+0x1F0] (496) = val  — primary friction dword
  //   mov [handle+0x64] (100) = val; mov [handle+0x68] (104) = val
  //   factor from fld [handle+0x15C] (348) vs flt_5F08F0 (bytes 00 00 80 3F
  //   = 1.0): if <1 → 1−x*x*flt_5F0F00 (9A 99 19 3F ≈0.6); else
  //   flt_5F0EFC (D0 CC CC 3D ≈0.1). Then
  //   [handle+0x6C]=factor*[+0x64]; [handle+0x70]=factor*[+0x68]
  //   (same formula twice; +0x15C written elsewhere e.g. Chassis_forceUpdate
  //   @ 0x448507 — not hosted). No fmul on the primary store (unlike
  //   setWidth @ 0x004416C0). NOT setSliction [+0x1E8] / setFrictn_x
  //   [+0x218] / setBearing [+0xF0]. Java Tyre.SetupTyre insider_friction.
  //   Host WR.friction = val (PE dword at +0x1F0). !self = handle 0.
  //   Derived +0x64/+0x68/+0x6C/+0x70 not in WheelRefState.
  if (self) WR(self).friction = val;
}

float wheelref_get_friction(InvObject* self) {
  return self ? WR(self).friction : 1.f;
}

float wheelref_get_sliction(InvObject* self) {
  return self ? WR(self).sliction : 1.f;
}

float wheelref_get_brake(InvObject* self) {
  return self ? WR(self).brake : 0.f;
}

float wheelref_get_hbrake(InvObject* self) {
  return self ? WR(self).hbrake : 0.f;
}

float wheelref_get_roll_res(InvObject* self) {
  return self ? WR(self).roll_res : 0.f;
}

float wheelref_get_pacejka(InvObject* self, int32_t i) {
  if (!self || i < 0 || i >= 17) return 0.f;
  return WR(self).pacejka[i];
}

bool wheelref_get_arm(InvObject* self, float out[7]) {
  if (!self || !out) return false;
  auto& w = WR(self);
  if (!w.has_arm) return false;
  for (int i = 0; i < 7; ++i) out[i] = w.arm[i];
  return true;
}

bool wheelref_get_hub(InvObject* self, float out[10]) {
  if (!self || !out) return false;
  auto& w = WR(self);
  if (!w.has_hub) return false;
  for (int i = 0; i < 10; ++i) out[i] = w.hub[i];
  return true;
}

float wheelref_get_force(InvObject* self) {
  return self ? WR(self).force : 0.f;
}

float wheelref_get_damp_bound(InvObject* self) {
  return self ? WR(self).damp_bound : 0.f;
}

float wheelref_get_rest_len(InvObject* self) {
  return self ? WR(self).rest_len : 0.f;
}

float wheelref_get_arm_len(InvObject* self) {
  if (!self) return 0.f;
  auto& w = WR(self);
  return w.has_arm ? w.arm[0] : 0.f;
}

void java_game_parts_WheelRef_setFrictn_x(InvObject* self, float val) {
  // PE @ 0x00441050 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0x218] (536) = val
  // bits @ 0x441080; no fmul (unlike setWidth @ 0x004416C0 fmul 0.5 into
  // [handle+0x1D4]). NOT setFriction [handle+0x1F0] (496) / setSliction
  // [handle+0x1E8] (488) / setRadius [handle+0x60] / setSteer [handle+0xC8].
  // No getFrictn_x native. Physics fmul of stored dword in unnamed
  // sub_454500 @ 0x45619C/0x4563A4 (use-site, not setter). Host
  // WR.frictn_x = val (PE dword). !self = handle 0.
  if (self) WR(self).frictn_x = val;
}

void java_game_parts_WheelRef_setSliction(InvObject* self, float val) {
  // PE @ 0x00441090 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0x1E8] (488) = val
  // bits @ 0x4410C0; no fmul (unlike setWidth @ 0x004416C0 fmul 0.5 into
  // [handle+0x1D4]). NOT setFriction [handle+0x1F0] (496) / setFrictn_x
  // [handle+0x218] (536) / setBearing [handle+0xF0] (240) / setStiffness
  // [handle+0x1F8] / setRadius [handle+0x60] / setSteer [handle+0xC8].
  // No getSliction native in this ticket. Host WR.sliction = val (PE dword).
  // !self = handle 0.
  if (self) WR(self).sliction = val;
}

void java_game_parts_WheelRef_setStiffness(InvObject* self, float val) {
  if (self) WR(self).stiffness = val;
}

void java_game_parts_WheelRef_setRollRes(InvObject* self, float val) {
  // PE @ 0x00441110 size 0x35: Unbox this+F; dword_62E008; silent if 0;
  // store float bits [handle+0x74] (no fmul).
  if (self) WR(self).roll_res = val;
}

void java_game_parts_WheelRef_setBearing(InvObject* self, float val) {
  // PE @ 0x00441150 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0xF0] (240) = val
  // bits @ 0x441180; no fmul (unlike setWidth @ 0x004416C0 fmul 0.5 into
  // [handle+0x1D4]). NOT setRollRes [handle+0x74] (116) / setDrive
  // [handle+0xCC]/[+0xE0] / setSteer [handle+0xC8] / setFrictn_x
  // [handle+0x218] / setFriction [handle+0x1F0] / setSliction [handle+0x1E8]
  // / setStiffness [handle+0x1F8] / setMaxLoad [handle+0x214]. No
  // getBearing native. Physics fadd of stored dword in unnamed
  // sub_454500 @ 0x455C41 (use-site, not setter; fld+fstp @ 0x455E4C).
  // Host WR.bearing = val (PE dword). !self = handle 0.
  if (self) WR(self).bearing = val;
}

void java_game_parts_WheelRef_setMaxLoad(InvObject* self, float val) {
  if (self) WR(self).max_load = val;
}

void java_game_parts_WheelRef_setLoadSmooth(InvObject* self, float val) {
  // PE @ 0x004411D0 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0x220] (544) = val
  // bits @ 0x441200 (opcode 89 90 20 02 00 00); no fmul (unlike setWidth @
  // 0x004416C0 fmul 0.5 into [handle+0x1D4]). Sibling setMaxLoad @ 0x441190
  // stores +0x214 (532); setBearing @ 0x441150 stores +0xF0 (240). NOT
  // setMaxLoad [handle+0x214] / setFrictn_x [handle+0x218] / setFriction
  // [handle+0x1F0] / setSliction [handle+0x1E8] / setStiffness [handle+0x1F8]
  // / setBearing [handle+0xF0] / setRollRes [handle+0x74]. No getLoadSmooth
  // native. Physics use (unnamed): sub_454500 fld [esi+220h] @ 0x4562a4
  // (with maxLoad +0x214 @ 0x4562aa). Java Tyre.SetupTyre 0.4; Wheel 0.0.
  // Host WR.load_smooth = val (PE dword). !self = handle 0.
  if (self) WR(self).load_smooth = val;
}

void java_game_parts_WheelRef_setPacejka(InvObject* self, int32_t i, float val) {
  // PE @ 0x00441210 size 0x57. Unbox this+I+F. Bounds: jl if i<0; cmp 0x11 /
  // jnb if i>=17 → silent ret (valid i = 0..16). Else Native.ptr
  // (dword_62E008); handle==0 → silent ret. NO Mighty ERROR. Else fstp dword
  // [handle+i*4+0x1E8] (488+4*i) @ 0x44125c. Aliases (same PE slots): i0 =
  // setSliction +0x1E8, i2 = setFriction +0x1F0, i4 = setStiffness +0x1F8.
  // Host WR.pacejka[i] = val (17 floats). !self = handle 0.
  if (!self || i < 0 || i >= 17) return;
  WR(self).pacejka[i] = val;
}

void java_game_parts_WheelRef_setForce(InvObject* self, float val) {
  if (self) WR(self).force = val;
}

void java_game_parts_WheelRef_setDamping(InvObject* self, float val) {
  if (self) WR(self).damping = val;
}

void java_game_parts_WheelRef_setDamping_1(InvObject* self, float bound,
                                          float rebound) {
  if (!self) return;
  auto& w = WR(self);
  w.damp_bound = bound;
  w.damp_rebound = rebound;
}

void java_game_parts_WheelRef_setRestLen(InvObject* self, float val) {
  // PE @ 0x004413E0 size 0x7f. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty. Else [handle+0x50]=val (restLen); [+0x5C]=
  // [+0x60]+val (radius+restLen); thiscall sub_491F80 (not hosted).
  // Host WR.rest_len = val.
  if (self) WR(self).rest_len = val;
}

void java_game_parts_WheelRef_setMinLen(InvObject* self, float val) {
  // PE @ 0x00441460 size 0x66. Unbox this+F. Native.ptr==0 → silent ret.
  // Else [handle+0x58]=val; thiscall sub_491F80 (not hosted). No derived
  // write (unlike setRestLen [+0x5C]). Twin of setMaxLen [+0x54].
  if (self) WR(self).min_len = val;
}

void java_game_parts_WheelRef_setMaxLen(InvObject* self, float val) {
  // PE @ 0x004414D0 size 0x66. Unbox this+F. Native.ptr==0 → silent ret.
  // Else [handle+0x54]=val; thiscall sub_491F80 (not hosted). Twin of
  // setMinLen [+0x58]; no derived write (unlike setRestLen [+0x5C]).
  if (self) WR(self).max_len = val;
}

void java_game_parts_WheelRef_setInstantCenter(InvObject* self, float Hx, float Hy,
                                              float Hz, float Lx, float Ly,
                                              float Lz) {
  // PE @ 0x00441A80 size 0xD4. Unbox this+FFFFFF. Native.ptr==0 → silent
  // ret. Else H'/L' = args + wheel pos [handle+0x78/+0x7C/+0x80] → store
  // [+0x27C..+0x290]. Host: WR.ic[] = args + WR.px/py/pz.
  if (!self) return;
  auto& w = WR(self);
  float* ic = w.ic;
  ic[0] = Hx + w.px;
  ic[1] = Hy + w.py;
  ic[2] = Hz + w.pz;
  ic[3] = Lx + w.px;
  ic[4] = Ly + w.py;
  ic[5] = Lz + w.pz;
}

void java_game_parts_WheelRef_setBrake(InvObject* self, float val) {
  // PE @ 0x00441580 size 0x7b. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else:
  //   mov [handle+0xD0] (208) = val  — primary brake dword
  //   factor from fld [handle+0x164] (356) vs flt_5F08F0 (bytes 00 00 80 3F
  //   = 1.0): if <1 → 1−x*x*flt_5F0F00 (9A 99 19 3F ≈0.6); else
  //   flt_5F0EFC (D0 CC CC 3D ≈0.1). Then [handle+0xD8]=factor*val
  //   (same constants as setFriction @ 0x00440F90, but factor slot +0x164
  //   not +0x15C; only one derived store). No fmul on the primary store
  //   (unlike setWidth @ 0x004416C0). getBrake @ 0x00441540 flds same
  //   slot [+0xD0]. NOT setHBrake / setFriction [+0x1F0] / setSteer
  //   [+0xC8] / setDrive [+0xCC]. Host WR.brake = val (PE dword at +0xD0).
  //   !self = handle 0. Derived +0xD8 not in WheelRefState.
  if (self) WR(self).brake = val;
}

void java_game_parts_WheelRef_setHBrake(InvObject* self, float val) {
  if (self) WR(self).hbrake = val;
}

// VA 0x00441540 / 0x00441600

float java_game_parts_WheelRef_getBrake(InvObject* self) {
  return self ? WR(self).brake : 0.f;
}

float java_game_parts_WheelRef_getHBrake(InvObject* self) {
  // PE @ 0x00441600 size 0x35. Unbox this. Native.ptr (dword_62E008)==0 →
  // fld flt_5E73CC (bytes 00 00 00 00 = 0.0). NO Mighty ERROR. Else
  // fld dword [handle+0xD4] (212) — primary store of setHBrake @ 0x00441640
  // (derived store +0xDC unread). Twin of getBrake @ 0x00441540 which
  // flds [+0xD0] (208). Host WR.hbrake; !self = handle 0.
  return self ? WR(self).hbrake : 0.f;
}

// VA 0x00441A10 — opposite wheel index on axle (-1 clears).

void java_game_parts_WheelRef_setOppWheel(InvObject* self, int32_t id) {
  if (!self) return;
  WR(self).opp_wheel = id;
  tree_field_set_int(self, "opp_wheel", id);
}

void java_game_parts_WheelRef_setArm(InvObject* self, float len, float px, float py,
                                    float pz, float nx, float ny, float nz) {
  // PE @ 0x00441770 size 0x107. Unbox this+7F. handle==0 → silent.
  // Stores: +0x234=len; +0x244..+0x24C = p + pos(+0x78..+0x80) with Y
  // bias −(+0x38++0x3C) unknown on host → use pos only; +0x238..+0x240 =
  // n̂ if ‖n‖≠0 else leave (host zeros then writes). Same pos slots as setPos.
  if (!self) return;
  auto& w = WR(self);
  w.arm[0] = len;
  const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (nlen != 0.f) {
    const float inv = 1.f / nlen;
    w.arm[4] = nx * inv;
    w.arm[5] = ny * inv;
    w.arm[6] = nz * inv;
  } else {
    w.arm[4] = nx;
    w.arm[5] = ny;
    w.arm[6] = nz;
  }
  w.arm[1] = px + w.px;
  w.arm[2] = py + w.py;  // PE also −(+0x38++0x3C); not in WheelRefState
  w.arm[3] = pz + w.pz;
  w.has_arm = true;
}

void java_game_parts_WheelRef_setHub(InvObject* self, float len, float p1x, float p1y,
                                    float p1z, float p2x, float p2y, float p2z,
                                    float pcx, float pcy, float pcz) {
  if (!self) return;
  auto& w = WR(self);
  w.hub[0] = len;
  w.hub[1] = p1x;
  w.hub[2] = p1y;
  w.hub[3] = p1z;
  w.hub[4] = p2x;
  w.hub[5] = p2y;
  w.hub[6] = p2z;
  w.hub[7] = pcx;
  w.hub[8] = pcy;
  w.hub[9] = pcz;
  w.has_hub = true;
}

}  // namespace inv
