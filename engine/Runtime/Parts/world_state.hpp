// Shared host state for runtime/Parts (+ Cars Painter, Resources anim/particles).
// W35-19: Soft PE arcade_body/drive — ground_y, contact gates, wear→friction.
// Soft PE deepen: phys defaults aligned WheelRef/Chassis (pacejka, physTick,
// forceUpdate mass/rpm, dmg f0) — host Soft only, no stock solver.
// OOS stock phys: Physics_Step @ 0x4A5190; WheelContact_pushSlots @ 0x00491F80
// (contact* slot push only — Chassis_physWheelTick caller, not hosted as solver).
#pragma once

#include "natives.hpp"
#include "runtime.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {
namespace world_state {

// PE Chassis.getWheel @ 0x00440C80: wheel* = *[veh+0x13E4] + id*0x2B4.
// forceUpdate_apply @ 0x00448430: scratch zero +0x15C..+0x170 then
// Engine_memcpy 68B pacejka from chassis+0x2E50 → wheel+0x1E8.
constexpr int32_t kWheelPhysStride = 0x2B4;       // 692
constexpr int32_t kChassisWheelTableOff = 0x13E4;  // 5092
constexpr int32_t kChassisPacejkaSrcOff = 0x2E50;  // 11856
constexpr int32_t kPacejkaBaseOff = 0x1E8;         // 488
constexpr int32_t kPacejkaCount = 17;
constexpr int32_t kPacejkaBytes = 68;  // 17*4; memcpy size @ forceUpdate

// PE float consts (IDA bytes @ .rdata): setWidth / setDamping(F) / wear.
constexpr float kPeHalf = 0.5f;              // flt_5F09D0 @ 0x005F09D0 (00 00 00 3F)
constexpr float kPeDampReboundScale = 0.7f;  // flt_5F0B0C @ 0x005F0B0C (33 33 33 3F)
constexpr float kPeWearOne = 1.0f;           // flt_5F08F0 @ 0x005F08F0 (00 00 80 3F)
constexpr float kPeWearQuad = 0.6f;          // flt_5F0F00 @ 0x005F0F00 (9A 99 19 3F)
constexpr float kPeWearFloor = 0.1f;         // flt_5F0EFC @ 0x005F0EFC (D0 CC CC 3D)
// PE Chassis_physWheelTick @ 0x455e5e / 0x455e4c / 0x455bf9 (get_bytes).
constexpr float kPePhysTickBrakeHiScale = 0.2f;  // flt_5F0CD4 @ 0x005F0CD4
constexpr float kPePhysTickBearingBump = 100.f;  // float_100_0 @ 0x005F09C8
constexpr float kPePhysTickOmegaEps = 0.001f;    // flt_5F09CC @ 0x005F09CC
// PE Chassis_setWheelDamage @ 0x43d4b0: [+0xF0] = f9 * this + rest[+0x2C].
constexpr float kPeDmgF0Scale = 200.f;           // flt_5F0C40 @ 0x005F0C40
// PE Chassis_forceUpdate_apply @ 0x4486cb / 0x4486b6.
constexpr float kPeEngineMassScale = 8000.f;     // flt_5F0FBC @ 0x005F0FBC
constexpr float kPeRpmToRad = 0.10471976f;       // flt_5F0EF0 @ 0x005F0EF0 (π/30)
// Soft host mass fallback (Chassis.getMass miss → 0.0 on PE; CarInfo stand-in).
constexpr float kPeChassisMassFallback = 1200.f;

// Soft PE arcade / body-integrate defaults (Java + host ResState 2.66–2.69).
// Not stock phys solver — formulas mirrored by physics_drive / physics_integrate.
constexpr float kArcadeDefaultRadius = 0.32f;    // WheelRef +0x60 ctor Soft
constexpr float kArcadeDefaultWidth = 0.45f;     // setWidth arg (Tyre mm→m typ.)
constexpr float kArcadeDefaultWidthHalf = 0.225f;  // PE +0x1D4 stores ×kPeHalf
constexpr float kArcadeDefaultRestLen = 0.39f;   // Spring.java restlength
constexpr float kArcadeDefaultArmLen = 0.244f;   // stock_suspension_*.setArm
constexpr float kArcadePkB = 15.2f;              // Wheel.java setPacejka(4)
constexpr float kArcadePkC = 1.49f;              // Wheel.java setPacejka(2)
constexpr float kArcadePkD = 1.4f;               // pacejka[0] / sliction default
constexpr float kArcadePkFrictnX = 1.f;          // pacejka[12] / setFrictn_x
constexpr float kArcadePkStock = kArcadePkD * kArcadePkC * kArcadePkB;  // stiff norm
constexpr float kArcadeBrakeTorqueUnit = 0.2f;   // Brake.java ~0.18 → arcade 0..1
                                                    // (= kPePhysTickBrakeHiScale)
constexpr float kArcadeRideBiasScale = 0.5f;     // Soft ride: (rest−0.39)*0.5
// Soft PE ride / airborne / grip clamps (host physics_drive residual).
constexpr float kArcadeRideBiasMin = -0.15f;
constexpr float kArcadeRideBiasMax = 0.25f;
constexpr float kArcadeAirborneClearance = 0.45f;  // py > support+this → air
// Soft residual (physics_drive airborne path) — not a stock PE const.
constexpr float kArcadeAirborneGripMul = 0.08f;
constexpr float kArcadeAirborneSteerMul = 0.08f;  // Soft steer cut when air
constexpr float kArcadeAirborneArmMul = 0.2f;     // Soft arm_steer cut when air
constexpr float kArcadeRadiusMulMin = 0.25f;
constexpr float kArcadeRadiusMulMax = 2.5f;
constexpr float kArcadeArmSteerMulMin = 0.5f;
constexpr float kArcadeArmSteerMulMax = 2.0f;
constexpr float kArcadeDriveMulMin = 0.f;
constexpr float kArcadeDriveMulMax = 2.f;
constexpr float kArcadeGripMulMin = 0.05f;
constexpr float kArcadeGripMulMax = 3.f;
constexpr float kArcadePkStiffMin = 0.05f;
constexpr float kArcadePkStiffMax = 4.f;
// Soft roll_res → drag extra (physics_drive); not a stock PE const.
constexpr float kArcadeRollExtraScale = 800.f;
constexpr float kArcadeRollExtraMax = 8.f;
// PE Physics_Step @ 0x4A5190: if (accum+dt) < 1e-4 return 0.
constexpr float kArcadeIntegrateDtMin = 1e-4f;
constexpr int32_t kArcadeGearMin = -1;  // R
constexpr int32_t kArcadeGearMax = 5;
constexpr int32_t kArcadeGearNeutral = 0;
// PE GameRef_queueActiveCollision @ 0x00498810: inner+0x54 |= 0x10000000
// (same dword as GameRef.getFlags @ 0x0047DF40). Host collide gate stand-in.
constexpr uint32_t kArcadeCollideQueuedBit = 0x10000000u;
// PE PhysicsRef.createBox @ 0x004805B0: *[prim+156] |= 1 after createPrimitive.
constexpr uint32_t kArcadePhysCreateFlagBit = 1u;

// PE wheel-handle offsets (Native.ptr → phys slot). Soft host mirrors.
namespace wheel_off {
constexpr int32_t contact_gate = 0x30;   // setCPatch gate
constexpr int32_t force = 0x44;          // setForce
constexpr int32_t damp_bound = 0x48;     // setDamping
constexpr int32_t damp_rebound = 0x4C;
constexpr int32_t rest_len = 0x50;       // setRestLen
constexpr int32_t max_len = 0x54;        // setMaxLen
constexpr int32_t min_len = 0x58;        // setMinLen
constexpr int32_t rest_plus_r = 0x5C;    // setRestLen derived (= radius+rest)
constexpr int32_t radius = 0x60;         // setRadius / getRadius
constexpr int32_t frict64 = 0x64;        // setFriction twins
constexpr int32_t frict68 = 0x68;
constexpr int32_t frict6c = 0x6C;        // wear-scaled
constexpr int32_t frict70 = 0x70;
constexpr int32_t roll_res = 0x74;       // setRollRes
constexpr int32_t pos_x = 0x78;          // setPos / setArm / setHub / IC
constexpr int32_t pos_y = 0x7C;
constexpr int32_t pos_z = 0x80;
constexpr int32_t mat90 = 0x90;          // setYpr → Ypr_toMatrix
constexpr int32_t flags100 = 0x100;
constexpr int32_t mat110 = 0x110;        // qmemcpy ← +0x90
constexpr int32_t bearing = 0xF0;        // setBearing
constexpr int32_t dmg_f0 = 0xF0;         // also damage path (Chassis)
constexpr int32_t dmg_f4 = 0xF4;
constexpr int32_t dmg_f8 = 0xF8;
constexpr int32_t steer = 0xC8;          // setSteer / getSteer
constexpr int32_t drive = 0xCC;          // setDrive / getDrive
constexpr int32_t brake = 0xD0;          // setBrake primary / getBrake
constexpr int32_t hbrake = 0xD4;         // setHBrake primary / getHBrake
constexpr int32_t brake_d8 = 0xD8;       // setBrake derived
constexpr int32_t hbrake_dc = 0xDC;      // setHBrake derived
constexpr int32_t drive_e0 = 0xE0;       // setDrive twin
constexpr int32_t wear15c = 0x15C;       // forceUpdate scratch / setFriction
constexpr int32_t scratch160 = 0x160;
constexpr int32_t scratch164 = 0x164;    // setBrake/setHBrake factor
constexpr int32_t scratch168 = 0x168;
constexpr int32_t scratch16c = 0x16C;
constexpr int32_t scratch170 = 0x170;
constexpr int32_t width = 0x1D4;         // setWidth (stores half)
constexpr int32_t sliction = 0x1E8;      // setSliction == pacejka[0]
constexpr int32_t friction = 0x1F0;      // setFriction == pacejka[2]
constexpr int32_t stiffness = 0x1F8;     // setStiffness == pacejka[4]
constexpr int32_t max_load = 0x214;      // setMaxLoad
constexpr int32_t frictn_x = 0x218;      // setFrictn_x == pacejka[12]
constexpr int32_t load_smooth = 0x220;   // setLoadSmooth
constexpr int32_t veh_back = 0x22C;      // chassis owner*
constexpr int32_t arm0 = 0x234;          // setArm block
constexpr int32_t opp_wheel = 0x250;     // setOppWheel → opp phys*
constexpr int32_t hub0 = 0x254;          // setHub block
constexpr int32_t ic0 = 0x27C;           // setInstantCenter H'
constexpr int32_t ic3 = 0x288;           // setInstantCenter L'
}  // namespace wheel_off

// PE WheelRef ctor / forceUpdate memcpy defaults @ pacejka[+0x1E8].
// Source chassis+0x2E50 (68B); host Soft PE table.
extern const float kDefaultPacejka[kPacejkaCount];

struct WheelRefState {
  // PE +0x78/+0x7C/+0x80 (setPos @ 0x00441D10)
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  bool has_pos = false;
  bool has_ypr = false;
  float drive = 1.f;       // PE +0xCC (setDrive @ 0x00440E00); twin +0xE0 in Extra
  float steer = 0;         // PE +0xC8
  float radius = kArcadeDefaultRadius;  // PE +0x60 Soft ctor
  float width = kArcadeDefaultWidthHalf;  // PE +0x1D4 stores half (setWidth ×0.5)
  float cpatch_hw = 0, cpatch_ang = 0, cpatch_off = 0;  // inner+0x84..
  // Named aliases aligned to Wheel.java setPacejka / kDefaultPacejka.
  float friction = kArcadePkC;     // PE +0x1F0 (== pacejka[2])
  float frictn_x = kArcadePkFrictnX;  // PE +0x218 (== pacejka[12])
  float sliction = kArcadePkD;     // PE +0x1E8 (== pacejka[0])
  float stiffness = kArcadePkB;    // PE +0x1F8 (== pacejka[4])
  float roll_res = 0;      // PE +0x74
  float bearing = 0;       // PE +0xF0
  float max_load = 0;      // PE +0x214
  float load_smooth = 0;   // PE +0x220
  // PE setPacejka @ 0x00441210: 17 slots [handle+0x1E8+4*i], i=0..16.
  // Same table as kDefaultPacejka / forceUpdate memcpy chassis+0x2E50 stand-in.
  float pacejka[17] = {kArcadePkD, 0.f, kArcadePkC, 0.f, kArcadePkB, 0.f, 0.f,
                       0.f, -1.f, 0.f, 0.f, 8000.f, kArcadePkFrictnX, 0.015f,
                       0.4f, 0.f, 0.f};
  float force = 0;         // PE +0x44
  float damping = 0;       // host mirror of damp_bound
  float damp_bound = 0;    // PE +0x48
  float damp_rebound = 0;  // PE +0x4C (setDamping(F) ×0.7)
  float rest_len = 0;      // PE +0x50 (unset; arcade → kArcadeDefaultRestLen)
  float rest_plus_r = 0;   // PE +0x5C (= radius+rest_len Soft PE)
  float min_len = 0;       // PE +0x58
  float max_len = 0;       // PE +0x54
  float ic[6] = {};        // PE +0x27C..+0x290 (setInstantCenter)
  float brake = 0;         // PE +0xD0 primary
  float hbrake = 0;        // PE +0xD4 primary
  int32_t opp_wheel = -1;  // Java id; PE +0x250 is opp phys*
  float arm[7] = {};       // PE +0x234.. (setArm @ 0x00441770)
  float hub[10] = {};      // PE +0x254.. (setHub @ 0x00441880)
  bool has_arm = false;
  bool has_hub = false;
};

// Soft PE side-band for wheel* fields not folded into WheelRefState.
// Live maps stay in WheelRef.cpp (g_chassis_phys_extra) until migrated.
struct WheelPhysExtra {
  uint32_t flags100 = 0;       // PE +0x100
  float dmg_f4 = 0.f;          // PE +0xF4
  float dmg_f8 = 0.f;          // PE +0xF8
  float dmg_f0 = 0.f;          // PE +0xF0 (damage path; overlaps bearing)
  uintptr_t opp_ptr = 0;       // PE +0x250
  InvObject* veh = nullptr;    // PE +0x22C
  float wear15c = 0.f;         // PE +0x15C — setFriction factor
  float scratch160 = 0.f;      // PE +0x160
  float scratch164 = 0.f;      // PE +0x164 — setBrake/setHBrake factor
  float scratch168 = 0.f;      // PE +0x168
  float scratch16c = 0.f;      // PE +0x16C
  float scratch170 = 0.f;      // PE +0x170
  float brake_d8 = 0.f;        // PE +0xD8 derived
  float hbrake_dc = 0.f;       // PE +0xDC derived
  float drive_e0 = 1.f;        // PE +0xE0 setDrive twin (ctor Soft = drive)
  float mat90[12] = {};        // PE +0x90
  float mat110[12] = {};       // PE +0x110
  bool has_mat90 = false;
  float frict64 = 0.f;         // PE +0x64
  float frict68 = 0.f;         // PE +0x68
  float frict6c = 0.f;         // PE +0x6C
  float frict70 = 0.f;         // PE +0x70
  bool contact30 = false;      // PE +0x30 gate stand-in
};

// Soft PE arcade drive aggregate (ResState Phase 2.66–2.69 mirror).
// Filled from WheelRefState; not stock phys solver.
struct ArcadeWheelAggregate {
  bool has = false;
  float steer = 0.f;
  float drive = 1.f;
  float radius = kArcadeDefaultRadius;
  float width_half = 0.f;  // PE +0x1D4 (setWidth stores ×0.5; Soft half default
                           // = kArcadeDefaultWidthHalf when seeded)
  float friction = kArcadePkC;    // named / pacejka[2]
  float friction_wear = kArcadePkC;  // Soft: wear-scaled (+0x6C) when Extra given
  float sliction = kArcadePkD;
  float frictn_x = kArcadePkFrictnX;  // PE +0x218 / pacejka[12] (setFrictn_x @ 0x441050)
  float brake = 0.f;       // PE +0xD0 primary (getBrake)
  float hbrake = 0.f;      // PE +0xD4 primary (getHBrake)
  // Soft: setBrake/setHBrake derived +0xD8/+0xDC (scratch164 wear) when Extra.
  float brake_wear = 0.f;
  float hbrake_wear = 0.f;
  float roll_res = 0.f;
  float pk_b = kArcadePkB;  // pacejka[4] / stiffness alias
  float pk_c = kArcadePkC;  // pacejka[2] / friction alias
  float pk_d = kArcadePkD;  // pacejka[0] / sliction alias
  float spring = 0.f;       // force +0x44 (N/m Soft PE)
  float damp = 0.f;         // damp_bound +0x48
  float damp_rebound = 0.f; // +0x4C
  float rest_len = kArcadeDefaultRestLen;
  float rest_plus_r = 0.f;  // Soft +0x5C (= radius+rest)
  float min_len = 0.f;      // +0x58
  float max_len = 0.f;      // +0x54
  float arm_len = kArcadeDefaultArmLen;
  // Soft: PE setCPatch +0x30 AND WheelContact_pushSlots a8(=radius)>0 → +0x40.
  bool contact_gate = true;
};

// Soft PE WheelContact_pushSlots @ 0x00491F80 result (contact* slots).
// this = *[wheel+0x30] (setCPatch); not stock solver — slot pack only.
struct ArcadeContactSlots {
  float force = 0.f;       // contact+0x44 ← a2 (wheel+0x44)
  float damp = 0.f;        // contact+0x48 ← a3 (wheel+0x48)
  float rest = 0.f;        // contact+0x34 ← a4 (wheel+0x5C rest+r)
  float max_len = 0.f;     // contact+0x38 ← a5 or a4 if a5<=0
  float min_len = 0.f;     // contact+0x3C ← a6 or 0 if a6<=0
  float rebound = 0.f;     // contact+0x4C ← a7 or a3*0.7 if a7<=0
  float radius_slot = 0.f; // contact+0x50 ← a8 if a8>0 else a4
  float contact40 = 0.f;   // contact+0x40 ← a8 if a8>0 else 0
  bool active = false;     // a8(=radius) > 0
};

// Soft PE body-level average (GameRef Phase 2.66–2.72 → physics_set_wheel_*).
// Accumulators before finalize; not stock Chassis phys blob.
struct ArcadeBodyAggregate {
  bool has = false;
  int32_t n = 0;
  int32_t n_steer = 0;
  int32_t n_arm = 0;
  int32_t n_radius = 0;
  int32_t n_contact = 0;  // wheels gated +0x30 × a8 (setCPatch / WheelContact_pushSlots)
  float sum_steer = 0.f;
  float sum_drive = 0.f;
  float sum_radius = 0.f;
  float sum_friction = 0.f;
  float sum_friction_wear = 0.f;  // Soft +0x6C wear-scaled
  float sum_sliction = 0.f;
  float sum_frictn_x = 0.f;       // PE +0x218 / pacejka[12]
  float sum_brake = 0.f;
  float sum_hbrake = 0.f;
  float sum_brake_wear = 0.f;     // Soft +0xD8 wear-scaled
  float sum_hbrake_wear = 0.f;    // Soft +0xDC wear-scaled
  float sum_roll_res = 0.f;
  float sum_pk_b = 0.f;
  float sum_pk_c = 0.f;
  float sum_pk_d = 0.f;
  float sum_spring = 0.f;
  float sum_damp = 0.f;
  float sum_damp_rebound = 0.f;   // PE +0x4C
  float sum_rest = 0.f;
  float sum_rest_plus_r = 0.f;    // Soft +0x5C (= radius+rest)
  float sum_min_len = 0.f;        // +0x58
  float sum_max_len = 0.f;        // +0x54
  float sum_width = 0.f;          // +0x1D4 half-width
  float sum_arm = 0.f;
  // After arcade_body_finalize:
  float steer = 0.f;
  float drive = 1.f;
  float radius = kArcadeDefaultRadius;
  float friction = kArcadePkC;
  float friction_wear = kArcadePkC;
  float sliction = kArcadePkD;
  float frictn_x = kArcadePkFrictnX;
  float brake = 0.f;        // 0..1 primary / kArcadeBrakeTorqueUnit
  float hbrake = 0.f;       // 0..1 primary
  float brake_wear = 0.f;   // 0..1 Soft +0xD8 path
  float hbrake_wear = 0.f;  // 0..1 Soft +0xDC path
  float roll_res = 0.f;
  float pk_b = kArcadePkB;
  float pk_c = kArcadePkC;
  float pk_d = kArcadePkD;
  float spring = 0.f;
  float damp = 0.f;
  float damp_rebound = 0.f;
  float rest_len = kArcadeDefaultRestLen;
  float rest_plus_r = 0.f;
  float min_len = 0.f;
  float max_len = 0.f;
  float width_half = 0.f;
  float arm_len = kArcadeDefaultArmLen;
  bool contact = false;     // n_contact > 0 (setCPatch × radius a8 Soft)
  float contact_frac = 0.f; // n_contact / n (partial plant Soft)
  // Soft: wear→friction × sliction × frictn_x × contact_frac.
  float grip = 1.f;
};

struct SfxItem {
  InvObject* sfx = nullptr;
  float pitch = 0, pmin = 0, pmax = 0, vmin = 0, vmax = 0;
};

struct DynoState {
  std::vector<float> nm;
  float max_rpm = 7000.f;
  int32_t steps = 0;
};

struct BuckEntry {
  int32_t part_id = 0, buck_id = 0;
  float freq = 0, prob = 0, rpmdep = 0, amp = 0;
};

// PE Animation native @ 0x0047ED80..0x0047F050: 8-byte queue {op, arg}.
// 0=play 1=loopPlay 3=pause 4=seek 5=setSpeed 6=setFade (2 unused in Java).
struct AnimOp {
  int32_t op = 0;
  float arg = 0.f;
};

struct AnimState {
  float speed = 1.f;
  float fade = 0.f;
  float pos = 0.f;
  float duration = 1.f;
  bool playing = false;
  bool loop = false;
  float last_t = -1.f;
  std::vector<AnimOp> queue;
};

struct ParticleAction {
  enum Kind : int32_t { None = 0, Source = 1, Direct = 2, Counter = 3 };
  Kind kind = None;
  float px = 0, py = 0, pz = 0;
  float rmin = 0, rmax = 0;
  float vx = 0, vy = 0, vz = 0;
  float vmin = 0, vmax = 0;
  float rate = 0;
  std::string bone;
  int32_t counter = 0;
};

struct ParticleState {
  InvObject* parent = nullptr;
  InvObject* type = nullptr;
  std::string sys_alias;
  float freq = 0.f;
  bool permanent = false;
  bool stopped = false;
  std::unordered_map<std::string, ParticleAction> actions;
};

struct PaintStroke {
  InvObject* cursor = nullptr;
  int32_t color = 0;
  int32_t brush = 0;
  int32_t temp = 0;
  float rot = 0, size = 1.f;
  int32_t flip = 0;
  bool part_fill = false;
};

extern std::unordered_map<InvObject*, WheelRefState> g_wheelrefs;
extern std::unordered_map<InvObject*, std::array<std::string, 4>> g_wheel_dmg;
extern std::unordered_map<InvObject*, std::vector<SfxItem>> g_sfxtables;
extern std::unordered_map<InvObject*, DynoState> g_dyno;
extern std::unordered_map<InvObject*, std::vector<BuckEntry>> g_bucks;
extern std::unordered_map<InvObject*, std::unordered_map<int32_t, std::string>>
    g_slot_dmg;
extern std::unordered_map<InvObject*, AnimState> g_anims;
extern std::unordered_map<InvObject*, ParticleState> g_particles;
extern std::unordered_map<InvObject*, std::vector<PaintStroke>> g_painter;

WheelRefState& WR(InvObject* self);
AnimState& AN(InvObject* self);
ParticleState& PS(InvObject* self);
void anim_advance(AnimState& a);
std::string alias_key(InvObject* alias);

// Soft PE helpers (VA-backed formulas; no raw *[veh+0x13E4]).
float wheel_wear_scale(float x);  // setFriction/setBrake/setHBrake factor
void wheel_seed_default_pacejka(float dst[kPacejkaCount]);
// Soft ctor: pacejka + named aliases + radius/width/drive (Wheel.java / Soft PE).
void wheel_seed_default_state(WheelRefState& w);
// PE forceUpdate_apply @ 0x4484f9: zero +0x15C..+0x170 then memcpy pacejka 68B.
void wheel_force_update_apply_scratch(WheelRefState& w, WheelPhysExtra& e);
// PE Chassis_physWheelTick refresh @ 0x455df4..0x455e7c (wear→+0xD8/+0xDC).
void wheel_phys_tick_refresh_derived(WheelRefState& w, WheelPhysExtra& e,
                                     bool accumulate_bearing);
// PE setWheelDamage @ 0x43d4b0: [+0xF0] = f9 * kPeDmgF0Scale + radius.
float wheel_dmg_f0_from_wear(float f9, float radius);
// PE forceUpdate_apply: engine_mass * kPeEngineMassScale → phys+0x1E58.
float chassis_engine_mass_to_phys(float eng_m);
// PE forceUpdate_apply: rpm_idle * kPeRpmToRad → hdr+0xEC.
float chassis_rpm_idle_to_rad(float rpm_idle);
// Soft getMass miss stand-in (PE → 0.0).
float chassis_mass_or_fallback(float mass);
void wheel_apply_drive(WheelRefState& w, WheelPhysExtra& e, float val);  // +0xCC/+0xE0
void wheel_apply_radius(WheelRefState& w, float val);  // +0x60 / Soft +0x5C
void wheel_apply_rest_len(WheelRefState& w, float val);  // +0x50 / Soft +0x5C
void wheel_apply_damping_f(WheelRefState& w, float val);  // +0x48 / ×0.7 +0x4C
void wheel_apply_damping_ff(WheelRefState& w, float bound, float rebound);  // +0x48/+0x4C
void wheel_apply_force(WheelRefState& w, float val);       // +0x44; WheelContact_pushSlots OOS
void wheel_apply_min_len(WheelRefState& w, float val);     // +0x58
void wheel_apply_max_len(WheelRefState& w, float val);     // +0x54
void wheel_apply_width(WheelRefState& w, float val);       // +0x1D4 = val×0.5
void wheel_apply_friction(WheelRefState& w, WheelPhysExtra& e, float val);
void wheel_apply_brake(WheelRefState& w, WheelPhysExtra& e, float val);
void wheel_apply_hbrake(WheelRefState& w, WheelPhysExtra& e, float val);
// Named ↔ pacejka alias slots (PE overlap Soft mirror).
void wheel_sync_named_from_pacejka(WheelRefState& w);
void wheel_sync_pacejka_from_named(WheelRefState& w);
void arcade_fill_from_wheel(ArcadeWheelAggregate& out, const WheelRefState& w);
// Wear→friction (+0x6C) + brake/hbrake wear (+0xD8/+0xDC) + contact_gate.
void arcade_fill_from_wheel_ex(ArcadeWheelAggregate& out, const WheelRefState& w,
                               const WheelPhysExtra* extra);
void arcade_body_reset(ArcadeBodyAggregate& body);
void arcade_body_add_wheel(ArcadeBodyAggregate& body,
                           const ArcadeWheelAggregate& w, bool front_steer);
// contact_gate: Extra.contact30 × radius a8 (PE setCPatch / WheelContact_pushSlots).
void arcade_body_add_wheel_ex(ArcadeBodyAggregate& body,
                              const ArcadeWheelAggregate& w, bool front_steer,
                              const WheelPhysExtra* extra);
void arcade_body_finalize(ArcadeBodyAggregate& body);
// Soft PE body-integrate / drive scalars (physics_drive / ride residual).
// Stock Physics_Step @ 0x4A5190 + WheelContact_pushSlots OOS as solver.
float arcade_radius_mul(float radius);
float arcade_arm_steer_mul(float arm_len);
float arcade_pacejka_stiff(float b, float c, float d);
float arcade_ride_bias(float rest_len);
float arcade_support_y(float ground_or_road_y, float half_height,
                       float ride_bias);
// Soft tire support: ground_y + radius (+ ride_bias); floor at ground+radius.
// PE setRadius @ 0x440ed0 a8→contact+0x40 when a8>0 (WheelContact_pushSlots).
float arcade_wheel_support_y(float ground_y, float radius, float ride_bias);
// Body box vs tire: contact+radius→wheel support; else half_height path.
float arcade_body_support_y(float ground_y, float half_height, float radius,
                            float ride_bias, bool contact);
bool arcade_is_airborne(float py, float support_y);
// Soft integrate residual: clamp py up to support (physics_integrate mirror).
float arcade_clamp_py_to_support(float py, float support_y);
// PE Physics_Step @ 0x4A5190: dt/accum gate < 1e-4 → skip step.
bool arcade_integrate_dt_ok(float dt);
// Drive / integrate early-out: shape==0 || static || asleep → blocked.
// Asleep Soft ← GameRef suspend/wakeup @ 0x459E75 / 0x459ED1 (not PE solver).
bool arcade_motion_allowed(int32_t shape, bool is_static, bool asleep);
// Collide Soft ← GameRef.setActiveCollision @ 0x0047DF80 → GameRef_queueActiveCollision.
bool arcade_collide_queued(uint32_t flags54);
// PE createBox @ 0x004805B0: *[prim+156] |= 1 after Physics_createPrimitive.
bool arcade_phys_created(uint32_t prim_flags156);
// PE createBox half-extents: full * flt_5F09D0 (same as setWidth ×0.5).
float arcade_box_half_extent(float full);
int32_t arcade_gear_clamp(int32_t gear);
float arcade_gear_engage(int32_t gear, float clutch);  // N→0; else 1−clutch
float arcade_drive_mul_clamp(float drive);
// Soft drive thrust: engage × drive_mul × radius_mul (physics_drive accel scale).
float arcade_drive_thrust(float drive, float radius, int32_t gear, float clutch);
float arcade_brake_norm(float raw_torque);  // / kArcadeBrakeTorqueUnit → 0..1
float arcade_roll_extra(float roll_res);    // Soft *800 clamp 0..8
float arcade_grip_mul(float friction, float sliction);
// Soft: × frictn_x (PE +0x218 / pacejka[12]) when >0; else plain grip_mul.
float arcade_grip_mul_ex(float friction, float sliction, float frictn_x);
// Soft drive grip: wear-scaled friction × sliction × frictn_x; ×0 if !contact;
// ×kArcadeAirborneGripMul if airborne (physics_drive residual).
float arcade_drive_grip(float friction_wear, float sliction, bool contact,
                        bool airborne);
float arcade_drive_grip_ex(float friction_wear, float sliction, float frictn_x,
                           float contact_frac, bool airborne);
// Soft PE WheelContact_pushSlots @ 0x00491F80 length gates; no solver.
// a8(=radius from setRadius @ 0x440ed0): <=0 → +0x40=0; else +0x40=+0x50=a8.
bool arcade_contact_slot_active(float radius);
float arcade_contact_rest_slot(float rest_plus_r, float radius);
float arcade_contact_rebound(float damp_bound, float damp_rebound);
float arcade_contact_max_len(float rest_plus_r, float max_plus_r);
float arcade_contact_min_len(float min_plus_r);
float arcade_len_plus_radius(float len, float radius);
// Pack Soft contact* slots from wheel fields (setRadius arg order @ 0x440f3e).
void arcade_contact_push_fill(ArcadeContactSlots& out, float force, float damp,
                              float rest_plus_r, float max_plus_r,
                              float min_plus_r, float damp_rebound,
                              float radius);
void arcade_contact_push_from_wheel(ArcadeContactSlots& out,
                                    const WheelRefState& w);

}  // namespace world_state

