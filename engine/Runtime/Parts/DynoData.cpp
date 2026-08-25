// Split from natives_generated_world.cpp — DynoData.cpp
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

// Host stand-ins for DynoSim slots on Native.ptr blob (PE 0x1EC).
// Cleared with g_dyno in newNative/deleteNative; filled in calcDyno.
static std::unordered_map<InvObject*, std::vector<float>> g_dyno_watts;  // +0x1E4
static std::unordered_map<InvObject*, int> g_dyno_rpm_step;              // +0x1D0

// PE @ 0x0046A960 DynoData.newNative()V size 0x59 (89).
// Java: DynoData extends Native; ctor DynoData() { newNative(); }.
// Flow (disasm): JVM_UnboxArg @ 0x0045D910 (this). Native.ptr =
//   JVM_vm_get_int_field(this, dword_62E008 @ 0x0062E008) @ 0x0042AB50.
//   test eax / jnz locret_46A9B8 — idempotent if ptr!=0 (NO Mighty ERROR).
//   Else push 0x1EC (492) → Engine_malloc @ 0x0054F560; jz → eax=0 (OOM);
//   else thiscall DynoSim_ctor @ 0x004B6600: DWORD* this+72 → +0x120=0;
//   loop zeros table ptrs +0x1DC/+0x1E0/+0x1E4/+0x1E8; this+118 → +0x1D8=0
//   (rest of blob uninitialized until calcDyno/DynoSim_resetDefaults).
//   Always JVM_vm_set_int_field @ 0x0042A9E0 (ctor ptr or 0 on OOM).
// Contrast deleteNative @ 0x0046A9C0: always clears Native.ptr.
// Xref data: Natives_Register_PartDyno @ 0x0046B8B9 push 0x46A960.
// dword_62E008 not renamed (high xref).
// Host: g_dyno entry = handle present; g_dyno_watts = +0x1E4; g_dyno_rpm_step =
//   +0x1D0 stand-ins.
// GAPS: no Native.ptr / 0x1EC blob / Engine_malloc OOM→set 0; host
//   always inserts; !self early-out (PE Unbox); DynoState{} value-inits
//   (PE ctor only nulls listed slots).
void java_game_parts_DynoData_newNative(InvObject* self) {
  if (!self) return;  // GAP: PE UnboxArg; host guard
  if (g_dyno.find(self) != g_dyno.end()) return;  // PE Native.ptr != 0
  // PE: malloc+ctor (or null OOM) → set_int_field; host always succeeds.
  g_dyno[self] = DynoState{};  // empty nm — DynoSim_ctor null +0x1DC tables
  g_dyno_watts[self] = {};     // PE +0x1E4 watts ptr null until calcDyno
  g_dyno_rpm_step[self] = 0;   // PE +0x1D0 until fillTables
}

// PE @ 0x0046A9C0 DynoData.deleteNative()V size 0x52 (82). Unbox this
// (JVM_UnboxArg @ 0x0045D910). Native.ptr via dword_62E008
// (JVM_vm_get_int_field @ 0x0042AB50). Xref data: Natives_Register_PartDyno
// @ 0x0046B8D8. test esi,esi / jz loc_46A9FA (NO Mighty ERROR). If handle!=0:
// thiscall DynoSim_dtor @ 0x004B6630 (free +0x1DC/+0x1E0/+0x1E4/+0x1E8) then
// Engine_free @ 0x0054F5B0 (0x1EC=492 blob). loc_46A9FA: ALWAYS
// JVM_vm_set_int_field @ 0x0042A9E0 (0) — even handle==0 (contrast newNative
// @ 0x0046A960 idempotent early-out if ptr!=0). Java finalize() → deleteNative.
// Host: g_dyno.nm = PE +0x1DC torque; g_dyno_watts = +0x1E4 watts;
// g_dyno_rpm_step = +0x1D0 (+0x1E0/+0x1E8 nitro tables unused on host
// getTorque/getHP). Handle present iff g_dyno entry.
void java_game_parts_DynoData_deleteNative(InvObject* self) {
  if (!self) return;
  if (g_dyno.find(self) != g_dyno.end()) {
    g_dyno_watts.erase(self);     // PE DynoSim_dtor @ 0x004B6630 (+0x1DC..+0x1E8)
    g_dyno_rpm_step.erase(self);  // PE +0x1D0 on freed blob
    g_dyno.erase(self);           // PE Engine_free @ 0x0054F5B0 (0x1EC=492 blob)
  }
  // PE loc_46A9FA: JVM_vm_set_int_field(Native.ptr, 0) even when handle==0.
}

