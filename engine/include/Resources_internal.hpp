#pragma once
#include "natives.hpp"
#include "host_objects.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace inv {

struct LineVert {
  float x = 0, y = 0, z = 0;
  float nx = 0, ny = 1, nz = 0;
  int32_t color = 0;
  float width = 1.f;
};
struct LineState {
  bool active = false;
  InvObject* parent = nullptr;
  int32_t type_id = 0;
  int32_t color = 0;
  float sx = 0.01f, sy = 0.f, sz = 0.01f;
  std::vector<LineVert> verts;
  float progress = 0.f;
  float target_len = 1.f;
  std::vector<LineVert> weld_pts;
};

struct ResState {
  int32_t id = 0;
  int32_t type = 0;
  int32_t parent_id = 0;
  InvObject* parent = nullptr;
  InvObject* first_child = nullptr;
  InvObject* next_child = nullptr;
  bool loaded = false;
  float sx = 1, sy = 1, sz = 1;
  // render/physics pose
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  // PhysicsRef body (Phase 2.18)
  float vx = 0, vy = 0, vz = 0;
  float wx = 0, wy = 0, wz = 0;  // ang vel
  float hx = 0, hy = 0, hz = 0;  // box half-extents / sphere radius in hx
  int32_t shape = 0;             // 0 none, 1 box, 2 sphere
  int32_t is_static = 0;
  int32_t asleep = 0;
  int32_t pose_set = 0;  // PhysicsRef.setMatrix always writes pose (null → origin)
  int32_t collide = 0;  // Phase 2.25 — setActiveCollision
  // Phase 2.93 — last drive tick: wheels off support.
  int32_t airborne = 0;
  // Phase 2.33 — arcade gearbox (-1=R, 0=N, 1..5).
  int32_t gear = 1;
  float gear_axis_prev = 0.f;
  // Phase 2.66 — WheelRef aggregate (steer / drive / radius) for arcade drive.
  bool has_wheel_params = false;
  float wheel_steer = 0.f;
  float wheel_drive = 1.f;
  float wheel_radius = 0.32f;
  // Phase 2.67 — contact-ish: friction/sliction → grip; brake/hbrake; roll drag.
  float wheel_friction = 1.f;
  float wheel_sliction = 1.f;
  float wheel_brake = 0.f;    // 0..1 effective
  float wheel_hbrake = 0.f;   // 0..1 effective
  float wheel_roll_res = 0.f;
  // Phase 2.68 — Pacejka B/C/D (Wheel.java idx 4/2/0); stock defaults.
  float wheel_pk_b = 15.2f;
  float wheel_pk_c = 1.49f;
  float wheel_pk_d = 1.4f;
  // Phase 2.69 — spring/shock + arm length (suspension).
  float wheel_spring = 0.f;     // N/m
  float wheel_damp = 0.f;       // N/(m/s) bound
  float wheel_rest_len = 0.39f; // m (Spring.java default)
  float wheel_arm_len = 0.244f; // m (stock wishbone)
  // Phase 2.81 — DynoData/Chassis torque (Nm) + estimated engine RPM.
  float drive_torque_nm = 0.f;  // 0 = legacy accel (no torque scale)
  float engine_rpm = 900.f;
  int32_t color = 0;
  int32_t type_id = 0;
  // Phase 2.105 — RenderRef.setLight / setFlare / changeResource.
  int32_t light_diffuse = 0;
  int32_t light_ambient = 0;
  int32_t light_specular = 0;
  int32_t flare_color = 0;
  float flare_min = 0.f;
  float flare_max = 0.f;
  int32_t flare_count = 0;
  int32_t flare_rays = 0;
  int32_t flare_tex_id = 0;
  int32_t swapped_tex_id = 0;
  InvObject* swapped_tex = nullptr;
  int32_t cached = 0;
  int32_t flags = 0;  // inner+0x54 (GameRef_getFlags / ground_precache 0x80000)
  // PE RenderRef_SetBoneMatrixParentLink @ 0x0048BF50 — bone_ref parent link.
  // Host mirrors: parent ptr, bone id, payload flags |= 0x1800, child list.
  // PE payload+0x178 (v8[94]) = list HEAD (insert-at-head @ 0x48C094);
  // sentinel at payload+0x168 (v8+90); bone+0x0C/+0x10 = next/prev.
  InvObject* bone_link_parent = nullptr;
  int32_t bone_link_id = 0;
  int32_t bone_link_flags = 0;       // PE payload+0xBC (v8[47]) |= 0x1800
  InvObject* bone_child_head = nullptr;  // PE payload+0x178 (v8[94]) list head
  InvObject* bone_sib_next = nullptr;    // PE bone+0x0C (→&payload+0x168 when last)
  InvObject* bone_sib_prev = nullptr;    // PE bone+0x10
  int32_t bone_pose_set = 0;           // PE bone+0xF0 = 1 after matrix write
  int32_t bone_flag_bits = 0;          // PE bone+0x3C identity bit2
  int32_t bone_stamp = 0;              // PE payload+0xF8 ← dword_6200A4
  std::string entry_path;
  std::string alias;
  uint32_t blob_size = 0;
};

extern std::mutex g_mu;
extern std::unordered_map<InvObject*, ResState> g_res;
extern std::unordered_map<InvObject*, LineState> g_lines;
extern int32_t g_next_id;
ResState& R(InvObject* self);

}  // namespace inv