// Bring symbols into inv:: for native bodies (drop world_state:: prefix).
// world_state::-only (twins still live in WheelRef.cpp / Chassis.cpp until
// those files migrate — avoid inv:: redefinition):
//   kWheelPhysStride, kDefaultPacejka,
//   kPePhysTick*, kPeDmgF0Scale, kPeEngineMassScale, kPeRpmToRad,
//   kPeChassisMassFallback,
//   wheel_seed_default_state, wheel_force_update_apply_scratch,
//   wheel_phys_tick_refresh_derived, wheel_dmg_f0_from_wear,
//   chassis_engine_mass_to_phys, chassis_rpm_idle_to_rad,
//   chassis_mass_or_fallback.
using world_state::kChassisWheelTableOff;
using world_state::kChassisPacejkaSrcOff;
using world_state::kPacejkaBaseOff;
using world_state::kPacejkaCount;
using world_state::kPacejkaBytes;
using world_state::kPeHalf;
using world_state::kPeDampReboundScale;
using world_state::kPeWearOne;
using world_state::kPeWearQuad;
using world_state::kPeWearFloor;
using world_state::kArcadeDefaultRadius;
using world_state::kArcadeDefaultWidth;
using world_state::kArcadeDefaultWidthHalf;
using world_state::kArcadeDefaultRestLen;
using world_state::kArcadeDefaultArmLen;
using world_state::kArcadePkB;
using world_state::kArcadePkC;
using world_state::kArcadePkD;
using world_state::kArcadePkFrictnX;
using world_state::kArcadePkStock;
using world_state::kArcadeBrakeTorqueUnit;
using world_state::kArcadeRideBiasScale;
using world_state::kArcadeRideBiasMin;
using world_state::kArcadeRideBiasMax;
using world_state::kArcadeAirborneClearance;
using world_state::kArcadeAirborneGripMul;
using world_state::kArcadeAirborneSteerMul;
using world_state::kArcadeAirborneArmMul;
using world_state::kArcadeRadiusMulMin;
using world_state::kArcadeRadiusMulMax;
using world_state::kArcadeArmSteerMulMin;
using world_state::kArcadeArmSteerMulMax;
using world_state::kArcadeDriveMulMin;
using world_state::kArcadeDriveMulMax;
using world_state::kArcadeGripMulMin;
using world_state::kArcadeGripMulMax;
using world_state::kArcadePkStiffMin;
using world_state::kArcadePkStiffMax;
using world_state::kArcadeRollExtraScale;
using world_state::kArcadeRollExtraMax;
using world_state::kArcadeIntegrateDtMin;
using world_state::kArcadeGearMin;
using world_state::kArcadeGearMax;
using world_state::kArcadeGearNeutral;
using world_state::kArcadeCollideQueuedBit;
using world_state::kArcadePhysCreateFlagBit;
using world_state::WheelRefState;
using world_state::WheelPhysExtra;
using world_state::ArcadeWheelAggregate;
using world_state::ArcadeContactSlots;
using world_state::ArcadeBodyAggregate;
using world_state::SfxItem;
using world_state::DynoState;
using world_state::BuckEntry;
using world_state::AnimOp;
using world_state::AnimState;
using world_state::ParticleAction;
using world_state::ParticleState;
using world_state::PaintStroke;
using world_state::g_wheelrefs;
using world_state::g_wheel_dmg;
using world_state::g_sfxtables;
using world_state::g_dyno;
using world_state::g_bucks;
using world_state::g_slot_dmg;
using world_state::g_anims;
using world_state::g_particles;
using world_state::g_painter;
using world_state::WR;
using world_state::AN;
using world_state::PS;
using world_state::anim_advance;
using world_state::alias_key;
using world_state::wheel_wear_scale;
using world_state::wheel_seed_default_pacejka;
using world_state::wheel_apply_drive;
using world_state::wheel_apply_radius;
using world_state::wheel_apply_rest_len;
using world_state::wheel_apply_damping_f;
using world_state::wheel_apply_damping_ff;
using world_state::wheel_apply_force;
using world_state::wheel_apply_min_len;
using world_state::wheel_apply_max_len;
using world_state::wheel_apply_width;
using world_state::wheel_apply_friction;
using world_state::wheel_apply_brake;
using world_state::wheel_apply_hbrake;
using world_state::wheel_sync_named_from_pacejka;
using world_state::wheel_sync_pacejka_from_named;
using world_state::arcade_fill_from_wheel;
using world_state::arcade_fill_from_wheel_ex;
using world_state::arcade_body_reset;
using world_state::arcade_body_add_wheel;
using world_state::arcade_body_add_wheel_ex;
using world_state::arcade_body_finalize;
using world_state::arcade_radius_mul;
using world_state::arcade_arm_steer_mul;
using world_state::arcade_pacejka_stiff;
using world_state::arcade_ride_bias;
using world_state::arcade_support_y;
using world_state::arcade_wheel_support_y;
using world_state::arcade_body_support_y;
using world_state::arcade_is_airborne;
using world_state::arcade_clamp_py_to_support;
using world_state::arcade_integrate_dt_ok;
using world_state::arcade_motion_allowed;
using world_state::arcade_collide_queued;
using world_state::arcade_phys_created;
using world_state::arcade_box_half_extent;
using world_state::arcade_gear_clamp;
using world_state::arcade_gear_engage;
using world_state::arcade_drive_mul_clamp;
using world_state::arcade_drive_thrust;
using world_state::arcade_brake_norm;
using world_state::arcade_roll_extra;
using world_state::arcade_grip_mul;
using world_state::arcade_grip_mul_ex;
using world_state::arcade_drive_grip;
using world_state::arcade_drive_grip_ex;
using world_state::arcade_contact_slot_active;
using world_state::arcade_contact_rest_slot;
using world_state::arcade_contact_rebound;
using world_state::arcade_contact_max_len;
using world_state::arcade_contact_min_len;
using world_state::arcade_len_plus_radius;
using world_state::arcade_contact_push_fill;
using world_state::arcade_contact_push_from_wheel;

}  // namespace inv