// PE getTorque @ 0x0046B0F0 / getHP @ 0x0046B210 size 0x115: integer
// rpm_step walk + lerp. DynoSim +0x1D0 step, +0x1D4 count.
// RPM < 0 / n<=1 / past last bin → 0.0 (flt_5E73CC).
static float dyno_sample_table(int rpm_step, const std::vector<float>& table,
                               float rpm) {
  if (table.empty()) return 0.f;  // GAP: PE no null-check on table ptr
  if (rpm < 0.f) return 0.f;      // PE @ 0x0046B16D fcomp flt_5E73CC
  const int n = static_cast<int>(table.size());  // PE +0x1D4
  int i = 1;
  if (n <= 1) return 0.f;
  const int step = rpm_step;  // PE +0x1D0 (host: g_dyno_rpm_step)
  int accum = step;
  // PE @ 0x0046B1A3: while (accum <= RPM) { ++i; accum += step; if i>=n ret 0 }
  while (static_cast<float>(accum) <= rpm) {
    ++i;
    accum += step;
    if (i >= n) return 0.f;
  }
  // PE @ 0x0046B1CB: (RPM - i0*step) * (t[i]-t[i0]) / step + t[i0]
  // (span = (i-i0)*step == step when i0=i-1)
  const int i0 = i - 1;
  const float lo = table[static_cast<size_t>(i0)];
  const float hi = table[static_cast<size_t>(i)];
  const float rpm_lo = static_cast<float>(i0 * step);
  const float span = static_cast<float>(step);
  if (span == 0.f) return lo;  // GAP: PE fidiv 0 → #INF
  return (rpm - rpm_lo) * (hi - lo) / span + lo;
}

