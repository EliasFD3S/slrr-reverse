#include "natives.hpp"
#include "Resources.h"
#include "Resources_internal.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"
#include "tree_interp.hpp"
#include "audio_win32.hpp"
#include "GameRef.h"
#include "../Parts/world_state.hpp"
#include <mutex>
#include <cmath>
#include <cstring>
#include <cstdio>
namespace inv {

namespace {

// Soft Valocity / arcade_body seed after PE create* (+156|=1 stand-in is
// shape≠0 — do not OR into ResState.flags: that DWORD is GameRef +0x54).
// PE createBox@0x4805B0 / createSphere@0x4806F0 → Physics_createPrimitive
// @0x49AB60 then PhysicsHandle_bindPhysinst@0x48A1E0 →
// Phys_allocBodyFromPrimitive@0x49B300 (fresh 536B body, vel slots 0).
void phys_arcade_body_seed(ResState& r) {
  r.asleep = 0;
  // PE mass=0 sets body+0x9C |=4 (inf mass) — Soft is_static stays 0
  // (setStatic Soft-only gate; Valocity cars must remain movable).
  r.is_static = 0;
  r.airborne = 0;
  r.collide = 0;
  r.vx = r.vy = r.vz = 0;
  r.wx = r.wy = r.wz = 0;
  r.gear = arcade_gear_clamp(1);
  r.gear_axis_prev = 0.f;
  r.has_wheel_params = false;
  r.wheel_steer = 0.f;
  r.wheel_drive = 1.f;
  r.wheel_radius = kArcadeDefaultRadius;
  r.wheel_friction = 1.f;
  r.wheel_sliction = 1.f;
  r.wheel_brake = 0.f;
  r.wheel_hbrake = 0.f;
  r.wheel_roll_res = 0.f;
  r.wheel_pk_b = kArcadePkB;
  r.wheel_pk_c = kArcadePkC;
  r.wheel_pk_d = kArcadePkD;
  r.wheel_spring = 0.f;
  r.wheel_damp = 0.f;
  r.wheel_rest_len = kArcadeDefaultRestLen;
  r.wheel_arm_len = kArcadeDefaultArmLen;
  r.drive_torque_nm = 0.f;
  r.engine_rpm = 900.f;
  // PE create*/createBox write identity ori + zero pos before bind.
  r.px = r.py = r.pz = 0.f;
  r.oy = r.op = r.or_ = 0.f;
  r.pose_set = 1;
}

// PE fresh primitive body: linear+ang vel zero (createPrimitive ctor zeros +
// Phys_allocBodyFromPrimitive@0x49B300). Re-assert after shape bind so a
// recreate on a live ResState cannot keep prior arcade_drive vx/wx.
void phys_arcade_body_zero_vel(ResState& r) {
  r.vx = r.vy = r.vz = 0.f;
  r.wx = r.wy = r.wz = 0.f;
  r.asleep = 0;
  r.is_static = 0;
}

}  // namespace

void java_util_resource_PhysicsRef_create(InvObject* self, InvObject* parent,
                                          int32_t typeRid, InvObject*) {
  // PE @ 0x004807F0 size 0x12c: Unbox this+parent+typeRid+alias.
  // Handle 0 OR parent==0 → Mighty ERROR ("!" @ 0x6131EC + "Mighty ERROR"
  // @ 0x6131F0). Host: !self ret; !parent still creates (smokes/TREE pass
  // nullptr; PE would log Mighty). typeRid: ResHandle_Relink → sub_536820;
  // sub_49A990; body+0x9C |= 1; PhysicsHandle_bindPhysinst@0x48A1E0 →
  // INSTANCE_PHYSICS=2.
  // Not createBox/createSphere (Physics_createPrimitive @ 0x0049AB60).
  if (!self) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.parent = parent;
    r.parent_id = parent ? R(parent).id : 0;
    r.type = 2;  // INSTANCE_PHYSICS
    r.type_id = typeRid;
    r.id = g_next_id++;
    r.shape = 0;
    phys_arcade_body_seed(r);
  }
  gameref_on_res_bound(self);
}
void java_util_resource_PhysicsRef_createBox(InvObject* self, InvObject* parent,
                                             float x, float y, float z,
                                             InvObject* alias) {
  // PE @ 0x004805B0 size 0x132 (306). Unbox this/parent/x/y/z/alias.
  // Handle via dword_62E008; handle==0 OR parent==null → Mighty ERROR
  // (void). Else identity ori + zero pos; Physics_createPrimitive @
  // 0x0049AB60 type=5 with half-extents (x*flt_5F09D0=0.5,…) — Soft
  // arcade_box_half_extent; *(body+0x9C)|=1 then
  // PhysicsHandle_bindPhysinst @ 0x0048A1E0 → INSTANCE_PHYSICS.
  // Contrast createSphere @ 0x004806F0: type=1 radius RAW (no ×0.5).
  // Host: create() typeRid=1; keep parent-null (probes); alias unused.
  // Soft: arcade_body seed + explicit zero vel after shape (PE fresh body).
  (void)alias;
  if (!self) return;
  java_util_resource_PhysicsRef_create(self, parent, 1, nullptr);
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.shape = 1;  // Soft stand-in PE body+0x9C |= 1
  r.hx = arcade_box_half_extent(x);
  r.hy = arcade_box_half_extent(y);
  r.hz = arcade_box_half_extent(z);
  phys_arcade_body_zero_vel(r);
}
void java_util_resource_PhysicsRef_createSphere(InvObject* self, InvObject* parent,
                                                float radius, InvObject* alias) {
  // PE @ 0x004806F0 size 0xFF (255). Unbox this/parent/r/alias. Handle 0 OR
  // parent 0 → Mighty ERROR. Physics_createPrimitive type=1, radius RAW
  // (no flt_5F09D0 ×0.5). Contrast createBox @ 0x004805B0 type=5
  // half-extents. *(body+0x9C)|=1 then PhysicsHandle_bindPhysinst @
  // 0x0048A1E0. Host: create() typeRid=2 Soft marker (not primitive type 1).
  // Keep parent-null (probes). Soft: zero vel after shape like createBox.
  (void)alias;
  if (!self) return;
  java_util_resource_PhysicsRef_create(self, parent, 2, nullptr);
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.shape = 2;  // Soft stand-in PE body+0x9C |= 1
  r.hx = radius;
  r.hy = r.hz = 0;
  phys_arcade_body_zero_vel(r);
}
void java_util_resource_PhysicsRef_setMatrix(InvObject* self, InvObject* pos,
                                             InvObject* ori) {
  // PE @ 0x00480920 size 0x1DC (476): Unbox this+Vector3+Ypr. Handle 0 →
  // Mighty ERROR. Null Vector3 → pos 0,0,0 (stores before jz).
  // Vec3_store(0,0,0) zeros YPR; Ypr fields overwrite if non-null.
  // Veh_ensureSceneBound @ 0x0048AEA0; pos into both ping-pong slots at
  // base+212*index+100; Ypr_toMatrix @ 0x54ECD0; sub_54FF20;
  // sub_4986F0 (AABB only — no vel wipe). No asleep here.
  // Null,null = origin pose; PE keeps prior body vel.
  // Soft: zero vel only on null,null (smoke origin reset) or is_static.
  // Non-null pose writes preserve vx/wx so Valocity ensure/rebind does
  // not kill arcade_drive mid-tick.
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  if (pos) vec3_get(pos, &x, &y, &z);
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  if (ori) ypr_get(ori, &yaw, &pitch, &roll);
  const bool origin_reset = (pos == nullptr && ori == nullptr);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.px = x;
    r.py = y;
    r.pz = z;
    r.oy = yaw;
    r.op = pitch;
    r.or_ = roll;
    r.pose_set = 1;
    if (origin_reset || r.is_static) {
      r.vx = r.vy = r.vz = 0;
      r.wx = r.wy = r.wz = 0;
    }
  }
  render_d3d9_mesh_set_transform(self, x, y, z, yaw, pitch, roll, 1.f, 1.f, 1.f);
}
void java_util_resource_PhysicsRef_setStatic(InvObject* self, int32_t mode) {
  // Soft host-only — NOT in stock Natives_RegisterAll @ 0x00487F20.
  // IDA PhysicsRef block @ 0x00489CA2..0x00489D42 registers only create /
  // createBox / createSphere / setMatrix / getPos / getOri (0 setStatic
  // string hits). Java PhysicsRef.java still declares native setStatic.
  // Soft arcade_drive/integrate gate: is_static blocks motion
  // (arcade_motion_allowed). mode≠0 → freeze vel; mode==0 → wake for
  // Valocity re-enter after garage park (setStatic(1) @ return).
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.is_static = mode ? 1 : 0;
  if (r.is_static) {
    r.vx = r.vy = r.vz = 0;
    r.wx = r.wy = r.wz = 0;
  } else {
    r.asleep = 0;
  }
}
InvObject* java_util_resource_PhysicsRef_getPos(InvObject* self) {
  // PE @ 0x00480B00 size 0x110 (272): Unbox this only. Handle via
  // dword_62E008. handle==0 → Mighty ERROR, return nullptr. No
  // *(handle+8) skip (unlike GameRef.getPos @ 0x0047DAD0). Always alloc
  // Vector3 0x1C when handle≠0: Veh_ensureSceneBound @ 0x0048AEA0; xyz
  // from *(body+132)+212*slot+100. Fallback reads unboxed this as xyz.
  // Host: !self → nullptr. Existing object (even unposed / pose_set==0)
  // → Vector3 from cached px/py/pz (zeros if never setMatrix).
  if (!self) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  return vec3_new(r.px, r.py, r.pz);
}
InvObject* java_util_resource_PhysicsRef_getVel(InvObject* self) {
  // Soft host-only PhysicsRef.getVel (Java native; not in
  // Natives_RegisterAll). Soft parallel GameRef channel.
  // PE @ 0x0047DCE0 Soft related (GameRef.getVel); sibling getPos
  // PE @ 0x00480B00 / getOri PE @ 0x00480C10. Static → zeros (setStatic).
  if (!self) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  if (r.is_static) return vec3_new(0.f, 0.f, 0.f);
  return vec3_new(r.vx, r.vy, r.vz);
}
InvObject* java_util_resource_PhysicsRef_getVel_1(InvObject* self, InvObject* pos) {
  // Soft host-only getVel(V) point velocity: v + ω × (p − origin).
  // PE @ 0x0047DCE0 Soft related (GameRef.getVel); getPos PE @ 0x00480B00.
  // Static / no-motion → zero (arcade_motion_allowed soft gate).
  if (!self || !pos) return nullptr;
  float px = 0.f, py = 0.f, pz = 0.f;
  vec3_get(pos, &px, &py, &pz);
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  if (!arcade_motion_allowed(r.shape, r.is_static != 0, r.asleep != 0))
    return vec3_new(0.f, 0.f, 0.f);
  const float rx = px - r.px, ry = py - r.py, rz = pz - r.pz;
  const float vx = r.vx + (r.wy * rz - r.wz * ry);
  const float vy = r.vy + (r.wz * rx - r.wx * rz);
  const float vz = r.vz + (r.wx * ry - r.wy * rx);
  return vec3_new(vx, vy, vz);
}
InvObject* java_util_resource_PhysicsRef_getAngVel(InvObject* self) {
  // Soft host-only getAngVel (Java native; not in Natives_RegisterAll).
  // Soft parallel getVel / getPos layout.
  // PE @ 0x00480B00 Soft related (PhysicsRef.getPos); getOri PE @ 0x00480C10.
  if (!self) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  if (r.is_static) return vec3_new(0.f, 0.f, 0.f);
  return vec3_new(r.wx, r.wy, r.wz);
}
InvObject* java_util_resource_PhysicsRef_getOri(InvObject* self) {
  // PE @ 0x00480C10: Unbox this only. Handle 0 → Mighty ERROR + nullptr.
  // No handle+8 skip. Always alloc Ypr 0x1C when handle≠0.
  // Veh_ensureSceneBound @ 0x0048AEA0; if body: Ypr_fromMatrix @
  // 0x00551C90 (matrix at +164). If helper==0, PE still news Ypr from
  // uninit stack. Host: !self → nullptr. Else Ypr from cached oy/op/or_
  // (zeros if unposed).
  if (!self) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  return ypr_new(r.oy, r.op, r.or_);
}

}  // namespace inv
