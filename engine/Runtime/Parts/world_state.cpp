#include "world_state.hpp"

#include <cstring>

namespace inv {
namespace world_state {

// PE defaults mirrored by WheelRef ctor / Chassis_forceUpdate_apply
// Engine_memcpy 68B chassis+0x2E50 → wheel+0x1E8 (call @ 0x44852e).
// Host Soft table = Wheel.java setPacejka — NO invent raw +0x2E50 blob.
const float kDefaultPacejka[kPacejkaCount] = {
    kArcadePkD, 0.f, kArcadePkC, 0.f, kArcadePkB, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f,
    8000.f, kArcadePkFrictnX, 0.015f, 0.4f, 0.f, 0.f};

std::unordered_map<InvObject*, WheelRefState> g_wheelrefs;
std::unordered_map<InvObject*, std::array<std::string, 4>> g_wheel_dmg;
std::unordered_map<InvObject*, std::vector<SfxItem>> g_sfxtables;
std::unordered_map<InvObject*, DynoState> g_dyno;
std::unordered_map<InvObject*, std::vector<BuckEntry>> g_bucks;
std::unordered_map<InvObject*, std::unordered_map<int32_t, std::string>> g_slot_dmg;
std::unordered_map<InvObject*, AnimState> g_anims;
std::unordered_map<InvObject*, ParticleState> g_particles;
std::unordered_map<InvObject*, std::vector<PaintStroke>> g_painter;

WheelRefState& WR(InvObject* self) { return g_wheelrefs[self]; }
AnimState& AN(InvObject* self) { return g_anims[self]; }
ParticleState& PS(InvObject* self) { return g_particles[self]; }

void anim_advance(AnimState& a) {
  const float now = time_current();
  if (a.last_t < 0.f) a.last_t = now;
  const float dt = now - a.last_t;
  a.last_t = now;
  if (!a.playing || dt <= 0.f) return;
  a.pos += dt * a.speed;
  if (a.duration < 0.01f) a.duration = 1.f;
  if (a.loop) {
    while (a.pos >= a.duration) a.pos -= a.duration;
    while (a.pos < 0.f) a.pos += a.duration;
  } else if (a.pos >= a.duration) {
    a.pos = a.duration;
    a.playing = false;
  } else if (a.pos < 0.f) {
    a.pos = 0.f;
    a.playing = false;
  }
}

std::string alias_key(InvObject* alias) {
  const char* s = alias ? string_cstr(alias) : nullptr;
  return s ? std::string(s) : std::string();
}

// PE setFriction @ 0x00440F90 / setBrake @ 0x00441580 / setHBrake @
// 0x00441640: if x >= 1.0 → 0.1 else 1 − x*x*0.6 (flt_5F0EFC / flt_5F0F00).
float wheel_wear_scale(float x) {
  if (x >= kPeWearOne) return kPeWearFloor;
  return kPeWearOne - x * x * kPeWearQuad;
}

void wheel_seed_default_pacejka(float dst[kPacejkaCount]) {
  if (!dst) return;
  std::memcpy(dst, kDefaultPacejka, sizeof(kDefaultPacejka));
}

// Soft ctor defaults aligned to Wheel.java + host radius/width Soft PE.
void wheel_seed_default_state(WheelRefState& w) {
  wheel_seed_default_pacejka(w.pacejka);
  wheel_sync_named_from_pacejka(w);
  w.radius = kArcadeDefaultRadius;
  w.width = kArcadeDefaultWidthHalf;
  w.drive = 1.f;
  w.rest_plus_r = w.radius + w.rest_len;
}

// PE Chassis_forceUpdate_apply @ 0x4484f9: zero [87..92] (+0x15C..+0x170)
// then Engine_memcpy 68B pacejka @ +0x1E8 from chassis+0x2E50.
// Soft: seed kDefaultPacejka stand-in; do NOT clear brake/hbrake/drive.
void wheel_force_update_apply_scratch(WheelRefState& w, WheelPhysExtra& e) {
  e.wear15c = 0.f;
  e.scratch160 = 0.f;
  e.scratch164 = 0.f;
  e.scratch168 = 0.f;
  e.scratch16c = 0.f;
  e.scratch170 = 0.f;
  wheel_seed_default_pacejka(w.pacejka);
  wheel_sync_named_from_pacejka(w);
}

// PE Chassis_physWheelTick refresh @ 0x455df4..0x455e7c.
// Re-derives +0xD8/+0xDC from primary × wear_scale(+0x164).
// wear>=1 (clamped): ×kPePhysTickBrakeHiScale; optional bearing += 100.
void wheel_phys_tick_refresh_derived(WheelRefState& w, WheelPhysExtra& e,
                                     bool accumulate_bearing) {
  if (e.scratch164 >= kPeWearOne) e.scratch164 = kPeWearOne;
  const float k = wheel_wear_scale(e.scratch164);
  e.brake_d8 = k * w.brake;
  e.hbrake_dc = k * w.hbrake;
  if (e.scratch164 >= kPeWearOne) {
    if (accumulate_bearing) w.bearing += kPePhysTickBearingBump;
    e.brake_d8 *= kPePhysTickBrakeHiScale;
    e.hbrake_dc *= kPePhysTickBrakeHiScale;
  }
}

// PE Chassis_setWheelDamage @ 0x43d4b0: [+0xF0] = f9 * flt_5F0C40 + radius.
float wheel_dmg_f0_from_wear(float f9, float radius) {
  return f9 * kPeDmgF0Scale + radius;
}

float chassis_engine_mass_to_phys(float eng_m) {
  return eng_m * kPeEngineMassScale;
}

float chassis_rpm_idle_to_rad(float rpm_idle) {
  return rpm_idle * kPeRpmToRad;
}

float chassis_mass_or_fallback(float mass) {
  return mass > 0.f ? mass : kPeChassisMassFallback;
}

// PE setDrive @ 0x00440E00: [+0xCC] and [+0xE0] = val.
void wheel_apply_drive(WheelRefState& w, WheelPhysExtra& e, float val) {
  w.drive = val;
  e.drive_e0 = val;
}

// PE setRadius @ 0x00440ED0: [+0x60]=val; Soft [+0x5C]=val+[+0x50].
void wheel_apply_radius(WheelRefState& w, float val) {
  w.radius = val;
  w.rest_plus_r = val + w.rest_len;
}

// PE setRestLen @ 0x004413E0: [+0x50]=val; Soft [+0x5C]=radius+val
// (WheelContact_pushSlots OOS — host only stores the derived float).
void wheel_apply_rest_len(WheelRefState& w, float val) {
  w.rest_len = val;
  w.rest_plus_r = w.radius + val;
}

// PE setDamping(F) @ 0x004412E0: [+0x48]=val; [+0x4C]=val*flt_5F0B0C.
void wheel_apply_damping_f(WheelRefState& w, float val) {
  w.damping = val;
  w.damp_bound = val;
  w.damp_rebound = val * kPeDampReboundScale;
}

// PE setDamping(FF) @ 0x00441360: [+0x48]=bound; [+0x4C]=rebound (no ×0.7).
void wheel_apply_damping_ff(WheelRefState& w, float bound, float rebound) {
  w.damping = bound;
  w.damp_bound = bound;
  w.damp_rebound = rebound;
}

// PE setForce @ 0x00441270: [+0x44]=val; WheelContact_pushSlots OOS.
void wheel_apply_force(WheelRefState& w, float val) { w.force = val; }

// PE setMinLen @ 0x00441460: [+0x58]=val; passes (min+radius) to WheelContact_pushSlots OOS.
void wheel_apply_min_len(WheelRefState& w, float val) { w.min_len = val; }

// PE setMaxLen @ 0x004414D0: [+0x54]=val; passes (max+radius) to WheelContact_pushSlots OOS.
void wheel_apply_max_len(WheelRefState& w, float val) { w.max_len = val; }

// PE setWidth @ 0x004416C0: [+0x1D4] = val * flt_5F09D0 (half).
void wheel_apply_width(WheelRefState& w, float val) {
  w.width = val * kPeHalf;
}

// PE setFriction @ 0x00440F90: +0x1F0/+0x64/+0x68; wear from +0x15C → +0x6C/+0x70.
void wheel_apply_friction(WheelRefState& w, WheelPhysExtra& e, float val) {
  w.friction = val;
  e.frict64 = val;
  e.frict68 = val;
  const float k = wheel_wear_scale(e.wear15c);
  e.frict6c = k * val;
  e.frict70 = k * val;
}

// PE setBrake @ 0x00441580: +0xD0=val; +0xD8 = scale(+0x164)*val.
void wheel_apply_brake(WheelRefState& w, WheelPhysExtra& e, float val) {
  w.brake = val;
  e.brake_d8 = wheel_wear_scale(e.scratch164) * val;
}

// PE setHBrake @ 0x00441640: +0xD4=val; +0xDC = scale(+0x164)*val.
void wheel_apply_hbrake(WheelRefState& w, WheelPhysExtra& e, float val) {
  w.hbrake = val;
  e.hbrake_dc = wheel_wear_scale(e.scratch164) * val;
}

// PE aliases: pacejka[0]=sliction +0x1E8, [2]=friction +0x1F0,
// [4]=stiffness +0x1F8, [12]=frictn_x +0x218 (setPacejka @ 0x00441210).
void wheel_sync_named_from_pacejka(WheelRefState& w) {
  w.sliction = w.pacejka[0];
  w.friction = w.pacejka[2];
  w.stiffness = w.pacejka[4];
  w.frictn_x = w.pacejka[12];
}

void wheel_sync_pacejka_from_named(WheelRefState& w) {
  w.pacejka[0] = w.sliction;
  w.pacejka[2] = w.friction;
  w.pacejka[4] = w.stiffness;
  w.pacejka[12] = w.frictn_x;
}

// Soft PE arcade aggregate from WheelRefState (ResState 2.66–2.69 mirror).
// Prefer named slots; pacejka B/C/D = idx 4/2/0 (Wheel.java / stock defaults).
void arcade_fill_from_wheel(ArcadeWheelAggregate& out, const WheelRefState& w) {
  arcade_fill_from_wheel_ex(out, w, nullptr);
}

void arcade_fill_from_wheel_ex(ArcadeWheelAggregate& out, const WheelRefState& w,
                               const WheelPhysExtra* extra) {
  out.has = true;
  out.steer = w.steer;
  out.drive = w.drive;
  out.radius = (w.radius > 0.05f) ? w.radius : kArcadeDefaultRadius;
  out.width_half =
      (w.width > 0.f) ? w.width : kArcadeDefaultWidthHalf;  // PE +0x1D4 half
  out.friction = (w.friction > 0.f) ? w.friction : kArcadePkC;
  out.sliction = (w.sliction > 0.f) ? w.sliction : kArcadePkD;
  out.frictn_x = (w.frictn_x > 0.f) ? w.frictn_x : kArcadePkFrictnX;
  out.brake = w.brake;
  out.hbrake = w.hbrake;
  out.roll_res = w.roll_res;
  // Pacejka B/C/D = idx 4/2/0 (Wheel.java / setPacejka aliases).
  out.pk_b = (w.pacejka[4] > 0.f) ? w.pacejka[4] : kArcadePkB;
  out.pk_c = (w.pacejka[2] > 0.f) ? w.pacejka[2] : kArcadePkC;
  out.pk_d = (w.pacejka[0] > 0.f) ? w.pacejka[0] : kArcadePkD;
  out.spring = w.force;
  out.damp = w.damp_bound;
  out.damp_rebound = w.damp_rebound;
  out.rest_len =
      (w.rest_len > 0.f) ? w.rest_len : kArcadeDefaultRestLen;
  out.rest_plus_r =
      (w.rest_plus_r > 0.f) ? w.rest_plus_r : (out.radius + out.rest_len);
  out.min_len = w.min_len;
  out.max_len = w.max_len;
  out.arm_len = w.has_arm ? w.arm[0] : kArcadeDefaultArmLen;
  // Soft PE setFriction @ 0x00440F90: +0x6C/+0x70 = wear_scale(+0x15C)*val.
  // Soft PE setBrake @ 0x00441580 / setHBrake @ 0x00441640: +0xD8/+0xDC.
  if (extra) {
    out.friction_wear = wheel_wear_scale(extra->wear15c) * out.friction;
    out.brake_wear = extra->brake_d8;
    out.hbrake_wear = extra->hbrake_dc;
  } else {
    out.friction_wear = out.friction;
    out.brake_wear = out.brake;
    out.hbrake_wear = out.hbrake;
  }
  // Soft: setCPatch needs *[handle+0x30]; a8=radius gates contact+0x40
  // (setRadius @ 0x440ed0 → WheelContact_pushSlots).
  const bool radius_slot = arcade_contact_slot_active(out.radius);
  if (extra) {
    out.contact_gate = extra->contact30 && radius_slot;
  } else {
    out.contact_gate = radius_slot;  // no Extra → +0x30 assumed live
  }
}

void arcade_body_reset(ArcadeBodyAggregate& body) {
  body = ArcadeBodyAggregate{};
}

void arcade_body_add_wheel(ArcadeBodyAggregate& body,
                           const ArcadeWheelAggregate& w, bool front_steer) {
  arcade_body_add_wheel_ex(body, w, front_steer, nullptr);
}

void arcade_body_add_wheel_ex(ArcadeBodyAggregate& body,
                              const ArcadeWheelAggregate& w, bool front_steer,
                              const WheelPhysExtra* extra) {
  if (!w.has) return;
  body.has = true;
  ++body.n;
  if (front_steer) {
    body.sum_steer += w.steer;
    ++body.n_steer;
  }
  body.sum_drive += w.drive;
  if (w.radius > 0.05f) {
    body.sum_radius += w.radius;
    ++body.n_radius;
  }
  body.sum_friction += w.friction;
  body.sum_friction_wear += w.friction_wear;
  body.sum_sliction += w.sliction;
  body.sum_frictn_x += w.frictn_x;
  body.sum_brake += w.brake;
  body.sum_hbrake += w.hbrake;
  body.sum_brake_wear += w.brake_wear;
  body.sum_hbrake_wear += w.hbrake_wear;
  body.sum_roll_res += w.roll_res;
  body.sum_pk_b += w.pk_b;
  body.sum_pk_c += w.pk_c;
  body.sum_pk_d += w.pk_d;
  body.sum_spring += w.spring;
  body.sum_damp += w.damp;
  body.sum_damp_rebound += w.damp_rebound;
  body.sum_rest += w.rest_len;
  body.sum_rest_plus_r +=
      (w.rest_plus_r > 0.f) ? w.rest_plus_r : (w.radius + w.rest_len);
  body.sum_min_len += w.min_len;
  body.sum_max_len += w.max_len;
  body.sum_width += w.width_half;
  if (w.arm_len > 0.05f) {
    body.sum_arm += w.arm_len;
    ++body.n_arm;
  }
  // PE setCPatch @ 0x00441700: *[handle+0x30]==0 → silent.
  // Soft × WheelContact_pushSlots a8(=radius)>0 → contact+0x40 (setRadius).
  bool gate = w.contact_gate;
  if (extra) {
    gate = extra->contact30 && arcade_contact_slot_active(w.radius);
  }
  if (gate) ++body.n_contact;
}

void arcade_body_finalize(ArcadeBodyAggregate& body) {
  if (body.n <= 0) return;
  const float inv = 1.f / static_cast<float>(body.n);
  body.steer =
      body.n_steer ? (body.sum_steer / static_cast<float>(body.n_steer)) : 0.f;
  body.drive = arcade_drive_mul_clamp(body.sum_drive * inv);
  body.radius = body.n_radius
                    ? (body.sum_radius / static_cast<float>(body.n_radius))
                    : kArcadeDefaultRadius;
  body.friction = body.sum_friction * inv;
  if (body.friction <= 0.f) body.friction = kArcadePkC;
  body.friction_wear = body.sum_friction_wear * inv;
  if (body.friction_wear <= 0.f) body.friction_wear = body.friction;
  body.sliction = body.sum_sliction * inv;
  if (body.sliction <= 0.f) body.sliction = kArcadePkD;
  body.frictn_x = body.sum_frictn_x * inv;
  if (body.frictn_x <= 0.f) body.frictn_x = kArcadePkFrictnX;
  // Soft PE: Brake.java torque≈0.18 → arcade 0..1 when / kArcadeBrakeTorqueUnit.
  body.brake = arcade_brake_norm(body.sum_brake * inv);
  body.hbrake = arcade_brake_norm(body.sum_hbrake * inv);
  body.brake_wear = arcade_brake_norm(body.sum_brake_wear * inv);
  body.hbrake_wear = arcade_brake_norm(body.sum_hbrake_wear * inv);
  body.roll_res = body.sum_roll_res * inv;
  body.pk_b = body.sum_pk_b * inv;
  if (body.pk_b <= 0.f) body.pk_b = kArcadePkB;
  body.pk_c = body.sum_pk_c * inv;
  if (body.pk_c <= 0.f) body.pk_c = kArcadePkC;
  body.pk_d = body.sum_pk_d * inv;
  if (body.pk_d <= 0.f) body.pk_d = kArcadePkD;
  body.spring = body.sum_spring * inv;
  body.damp = body.sum_damp * inv;
  body.damp_rebound = body.sum_damp_rebound * inv;
  body.rest_len = body.sum_rest * inv;
  if (body.rest_len <= 0.01f) body.rest_len = kArcadeDefaultRestLen;
  body.rest_plus_r = body.sum_rest_plus_r * inv;
  if (body.rest_plus_r <= 0.01f)
    body.rest_plus_r = body.radius + body.rest_len;
  body.min_len = body.sum_min_len * inv;
  body.max_len = body.sum_max_len * inv;
  body.width_half = body.sum_width * inv;
  body.arm_len = body.n_arm
                     ? (body.sum_arm / static_cast<float>(body.n_arm))
                     : kArcadeDefaultArmLen;
  body.contact = body.n_contact > 0;
  body.contact_frac = static_cast<float>(body.n_contact) /
                      static_cast<float>(body.n);
  // Soft car feel: wear-scaled friction (+0x6C) × sliction × frictn_x × plant.
  body.grip = arcade_drive_grip_ex(body.friction_wear, body.sliction,
                                   body.frictn_x, body.contact_frac,
                                   /*airborne=*/false);
}

float arcade_radius_mul(float radius) {
  float m = radius / kArcadeDefaultRadius;
  if (m < kArcadeRadiusMulMin) m = kArcadeRadiusMulMin;
  if (m > kArcadeRadiusMulMax) m = kArcadeRadiusMulMax;
  return m;
}

float arcade_arm_steer_mul(float arm_len) {
  if (arm_len <= 0.05f) return 1.f;
  float m = kArcadeDefaultArmLen / arm_len;
  if (m < kArcadeArmSteerMulMin) m = kArcadeArmSteerMulMin;
  if (m > kArcadeArmSteerMulMax) m = kArcadeArmSteerMulMax;
  return m;
}

float arcade_pacejka_stiff(float b, float c, float d) {
  if (kArcadePkStock <= 0.f) return 1.f;
  float stiff = (d * c * b) / kArcadePkStock;
  if (stiff < kArcadePkStiffMin) stiff = kArcadePkStiffMin;
  if (stiff > kArcadePkStiffMax) stiff = kArcadePkStiffMax;
  return stiff;
}

float arcade_ride_bias(float rest_len) {
  float b = (rest_len - kArcadeDefaultRestLen) * kArcadeRideBiasScale;
  if (b < kArcadeRideBiasMin) b = kArcadeRideBiasMin;
  if (b > kArcadeRideBiasMax) b = kArcadeRideBiasMax;
  return b;
}

// Soft PE ground/road support (physics_drive post-integrate / physics_integrate).
// support = ground_or_road_y + half + ride_bias; floor at ground+half.
float arcade_support_y(float ground_or_road_y, float half_height,
                       float ride_bias) {
  const float min_y = ground_or_road_y + half_height;
  float y = min_y + ride_bias;
  if (y < min_y) y = min_y;
  return y;
}

// Soft tire plane: ground_y + radius (PE a8→contact+0x40 when radius>0).
float arcade_wheel_support_y(float ground_y, float radius, float ride_bias) {
  const float r = (radius > 0.05f) ? radius : kArcadeDefaultRadius;
  return arcade_support_y(ground_y, r, ride_bias);
}

float arcade_body_support_y(float ground_y, float half_height, float radius,
                            float ride_bias, bool contact) {
  if (contact && arcade_contact_slot_active(radius))
    return arcade_wheel_support_y(ground_y, radius, ride_bias);
  return arcade_support_y(ground_y, half_height, ride_bias);
}

bool arcade_is_airborne(float py, float support_y) {
  return py > support_y + kArcadeAirborneClearance;
}

float arcade_clamp_py_to_support(float py, float support_y) {
  return (py < support_y) ? support_y : py;
}

// Soft PE Physics_Step @ 0x004A5190 (ecx=physWorld, a2=dt).
// Callers: Engine_SimulateFrame @ 0x42859A / Engine_SimulateControlFrame
// @ 0x42892B. MarkStepReset @ 0x4A5180 sets Physics_stepResetPending.
//
// Gate (disasm @ 0x4A5197..0x4A51AE):
//   v3 = a2 + *(world+4); *(world+4)=v3; fst arg;
//   fcomp flt_5F09D4@0x5F09D4 (bytes 17 b7 d1 38 ≈ 1e-4);
//   C0 set → return 0 (leave residue).
// After take @ 0x4A51FD: *(world+4) -= v22 (Soft clear → residue≡0).
// Soft `dt` = taken-step candidate with residue already folded/cleared
// (System SimulateFrame Soft) → gate ≡ !(dt < kArcadeIntegrateDtMin).
//
// OOS body (no Soft dllist / solver): MarkStepReset wipe +0x118 nodes
// (+0xA0/+0xA4/+0xA8) @ 0x4A51BF; bodyApplyForces@0x4A3F90; cull+0x9C;
// TickDriveList@0x426F60; bodyIntegrate@0x4A3BC0; cull+0xB8;
// solver≤20 (prep@0x4A3900/resolve@0x4A2D10; iter19 drain/flush/dispatch);
// expireDeferred@0x4A2B90; bodyCommitSlot@0x4A4ED0; clock+=v22 @ 0x4A548E;
// solverIterEma @ 0x4A54A6.
bool arcade_integrate_dt_ok(float dt) {
  // Soft PE fcomp C0: pass when !(accum < eps). Soft residue≡0 → accum=dt.
  return !(dt < kArcadeIntegrateDtMin);
}

// Soft PE motion gate for physics_drive / physics_integrate early-out.
// Soft stand-in for Physics_Step body-list walks (+0x84 apply/integrate,
// +0x9C cull, +0xB8 live) — not PE vtbl+12/+24. Asleep Soft from
// GameRef suspend/wakeup voidEvent strcmp @ 0x459E75 / 0x459ED1.
bool arcade_motion_allowed(int32_t shape, bool is_static, bool asleep) {
  return shape != 0 && !is_static && !asleep;
}

// PE GameRef_queueActiveCollision @ 0x00498810: *(inner+0x54) & 0x10000000.
bool arcade_collide_queued(uint32_t flags54) {
  return (flags54 & kArcadeCollideQueuedBit) != 0u;
}

// PE PhysicsRef.createBox @ 0x004805B0: *[prim+156] |= 1.
bool arcade_phys_created(uint32_t prim_flags156) {
  return (prim_flags156 & kArcadePhysCreateFlagBit) != 0u;
}

// PE createBox @ 0x480648..0x48066a: half = full * flt_5F09D0.
float arcade_box_half_extent(float full) { return full * kPeHalf; }

int32_t arcade_gear_clamp(int32_t gear) {
  if (gear < kArcadeGearMin) return kArcadeGearMin;
  if (gear > kArcadeGearMax) return kArcadeGearMax;
  return gear;
}

// Soft PE gearbox engage (physics_drive): N→0; else 1−clutch clamped.
float arcade_gear_engage(int32_t gear, float clutch) {
  if (arcade_gear_clamp(gear) == kArcadeGearNeutral) return 0.f;
  float engage = 1.f - clutch;
  if (engage < 0.f) engage = 0.f;
  if (engage > 1.f) engage = 1.f;
  return engage;
}

float arcade_drive_mul_clamp(float drive) {
  if (drive < kArcadeDriveMulMin) return kArcadeDriveMulMin;
  if (drive > kArcadeDriveMulMax) return kArcadeDriveMulMax;
  return drive;
}

// Soft physics_drive accel scale: engage × drive × radius_mul.
float arcade_drive_thrust(float drive, float radius, int32_t gear,
                          float clutch) {
  return arcade_gear_engage(gear, clutch) * arcade_drive_mul_clamp(drive) *
         arcade_radius_mul(radius);
}

// Soft Brake.java ~0.18 torque → arcade 0..1 (/ kArcadeBrakeTorqueUnit).
float arcade_brake_norm(float raw_torque) {
  if (kArcadeBrakeTorqueUnit <= 0.f) return 0.f;
  float b = raw_torque / kArcadeBrakeTorqueUnit;
  if (b < 0.f) b = 0.f;
  if (b > 1.f) b = 1.f;
  return b;
}

// Soft physics_drive roll_res → drag extra (*800, clamp 0..8).
float arcade_roll_extra(float roll_res) {
  float e = roll_res * kArcadeRollExtraScale;
  if (e < 0.f) e = 0.f;
  if (e > kArcadeRollExtraMax) e = kArcadeRollExtraMax;
  return e;
}

float arcade_grip_mul(float friction, float sliction) {
  float g = friction * sliction;
  if (g < kArcadeGripMulMin) g = kArcadeGripMulMin;
  if (g > kArcadeGripMulMax) g = kArcadeGripMulMax;
  return g;
}

// Soft × PE setFrictn_x @ 0x441050 (+0x218 / pacejka[12]) when >0.
float arcade_grip_mul_ex(float friction, float sliction, float frictn_x) {
  float g = arcade_grip_mul(friction, sliction);
  if (frictn_x > 0.f) g *= frictn_x;
  if (g < kArcadeGripMulMin) g = kArcadeGripMulMin;
  if (g > kArcadeGripMulMax) g = kArcadeGripMulMax;
  return g;
}

// Soft drive grip: setFriction wear path (+0x6C) × sliction; plant gates.
float arcade_drive_grip(float friction_wear, float sliction, bool contact,
                        bool airborne) {
  if (!contact) return 0.f;
  float g = arcade_grip_mul(friction_wear, sliction);
  if (airborne) g *= kArcadeAirborneGripMul;
  return g;
}

// Soft: wear×sliction×frictn_x × contact_frac; airborne grip cut.
float arcade_drive_grip_ex(float friction_wear, float sliction, float frictn_x,
                           float contact_frac, bool airborne) {
  if (contact_frac <= 0.f) return 0.f;
  float g = arcade_grip_mul_ex(friction_wear, sliction, frictn_x);
  g *= contact_frac;
  if (airborne) g *= kArcadeAirborneGripMul;
  return g;
}

// Soft PE WheelContact_pushSlots @ 0x00491F80: a8<=0 → contact+0x40=0; else =a8.
// Callers setRadius @ 0x440ed0 / setRestLen pass a8=radius (+0x60).
bool arcade_contact_slot_active(float radius) { return radius > 0.f; }

// Soft PE WheelContact_pushSlots: a8<=0 → +0x50=a4(rest_plus_r); else +0x50=a8.
float arcade_contact_rest_slot(float rest_plus_r, float radius) {
  if (radius <= 0.f) return rest_plus_r;
  return radius;
}

// Soft PE WheelContact_pushSlots: a7<=0 → a3*flt_5F0B0C else a7 → contact+0x4C.
float arcade_contact_rebound(float damp_bound, float damp_rebound) {
  if (damp_rebound <= 0.f) return damp_bound * kPeDampReboundScale;
  return damp_rebound;
}

// Soft PE WheelContact_pushSlots: a5<=0 → a4 else a5 → contact+0x38 (max).
float arcade_contact_max_len(float rest_plus_r, float max_plus_r) {
  if (max_plus_r <= 0.f) return rest_plus_r;
  return max_plus_r;
}

// Soft PE WheelContact_pushSlots: a6<=0 → 0 else a6 → contact+0x3C (min).
float arcade_contact_min_len(float min_plus_r) {
  if (min_plus_r <= 0.f) return 0.f;
  return min_plus_r;
}

float arcade_len_plus_radius(float len, float radius) {
  return len + radius;
}

// Soft pack of WheelContact_pushSlots outputs (setRadius arg order @ 0x440f3e).
void arcade_contact_push_fill(ArcadeContactSlots& out, float force, float damp,
                              float rest_plus_r, float max_plus_r,
                              float min_plus_r, float damp_rebound,
                              float radius) {
  out.force = force;
  out.damp = damp;
  out.rest = rest_plus_r;
  out.max_len = arcade_contact_max_len(rest_plus_r, max_plus_r);
  out.min_len = arcade_contact_min_len(min_plus_r);
  out.rebound = arcade_contact_rebound(damp, damp_rebound);
  out.active = arcade_contact_slot_active(radius);
  out.radius_slot = arcade_contact_rest_slot(rest_plus_r, radius);
  out.contact40 = out.active ? radius : 0.f;
}

void arcade_contact_push_from_wheel(ArcadeContactSlots& out,
                                    const WheelRefState& w) {
  const float r = w.radius;
  const float rest_pr =
      (w.rest_plus_r > 0.f) ? w.rest_plus_r : (w.radius + w.rest_len);
  arcade_contact_push_fill(out, w.force, w.damp_bound, rest_pr,
                           arcade_len_plus_radius(w.max_len, r),
                           arcade_len_plus_radius(w.min_len, r), w.damp_rebound,
                           r);
}

}  // namespace world_state
}  // namespace inv