// PE @ 0x0046AA20 DynoData.calcDyno(F)F size 0x6c3 (1731).
// Unbox this+tablesize (JVM_UnboxArg @ 0x0045D910).
// Handle: Class_isInheritedFrom_desc("java.lang.Native") @ 0x004044E0 +
//   JVM_vm_get_int_field dword_62E008 @ 0x0042AB50. Fail or ptr==0 → var_C=1,
//   Engine_malloc(0x1EC=492) @ 0x0054F560 + DynoSim_ctor @ 0x004B6600 (else 0).
// Always DynoSim_resetDefaults @ 0x004B6670 on handle.
// Field dump (get_float / optional sub_42A800 present): cylinders→+0x14,
//   bore*0.5→+4, stroke*0.5→+0, Vmin→+0x10, in_min/max out_min/max,
//   time_in/out_open/close (+ derived +0xA4..+0xBC), time_burn, RPM_limit,
//   time_spark_*, rpm_turbo_*, P_turbo_max, P_turbo_waste (else = max),
//   mixture_H/ratio, max_air/fuel_consumption, +0x34 = 293.15f (0x43929333)
//   then *= intercooling if field present.
// DynoSim_updateMixture @ 0x004B6A80; DynoSim_calcGeometry @ 0x004B69A0
//   (+0x64 Displacement, +0x74 Compression, +0x8C default maxRPM).
// maxRPM: if Java >0 store +0x8C; else set_float maxRPM from +0x8C.
// rpm_step = maxRPM/tablesize; fcom 1.0: <=1|unordered → 1.0; no tablesize==0
//   guard (fdiv0 → +inf). (int)sub_5D6750 @ 0x005D6750; +0x1D8=0;
//   DynoSim_fillTables @ 0x004B77F0 (this+0x1D0=step, +0x1D4=n,
//   +0x1DC Nm table[0]=0 then combustion, +0x1E4 watts; peaks +0x1AC/+0x1B0).
// var_C temp: set table_stepsize, torque=1.0f (0x3F800000), copy Nm→torquetable
//   and watts→HPtable via sub_42B080 (stock Java has those arrays commented out).
// Always set Displacement/Compression/maxTorque/RPM_maxTorque/maxHP/RPM_maxHP
//   from blob +0x64/+0x74/+0x1AC/+0x1C0/+0x1B0/+0x1C4 (maxHP = watts*0.001341).
// Nitro: if nitro_H-1>1e-4 or nitro_cooling-1>1e-4 → scale mixture/+0x34,
//   updateMixture+calcGeometry, +0x1D8=1, fillTables again; persistent handle
//   early-ret 0.0; temp sets torque2=1.0 + torquetable2 from +0x1E0.
// Temp: nullsub_9 @ 0x004B68B0, DynoSim_dtor @ 0x004B6630, Engine_free.
// Always fld flt_5E73CC (0.0) @ 0x0046B0D5.
// GAPS (host): no DynoSim 0x1EC blob / resetDefaults / updateMixture /
//   fillTables combustion (sub_4B6BF0/sub_4B68C0/sub_4B7740) — sin stand-in
//   Nm/watts only; no cam/turbo/mixture/intercooling field dump; no nitro 2nd
//   pass; no temp torquetable/HPtable/table_stepsize/torque writes; tablesize==0
//   clamped (PE +inf); n<1 forced to 1; vmin==0 → Compression 0 (PE fdiv0).
float java_game_parts_DynoData_calcDyno(InvObject* self, float tablesize) {
  if (!self) return 0.f;  // PE derefs Unbox this; host guard

  // PE: inherited Native + Native.ptr; else temp 0x1EC. Host: g_dyno miss = temp.
  const bool had_handle = g_dyno.find(self) != g_dyno.end();

  const float cyl = tree_field_get_float(self, "cylinders");
  const float bore = tree_field_get_float(self, "bore");
  const float stroke = tree_field_get_float(self, "stroke");
  const float vmin = tree_field_get_float(self, "Vmin");
  // DynoSim_calcGeometry @ 0x004B69A0: r=bore/2, stroke full, Vd=π*r²*stroke.
  constexpr float kPi = 3.14159274f;  // flt used at 0x004B69E1
  const float vd_cyl = kPi * (bore * 0.5f) * (bore * 0.5f) * stroke;
  const float displ_m3 = vd_cyl * cyl;  // PE this+0x64
  float compression = 0.f;
  if (vmin != 0.f) compression = (vd_cyl + vmin) / vmin;  // PE this+0x74

  float max_rpm = tree_field_get_float(self, "maxRPM");
  if (max_rpm <= 0.f) {
    // PE else: set_float maxRPM from geometry this+0x8C =
    //   45.0 / (stroke/2) * 9.5492964
    const float half_stroke = stroke * 0.5f;
    max_rpm =
        half_stroke > 0.f ? 45.f / half_stroke * 9.5492964f : 0.f;
    tree_field_set_float(self, "maxRPM", max_rpm);
  }

  // PE: fld maxRPM; fdiv tablesize; fcom 1.0 → step; (int)sub_5D6750.
  float step_f = 1.f;
  if (tablesize != 0.f) {
    const float s = max_rpm / tablesize;
    if (s > 1.f) step_f = s;
  }
  int rpm_step = static_cast<int>(step_f);
  if (rpm_step < 1) rpm_step = 1;
  int n = static_cast<int>(max_rpm) / rpm_step;  // PE fillTables this+0x1D4
  if (n < 1) n = 1;  // GAP: PE may be 0

  // GAP: PE DynoSim_fillTables @ 0x004B77F0 — host sin stand-in for +0x1DC/+0x1E4.
  const float liters = displ_m3 * 1000.f;
  const float peak_nm = (liters > 0.2f ? liters : 2.f) * 85.f;
  DynoState st;
  st.max_rpm = max_rpm;
  st.steps = n;
  st.nm.resize(static_cast<size_t>(n));
  std::vector<float> watts_tbl(static_cast<size_t>(n));
  float best_t = 0.f, best_t_rpm = 0.f;
  float best_hp = 0.f, best_hp_rpm = 0.f;
  for (int i = 0; i < n; ++i) {
    const float rpm = static_cast<float>(i * rpm_step);
    const float x =
        n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.f;
    float shape = std::sin(kPi * std::pow(x <= 0.f ? 0.f : x, 1.8f));
    if (shape < 0.f) shape = 0.f;
    const float nm = peak_nm * shape;
    st.nm[static_cast<size_t>(i)] = nm;
    if (nm > best_t) {
      best_t = nm;
      best_t_rpm = rpm;
    }
    const float watts = nm * rpm * (kPi * 2.f / 60.f);
    watts_tbl[static_cast<size_t>(i)] = watts;  // PE +0x1E4 ← this+101
    const float hp = watts * 0.001341f;  // PE maxHP field ← watts*0.001341
    if (hp > best_hp) {
      best_hp = hp;
      best_hp_rpm = rpm;
    }
  }
  if (had_handle) {
    // PE persistent: keep blob tables. Temp (var_C): free after field copy.
    g_dyno[self] = std::move(st);
    g_dyno_watts[self] = std::move(watts_tbl);
    g_dyno_rpm_step[self] = rpm_step;  // PE fillTables this+0x1D0
  }
  // PE always (temp + persistent) — six stats only on stock Java.
  tree_field_set_float(self, "Displacement", displ_m3);
  tree_field_set_float(self, "Compression", compression);
  tree_field_set_float(self, "maxTorque", best_t);
  tree_field_set_float(self, "maxHP", best_hp);
  tree_field_set_float(self, "RPM_maxTorque", best_t_rpm);
  tree_field_set_float(self, "RPM_maxHP", best_hp_rpm);
  // GAP: nitro 2nd fillTables / torque2 — not hosted.
  return 0.f;  // PE @ 0x0046B0D5
}

