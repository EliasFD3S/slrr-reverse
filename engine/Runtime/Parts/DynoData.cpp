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
// Chassis forceUpdate_apply @ 0x448773 C18 consumers (hdr+0xC18 → DynoSim*):
//   count dword[117]=+0x1D4, step dword[116]=+0x1D0 (as float x),
//   y0 dword[119]=+0x1DC, y1 dword[120]=+0x1E0 (null → dup y0 @ 0x448844),
//   meta dword[112]=+0x1C0 → hdr+0x10 (RPM_maxTorque after last fillTables).
static std::unordered_map<InvObject*, std::vector<float>> g_dyno_watts;       // +0x1E4
static std::unordered_map<InvObject*, std::vector<float>> g_dyno_nm_nitro;    // +0x1E0
static std::unordered_map<InvObject*, std::vector<float>> g_dyno_watts_nitro; // +0x1E8
static std::unordered_map<InvObject*, int> g_dyno_rpm_step;                   // +0x1D0
static std::unordered_map<InvObject*, float> g_dyno_c18_meta;                 // +0x1C0

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
// race121: Engine_malloc / DynoSim_ctor / JVM_vm_set_int_field renamed.
// Host: g_dyno entry = handle present; g_dyno_watts = +0x1E4; g_dyno_rpm_step =
//   +0x1D0; g_dyno_nm_nitro/+watts_nitro = +0x1E0/+0x1E8; g_dyno_c18_meta = +0x1C0.
// GAPS: no Native.ptr / 0x1EC blob / Engine_malloc OOM→set 0; host
//   always inserts; !self early-out (PE Unbox); DynoState{} value-inits
//   (PE ctor only nulls listed slots).
void java_game_parts_DynoData_newNative(InvObject* self) {
  // PE @ 0x0046A960 size 0x59. Unbox; Native.ptr!=0 early-out; else
  // Engine_malloc(0x1EC)+DynoSim_ctor → set_int_field (0 on OOM).
  if (!self) return;  // GAP: PE UnboxArg; host guard
  if (g_dyno.find(self) != g_dyno.end()) return;  // PE Native.ptr != 0
  // PE: malloc+ctor (or null OOM) → set_int_field; host always succeeds.
  g_dyno[self] = DynoState{};  // empty nm — DynoSim_ctor null +0x1DC tables
  g_dyno_watts[self] = {};     // PE +0x1E4 watts ptr null until calcDyno
  g_dyno_nm_nitro[self] = {};  // PE +0x1E0 null until nitro 2nd fillTables
  g_dyno_watts_nitro[self] = {};
  g_dyno_rpm_step[self] = 0;   // PE +0x1D0 until fillTables
  g_dyno_c18_meta[self] = 0.f; // PE +0x1C0 until fillTables peaks
}

