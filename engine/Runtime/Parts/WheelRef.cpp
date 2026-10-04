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
#include <cstdint>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "world_state.hpp"

namespace inv {

// Host stand-in for PE phys wheel table (*[veh+0x13E4] + id*0x2B4).
// Always-new WheelRef wrappers share this slot so setters survive getWheel.
// Stride/wear/apply: world_state::kWheelPhysStride / wheel_apply_* (W34-19).
static std::unordered_map<InvObject*, std::array<WheelRefState, 8>>
    g_chassis_phys_wheels;

// Side-band = world_state::WheelPhysExtra (W35-10: drop local twin).
// Live maps stay here; Chassis.cpp only via helpers. Soft PE — no raw
// *[veh+0x13E4]. Phys tick consumer of +0xD8/+0xDC: Chassis_physWheelTick
// @ 0x455c21 (fmul sibling/self derived) + refresh @ 0x455df4
// (wear_scale×primary; wear≥1 → ×flt_physTick_brakeHiScale_0_2).
// Soft table walk: chassis_phys_wheel_table_tick @ PE 0x455ba8..0x455f15.
static std::unordered_map<InvObject*, std::array<WheelPhysExtra, 8>>
    g_chassis_phys_extra;

// PE WheelRef ctor defaults @ pacejka[+0x1E8] — also Chassis_forceUpdate_apply
// Engine_memcpy 68B from chassis+0x2E50 → wheel+0x1E8 (IDA @ 0x005D6780 /
// call @ 0x44852e).
static constexpr float kDefaultPacejka[17] = {
    1.4f, 0.f, 1.49f, 0.f, 15.20f, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 8000.f,
    1.f, 0.015f, 0.4f, 0.f, 0.f};

// PE Chassis_physWheelTick @ 0x455e5e / 0x455e4c (IDA rename + get_bytes).
// flt_physTick_brakeHiScale_0_2 @ 0x005F0CD4 = 0.2; float_100_0 @ 0x005F09C8.
// flt_5F09CC @ 0x005F09CC = 0.001 — gate |wheel[+0xC0]| before brake consumer.
static constexpr float kPePhysTickBrakeHiScale = 0.2f;
static constexpr float kPePhysTickBearingBump = 100.f;
static constexpr float kPePhysTickOmegaEps = 0.001f;

static WheelPhysExtra& extra_slot(InvObject* chassis, int32_t id) {
  static WheelPhysExtra orphan;
  if (!chassis || id < 0 || id >= 8) return orphan;
  return g_chassis_phys_extra[chassis][static_cast<size_t>(id)];
}

namespace {

WheelRefState& WR_eff(InvObject* self) {
  if (!self) {
    static WheelRefState orphan;
    return orphan;
  }
  InvObject* ch = tree_field_get_obj(self, "chassis");
  const int32_t id = tree_field_get_int(self, "wheel_id");
  if (ch && id >= 0 && id < 8)
    return g_chassis_phys_wheels[ch][static_cast<size_t>(id)];
  return WR(self);
}

WheelPhysExtra& extra_eff(InvObject* self) {
  static WheelPhysExtra orphan;
  if (!self) return orphan;
  InvObject* ch = tree_field_get_obj(self, "chassis");
  const int32_t id = tree_field_get_int(self, "wheel_id");
  if (ch && id >= 0 && id < 8)
    return g_chassis_phys_extra[ch][static_cast<size_t>(id)];
  return orphan;
}

// Soft PE setBrake/setHBrake side-map: PE field offs (wheel_off) + id*0x2B4
// LEA into Soft table — NOT a raw *[veh+0x13E4] ptr (OOS). Proven:
// setBrake @ 0x00441580 (+0xD0/+0xD8/+0x164), setHBrake @ 0x00441640
// (+0xD4/+0xDC twin), getWheel/setOppWheel LEA id*0x2B4 (int_convert 692),
// pos[+0x78] shared by setPos/setArm (phys78 on the wheel handle).
void wheel_soft_brake_sidemap(InvObject* self, InvObject* chassis, int32_t id) {
  const int32_t stride = world_state::kWheelPhysStride;  // 0x2B4
  const int32_t lea = (id >= 0) ? (id * stride) : 0;
  auto publish = [&](InvObject* dst) {
    if (!dst) return;
    tree_field_set_int(dst, "phys_wheel_stride", stride);
    tree_field_set_int(dst, "phys_wheels_table_off",
                       world_state::kChassisWheelTableOff);  // PE +0x13E4 off
    tree_field_set_int(dst, "phys_off_brake_d0",
                       world_state::wheel_off::brake);      // 0xD0 = 208
    tree_field_set_int(dst, "phys_off_hbrake_d4",
                       world_state::wheel_off::hbrake);    // 0xD4 = 212
    tree_field_set_int(dst, "phys_off_brake_d8",
                       world_state::wheel_off::brake_d8);   // 0xD8 = 216
    tree_field_set_int(dst, "phys_off_hbrake_dc",
                       world_state::wheel_off::hbrake_dc); // 0xDC = 220
    tree_field_set_int(dst, "phys_off_scratch164",
                       world_state::wheel_off::scratch164);  // 0x164 = 356
    tree_field_set_int(dst, "phys_off_pos_x",
                       world_state::wheel_off::pos_x);  // +0x78
    if (id >= 0) {
      tree_field_set_int(dst, "native_ptr_lea", lea);
      tree_field_set_int(dst, "phys_brake_lea_d0",
                         lea + world_state::wheel_off::brake);
      tree_field_set_int(dst, "phys_brake_lea_d8",
                         lea + world_state::wheel_off::brake_d8);
      tree_field_set_int(dst, "phys_hbrake_lea_d4",
                         lea + world_state::wheel_off::hbrake);
      tree_field_set_int(dst, "phys_hbrake_lea_dc",
                         lea + world_state::wheel_off::hbrake_dc);
      tree_field_set_int(dst, "phys_pos_lea_78",
                         lea + world_state::wheel_off::pos_x);
    }
  };
  publish(self);
  if (chassis && id >= 0 && id < 8) {
    // Soft deepen: chassis-indexed LEA parity for brake AND hbrake
    // (was brake d0/d8 + pos78 only). Still byte offs into Soft table —
    // never a dereferenced *[veh+0x13E4] blob.
    char lk[40], d0[48], d8[48], d4[48], dc[48], p78[40], s164[48];
    std::snprintf(lk, sizeof(lk), "phys_wheel_lea_%d", static_cast<int>(id));
    std::snprintf(d0, sizeof(d0), "phys_brake_lea_d0_%d", static_cast<int>(id));
    std::snprintf(d8, sizeof(d8), "phys_brake_lea_d8_%d", static_cast<int>(id));
    std::snprintf(d4, sizeof(d4), "phys_hbrake_lea_d4_%d", static_cast<int>(id));
    std::snprintf(dc, sizeof(dc), "phys_hbrake_lea_dc_%d", static_cast<int>(id));
    std::snprintf(p78, sizeof(p78), "phys_pos_lea_78_%d", static_cast<int>(id));
    std::snprintf(s164, sizeof(s164), "phys_scratch_lea_164_%d",
                  static_cast<int>(id));
    tree_field_set_int(chassis, "phys_wheel_stride", stride);
    tree_field_set_int(chassis, "phys_wheels_table_off",
                       world_state::kChassisWheelTableOff);
    tree_field_set_int(chassis, lk, lea);
    tree_field_set_int(chassis, d0, lea + world_state::wheel_off::brake);
    tree_field_set_int(chassis, d8, lea + world_state::wheel_off::brake_d8);
    tree_field_set_int(chassis, d4, lea + world_state::wheel_off::hbrake);
    tree_field_set_int(chassis, dc, lea + world_state::wheel_off::hbrake_dc);
    tree_field_set_int(chassis, p78, lea + world_state::wheel_off::pos_x);
    tree_field_set_int(chassis, s164,
                       lea + world_state::wheel_off::scratch164);
  }
}

// Soft PE: publish PE layout slots Java setters land in (same offsets the
// phys tick reads). No raw *[veh+0x13E4] blob — TREE + Extra only.
void wheel_mirror_brake_tree(InvObject* self, InvObject* chassis, int32_t id,
                             const WheelRefState& w, const WheelPhysExtra& e) {
  if (self) {
    tree_field_set_float(self, "brake_d0", w.brake);          // PE +0xD0
    tree_field_set_float(self, "hbrake_d4", w.hbrake);        // PE +0xD4
    tree_field_set_float(self, "brake_d8", e.brake_d8);       // PE +0xD8
    tree_field_set_float(self, "hbrake_dc", e.hbrake_dc);     // PE +0xDC
    tree_field_set_float(self, "brake_scale_k",
                         wheel_wear_scale(e.scratch164));
    tree_field_set_float(self, "scratch164", e.scratch164);   // PE +0x164
    tree_field_set_float(self, "bearing_f0", w.bearing);      // PE +0xF0
    tree_field_set_int(self, "contact30", e.contact30 ? 1 : 0);  // PE +0x30
  }
  wheel_soft_brake_sidemap(self, chassis, id);
  if (!chassis || id < 0 || id >= 8) return;
  char bk[40], hk[40], pk[40], hk0[40], sk[40], brk[40], ck[40];
  std::snprintf(bk, sizeof(bk), "phys_tick_brake_d8_%d", static_cast<int>(id));
  std::snprintf(hk, sizeof(hk), "phys_tick_hbrake_dc_%d", static_cast<int>(id));
  std::snprintf(pk, sizeof(pk), "phys_wheel_brake_d0_%d", static_cast<int>(id));
  std::snprintf(hk0, sizeof(hk0), "phys_wheel_hbrake_d4_%d",
                static_cast<int>(id));
  std::snprintf(sk, sizeof(sk), "phys_wheel_scratch164_%d", static_cast<int>(id));
  std::snprintf(brk, sizeof(brk), "phys_wheel_bearing_f0_%d",
                static_cast<int>(id));
  std::snprintf(ck, sizeof(ck), "phys_wheel_contact30_%d",
                static_cast<int>(id));
  tree_field_set_float(chassis, bk, e.brake_d8);
  tree_field_set_float(chassis, hk, e.hbrake_dc);
  tree_field_set_float(chassis, pk, w.brake);
  tree_field_set_float(chassis, hk0, w.hbrake);
  tree_field_set_float(chassis, sk, e.scratch164);
  tree_field_set_float(chassis, brk, w.bearing);
  tree_field_set_int(chassis, ck, e.contact30 ? 1 : 0);
}

// Soft PE stand-in for 0x455c12..0x455c47 (disasm confirmed):
//   fld [self+0xFC]; fadd chassis+0x1DB8; fmul sibling[+0xD8]; fmul self[+0xD8]
//   fld sibling[+0xDC]; fmul chassis+0x1DC0; fmul self[+0xDC]; faddp
//   fadd self[+0xF0] → brake_torque_sum.
// PE sibling_phys = *[arg_veh+0x1FBC] (hdr Soft OOS — do NOT invent blob;
// not setOppWheel +0x250). Soft deepen: prefer TREE phys_sibling_brake_d8/dc
// when phys_sibling_set (Chassis may seed hdr stand-in); else setOppWheel
// Extra derived. Core pair = sibling×self +0xD8 / +0xDC. Scales via TREE
// only (defaults 0 — no invented Chassis / *[veh+0x13E4] blob).
void wheel_mirror_opp_brake_product(InvObject* self, InvObject* chassis,
                                    int32_t self_id, const WheelRefState& w,
                                    const WheelPhysExtra& me) {
  const int32_t oid = w.opp_wheel;
  float opp_d8 = 0.f, opp_dc = 0.f;
  int32_t opp_src = 0;  // 0=none, 1=hdr TREE, 2=opp Soft Extra
  InvObject* ch = chassis ? chassis : me.veh;
  // Soft PE deepen: hdr sibling TREE first (PE *[arg_veh+0x1FBC]) — only
  // when Chassis marked the stand-in; never invent hdr from self.
  if (ch && tree_field_get_int(ch, "phys_sibling_set") != 0) {
    opp_d8 = tree_field_get_float(ch, "phys_sibling_brake_d8");
    opp_dc = tree_field_get_float(ch, "phys_sibling_hbrake_dc");
    opp_src = 1;
  } else if (ch && oid >= 0 && oid < 8) {
    const WheelPhysExtra& opp =
        g_chassis_phys_extra[ch][static_cast<size_t>(oid)];
    opp_d8 = opp.brake_d8;
    opp_dc = opp.hbrake_dc;
    opp_src = 2;
  }
  const float pair_d8 = opp_d8 * me.brake_d8;
  const float pair_dc = opp_dc * me.hbrake_dc;
  float add_1db8 = 0.f;
  float sc_1dc0 = 0.f;
  float wheel_fc = 0.f;
  if (ch) {
    add_1db8 = tree_field_get_float(ch, "phys_brake_scale_1db8");
    sc_1dc0 = tree_field_get_float(ch, "phys_hbrake_scale_1dc0");
  }
  if (self) {
    wheel_fc = tree_field_get_float(self, "phys_wheel_fc");  // PE +0xFC
  } else if (ch && self_id >= 0 && self_id < 8) {
    char fk[40];
    std::snprintf(fk, sizeof(fk), "phys_wheel_fc_%d",
                  static_cast<int>(self_id));
    wheel_fc = tree_field_get_float(ch, fk);
  }
  // PE @ 0x455c12..0x455c47 → brake_torque_sum (stack rename).
  const float torque_sum =
      (wheel_fc + add_1db8) * pair_d8 + sc_1dc0 * pair_dc + w.bearing;
  if (self) {
    tree_field_set_float(self, "phys_brake_opp_d8", opp_d8);
    tree_field_set_float(self, "phys_hbrake_opp_dc", opp_dc);
    tree_field_set_float(self, "phys_brake_opp_self", pair_d8);
    tree_field_set_float(self, "phys_hbrake_opp_self", pair_dc);
    tree_field_set_float(self, "phys_brake_torque_sum", torque_sum);
    tree_field_set_float(self, "phys_wheel_fc", wheel_fc);
    tree_field_set_int(self, "phys_brake_opp_src", opp_src);
    if (oid >= 0) {
      tree_field_set_int(self, "phys_opp_lea",
                         oid * world_state::kWheelPhysStride);
      tree_field_set_int(self, "phys_opp_brake_lea_d8",
                         oid * world_state::kWheelPhysStride +
                             world_state::wheel_off::brake_d8);
      tree_field_set_int(self, "phys_opp_hbrake_lea_dc",
                         oid * world_state::kWheelPhysStride +
                             world_state::wheel_off::hbrake_dc);
    }
  }
  if (ch && self_id >= 0 && self_id < 8) {
    char tk[48], pk[48], hk[48], od8[48], odc[48], osrc[40];
    std::snprintf(tk, sizeof(tk), "phys_brake_torque_sum_%d",
                  static_cast<int>(self_id));
    std::snprintf(pk, sizeof(pk), "phys_brake_opp_self_%d",
                  static_cast<int>(self_id));
    std::snprintf(hk, sizeof(hk), "phys_hbrake_opp_self_%d",
                  static_cast<int>(self_id));
    std::snprintf(od8, sizeof(od8), "phys_brake_opp_d8_%d",
                  static_cast<int>(self_id));
    std::snprintf(odc, sizeof(odc), "phys_hbrake_opp_dc_%d",
                  static_cast<int>(self_id));
    std::snprintf(osrc, sizeof(osrc), "phys_brake_opp_src_%d",
                  static_cast<int>(self_id));
    tree_field_set_float(ch, tk, torque_sum);
    tree_field_set_float(ch, pk, pair_d8);
    tree_field_set_float(ch, hk, pair_dc);
    tree_field_set_float(ch, od8, opp_d8);
    tree_field_set_float(ch, odc, opp_dc);
    tree_field_set_int(ch, osrc, opp_src);
  }
}

// Soft PE unified publish: optional rederive (phys tick @ 0x455df4) then
// TREE mirror + Soft consumer (0x455c21). Call sites: forceUpdate_tail
// (rederive=true, bearing=false), setters (rederive=false — PE setter
// formula already applied), ingest type6 (rederive=true after +0x164).
void wheel_soft_phys_publish(InvObject* self, InvObject* chassis, int32_t id,
                             bool rederive, bool accumulate_bearing) {
  WheelRefState* wp = nullptr;
  WheelPhysExtra* ep = nullptr;
  if (chassis && id >= 0 && id < 8) {
    wp = &g_chassis_phys_wheels[chassis][static_cast<size_t>(id)];
    ep = &extra_slot(chassis, id);
    ep->veh = chassis;
  } else if (self) {
    wp = &WR_eff(self);
    ep = &extra_eff(self);
    if (!chassis) chassis = tree_field_get_obj(self, "chassis");
    if (!chassis) chassis = ep->veh;
    if (id < 0) id = tree_field_get_int(self, "wheel_id");
  } else {
    return;
  }
  if (rederive)
    world_state::wheel_phys_tick_refresh_derived(*wp, *ep, accumulate_bearing);
  wheel_mirror_brake_tree(self, chassis, id, *wp, *ep);
  wheel_mirror_opp_brake_product(self, chassis, id, *wp, *ep);
}

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

// Soft PE public entry — declared early so forceUpdate_tail / ingest can call.
void chassis_phys_wheel_tick_refresh(InvObject* chassis, int32_t id,
                                     bool accumulate_bearing);
// Soft PE wheel-table walk (Chassis_physWheelTick @ 0x455ba8..0x455f15).
void chassis_phys_wheel_table_tick(InvObject* chassis, bool accumulate_bearing);

// Shared phys entry for Chassis.getWheel / forceUpdate_apply scratch.
// PE layout (Chassis_physWheelTick @ 0x4545d4 / 0x455be5):
//   wheel* = *[veh+0x13E4] + i*0x2B4; count = this+0x1F40; gate = wheel+0x30.
// Host Soft: contiguous table; phys_wheels_base = &table[0] ONLY as Soft
// LEA arithmetic stand-in for setOppWheel (id*0x2B4) — do NOT invent a
// live PE *[veh+0x13E4] phys blob (OOS / blocked). Raw solver still Chassis.
WheelRefState& chassis_phys_wheel_slot(InvObject* chassis, int32_t id) {
  static WheelRefState orphan;
  if (!chassis || id < 0 || id >= 8) return orphan;
  auto& table = g_chassis_phys_wheels[chassis];
  // Soft host address of Soft table[0] — not a PE *[veh+0x13E4] invent.
  tree_field_set_int(
      chassis, "phys_wheels_base",
      static_cast<int32_t>(reinterpret_cast<uintptr_t>(&table[0])));
  // W34-10: advertise PE LEA stride (getWheel @ 0x440C80 / setOppWheel).
  tree_field_set_int(chassis, "phys_wheel_stride", world_state::kWheelPhysStride);
  tree_field_set_int(chassis, "phys_wheels_table_off",
                     world_state::kChassisWheelTableOff);  // PE +0x13E4
  // Soft PE deepen: this+0x1F40 count stand-in (getWheel jge / phys tick).
  int32_t nw = tree_field_get_int(chassis, "wheels");
  if (nw <= 0) nw = tree_field_get_int(chassis, "phys_wheels_count");
  if (nw <= 0) nw = 4;
  if (nw > 8) nw = 8;
  tree_field_set_int(chassis, "phys_wheels_count", nw);
  WheelPhysExtra& ex = g_chassis_phys_extra[chassis][static_cast<size_t>(id)];
  ex.veh = chassis;  // PE wheel[+0x22C]
  // PE setCPatch gate *[handle+0x30] — host marks contact live once slotted.
  ex.contact30 = true;
  // Soft byte offset into Soft table (id*0x2B4) — not a raw wheel* ptr.
  char lea_k[32];
  std::snprintf(lea_k, sizeof(lea_k), "phys_wheel_lea_%d",
                static_cast<int>(id));
  tree_field_set_int(chassis, lea_k, id * world_state::kWheelPhysStride);
  // Publish current Soft derived brake slots (PE +0xD0/+0xD8 / +0xD4/+0xDC).
  wheel_mirror_brake_tree(nullptr, chassis, id, table[static_cast<size_t>(id)],
                          ex);
  return table[static_cast<size_t>(id)];
}

// PE apply @ 0x4484f9: zero +0x15C..+0x170 then memcpy pacejka 68B @ +0x1E8.
void chassis_phys_wheel_scratch_reset(InvObject* chassis, int32_t id) {
  auto& w = chassis_phys_wheel_slot(chassis, id);
  auto& e = extra_slot(chassis, id);
  // race121: PE zeros only [87..92] (+0x15C..+0x170). Do NOT clear brake
  // (+0xD0) / hbrake (+0xD4) — those survive forceUpdate_apply scratch.
  e.wear15c = 0.f;
  e.scratch160 = 0.f;
  e.scratch164 = 0.f;
  e.scratch168 = 0.f;
  e.scratch16c = 0.f;
  e.scratch170 = 0.f;
  // Engine_memcpy(wheel+0x1E8, chassis+0x2E50, 68): restore shared Pacejka tmpl.
  // race124: host kDefaultPacejka stand-in — NO invent raw +0x2E50 blob.
  std::memcpy(w.pacejka, kDefaultPacejka, sizeof(kDefaultPacejka));
  wheel_sync_named_from_pacejka(w);  // Soft: sliction/friction/stiffness/frictn_x
}

// PE Chassis_forceUpdate_ingestPart @ 0x0043C520 — types 5/6/7 only.
// LOWORD(part+0xB0)=type, HIWORD=wheel idx; pair from part+0xB8/+0xBC.
// type5→+0x15C/+0x160, type6→+0x164/+0x168, type7→+0x16C/+0x170.
// Called from Chassis forceUpdate slot-walk stand-in for *[phys+0xDC].
// Types 8/9/10/12 ResHandle Link — Chassis TREE null-safe only.
void chassis_phys_wheel_ingest_scratch(InvObject* chassis, int32_t type,
                                       int32_t wheel_idx, float a, float b) {
  if (!chassis || wheel_idx < 0 || wheel_idx >= 8) return;
  auto& e = extra_slot(chassis, wheel_idx);
  switch (type) {
    case 5:
      e.wear15c = a;
      e.scratch160 = b;
      break;
    case 6:
      e.scratch164 = a;
      e.scratch168 = b;
      // Soft PE: scratch164 feeds setBrake/setHBrake + phys-tick refresh
      // @ 0x455df4 — rederive +0xD8/+0xDC (no bearing; live tick OOS).
      wheel_soft_phys_publish(nullptr, chassis, wheel_idx,
                              /*rederive=*/true, /*accumulate_bearing=*/false);
      break;
    case 7:
      e.scratch16c = a;
      e.scratch170 = b;
      break;
    default:
      break;
  }
}

// PE Chassis_forceUpdate_tail @ 0x004479D0 size 0x15f — called at end of
// Chassis_forceUpdate_apply @ 0x448cf3 ALWAYS after hop ok (even when
// suspend_update skipped the body). Gate phys[+0x1F24]>=0 (int_convert
// 7972); per-wheel front[+0x1F28]/rear[+0x1F2C] → wheel[+0xCC]/[+0xE0]
// when >0 (i*0x2B4>=0x568 → rear); scale [+0x64]/[+0x68] by
// phys[+0x1F14+i]; wear[+0x15C] → k → [+0x6C]/[+0x70]; hdr[+0xD0]/[+0xD4]
// *= [+0x1F24].
void chassis_force_update_tail(InvObject* chassis, int32_t nw) {
  if (!chassis) return;
  // PE: if (phys[+0x1F24] < 0.0) skip entire tail. Host TREE stand-in.
  float gate = tree_field_get_float(chassis, "force_update_tail_gate");
  if (tree_field_get_int(chassis, "force_update_tail_gate_set") == 0) {
    gate = tree_field_get_float(chassis, "V_spring");
    if (gate == 0.f) gate = 1.f;  // unset → run (PE post-apply ≥0 typical)
  }
  if (gate < 0.f) return;
  float stiff_f = tree_field_get_float(chassis, "force_update_stiff_f");
  float stiff_r = tree_field_get_float(chassis, "force_update_stiff_r");
  if (stiff_f == 0.f) stiff_f = tree_field_get_float(chassis, "V_spring_front");
  if (stiff_r == 0.f) stiff_r = tree_field_get_float(chassis, "V_spring_rear");
  // PE loops this+0x1F40 (layout count); host nw / TREE wheels.
  const int32_t n = (nw > 0) ? nw : 4;
  for (int32_t i = 0; i < n && i < 8; ++i) {
    auto& w = chassis_phys_wheel_slot(chassis, i);
    auto& e = extra_slot(chassis, i);
    // PE: byte offset i*0x2B4 >= 0x568 (i>=2) → rear else front.
    const float axle = (i >= 2) ? stiff_r : stiff_f;
    if (axle > 0.f) {
      // PE writes both wheel[+0xCC] and [+0xE0] (setDrive twin @ 0x440E00).
      wheel_apply_drive(w, e, axle);
    }
    char sk[32];
    std::snprintf(sk, sizeof(sk), "wheel_scale_%d", static_cast<int>(i));
    float sc = tree_field_get_float(chassis, sk);
    if (sc == 0.f) sc = tree_field_get_float(chassis, "tyre_wear_scale");
    // PE @ 0x4479d0: if (*(+0x1F14+i) > 0) [+0x64]/[+0x68] *= scale in-place.
    // Seed frict64/68 from WR.friction when unset (setFriction twin).
    if (e.frict64 == 0.f && e.frict68 == 0.f && w.friction != 0.f) {
      e.frict64 = w.friction;
      e.frict68 = w.friction;
    }
    if (sc > 0.f) {
      e.frict64 *= sc;
      e.frict68 *= sc;
    }
    const float k = wheel_wear_scale(e.wear15c);
    // PE: [+0x6C]=k*[+0x64]; [+0x70]=k*[+0x68] (order from disasm v5[27]/[28]).
    e.frict6c = k * e.frict64;
    e.frict70 = k * e.frict68;
    char kk[32];
    std::snprintf(kk, sizeof(kk), "force_update_wheel_k_%d", static_cast<int>(i));
    tree_field_set_float(chassis, kk, k);
    char dk[32];
    std::snprintf(dk, sizeof(dk), "force_update_wheel_d64_%d", static_cast<int>(i));
    tree_field_set_float(chassis, dk, e.frict6c);
    if (sc > 0.f) tree_field_set_float(chassis, sk, sc);
  }
  // Soft PE deepen: Chassis_physWheelTick wheel-table consumer @ 0x455ba8
  // (fabs +0x1DB8/+0x1DC0, count +0x1F40, gate +0x30, refresh +0xD8/+0xDC
  // + Soft consumer 0x455c21). forceUpdate is not a per-frame tick →
  // accumulate_bearing=false (bearing+=float_100_0 @ 0x455e4c OOS).
  chassis_phys_wheel_table_tick(chassis, /*accumulate_bearing=*/false);
  // PE: ALWAYS hdr[+0xD0]/[+0xD4] *= gate (+0x1F24) when gate>=0 (incl. *0).
  // Does NOT rewrite phys +0x1DD0/+0x1DD4 (already k*hdr from apply).
  // race125: multiply hdr_d0/d4 stand-ins only — leave force_update_scale*.
  {
    float d0 = tree_field_get_float(chassis, "force_update_hdr_d0");
    float d4 = tree_field_get_float(chassis, "force_update_hdr_d4");
    if (d0 == 0.f && d4 == 0.f) {
      // Pre-race125 sessions may only have scale* — leave scales alone.
      d0 = 1.f;
      d4 = 1.f;
    }
    tree_field_set_float(chassis, "force_update_hdr_d0", d0 * gate);
    tree_field_set_float(chassis, "force_update_hdr_d4", d4 * gate);
  }
}

// Soft PE public entry for Chassis_physWheelTick refresh @ 0x455df4..0x455e7c
// + Soft consumer @ 0x455c21. Callable from chassis_force_update_tail /
// setters / Soft table tick. accumulate_bearing=true only for live
// Chassis_physWheelTick (bearing+=float_100_0 @ 0x455e4c — Soft OOS).
void chassis_phys_wheel_tick_refresh(InvObject* chassis, int32_t id,
                                     bool accumulate_bearing) {
  if (!chassis || id < 0 || id >= 8) return;
  (void)chassis_phys_wheel_slot(chassis, id);
  wheel_soft_phys_publish(nullptr, chassis, id, /*rederive=*/true,
                          accumulate_bearing);
}

// Soft PE deepen: Chassis_physWheelTick wheel-table brake consumer
// @ 0x455ba8..0x455f15 (IDA comments + disasm). Not a phys solver —
// TREE + Soft table only (no raw *[veh+0x13E4] / hdr+0x1FBC blob).
//   gate (phys+0x70)&1 → skip
//   fabs chassis+0x1DB8 / +0x1DC0
//   count = this+0x1F40 (TREE wheels / phys_wheels_count)
//   wheel* Soft = table[i]; gate Extra.contact30 (+0x30)
//   |+0xC0| > 0.001 only when TREE phys_wheel_c0_set_%d (else Soft runs)
//   then refresh @ 0x455df4 + consumer @ 0x455c21
void chassis_phys_wheel_table_tick(InvObject* chassis, bool accumulate_bearing) {
  if (!chassis) return;
  // PE @ 0x455ba8: test byte [ebx+0x70],1 → skip whole table consumer.
  const int32_t flags70 =
      tree_field_get_int(chassis, "force_update_flags70");
  if ((flags70 & 1) != 0) return;

  // PE @ 0x455bb2..0x455bcd: fabs +0x1DB8 / +0x1DC0 in-place.
  float s_1db8 = tree_field_get_float(chassis, "phys_brake_scale_1db8");
  float s_1dc0 = tree_field_get_float(chassis, "phys_hbrake_scale_1dc0");
  if (s_1db8 < 0.f) s_1db8 = -s_1db8;
  if (s_1dc0 < 0.f) s_1dc0 = -s_1dc0;
  tree_field_set_float(chassis, "phys_brake_scale_1db8", s_1db8);
  tree_field_set_float(chassis, "phys_hbrake_scale_1dc0", s_1dc0);

  // PE @ 0x455bd3: cmp [this+0x1F40], 0 — Soft TREE wheels / count.
  int32_t n = tree_field_get_int(chassis, "wheels");
  if (n <= 0) n = tree_field_get_int(chassis, "phys_wheels_count");
  if (n <= 0) n = 4;
  if (n > 8) n = 8;
  tree_field_set_int(chassis, "phys_wheels_count", n);

  for (int32_t i = 0; i < n; ++i) {
    (void)chassis_phys_wheel_slot(chassis, i);
    WheelPhysExtra& e = extra_slot(chassis, i);
    // PE @ 0x455beb: *[wheel+0x30]==0 → skip.
    if (!e.contact30) continue;

    // PE @ 0x455bf9: |wheel[+0xC0]| <= flt_5F09CC (0.001) → skip.
    // Soft: only enforce when TREE spin seeded (no invent omega solver).
    char cset[48], ck[40];
    std::snprintf(cset, sizeof(cset), "phys_wheel_c0_set_%d",
                  static_cast<int>(i));
    if (tree_field_get_int(chassis, cset) != 0) {
      std::snprintf(ck, sizeof(ck), "phys_wheel_c0_%d", static_cast<int>(i));
      const float c0 = tree_field_get_float(chassis, ck);
      if (std::fabs(c0) <= kPePhysTickOmegaEps) continue;
    }

    chassis_phys_wheel_tick_refresh(chassis, i, accumulate_bearing);
  }
}

bool chassis_phys_wheel_dmg_active(InvObject* chassis, int32_t id) {
  if (!chassis || id < 0 || id >= 8) return false;
  return (extra_slot(chassis, id).flags100 & 0x20u) != 0;
}

void chassis_phys_wheel_dmg_clear(InvObject* chassis, int32_t id) {
  if (!chassis || id < 0 || id >= 8) return;
  auto& w = chassis_phys_wheel_slot(chassis, id);
  auto& e = extra_slot(chassis, id);
  e.flags100 &= ~0x20u;
  e.dmg_f4 = 0.f;
  e.dmg_f8 = 0.f;
  // PE clear @ 0x43D5E6: [+0xF0]=rest[+0x2C] (radius stand-in).
  e.dmg_f0 = w.radius;
  // PE @ 0x43D5B6..0x43D5E0: copy primary → derived twins (NOT wear-rescale):
  //   [+0xD8]=[+0xD0] brake; [+0xDC]=[+0xD4] hbrake; [+0xE0]=[+0xCC] drive.
  e.brake_d8 = w.brake;
  e.hbrake_dc = w.hbrake;
  e.drive_e0 = w.drive;
  // Soft: primary→derived copy (dmg clear) then Soft consumer TREE publish.
  wheel_soft_phys_publish(nullptr, chassis, id, /*rederive=*/false,
                          /*accumulate_bearing=*/false);
}

void chassis_phys_wheel_dmg_apply(InvObject* chassis, int32_t id, float px,
                                  float py, float pz, float yaw, float pitch,
                                  float roll, float f6, float f9) {
  if (!chassis || id < 0 || id >= 8) return;
  auto& w = chassis_phys_wheel_slot(chassis, id);
  auto& e = extra_slot(chassis, id);
  e.flags100 |= 0x20u;
  w.px = px;
  w.py = py;
  w.pz = pz;
  w.has_pos = true;
  w.oy = yaw;
  w.op = pitch;
  w.or_ = roll;
  w.has_ypr = true;
  // PE setWheelDamage: Ypr_toMatrix into slot+0x90 (same as setYpr).
  wheelref_ypr_to_mat34(e.mat90, yaw, pitch, roll);
  std::memcpy(e.mat110, e.mat90, sizeof(e.mat90));
  e.has_mat90 = true;
  e.dmg_f4 = f6;
  e.dmg_f8 = f9;
  // PE +0xF0 = f9 * flt_5F0C40 (200.0) + rest[+0x2C]; rest radius≈WR.radius.
  e.dmg_f0 = f9 * 200.f + w.radius;
}

bool chassis_phys_wheel_dmg_format(InvObject* chassis, int32_t id, char* buf,
                                   size_t buflen) {
  if (!buf || buflen < 8 || !chassis || id < 0 || id >= 8) return false;
  if ((extra_slot(chassis, id).flags100 & 0x20u) == 0) {
    buf[0] = '\0';
    return true;
  }
  const auto& w = chassis_phys_wheel_slot(chassis, id);
  const auto& e = extra_slot(chassis, id);
  // PE getWheelDamage @ 0x43D255: xyz, Ypr_fromMatrix(+0x90), +0xF4, 1, 1, +0xF8.
  float y = w.oy, p = w.op, r = w.or_;
  if (e.has_mat90) wheelref_ypr_from_mat34(e.mat90, y, p, r);
  std::snprintf(buf, buflen, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                w.px, w.py, w.pz, y, p, r, e.dmg_f4, 1.f, 1.f, e.dmg_f8);
  return true;
}

void java_game_parts_WheelRef_finalize(InvObject* self) {
  // Soft GC stand-in — no WheelRef.finalize in stock RegisterNative.
  // Soft related: Animation.finalize PE @ 0x0047EBA0 / String.finalize
  // PE @ 0x00486190; Object_FinalizeFree @ 0x00408560.
  if (!self) return;
  tree_field_set_float(self, "brake_d0", 0.f);
  tree_field_set_float(self, "hbrake_d4", 0.f);
  tree_field_set_float(self, "brake_d8", 0.f);
  tree_field_set_float(self, "hbrake_dc", 0.f);
  tree_field_set_float(self, "brake_scale_k", 0.f);
  tree_field_set_float(self, "scratch164", 0.f);
  tree_field_set_float(self, "bearing_f0", 0.f);
  tree_field_set_int(self, "contact30", 0);
  tree_field_set_int(self, "phys_off_brake_d0", 0);
  tree_field_set_int(self, "phys_off_hbrake_d4", 0);
  tree_field_set_int(self, "phys_off_brake_d8", 0);
  tree_field_set_int(self, "phys_off_hbrake_dc", 0);
  tree_field_set_int(self, "phys_off_scratch164", 0);
  tree_field_set_int(self, "phys_off_pos_x", 0);
  tree_field_set_int(self, "phys_wheel_stride", 0);
  tree_field_set_int(self, "phys_wheels_table_off", 0);
  tree_field_set_int(self, "native_ptr_lea", 0);
  tree_field_set_int(self, "phys_brake_lea_d0", 0);
  tree_field_set_int(self, "phys_brake_lea_d8", 0);
  tree_field_set_int(self, "phys_hbrake_lea_d4", 0);
  tree_field_set_int(self, "phys_hbrake_lea_dc", 0);
  tree_field_set_int(self, "phys_pos_lea_78", 0);
  tree_field_set_int(self, "opp_wheel", -1);
  tree_field_set_int(self, "opp_wheel_ptr", 0);
  tree_field_set_int(self, "opp_wheel_lea", 0);
  tree_field_set_int(self, "phys_opp_lea", 0);
  tree_field_set_int(self, "phys_opp_brake_lea_d8", 0);
  tree_field_set_int(self, "phys_opp_hbrake_lea_dc", 0);
  tree_field_set_int(self, "phys_brake_opp_src", 0);
  tree_field_set_float(self, "phys_brake_opp_d8", 0.f);
  tree_field_set_float(self, "phys_hbrake_opp_dc", 0.f);
  tree_field_set_float(self, "phys_brake_opp_self", 0.f);
  tree_field_set_float(self, "phys_hbrake_opp_self", 0.f);
  tree_field_set_float(self, "phys_brake_torque_sum", 0.f);
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
  // PE veh=[handle+0x22C]; host TREE chassis (set by Chassis.getWheel).
  InvObject* car = tree_field_get_obj(self, "chassis");
  int32_t wid = -1;
  if (car) {
    wid = tree_field_get_int(self, "wheel_id");
  } else {
    car = part_car_root(self);
    wid = part_wheel_id(self);
  }
  auto& w = WR_eff(self);
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
  // PE @ 0x00441FD0 size 0xBF (IDA WheelRef_getYpr). Unbox this.
  // Native.ptr==0 → null. NO Mighty. Ypr_fromMatrix(local,[handle+0x90])
  // @ 0x00551C90; malloc Ypr; set y/p/r. Inverse of setYpr Ypr_toMatrix
  // @ 0x0054ECD0 into +0x90 then qmemcpy → +0x110 (0x30). race125
  // PARTIAL: WheelPhysExtra.mat90 stand-in (no raw wheel* +0x90 blob).
  if (!self) return nullptr;
  auto& w = WR_eff(self);
  auto& e = extra_eff(self);
  float y = 0, p = 0, r = 0;
  if (e.has_mat90) {
    wheelref_ypr_from_mat34(e.mat90, y, p, r);
  } else if (w.has_ypr) {
    float m[12] = {};
    wheelref_ypr_to_mat34(m, w.oy, w.op, w.or_);
    wheelref_ypr_from_mat34(m, y, p, r);
  }
  return ypr_new(y, p, r);
}

float java_game_parts_WheelRef_getDrive(InvObject* self) {
  // PE @ 0x00440DC0 size 0x35 (53). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Native.ptr via JVM_vm_get_int_field(this, dword_62E008) @ 0x0042AB50.
  // handle==0 → fld flt_5E73CC (bytes 00 00 00 00 = 0.0). NO Mighty ERROR.
  // Else fld dword [handle+0xCC] (204) — first store of setDrive @ 0x00440E00
  // (also writes +0xE0/224, unread here). NO fmul. NOT getSteer [+0xC8]/
  // getRadius [+0x60] / setWidth [+0x1D4]. Callees: JVM_UnboxArg,
  // JVM_vm_get_int_field. Host WR.drive; !self = handle 0. race117 SKIP
  // (primary store 1:1; ctor default 1.f gap only).
  return self ? WR_eff(self).drive : 0.f;
}

float java_game_parts_WheelRef_getSteer(InvObject* self) {
  // PE @ 0x00440E50 size 0x35. Unbox this. Native.ptr (dword_62E008)==0 →
  // fld flt_5E73CC (bytes 00 00 00 00 = 0.0). NO Mighty ERROR. Else
  // fld dword [handle+0xC8] (200) — primary store of setSteer @ 0x00440E90
  // (mov [handle+0xC8] @ 0x440EC0). NO fmul. NOT getDrive [+0xCC] /
  // getRadius [+0x60]. Host WR.steer; !self = handle 0. race117 SKIP 1:1.
  return self ? WR_eff(self).steer : 0.f;
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
  // JVM_vm_get_int_field. Host WR.radius; !self = handle 0. race125
  // deepen_partial SKIP — primary [+0x60] already 1:1 (ctor 0.32f gap only).
  return self ? WR_eff(self).radius : 0.f;
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
  auto& w = WR_eff(self);
  vec3_get(val, &w.px, &w.py, &w.pz);
  w.has_pos = true;
  InvObject* car = tree_field_get_obj(self, "chassis");
  int32_t wid = -1;
  if (car) {
    wid = tree_field_get_int(self, "wheel_id");
  } else {
    car = part_car_root(self);
    wid = part_wheel_id(self);
  }
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
  //   Else (null Ypr): Engine_queryGameRefChannel([handle+0x22C], 6);
  //   sub_5447D0(0x80000000) → ctx; Ypr_toMatrix from table entry
  //   +0x1F50. Then ALWAYS qmemcpy [handle+0x110] ← [handle+0x90]
  //   size 0x30. race125 PARTIAL: mat90/mat110 stand-in; null Ypr
  //   table path (+0x1F50 via GameRef channel) still OOS.
  if (!self || !val) return;
  auto& w = WR_eff(self);
  auto& e = extra_eff(self);
  ypr_get(val, &w.oy, &w.op, &w.or_);
  w.has_ypr = true;
  wheelref_ypr_to_mat34(e.mat90, w.oy, w.op, w.or_);
  std::memcpy(e.mat110, e.mat90, sizeof(e.mat90));  // PE +0x110 ← +0x90
  e.has_mat90 = true;
  InvObject* car = tree_field_get_obj(self, "chassis");
  int32_t wid = -1;
  if (car) {
    wid = tree_field_get_int(self, "wheel_id");
  } else {
    car = part_car_root(self);
    wid = part_wheel_id(self);
  }
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
  // 1-drive_front). W35-10: wheel_apply_drive (+0xCC/+0xE0).
  if (!self) return;
  wheel_apply_drive(WR_eff(self), extra_eff(self), val);
}

void java_game_parts_WheelRef_setSteer(InvObject* self, float val) {
  // PE @ 0x00440E90 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0xC8] (200) = val
  // bits @ 0x440EC0; no fmul (unlike setWidth @ 0x004416C0 fmul 0.5 into
  // [handle+0x1D4]). getSteer @ 0x00440E50 flds same slot. NOT setDrive
  // [handle+0xCC]/[+0xE0]. Host WR.steer = val (PE dword). !self = handle 0.
  if (self) WR_eff(self).steer = val;
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
  if (self) WR_eff(self).radius = val;
}

void java_game_parts_WheelRef_setWidth(InvObject* self, float val) {
  // PE @ 0x004416C0 size 0x3e. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else fld val; fmul flt_5F09D0 (bytes
  // 00 00 00 3F = 0.5); fstp dword [handle+0x1D4] (468). NOT setRadius
  // slot [handle+0x60] (96). Java metres (Tyre tyre_width/1000). Host
  // WR.width = half-width (PE dword).
  if (self) WR_eff(self).width = val * 0.5f;
}

void java_game_parts_WheelRef_setCPatch(InvObject* self, float halfwidth, float angle,
                                       float offset) {
  // PE @ 0x00441700 size 0x68 (IDA WheelRef_setCPatch). Unbox this+FFF.
  // Native.ptr==0 → silent. Gate *[handle+0x30]==0 → silent (int_convert
  // 48). Else inner=*[+0x30]: [+0x84]=fabs(hw), [+0x88]=sin(fabs(ang))
  // (fsin after fabs @ 0x441752), [+0x8C]=offset (no fabs). race125
  // PARTIAL: contact30 stand-in for +0x30 (gate alone — not chassis-and);
  // WR.cpatch_* mirrors inner+0x84..+0x8C.
  if (!self) return;
  auto& e = extra_eff(self);
  if (!e.contact30) return;  // PE *[handle+0x30]==0
  auto& w = WR_eff(self);
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
  //   W35-10: wheel_apply_friction (+0x1F0/+0x64..+0x70, wear +0x15C).
  //   Soft: also pacejka[2] alias sync (same PE dword as setPacejka i=2).
  if (!self) return;
  wheel_apply_friction(WR_eff(self), extra_eff(self), val);
  wheel_sync_pacejka_from_named(WR_eff(self));
}

float wheelref_get_friction(InvObject* self) {
  return self ? WR_eff(self).friction : 1.f;
}

float wheelref_get_sliction(InvObject* self) {
  return self ? WR_eff(self).sliction : 1.f;
}

float wheelref_get_brake(InvObject* self) {
  return self ? WR_eff(self).brake : 0.f;
}

float wheelref_get_hbrake(InvObject* self) {
  return self ? WR_eff(self).hbrake : 0.f;
}

float wheelref_get_roll_res(InvObject* self) {
  return self ? WR_eff(self).roll_res : 0.f;
}

float wheelref_get_pacejka(InvObject* self, int32_t i) {
  if (!self || i < 0 || i >= 17) return 0.f;
  return WR_eff(self).pacejka[i];
}

bool wheelref_get_arm(InvObject* self, float out[7]) {
  if (!self || !out) return false;
  auto& w = WR_eff(self);
  if (!w.has_arm) return false;
  for (int i = 0; i < 7; ++i) out[i] = w.arm[i];
  return true;
}

bool wheelref_get_hub(InvObject* self, float out[10]) {
  if (!self || !out) return false;
  auto& w = WR_eff(self);
  if (!w.has_hub) return false;
  for (int i = 0; i < 10; ++i) out[i] = w.hub[i];
  return true;
}

float wheelref_get_force(InvObject* self) {
  return self ? WR_eff(self).force : 0.f;
}

float wheelref_get_damp_bound(InvObject* self) {
  return self ? WR_eff(self).damp_bound : 0.f;
}

float wheelref_get_rest_len(InvObject* self) {
  return self ? WR_eff(self).rest_len : 0.f;
}

float wheelref_get_arm_len(InvObject* self) {
  if (!self) return 0.f;
  auto& w = WR_eff(self);
  return w.has_arm ? w.arm[0] : 0.f;
}

void java_game_parts_WheelRef_setFrictn_x(InvObject* self, float val) {
  // PE @ 0x00441050 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0x218] (536) = val
  // bits @ 0x441080; no fmul (unlike setWidth @ 0x004416C0 fmul 0.5 into
  // [handle+0x1D4]). NOT setFriction [handle+0x1F0] (496) / setSliction
  // [handle+0x1E8] (488) / setRadius [handle+0x60] / setSteer [handle+0xC8].
  // No getFrictn_x native. Physics fmul of stored dword in
  // Chassis_physWheelTick @ 0x45619C/0x4563A4 (use-site, not setter). Host
  // WR.frictn_x = val (PE dword); Soft pacejka[12] alias. !self = handle 0.
  if (!self) return;
  auto& w = WR_eff(self);
  w.frictn_x = val;
  wheel_sync_pacejka_from_named(w);
}

void java_game_parts_WheelRef_setSliction(InvObject* self, float val) {
  // PE @ 0x00441090 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0x1E8] (488) = val
  // bits @ 0x4410C0; no fmul (unlike setWidth @ 0x004416C0 fmul 0.5 into
  // [handle+0x1D4]). NOT setFriction [handle+0x1F0] (496) / setFrictn_x
  // [handle+0x218] (536) / setBearing [handle+0xF0] (240) / setStiffness
  // [handle+0x1F8] / setRadius [handle+0x60] / setSteer [handle+0xC8].
  // Soft: pacejka[0] alias (same dword as setPacejka i=0). Host
  // WR.sliction = val (PE dword). !self = handle 0.
  if (!self) return;
  auto& w = WR_eff(self);
  w.sliction = val;
  wheel_sync_pacejka_from_named(w);
}

void java_game_parts_WheelRef_setStiffness(InvObject* self, float val) {
  // PE @ 0x004410D0 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0x1F8] (504) = val
  // bits. No fmul. Same slot as setPacejka i=4. Sibling setSliction +0x1E8 /
  // setFriction +0x1F0. Soft pacejka[4] alias. Host WR.stiffness = val.
  // !self = handle 0.
  if (!self) return;
  auto& w = WR_eff(self);
  w.stiffness = val;
  wheel_sync_pacejka_from_named(w);
}

void java_game_parts_WheelRef_setRollRes(InvObject* self, float val) {
  // PE @ 0x00441110 size 0x35: Unbox this+F; dword_62E008; silent if 0;
  // Host WR.roll_res = val. race125 filler SKIP 1:1 (primary [+0x74]).
  if (self) WR_eff(self).roll_res = val;
}

void java_game_parts_WheelRef_setBearing(InvObject* self, float val) {
  // PE @ 0x00441150 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0xF0] (240) = val
  // bits @ 0x441180; no fmul (unlike setWidth @ 0x004416C0 fmul 0.5 into
  // [handle+0x1D4]). NOT setRollRes [handle+0x74] (116) / setDrive
  // [handle+0xCC]/[+0xE0] / setSteer [handle+0xC8] / setFrictn_x
  // [handle+0x218] / setFriction [handle+0x1F0] / setSliction [handle+0x1E8]
  // / setStiffness [handle+0x1F8] / setMaxLoad [handle+0x214]. No
  // getBearing native. Physics fadd of stored dword in Chassis_physWheelTick
  // @ 0x455C41 (use-site in brake_torque_sum); +=float_100_0 @ 0x455E4C
  // is live-tick only (Soft OOS). Host WR.bearing = val.
  if (!self) return;
  WR_eff(self).bearing = val;
  // Soft: bearing feeds brake_torque_sum @ 0x455c41 — republish consumer.
  InvObject* ch = tree_field_get_obj(self, "chassis");
  if (!ch) ch = extra_eff(self).veh;
  const int32_t id = tree_field_get_int(self, "wheel_id");
  wheel_soft_phys_publish(self, ch, id, /*rederive=*/false,
                          /*accumulate_bearing=*/false);
}

void java_game_parts_WheelRef_setMaxLoad(InvObject* self, float val) {
  // PE @ 0x00441190 size 0x38. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov dword [handle+0x214] (532) = val
  // bits @ 0x4411C0; no fmul. Sibling setLoadSmooth +0x220 / setBearing
  // +0xF0. Host WR.max_load = val. !self = handle 0. race125 filler SKIP 1:1.
  if (self) WR_eff(self).max_load = val;
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
  if (self) WR_eff(self).load_smooth = val;
}

void java_game_parts_WheelRef_setPacejka(InvObject* self, int32_t i, float val) {
  // PE @ 0x00441210 size 0x57. Unbox this+I+F. Bounds: jl if i<0; cmp 0x11 /
  // jnb if i>=17 → silent ret (valid i = 0..16). Else Native.ptr
  // (dword_62E008); handle==0 → silent ret. NO Mighty ERROR. Else fstp dword
  // [handle+i*4+0x1E8] (488+4*i) @ 0x44125c. Aliases (same PE slots): i0 =
  // setSliction +0x1E8, i2 = setFriction +0x1F0, i4 = setStiffness +0x1F8,
  // i12 = setFrictn_x +0x218. Soft: sync named when alias index written.
  if (!self || i < 0 || i >= 17) return;
  auto& w = WR_eff(self);
  w.pacejka[i] = val;
  if (i == 0 || i == 2 || i == 4 || i == 12) wheel_sync_named_from_pacejka(w);
}

void java_game_parts_WheelRef_setForce(InvObject* self, float val) {
  // PE @ 0x00441270 size 0x66. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty ERROR. Else mov [handle+0x44] (68) = val bits;
  // thiscall sub_491F80 (not hosted; shared setRadius/setDamping/set*Len).
  // No derived handle store (unlike setRestLen [+0x5C]). Host WR.force.
  // !self = handle 0.
  if (self) WR_eff(self).force = val;
}

void java_game_parts_WheelRef_setDamping(InvObject* self, float val) {
  // PE @ 0x004412E0 size 0x7b — setDamping(F)V (registry/IDA). Ticket VA
  // 0x00441360 is the (FF)V overload, not this body. Unbox this+F.
  // Native.ptr==0 → silent ret. Else [handle+0x48]=val; [handle+0x4C]=
  // val*flt_5F0B0C (bytes 33 33 33 3F ≈0.7); thiscall sub_491F80 (not
  // hosted). W35-10: wheel_apply_damping_f.
  if (!self) return;
  wheel_apply_damping_f(WR_eff(self), val);
}

void java_game_parts_WheelRef_setDamping_1(InvObject* self, float bound,
                                          float rebound) {
  // PE @ 0x00441360 size 0x77 — setDamping(FF)V. Separate impl from (F)V
  // @ 0x004412E0 (not shared). Unbox this+bound+rebound. Native.ptr==0 →
  // silent ret. Else [handle+0x48]=bound, [+0x4C]=rebound (no *0.7);
  // thiscall sub_491F80 (not hosted). Host WR.damp_bound / damp_rebound.
  // W35-10: wheel_apply_damping_ff declared Soft PE but body still local
  // (world_state.cpp OOS this ticket — no *0.7 path).
  if (!self) return;
  auto& w = WR_eff(self);
  w.damp_bound = bound;
  w.damp_rebound = rebound;
  w.damping = bound;
}

void java_game_parts_WheelRef_setRestLen(InvObject* self, float val) {
  // PE @ 0x004413E0 size 0x7f. Unbox this+F. Native.ptr (dword_62E008)==0 →
  // silent ret. NO Mighty. Else [handle+0x50]=val (restLen); [+0x5C]=
  // [+0x60]+val (radius+restLen); thiscall sub_491F80 (not hosted).
  // W35-10: wheel_apply_rest_len Soft stores +0x5C (=radius+val).
  if (self) wheel_apply_rest_len(WR_eff(self), val);
}

void java_game_parts_WheelRef_setMinLen(InvObject* self, float val) {
  // PE @ 0x00441460 size 0x66. Unbox this+F. Native.ptr==0 → silent ret.
  // Else [handle+0x58]=val; thiscall sub_491F80 (not hosted). No derived
  // write (unlike setRestLen [+0x5C]). Twin of setMaxLen [+0x54].
  // race125 filler SKIP 1:1.
  if (self) WR_eff(self).min_len = val;
}

void java_game_parts_WheelRef_setMaxLen(InvObject* self, float val) {
  // PE @ 0x004414D0 size 0x66. Unbox this+F. Native.ptr==0 → silent ret.
  // Else [handle+0x54]=val; thiscall sub_491F80 (not hosted). Twin of
  // setMinLen [+0x58]; no derived write (unlike setRestLen [+0x5C]).
  // race125 filler SKIP 1:1.
  if (self) WR_eff(self).max_len = val;
}

void java_game_parts_WheelRef_setInstantCenter(InvObject* self, float Hx, float Hy,
                                              float Hz, float Lx, float Ly,
                                              float Lz) {
  // PE @ 0x00441A80 size 0xD4. Unbox this+FFFFFF. Native.ptr==0 → silent
  // ret. Else H'/L' = args + wheel pos [handle+0x78/+0x7C/+0x80] → store
  // [+0x27C..+0x290]. Host: WR.ic[] = args + WR.px/py/pz.
  if (!self) return;
  auto& w = WR_eff(self);
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
  // silent ret. NO Mighty ERROR. Else (ASM @ 0x4415ae..0x4415f3):
  //   fld [handle+0x164]; mov [handle+0xD0]=val; fcom flt_5F08F0 (1.0);
  //   CF set (x<1) → fstp [+0xD8]=(1−x*x*flt_wheel_wear_scale_sq)*val;
  //   else fstp [+0xD8]=flt_wheel_wear_scale_lo*val. getBrake @ 0x00441540
  //   flds [+0xD0] only (never +0xD8). Factor twin of setFriction (+0x15C)
  //   but slot +0x164 (ingestPart type6 → scratch164). W35-10:
  //   wheel_apply_brake (= setter formula; phys-tick ×0.2 @ 0x455e5e is
  //   later, NOT here). Soft publish consumer (+0xD8 @ 0x455c21) +
  //   wheel_soft_brake_sidemap (offs + id*0x2B4 lea; no *[veh+0x13E4]).
  if (!self) return;
  InvObject* ch = tree_field_get_obj(self, "chassis");
  const int32_t id = tree_field_get_int(self, "wheel_id");
  // Soft deepen: live Soft phys-table slot (id*0x2B4 LEA) before write —
  // never invent a dereferenced *[veh+0x13E4] blob (OOS).
  if (ch && id >= 0 && id < 8) (void)chassis_phys_wheel_slot(ch, id);
  auto& w = WR_eff(self);
  auto& e = extra_eff(self);
  if (!ch) ch = e.veh;
  wheel_apply_brake(w, e, val);
  // Soft: setter PE stops at wear_scale — rederive=false (no ×0.2).
  wheel_soft_phys_publish(self, ch, id, /*rederive=*/false,
                          /*accumulate_bearing=*/false);
}

void java_game_parts_WheelRef_setHBrake(InvObject* self, float val) {
  // PE @ 0x00441640 size 0x7b. Twin of setBrake. ASM @ 0x44166e..0x4416b3:
  //   mov [handle+0xD4]=val; derived [+0xDC] from factor [+0x164] vs 1.0
  //   (same wheel_wear_scale). getHBrake @ 0x00441600 flds [+0xD4] only.
  //   W35-10: wheel_apply_hbrake; Soft publish + sidemap (twin setBrake).
  // Soft deepen: chassis Soft-table slot + HBrake LEA parity on chassis
  // (phys_hbrake_lea_d4/dc_%d) — still no *[veh+0x13E4] invent.
  if (!self) return;
  InvObject* ch = tree_field_get_obj(self, "chassis");
  const int32_t id = tree_field_get_int(self, "wheel_id");
  if (ch && id >= 0 && id < 8) (void)chassis_phys_wheel_slot(ch, id);
  auto& w = WR_eff(self);
  auto& e = extra_eff(self);
  if (!ch) ch = e.veh;
  wheel_apply_hbrake(w, e, val);
  wheel_soft_phys_publish(self, ch, id, /*rederive=*/false,
                          /*accumulate_bearing=*/false);
}

float java_game_parts_WheelRef_getBrake(InvObject* self) {
  // PE @ 0x00441540 size 0x35. Unbox this. Native.ptr (dword_62E008)==0 →
  // fld 0.0 (flt_5E73CC). NO Mighty ERROR. Else fld dword [handle+0xD0]
  // (208) — primary store of setBrake @ 0x00441580. Twin getHBrake lit
  // [+0xD4]. Host WR.brake; !self = handle 0. race125 filler SKIP 1:1.
  return self ? WR_eff(self).brake : 0.f;
}

float java_game_parts_WheelRef_getHBrake(InvObject* self) {
  // PE @ 0x00441600 size 0x35. Unbox this. Native.ptr (dword_62E008)==0 →
  // fld flt_5E73CC (bytes 00 00 00 00 = 0.0). NO Mighty ERROR. Else
  // fld dword [handle+0xD4] (212) — primary store of setHBrake @ 0x00441640
  // (derived store +0xDC unread). Twin of getBrake @ 0x00441540 which
  // flds [+0xD0] (208). Host WR.hbrake; !self = handle 0.
  return self ? WR_eff(self).hbrake : 0.f;
}

void java_game_parts_WheelRef_setOppWheel(InvObject* self, int32_t id) {
  // PE @ 0x00441A10 size 0x6b (IDA WheelRef_setOppWheel). Unbox this+id.
  // Native.ptr==0 → silent ret. ASM @ 0x441a42: id<0 (test/jl) →
  // [handle+0x250]=0. id≥0 → LEA id*0x2B4; esi=[handle+0x22C];
  // ecx=[esi+0x13E4]; [handle+0x250]=ecx+edx — wheel* address, NO bounds
  // vs +0x1F40 (contrast getWheel jge). race125 PARTIAL: LEA via
  // phys_wheels_base + id*0x2B4 (unbounded arithmetic like PE); id∈[0,8)
  // also touches slot so +0x22C/base stay live.
  if (!self) return;
  WR_eff(self).opp_wheel = id;
  tree_field_set_int(self, "opp_wheel", id);
  WheelPhysExtra& me = extra_eff(self);
  if (id < 0) {
    me.opp_ptr = 0;
    tree_field_set_int(self, "opp_wheel_ptr", 0);
    tree_field_set_int(self, "opp_wheel_lea", 0);
    tree_field_set_int(self, "phys_opp_lea", 0);
    tree_field_set_int(self, "phys_opp_brake_lea_d8", 0);
    tree_field_set_int(self, "phys_opp_hbrake_lea_dc", 0);
    tree_field_set_int(self, "phys_brake_opp_src", 0);
    tree_field_set_float(self, "phys_brake_opp_d8", 0.f);
    tree_field_set_float(self, "phys_hbrake_opp_dc", 0.f);
    tree_field_set_float(self, "phys_brake_opp_self", 0.f);
    tree_field_set_float(self, "phys_hbrake_opp_self", 0.f);
    tree_field_set_float(self, "phys_brake_torque_sum", 0.f);
    return;
  }
  const int32_t lea = id * world_state::kWheelPhysStride;
  tree_field_set_int(self, "opp_wheel_lea", lea);
  InvObject* ch = tree_field_get_obj(self, "chassis");  // Java mirror
  if (!ch) ch = me.veh;  // PE handle+0x22C stand-in
  uintptr_t base = 0;
  if (ch) {
    // Ensure table + phys_wheels_base (*[veh+0x13E4] stand-in).
    if (id < 8) (void)chassis_phys_wheel_slot(ch, id);
    else (void)chassis_phys_wheel_slot(ch, 0);
    base = static_cast<uintptr_t>(
        static_cast<uint32_t>(tree_field_get_int(ch, "phys_wheels_base")));
    if (base == 0)
      base = reinterpret_cast<uintptr_t>(&chassis_phys_wheel_slot(ch, 0));
  }
  // PE: [+0x250] = base + id*0x2B4 even when id >= count (unbounded).
  me.opp_ptr = base ? (base + static_cast<uintptr_t>(lea)) : 0;
  tree_field_set_int(self, "opp_wheel_ptr",
                     static_cast<int32_t>(me.opp_ptr));
  // Soft PE: re-publish opp×self + brake_torque_sum (0x455c21 / 0x455c47).
  const int32_t self_id = tree_field_get_int(self, "wheel_id");
  wheel_soft_phys_publish(self, ch, self_id, /*rederive=*/false,
                          /*accumulate_bearing=*/false);
}

void java_game_parts_WheelRef_setArm(InvObject* self, float len, float px, float py,
                                    float pz, float nx, float ny, float nz) {
  // PE @ 0x00441770 size 0x107. Unbox this+7F. handle==0 → silent.
  // Stores: +0x234=len; +0x244..+0x24C = p + pos(+0x78..+0x80) with Y
  // bias −(+0x38++0x3C) unknown on host → use pos only; +0x238..+0x240 =
  // n̂ if ‖n‖≠0 else leave (host zeros then writes). Same pos slots as setPos.
  if (!self) return;
  auto& w = WR_eff(self);
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
  // PE @ 0x00441880 size 0x184 (IDA WheelRef_setHub). Unbox
  // this+len+p1(3)+p2(3)+pc(3). Native.ptr==0 → silent (NO Mighty).
  // pc' = pc + pos[+0x78/+0x7C/+0x80]; pcy' −= (+0x38++0x3C). Store
  // len→[+0x254]; p1→[+0x258..+0x260]; p2→[+0x264..+0x26C];
  // pc'→[+0x270..+0x278]. If len==0 (exact): unit p2 if ‖p2‖≠0;
  // pc' −= p1. len≠0 (incl. −1 sentinel): store only. Twin setArm
  // @ 0x00441770: same pos/Y-bias on adjusted point.
  // Host hub[10] = PE block order; Y-bias slots absent → 0; pos from WR.
  if (!self) return;
  auto& w = WR_eff(self);
  float pc_x = pcx + (w.has_pos ? w.px : 0.f);
  float pc_y = pcy + (w.has_pos ? w.py : 0.f);  // PE also −(+0x38++0x3C)
  float pc_z = pcz + (w.has_pos ? w.pz : 0.f);
  w.hub[0] = len;
  w.hub[1] = p1x;
  w.hub[2] = p1y;
  w.hub[3] = p1z;
  w.hub[4] = p2x;
  w.hub[5] = p2y;
  w.hub[6] = p2z;
  w.hub[7] = pc_x;
  w.hub[8] = pc_y;
  w.hub[9] = pc_z;
  if (len == 0.f) {
    const float nlen =
        std::sqrt(w.hub[4] * w.hub[4] + w.hub[5] * w.hub[5] + w.hub[6] * w.hub[6]);
    if (nlen != 0.f) {
      const float inv = 1.f / nlen;
      w.hub[4] *= inv;
      w.hub[5] *= inv;
      w.hub[6] *= inv;
    }
    w.hub[7] -= w.hub[1];
    w.hub[8] -= w.hub[2];
    w.hub[9] -= w.hub[3];
  }
  w.has_hub = true;
}

}  // namespace inv