// PE @ 0x0046B0F0 DynoData.getTorque(FF)F size 0x115 (277).
// Unbox this/RPM/nitro (JVM_UnboxArg @ 0x0045D910); nitro dest written then
// never read — no torque/torque2 Java multiply, no +0x1E0 nitro table.
// Class_isInheritedFrom_desc("java.lang.Native") @ 0x004044E0; else 0.0.
// Native.ptr = JVM_vm_get_int_field(dword_62E008) @ 0x0042AB50; null → 0.0.
// RPM < 0 → 0.0 (flt_5E73CC). n=+0x1D4; if n<=1 → 0.0.
// Walk: step=+0x1D0, accum=step, i=1; while accum<=RPM {++i; accum+=step;
//   if i>=n → 0.0}. Lerp float table +0x1DC only:
//   (RPM - (i-1)*step) * (t[i]-t[i-1]) / step + t[i-1].
// Xref data: Natives_Register_PartDyno @ 0x0046B916.
// GAPS: g_dyno vs Native.ptr / Class_isInheritedFrom; empty-nm guard (PE no
//   null-check on +0x1DC); !self (PE Unbox); step=0 host early lo (PE #INF);
//   Nm table from host calcDyno sin stand-in (not DynoSim_fillTables).
float java_game_parts_DynoData_getTorque(InvObject* self, float RPM,
                                        float nitro) {
  (void)nitro;  // PE unboxed, unused
  if (!self) return 0.f;  // GAP: PE UnboxArg
  auto it = g_dyno.find(self);
  if (it == g_dyno.end() || it->second.nm.empty()) return 0.f;  // PE ptr/table
  auto sit = g_dyno_rpm_step.find(self);
  const int step = sit != g_dyno_rpm_step.end() ? sit->second : 0;
  return dyno_sample_table(step, it->second.nm, RPM);
}

// PE @ 0x0046B210 DynoData.getHP(FF)F size 0x115 (277). Unbox this/RPM/nitro;
// nitro dest unused (same as getTorque). Lerp watts table this+0x1E4
// (DynoSim_fillTables this+101), not live Nm*omega. CarInfo: *0.001*1.341 → HP.
float java_game_parts_DynoData_getHP(InvObject* self, float RPM, float nitro) {
  (void)nitro;
  if (!self) return 0.f;
  auto it = g_dyno.find(self);
  auto wit = g_dyno_watts.find(self);
  if (it == g_dyno.end() || wit == g_dyno_watts.end() || wit->second.empty())
    return 0.f;
  auto sit = g_dyno_rpm_step.find(self);
  const int step = sit != g_dyno_rpm_step.end() ? sit->second : 0;
  return dyno_sample_table(step, wit->second, RPM);
}

}  // namespace inv