// PE @ 0x0046A9C0 DynoData.deleteNative()V size 0x52 (82). Unbox this
// (JVM_UnboxArg @ 0x0045D910). Native.ptr via dword_62E008
// (JVM_vm_get_int_field @ 0x0042AB50). Xref data: Natives_Register_PartDyno
// @ 0x0046B8D8. test esi,esi / jz loc_46A9FA (NO Mighty ERROR). If handle!=0:
// thiscall DynoSim_dtor @ 0x004B6630 (free +0x1DC/+0x1E0/+0x1E4/+0x1E8) then
// Engine_free @ 0x0054F5B0 (0x1EC=492 blob). loc_46A9FA: ALWAYS
// JVM_vm_set_int_field @ 0x0042A9E0 (0) — even handle==0 (contrast newNative
// @ 0x0046A960 idempotent early-out if ptr!=0). Java finalize() → deleteNative.
// race121: DynoSim_dtor / Engine_free / JVM_vm_set_int_field renamed.
// Host: g_dyno.nm = PE +0x1DC torque; g_dyno_watts = +0x1E4 watts;
// g_dyno_rpm_step = +0x1D0; nitro banks +0x1E0/+0x1E8; c18_meta +0x1C0.
// getTorque/getHP lerp +0x1DC/+0x1E4 only (nitro unused — PE same for get*).
// Handle present iff g_dyno entry.
void java_game_parts_DynoData_deleteNative(InvObject* self) {
  // PE @ 0x0046A9C0 size 0x52. Unbox; if ptr: DynoSim_dtor+Engine_free;
  // ALWAYS set_int_field(0).
  if (!self) return;
  if (g_dyno.find(self) != g_dyno.end()) {
    g_dyno_watts.erase(self);        // PE DynoSim_dtor @ 0x004B6630 (+0x1DC..+0x1E8)
    g_dyno_nm_nitro.erase(self);     // PE +0x1E0
    g_dyno_watts_nitro.erase(self);  // PE +0x1E8
    g_dyno_rpm_step.erase(self);     // PE +0x1D0 on freed blob
    g_dyno_c18_meta.erase(self);     // PE +0x1C0
    g_dyno.erase(self);              // PE Engine_free @ 0x0054F5B0 (0x1EC=492 blob)
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
// Field dump (get_float / optional JVM_Object_hasField @ 0x0042A800):
//   cylinders→+0x14, bore*0.5→+4, stroke*0.5→+0, Vmin→+0x10,
//   in_min/max out_min/max, time_in/out_open/close (+ derived +0xA4..+0xBC),
//   time_burn→+0x1C, RPM_limit→+0x58, time_spark_*→+0xCC..+0xD8,
//   rpm_turbo_mul/opt/range→+0xDC/+0xE0/+0xE8, +0xE4:=0 @ 0x46ac82,
//   P_turbo_max→+0xEC, P_turbo_waste→+0xF0 (else=max via hasField),
//   mixture_H→+0x3C, mixture_ratio→+0xF4,
//   max_air_consumption→+0xFC, max_fuel_consumption→+0xF8 (Java 0=unlimited),
//   +0x34 = 293.15f (0x43929333) then *= intercooling if hasField.
//   Pin/Pout/dens stay resetDefaults (+0x2C=101325, +0x30=111457.5,
//   +0x24=1.2929); in_min/max out_min/max → +0x44/+0x4C/+0x48/+0x50.
//   NO "T_loss" string in PE dump — +0x54 stays resetDefaults 95 (0x42be0000)
//   (Java has T_loss=95 but calcDyno never reads it).
// DynoSim_updateMixture @ 0x004B6A80; DynoSim_calcGeometry @ 0x004B69A0
//   (+0x64 Displacement, +0x74 Compression, +0x8C default maxRPM).
// maxRPM: if Java >0 store +0x8C; else set_float maxRPM from +0x8C.
// rpm_step = maxRPM/tablesize; fcom 1.0: <=1|unordered → 1.0; no tablesize==0
//   guard (fdiv0 → +inf). (int)Engine_ftol @ 0x005D6750; +0x1D8=0;
//   DynoSim_fillTables @ 0x004B77F0 (this+0x1D0=step, +0x1D4=n,
//   +0x1DC Nm table[0]=0 then combustion, +0x1E4 watts; peaks +0x1AC/+0x1B0).
// var_C temp: set table_stepsize, torque=1.0f (0x3F800000), copy Nm→torquetable
//   and watts→HPtable via JVM_vm_set_float_array_elem @ 0x0042B080
//   (stock Java has those arrays commented out).
// Always set Displacement/Compression/maxTorque/RPM_maxTorque/maxHP/RPM_maxHP
//   from blob +0x64/+0x74/+0x1AC/+0x1C0/+0x1B0/+0x1C4 (maxHP = watts*0.001341).
// Nitro: if nitro_H-1>1e-4 or nitro_cooling-1>1e-4 → scale mixture/+0x34,
//   updateMixture+calcGeometry, +0x1D8=1, fillTables again; persistent handle
//   early-ret 0.0 (NO torque2); temp sets torque2=1.0 + torquetable2 from +0x1E0.
//   Peaks +0x1AC..+0x1C4 overwritten by 2nd pass; Java stats already from 1st.
//   C18 meta +0x1C0 after return = last fillTables RPM_maxTorque (nitro if any).
// Temp: nullsub_9 @ 0x004B68B0, DynoSim_dtor @ 0x004B6630, Engine_free.
// Always fld flt_5E73CC (0.0) @ 0x0046B0D5.
//
// OOS Native.ptr / DynoSim walks (need 0x1EC blob + P-V cycle):
//   DynoSim_prepCycle @ 0x004B6BF0 full mass/valve (+0x14C..+0x160)
//   DynoSim_resetCycleState @ 0x004B68C0
//   DynoSim_runCombustion @ 0x004B7740 → DynoSim_stepCombustion @ 0x004B7020
//   fillTables stabilize ×10 |Δmep|<0.05 blend 0.4/0.6 @ 0x4B7969
// Host soft: g_dyno.nm = +0x1DC, g_dyno_watts = +0x1E4, g_dyno_rpm_step =
//   +0x1D0, g_dyno_nm_nitro = +0x1E0, g_dyno_c18_meta = +0x1C0 —
//   Chassis forceUpdate C18 side-band (Chassis.cpp rebuild_lerp_from_dyno
//   reads g_dyno.nm + table_stepsize; +0x1E0 null → duplicate y0; hdr+0x10
//   still 0 until Chassis ticket consumes g_dyno_c18_meta).
// GAPS: no 0x1EC blob; fillTables P-V → sin + PE residuals; tablesize==0
//   clamped (PE +inf); n<1→1; vmin==0 → Compression 0 (PE fdiv0);
//   stabilize loop OOS (soft steady sin).
// W36-08: turbo deadzone (+0xE4=0) + T_loss=95 (PE no Java dump).
// W36-09 deepen: mixture fuel_frac + cond Pin/Pout/dens map corrected;
//   valve aperture (in/out min/max +0x44..+0x50); turbo dens*(P+1) AND
//   valve*sqrt(P+1); fillTables period/0.125 watts; RPM_limit no table clamp.
// W36-10 deepen: soft max_air/fuel_consumption caps (prepCycle @ 0x4B6F42 /
//   0x4B6F7E; meter≈dens*Vd*rpm/120 — PE meters +0x198/+0x19C from prior
//   runCombustion); soft Compression residual vs stock ~10.5 (+0x74).
// W36-11 deepen C18 side-band: soft nitro 2nd fillTables → +0x1E0/+0x1E8;
//   g_dyno_c18_meta = +0x1C0 (RPM_maxTorque last pass); torque2 only temp
//   (PE persistent early-ret @ 0x46af9a after 2nd fill).
// W36-12 deepen (PATH-TO-WORLD powertrain): nitro ambient reset @ 0x46af76
//   (+0x34:=293.15 before nitro_cooling — drop 1st-pass intercool); soft VE
//   = min*(1−duty)+max*duty (stepCombustion open→max / closed→min @ +0x44..
//   +0x50); table_stepsize/torque Java writes temp-only (PE @ 0x46adf3).
// W36-13 Soft gate (dyno_calc_host): turbo deadzone + T_loss closed behind
//   already-unboxed calcDyno(F)F. Ticket VA 0x470000 is NOT this fn
//   (Part_bindLodRenderSlots mid-body); real entry PE @ 0x0046AA20 size 0x6c3.
//   T_loss: PE string absent (find_regex 0); resetDefaults +0x54=0x42be0000 (95);
//   stepCombustion @ 0x4B764F fdiv dt(+0x84)/T_loss — Soft NEVER reads Java
//   "T_loss". Turbo: calcDyno @ 0x46ac82 mov [esi+0E4h],0; prepCycle @ 0x4B6E91
//   gate mul(+0xDC)>0 && range(+0xE8)>0. Native.ptr / 0x1EC P-V walks OOS.
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
  const float displ_m3 = vd_cyl * cyl;  // PE this+0x64 (= this+25 float idx)
  float compression = 0.f;
  if (vmin != 0.f) compression = (vd_cyl + vmin) / vmin;  // PE this+0x74

  float max_rpm = tree_field_get_float(self, "maxRPM");
  if (max_rpm <= 0.f) {
    // PE else: set_float maxRPM from geometry this+0x8C =
    //   45.0 / (stroke/2) * 9.5492964  (DynoSim_calcGeometry @ 0x4B6A0B)
    const float half_stroke = stroke * 0.5f;
    max_rpm =
        half_stroke > 0.f ? 45.f / half_stroke * 9.5492964f : 0.f;
    tree_field_set_float(self, "maxRPM", max_rpm);
  }
  // PE dumps RPM_limit → +0x58; fillTables span uses maxRPM +0x8C only
  // (this+35). +0x88 is overwritten per-bin with loop RPM — no table clamp.

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

  // PE temp-handle path @ 0x46adf3 only: set_float table_stepsize + torque=1.0
  // (0x3F800000). Persistent Native.ptr skips Java table writes (blob +0x1D0).
  // Chassis rebuild_lerp falls back to max_rpm/steps when table_stepsize<=0.
  if (!had_handle) {
    tree_field_set_float(self, "table_stepsize", static_cast<float>(rpm_step));
    tree_field_set_float(self, "torque", 1.f);
  }

  // PE +0x34 = 293.15f (0x43929333) then *= intercooling if
  // JVM_Object_hasField @ 0x46ad3d. Host: ambient scale on peak_nm.
  constexpr float kAmbientK = 293.15f;  // DynoSim_resetDefaults +0x34
  float ambient_k = kAmbientK;
  const float intercool = tree_field_get_float(self, "intercooling");
  if (intercool != 0.f) ambient_k *= intercool;

  // PE P_turbo_waste: JVM_Object_hasField @ 0x46aca8 → +0xF0 else = P_max.
  const float p_turbo = tree_field_get_float(self, "P_turbo_max");
  float p_waste = tree_field_get_float(self, "P_turbo_waste");
  if (p_waste == 0.f && p_turbo != 0.f) p_waste = p_turbo;

  // PE nitro gate @ 0x46af9a: nitro_H-1 > 1e-4 OR nitro_cooling-1 > 1e-4
  // → 2nd fillTables (+0x1D8=1, tables at +0x1E0/+0x1E8). Persistent early-ret
  // keeps +0x1DC from 1st pass; getTorque lerps +0x1DC only. C18 @ 0x448773
  // reads y1=+0x1E0 when non-null (else dup y0) + meta=+0x1C0 last peaks.
  const float nitro_h = tree_field_get_float(self, "nitro_H");
  const float nitro_cool = tree_field_get_float(self, "nitro_cooling");
  const bool nitro_pass =
      (nitro_h - 1.f > 1e-4f) || (nitro_cool - 1.f > 1e-4f);

  // DynoSim_fillTables @ 0x004B77F0: malloc Nm/watts, **[0]=0, then i=1..n-1
  // combustion (DynoSim_prepCycle→resetCycleState→runCombustion) writes
  // this+98 Nm / this+101 watts. Host: sin stand-in + PE residual scales.
  const float liters = displ_m3 * 1000.f;
  float peak_nm = (liters > 0.2f ? liters : 2.f) * 85.f;
  // PE dumps mixture_H → +0x3C (Java default 1e6 / resetDefaults same).
  const float mixture_h = tree_field_get_float(self, "mixture_H");
  if (mixture_h > 0.f) peak_nm *= mixture_h / 1000000.f;
  // DynoSim_updateMixture @ 0x004B6A80 / prepCycle head @ 0x4B6C05:
  //   mixture_ratio +0xF4 (Java/resetDefaults 14.0).
  //   air_frac = ratio/(ratio+1) @ +0x14C; fuel_frac = 1/(ratio+1) @ +0x150.
  // Soft heat ~ fuel_frac vs stoich 1/15 (AFR 14 → ratio+1=15).
  // Cond = P*9.8692326e-6*dens (flt_5F1510); resetDefaults dens+0x24=1.2929,
  //   Pin+0x2C=101325, Pout+0x30=111457.5 (NOT in_max — those are +0x4C/+0x50).
  //   calcDyno never dumps Pin/Pout/dens → cond stays defaults (no soft scale).
  const float mix_ratio = tree_field_get_float(self, "mixture_ratio");
  if (mix_ratio > 0.f) {
    const float fuel_frac = 1.f / (mix_ratio + 1.f);
    peak_nm *= (fuel_frac / (1.f / 15.f));  // == 15/(ratio+1)
  }
  // Ambient/intercool: cooler charge denser — peak *= (293.15/ambient).
  auto amb_scale = [&](float amb) {
    return amb > 1.f ? (kAmbientK / amb) : 1.f;
  };
  peak_nm *= amb_scale(ambient_k);
  // Soft Compression residual (calcGeometry +0x74). Stock Java defaults
  // bore/stroke/Vmin → ~10.505; PE MEP scales with Vmax/Vmin in P-V.
  constexpr float kStockComp = 10.5f;
  if (compression > 1.f) peak_nm *= (compression / kStockComp);
  // PE DynoSim_prepCycle @ 0x4B6E91 turbo (per-RPM, applied in loop below):
  //   gate: rpm_turbo_mul(+0xDC)>0 && rpm_turbo_range(+0xE8)>0 (no P_max test)
  //   dist = |RPM*mul - opt(+0xE0)|; dead = +0xE4 (calcDyno @ 0x46ac82 := 0)
  //   if dist >= dead: fade = 1 - (dist-dead)/(range-dead); else fade = 1
  //   clamp fade≥0; P = fade*P_max(+0xEC); if P≥waste(+0xF0) → waste
  //   dens(+0x158)*=(P+1); ambient(+0x15C)*=sqrt(P+1); cond(+0x160)*=sqrt.
  //   Soft Nm: *(P+1)*sqrt (dens+one slot); P=0 → identity (PE same).
  const float rpm_turbo_mul = tree_field_get_float(self, "rpm_turbo_mul");
  const float rpm_turbo_opt = tree_field_get_float(self, "rpm_turbo_opt");
  const float rpm_turbo_range = tree_field_get_float(self, "rpm_turbo_range");
  constexpr float kTurboDead = 0.f;  // PE +0xE4 hardcoded @ 0x46ac82
  // PE gate @ 0x4B6E91: mul>0 && range>0 only (P_max may be 0 → no-op scale).
  const bool turbo_gate = (rpm_turbo_mul > 0.f && rpm_turbo_range > 0.f);
  // PE dumps max_air_consumption→+0xFC, max_fuel_consumption→+0xF8
  // (Java 0 = unlimited). prepCycle @ 0x4B6F42 / 0x4B6F7E after turbo:
  //   if max>0 && meter>max → dens *= max/meter (fuel→+0x154, air→+0x158).
  // Soft meter (no prior-bin runCombustion): dens_eff*Vd*rpm/120
  //   (= dens_eff*Vd*(rpm*0.0083333338)); air/fuel via mixture_ratio.
  const float max_air_c =
      tree_field_get_float(self, "max_air_consumption");
  const float max_fuel_c =
      tree_field_get_float(self, "max_fuel_consumption");
  constexpr float kDens = 1.2929f;  // resetDefaults +0x24 (0x3fa57dbf)
  // Charge dens vs ambient (intercool); PE +0x34 feeds valve ambient slot.
  const float dens_base = kDens * amb_scale(ambient_k);
  // fillTables @ 0x4B791F: 1/period = rpm*0.0083333338 (= rpm/120).
  constexpr float kDtPeriod = 0.0083333338f;
  // time_burn → +0x1C (resetDefaults ~0.003); base peak scale (burn residual
  // per-RPM is separate — prepCycle tail @ 0x4B6FC2).
  const float time_burn = tree_field_get_float(self, "time_burn");
  const float time_burn_eff = time_burn > 0.f ? time_burn : 0.003f;
  peak_nm *= (time_burn_eff / 0.003f);
  // PE T_loss @ +0x54 = resetDefaults dword 0x42be0000 (95.f). calcDyno NEVER
  // dumps Java "T_loss" (PE string table: 0 hits) — Soft must ignore tree field
  // even when CylinderHead writes dd.T_loss. PE heat @ 0x4B764F:
  //   frac=min(1, dt(+0x84)/T_loss); ΔT heat on temp — Soft Nm residual only.
  // fillTables dt @ 0x4B794C: period*(0.0043290043), period=1/(rpm*0.0083333338).
  constexpr float kTLoss = 95.f;  // PE resetDefaults +0x54 — not Java T_loss
  // PE valve dump: in_min/max +0x44/+0x4C, out_min/max +0x48/+0x50
  // (resetDefaults 0.005/0.8). stepCombustion open window → max, else min.
  // Soft VE ≈ min*(1−duty)+max*duty (duty from time_* → +0xA8/+0xBC).
  // Stock in: 0.005*(1−0.249)+0.8*0.249 ≈ 0.202955; out ≈ 0.1958.
  const float in_min = tree_field_get_float(self, "in_min");
  const float in_max = tree_field_get_float(self, "in_max");
  const float out_min = tree_field_get_float(self, "out_min");
  const float out_max = tree_field_get_float(self, "out_max");
  const float tin_open = tree_field_get_float(self, "time_in_open");
  const float tin_close = tree_field_get_float(self, "time_in_close");
  const float tout_open = tree_field_get_float(self, "time_out_open");
  const float tout_close = tree_field_get_float(self, "time_out_close");
  constexpr float kStockInVe = 0.202955f;   // resetDefaults in weighted
  constexpr float kStockOutVe = 0.1958f;    // resetDefaults out weighted
  {
    const float in_lo = in_min > 0.f ? in_min : 0.005f;
    const float in_hi = in_max > 0.f ? in_max : 0.8f;
    const float out_lo = out_min > 0.f ? out_min : 0.005f;
    const float out_hi = out_max > 0.f ? out_max : 0.8f;
    // calcDyno derived +0xA8/+0xBC; skip if close<=open (no invent wrap).
    if (tin_close > tin_open) {
      const float duty = tin_close - tin_open;
      const float ve = in_lo * (1.f - duty) + in_hi * duty;
      if (ve > 0.f) peak_nm *= (ve / kStockInVe);
    }
    if (tout_close > tout_open) {
      const float duty = tout_close - tout_open;
      const float ve = out_lo * (1.f - duty) + out_hi * duty;
      if (ve > 0.f) peak_nm *= (ve / kStockOutVe);
    }
  }
  // prepCycle spark lerp @ 0x4B6C4A → +0x90 (time_spark_* dump +0xCC..+0xD8).
  const float spark_min = tree_field_get_float(self, "time_spark_min");
  const float spark_inc = tree_field_get_float(self, "time_spark_inc");
  const float spark_rpm0 = tree_field_get_float(self, "time_spark_RPM0");
  const float spark_rpm1 = tree_field_get_float(self, "time_spark_RPM1");
  const float spark_base = spark_min > 0.f ? spark_min : 0.48f;
  // PE fillTables @ 0x4B791F..0x4B794C: period=1/(rpm*0.0083333338),
  // ω=rpm*0.10471976 (flt_5F0EF0), dt=period*0.0043290043.
  // stepCombustion @ 0x4B70DD: Nm=(Vmax−Vmin)*mep*cyl*0.125 (flt_5F1540);
  // grossW=(Vmax−Vmin)*(rpm/60)*mep*cyl → watts=nm*rpm/7.5 − rpm³*2e-8.
  constexpr float kWattsFromNmRpm = 1.f / 7.5f;  // (1/60)/0.125
  constexpr float kRpm3Parasitic = 1.99999999e-8f;  // flt_5F1538
  // resetDefaults +0x20 ≈ 5e-5 — prepCycle mixture/burn RPM term @ 0x4B6FC2.
  constexpr float kMixRpmK = 4.99999987e-5f;
  constexpr float kDtScale = 0.0043290043f;   // fillTables this+33

  // Soft DynoSim_fillTables body (sin stand-in). Reused for nitro bank1.
  // PE peaks: maxTorque+0x1AC / RPM_maxTorque+0x1C0 track mep(+0x184) bin
  // (disasm 0x4B7A26); host tracks Nm — same bin under soft shape.
  auto soft_fill_tables = [&](float peak, float dens, std::vector<float>& nm_out,
                              std::vector<float>& watts_out, float& out_t,
                              float& out_t_rpm, float& out_hp,
                              float& out_hp_rpm) {
    nm_out.assign(static_cast<size_t>(n), 0.f);
    watts_out.assign(static_cast<size_t>(n), 0.f);
    out_t = out_t_rpm = out_hp = out_hp_rpm = 0.f;
    // PE @ 0x4B78CE..0x4B78DD: Nm[0]=0, watts[0]=0 before combustion loop.
    for (int i = 1; i < n; ++i) {
      const float rpm = static_cast<float>(i * rpm_step);
      const float x =
          n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.f;
      // GAP: PE P-V cycle (DynoSim_stepCombustion) — sin stand-in for +0x188 Nm.
      // GAP: fillTables stabilize ×10 |Δmep|<0.05 @ 0x4B7969 — soft steady.
      float shape = std::sin(kPi * std::pow(x <= 0.f ? 0.f : x, 1.8f));
      if (shape < 0.f) shape = 0.f;
      float nm = peak * shape;
      // Per-RPM turbo envelope (DynoSim_prepCycle @ 0x4B6E91) — PE deadzone=0.
      float p_boost = 0.f;
      if (turbo_gate) {
        const float dist = std::fabs(rpm * rpm_turbo_mul - rpm_turbo_opt);
        float fade;
        if (dist >= kTurboDead) {
          const float denom = rpm_turbo_range - kTurboDead;
          fade = denom > 0.f ? (1.f - (dist - kTurboDead) / denom) : 0.f;
          if (fade < 0.f) fade = 0.f;
        } else {
          fade = 1.f;
        }
        float p = fade * p_turbo;  // PE +0xEC; 0 → identity dens/ambient
        if (p >= p_waste) p = p_waste;  // PE +0xF0 waste clamp
        p_boost = p;
        const float sp1 = std::sqrt(1.f + p);
        nm *= (sp1 * sp1);  // PE dens(+0x158) *= (P+1)
        nm *= sp1;          // Soft one-slot for ambient(+0x15C)/cond(+0x160)
      }
      // Soft max_*_consumption (prepCycle @ 0x4B6F42 fuel / 0x4B6F7E air).
      if ((max_air_c > 0.f || max_fuel_c > 0.f) && displ_m3 > 0.f &&
          rpm > 0.f) {
        const float dens_eff = dens * (1.f + p_boost);
        const float meter_air = dens_eff * displ_m3 * (rpm * kDtPeriod);
        if (max_air_c > 0.f && meter_air > max_air_c)
          nm *= (max_air_c / meter_air);
        if (max_fuel_c > 0.f && mix_ratio > 0.f) {
          const float meter_fuel = meter_air / mix_ratio;
          if (meter_fuel > max_fuel_c) nm *= (max_fuel_c / meter_fuel);
        }
      }
      // Mixture/burn residual @ 0x4B6FC2: (1-min(0.9,RPM*+0x20))*time_burn.
      {
        float v = rpm * kMixRpmK;
        if (v > 0.9f) v = 0.9f;
        const float burn_res = (1.f - v) * time_burn_eff;
        nm *= (burn_res / time_burn_eff);  // == (1-v); high-RPM falloff
      }
      // Spark timing residual (prepCycle @ 0x4B6C4A).
      {
        float spark = spark_base;
        const float span = spark_rpm1 - spark_rpm0;
        if (span > 0.f) {
          float t = (rpm - spark_rpm0) / span;
          if (t < 0.f) t = 0.f;
          else if (t > 1.f) t = 1.f;
          spark = spark_base + t * spark_inc;
        }
        if (spark_base > 0.f) nm *= (spark / spark_base);
      }
      // T_loss Soft residual: PE @ 0x4B764F heat on temp (dt/T_loss);
      // Soft applies frac to Nm (no P-V temp state). kTLoss=95 PE-only.
      if (rpm > 0.f) {
        const float period = 1.f / (rpm * kDtPeriod);  // PE +0x124 @ 0x4B791F
        const float dt_step = period * kDtScale;         // PE +0x84 @ 0x4B794C
        float loss_frac = dt_step / kTLoss;              // PE fdiv +0x54
        if (loss_frac > 1.f) loss_frac = 1.f;
        nm *= (1.f - loss_frac);
      }
      if (nm < 0.f) nm = 0.f;  // PE temp array copy clamps ≤0 → 0
      nm_out[static_cast<size_t>(i)] = nm;
      if (nm > out_t) {
        out_t = nm;
        out_t_rpm = rpm;
      }
      // PE fillTables stores +0x194 watts; peak HP *0.001341 @ 0x4B7A00.
      float watts = nm * rpm * kWattsFromNmRpm;
      if (rpm > 0.f) {
        const float rpm2 = rpm * rpm;
        watts -= rpm2 * rpm * kRpm3Parasitic;  // PE +0x180 parasitic
      }
      if (watts < 0.f) watts = 0.f;
      watts_out[static_cast<size_t>(i)] = watts;
      const float hp = watts * 0.001341f;
      if (hp > out_hp) {
        out_hp = hp;
        out_hp_rpm = rpm;
      }
    }
  };

  DynoState st;
  st.max_rpm = max_rpm;
  st.steps = n;
  std::vector<float> watts_tbl;
  float best_t = 0.f, best_t_rpm = 0.f;
  float best_hp = 0.f, best_hp_rpm = 0.f;
  soft_fill_tables(peak_nm, dens_base, st.nm, watts_tbl, best_t, best_t_rpm,
                   best_hp, best_hp_rpm);
  // PE +0x1C0 after 1st fill; nitro 2nd pass overwrites blob peaks (Java
  // stats already snapped below from pass 1).
  float c18_meta = best_t_rpm;

  // PE always (temp + persistent) — six stats from 1st fillTables only.
  tree_field_set_float(self, "Displacement", displ_m3);
  tree_field_set_float(self, "Compression", compression);
  tree_field_set_float(self, "maxTorque", best_t);
  tree_field_set_float(self, "maxHP", best_hp);
  tree_field_set_float(self, "RPM_maxTorque", best_t_rpm);
  tree_field_set_float(self, "RPM_maxHP", best_hp_rpm);

  std::vector<float> nm_nitro;
  std::vector<float> watts_nitro;
  if (nitro_pass) {
    // PE @ 0x46af76: +0x34 := 293.15 then @ 0x46af9a: mixture_H*=nitro_H
    // (+0x3C); +0x34*=nitro_cooling (intercool from 1st pass discarded);
    // updateMixture+calcGeometry; keep maxRPM; +0x1D8=1; fillTables bank1.
    const float nc = nitro_cool > 0.f ? nitro_cool : 1.f;
    const float amb2 = kAmbientK * nc;  // PE reset ambient before cooling
    const float peak2 =
        peak_nm / amb_scale(ambient_k) * nitro_h * amb_scale(amb2);
    const float dens2 = kDens * amb_scale(amb2);
    float n_t = 0.f, n_t_rpm = 0.f, n_hp = 0.f, n_hp_rpm = 0.f;
    soft_fill_tables(peak2, dens2, nm_nitro, watts_nitro, n_t, n_t_rpm, n_hp,
                     n_hp_rpm);
    c18_meta = n_t_rpm;  // PE last fillTables → +0x1C0 (C18 hdr+0x10)
    (void)n_t;
    (void)n_hp;
    (void)n_hp_rpm;
    // PE persistent: early-ret 0.0 — NO torque2. Temp only: torque2=1.0.
    if (!had_handle) tree_field_set_float(self, "torque2", 1.f);
  }

  if (had_handle) {
    // PE persistent: keep blob tables. Temp (var_C): free after field copy.
    // C18 side-band: +0x1DC nm, +0x1E0 nitro, +0x1D0 step, +0x1C0 meta.
    g_dyno[self] = std::move(st);
    g_dyno_watts[self] = std::move(watts_tbl);
    g_dyno_rpm_step[self] = rpm_step;  // PE fillTables this+0x1D0
    g_dyno_c18_meta[self] = c18_meta;  // PE +0x1C0 after last fillTables
    if (nitro_pass) {
      g_dyno_nm_nitro[self] = std::move(nm_nitro);      // PE +0x1E0
      g_dyno_watts_nitro[self] = std::move(watts_nitro);  // PE +0x1E8
    } else {
      g_dyno_nm_nitro[self] = {};
      g_dyno_watts_nitro[self] = {};
    }
  }
  return 0.f;  // PE @ 0x0046B0D5
}

// PE @ 0x0046B0F0 DynoData.getTorque(FF)F size 0x115 (277).
// Unbox this/RPM/nitro (JVM_UnboxArg @ 0x0045D910): dest layout var_10=RPM,
// var_C=this, var_8=0.0 ret scratch, var_4=nitro. nitro dest written then
// never read — no torque/torque2 Java multiply, no +0x1E0 nitro table.
// Class_isInheritedFrom_desc("java.lang.Native") @ 0x004044E0; else 0.0.
// Native.ptr = JVM_vm_get_int_field(dword_62E008) @ 0x0042AB50; null → 0.0.
// RPM < 0 → 0.0 (flt_5E73CC @ 0x46b16d fcomp). n=+0x1D4; if n<=1 → 0.0.
// Walk @ 0x46b1a3: step=+0x1D0, accum=step, i=1; while accum<=RPM (fcomp
// test ah,41h) {++i; accum+=step; if i>=n → 0.0}. Lerp +0x1DC only:
//   (RPM - (i-1)*step) * (t[i]-t[i-1]) / step + t[i-1]
//   (disasm 0x46b1cb..0x46b1fe; Hex-Rays garbles fsubr).
// Xref data: Natives_Register_PartDyno @ 0x0046B916.
// GAPS: g_dyno vs Native.ptr / Class_isInheritedFrom; empty-nm guard (PE no
//   null-check on +0x1DC); !self (PE Unbox); step=0 host early lo (PE #INF);
//   Nm table from host calcDyno (sin + PE residuals; not full P-V cycle).
float java_game_parts_DynoData_getTorque(InvObject* self, float RPM,
                                        float nitro) {
  // PE @ 0x0046B0F0 size 0x115. Unbox this/RPM/nitro(unused); Native.ptr
  // lerp +0x1DC table via +0x1D0 step / +0x1D4 count.
  (void)nitro;  // PE unboxed @ var_4, unused
  if (!self) return 0.f;  // GAP: PE UnboxArg
  auto it = g_dyno.find(self);
  if (it == g_dyno.end() || it->second.nm.empty()) return 0.f;  // PE ptr/table
  auto sit = g_dyno_rpm_step.find(self);
  const int step = sit != g_dyno_rpm_step.end() ? sit->second : 0;
  return dyno_sample_table(step, it->second.nm, RPM);
}

// PE @ 0x0046B210 DynoData_getHP(FF)F size 0x115 (277). Unbox this/RPM/nitro;
// nitro dest unused (same as getTorque). Lerp watts table this+0x1E4
// (DynoSim_fillTables this+101 / stepCombustion +0x194), not live Nm*omega.
// CarInfo: *0.001*1.341 → HP.
float java_game_parts_DynoData_getHP(InvObject* self, float RPM, float nitro) {
  // PE @ 0x0046B210 size 0x115 (277). Unbox this/RPM/nitro; nitro unused;
  // lerp watts table this+0x1E4 (not Nm*omega). Host g_dyno_watts sample.
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
