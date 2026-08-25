// Split from natives_generated_world.cpp — Chassis.cpp
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

#include "../world_state.hpp"

namespace inv {

// PE @ 0x0043C7F0 size 0x138 (IDA java_game_parts_bodypart_Chassis_getTorque).
float java_game_parts_bodypart_Chassis_getTorque(InvObject* self, float RPM,
                                                 float boost) {
  // Unbox this+RPM+boost (acc0/rpm/boost preset 0). Native.ptr
  // (dword_62E008)==0 → fld acc0 (0). NO Mighty. Walk=setTorque @
  // 0x0043C930: inner=*(handle+0xC); 0 → 0.0. [inner+0x4C]!=1 →
  // vtbl+0x14(0). sub_5447D0(0xA0000000 bytes 00 00 00 a0) test
  // 80000000h sign → 0.0. vtbl+0xC(1.0f=0x3F800000). second=*(obj+0x44),
  // edi=*(obj+0x4C). sub_5447D0(0x20000000) sign → 0.0; 2nd vtbl+0xC
  // null-check only. hdr=*(edi+0x1FBC) (int_convert 8124).
  // t0=lerp_xy_table0(RPM,hdr) @ 0x0043BE70 size 0x70: count[+0]
  // pairs[+4] stride 8; x<first/count<=1/past last → 0.
  // t1=lerp_xy_table1(RPM,hdr) @ 0x0043BEE0 size 0x71: count[+8]
  // pairs[+0xC]. fld flt_5F08F0 (1.0) fsub boost (unclamped).
  // return t0*(1-boost)*[edi+0x1DD0]+t1*boost*[edi+0x1DD4]
  // (int_convert 7632/7636; scales WRITE by setTorque wear).
  // Miss → fld acc0=0. Callees: JVM_UnboxArg @ 0x0045D910,
  // JVM_vm_get_int_field @ 0x0042AB50, sub_5447D0 @ 0x005447D0,
  // lerp_xy_table0/1. Xref: Natives_Register_Partial @ 0x004427E6.
  // NOT DynoData.getTorque @ 0x0046B0F0 (other FQN).
  // GAP: PE miss → 0.0; host TREE peaks (engine_torque/engine_torque2 —
  // setTorque twin) with curve≡1 (no +0x1FBC / lerp_xy / wear +0x1DD0).
  // DynoData only if peaks unset (host-only; not PE Chassis).
  if (!self) return 0.f;
  const float s0 = tree_field_get_float(self, "engine_torque");
  const float s1 = tree_field_get_float(self, "engine_torque2");
  if (s0 != 0.f || s1 != 0.f) {
    (void)RPM;  // PE: lerp_xy_table0/1(RPM); host curve≡1
    return (1.f - boost) * s0 + boost * s1;
  }
  InvObject* engine = tree_field_get_obj(self, "engine");
  InvObject* dyno =
      engine ? tree_field_get_obj(engine, "dynodata") : nullptr;
  if (!dyno) dyno = tree_field_get_obj(self, "dynodata");
  if (dyno) return java_game_parts_DynoData_getTorque(dyno, RPM, boost);
  (void)RPM;
  return 0.f;
}

// PE @ 0x0043C930 size 0x16d (IDA Chassis_setTorque).
void java_game_parts_bodypart_Chassis_setTorque(InvObject* self, float t) {
  // Unbox this + F. var_4 preset 1.0 (0x3F800000 bytes 00 00 80 3F) then
  // dest1. Native.ptr (dword_62E008)==0 → jz loc_43CA99 ret. NO Mighty.
  // Callees: JVM_UnboxArg @ 0x0045D910, JVM_vm_get_int_field @ 0x0042AB50,
  // sub_5447D0 @ 0x005447D0 (×2). Xref: Natives_Register_Partial @ 0x00442805.
  // inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0xA0000000) test 80000000h sign → ret. vtbl+0xC(1.0f).
  // second=*(obj+0x44), edi=*(obj+0x4C). [second+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0x20000000) sign → ret; 2nd vtbl+0xC null-check only.
  // hdr=*(edi+0x1FBC); *(hdr+0xD0)=*(hdr+0xD4)=var_4 (=t; Hex-rays
  // folded 1.0 — disasm mov from var_4 @ 0x43CA05/0x43CA0F).
  // wear=*(float*)(edi+0x1E38) fcom flt_5F08F0 (1.0 bytes 00 00 80 3F).
  // C0 (wear<1.0): k=1.0-wear*wear*flt_5F0F00 (0.6 bytes 9a 99 19 3f);
  // else k=flt_5F0EFC (0.1 bytes d0 cc cc 3d).
  // *(edi+0x1DD0)=k*[hdr+0xD0]; *(edi+0x1DD4)=k*[hdr+0xD4].
  // Contrast getTorque @ 0x0043C7F0: same walk then READS +0x1DD0/+0x1DD4
  // via lerp_xy_table0/1 (sub_43BE70/sub_43BEE0).
  // GAP: PE miss → silent ret; host TREE peaks (Java engine_torque /
  // engine_torque2 — no +0x1FBC hdr / wear scale / +0x1DD0 graph).
  if (!self) return;
  tree_field_set_float(self, "engine_torque", t);
  tree_field_set_float(self, "engine_torque2", t);
}

// PE @ 0x00440B70 size 0x80 (IDA Chassis.setAckermann).
void java_game_parts_bodypart_Chassis_setAckermann(InvObject* self, float a) {
  // Unbox this + F (var_4). Native.ptr (dword_62E008)==0 → jz loc_440BED
  // ret. NO Mighty. inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 →
  // vtbl+0x14(0). sub_5447D0(0xA0000000 bytes 00 00 00 a0) test
  // 80000000h sign → ret. vtbl+0xC(1.0f=0x3F800000 bytes 00 00 80 3F).
  // ecx=*(obj+0x4C); edx=*(ecx+0x1FBC); *(edx+0xA1C)=F.
  // Twin setSteerWheelRadius @ 0x00440AF0 size 0x80: same walk, store
  // +0xA14; setSteerWheel (FF) @ 0x00440A50 also writes +0xA18.
  // GAP: PE miss → silent ret; host TREE (no +0x1FBC phys graph).
  if (!self) return;
  tree_field_set_float(self, "ackermann", a);
}

// PE @ 0x0043CAA0 size 0xD5 (IDA Chassis_getMass).
float java_game_parts_bodypart_Chassis_getMass(InvObject* self) {
  // Unbox this. Native.ptr (dword_62E008)==0 → fld var_4 (0). NO Mighty.
  // inner=*(handle+0xC); 0 → 0.0. [inner+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0xA0000000 bytes 00 00 00 a0) sign → 0.0.
  // vtbl+0xC(1.0f=0x3F800000). second=*(obj+0x44), edi=*(obj+0x4C).
  // sub_5447D0(0x20000000). return flt_5F08F0 (bytes 00 00 80 3F = 1.0)
  // fdiv dword [*(*(*(edi+0x13BC))+0x5C)+0x14] (inv_mass → kg).
  // Contrast Part.getMass @ 0x00469290: same +0x14 fdiv via sub_48AEA0
  // plus *(float*)(+0x14)>0.0 else 0. Chassis has no >0 check.
  // Units: CarInfo prints getMass() as kg (*2.2 lb). Host TREE mass is
  // kg (no 1/x — host has no +0x13BC physics graph).
  // GAP: PE miss → 0.0; host 1200 boot stand-in (CarInfo / smoke mass0).
  if (!self) return 0.f;
  float m = tree_field_get_float(self, "mass");
  if (m <= 0.f) m = tree_field_get_float(self, "chassis_mass");
  return m > 0.f ? m : 1200.f;
}

// Phase 2.74: AABB + CM from wheel poses (CarInfo length/width); physics fallback.
static void chassis_bounds(InvObject* self, float mn[3], float mx[3],
                           float cm[3]) {
  mn[0] = mn[1] = mn[2] = 1.0e9f;
  mx[0] = mx[1] = mx[2] = -1.0e9f;
  float sx = 0.f, sy = 0.f, sz = 0.f;
  int n = 0;
  for (int32_t i = 0; i < 4; ++i) {
    float px = 0, py = 0, pz = 0;
    float r = 0.32f;
    InvObject* w = java_game_parts_bodypart_Chassis_getWheel(self, i);
    if (w) {
      if (InvObject* p = java_game_parts_WheelRef_getPos(w))
        vec3_get(p, &px, &py, &pz);
      r = java_game_parts_WheelRef_getRadius(w);
      if (r < 0.05f) r = 0.32f;
    } else if (!part_slot_get_pose(self, 101 + i, &px, &py, &pz, nullptr,
                                   nullptr, nullptr)) {
      continue;
    }
    if (px - r < mn[0]) mn[0] = px - r;
    if (py - r < mn[1]) mn[1] = py - r;
    if (pz - r < mn[2]) mn[2] = pz - r;
    if (px + r > mx[0]) mx[0] = px + r;
    if (py + r > mx[1]) mx[1] = py + r;
    if (pz + r > mx[2]) mx[2] = pz + r;
    sx += px;
    sy += py;
    sz += pz;
    ++n;
  }
  if (n == 0) {
    float hx = 1.f, hy = 0.5f, hz = 2.f;
    physics_extents(self, &hx, &hy, &hz);
    if (hx < 0.1f) hx = 1.f;
    if (hy < 0.1f) hy = 0.5f;
    if (hz < 0.1f) hz = 2.f;
    mn[0] = -hx;
    mn[1] = -hy;
    mn[2] = -hz;
    mx[0] = hx;
    mx[1] = hy;
    mx[2] = hz;
    cm[0] = cm[1] = cm[2] = 0.f;
  } else {
    const float inv = 1.f / static_cast<float>(n);
    cm[0] = sx * inv;
    cm[1] = sy * inv;
    cm[2] = sz * inv;
  }
  // Optional script overrides.
  if (tree_field_get_float(self, "cm_set") > 0.5f) {
    cm[0] = tree_field_get_float(self, "cm_x");
    cm[1] = tree_field_get_float(self, "cm_y");
    cm[2] = tree_field_get_float(self, "cm_z");
  }
}

InvObject* java_game_parts_bodypart_Chassis_getCM(InvObject* self) {
  // PE @ 0x0043CB80 size 0x1ef. Unbox this. Native.ptr (dword_62E008)==0 →
  // var_C/8/4 stay 0.0; still alloc Vector3. NO Mighty ERROR (jz
  // loc_43CCF2). inner=*(handle+0xC); 0 → zeros. Else
  // sub_419860(inner, 0xA0000000 bytes 00 00 00 a0). second=
  // *[eax+0x44], edi=*[eax+0x4C]; sub_419860(0x20000000). esi=
  // *[eax+0xC]. Slot CM: fld [esi+0x1EAC/0x1EB0/0x1EB4] fmul
  // flt_5F0C70 (bytes 00 00 80 bf = -1.0). Then sub_4A6F30 /
  // [ebx+0x13BC]+0x20C stride / fchs / fsub [+0x40]. Any jz →
  // Vector3(0,0,0). !self = handle 0. GAP: PE miss → (0,0,0);
  // host AABB / cm_set (CarInfo / smoke).
  float mn[3], mx[3], cm[3];
  if (!self) return vec3_new(0, 0, 0);
  chassis_bounds(self, mn, mx, cm);
  return vec3_new(cm[0], cm[1], cm[2]);
}

InvObject* java_game_parts_bodypart_Chassis_getMin(InvObject* self) {
  float mn[3], mx[3], cm[3];
  if (!self) return vec3_new(0, 0, 0);
  chassis_bounds(self, mn, mx, cm);
  return vec3_new(mn[0], mn[1], mn[2]);
}

InvObject* java_game_parts_bodypart_Chassis_getMax(InvObject* self) {
  // PE @ 0x0043D8B0 size 0x2a9. Unbox this. Native.ptr==0 → Vector3(0,0,0)
  // (var_18/14/10 stay 0). NO Mighty (jz loc_43DADE). Twin getMin @
  // 0x0043D600: min verts + 0.1. Origin [esi+0x1EAC..] NO fmul -1; mesh
  // AABB *(*(node+0x5C)+0x78) then origin+max-0.1. Contrast getCM @
  // 0x0043CB80: slot*-1, no vertex loop. Host chassis_bounds = wheels±r
  // (not mesh AABB). Body 0x2a9 not ported.
  float mn[3], mx[3], cm[3];
  if (!self) return vec3_new(0, 0, 0);
  chassis_bounds(self, mn, mx, cm);
  return vec3_new(mx[0], mx[1], mx[2]);
}

// PE @ 0x0043CE30 size 0x244 — Chassis.getWheelPos(I)Ljava.lang.Vector3;
InvObject* java_game_parts_bodypart_Chassis_getWheelPos(InvObject* self,
                                                       int32_t n) {
  // Unbox this+n (var_1C/var_20). xyz init 0. Native.ptr
  // (dword_62E008)==0 → still Engine_malloc Vector3(0,0,0). NO Mighty.
  // NO +0x1F40 count check (contrast getWheel @ 0x00440C80: id>=count
  // → null; id<0 → null). Dual Res via sub_419860(0xA0000000 then
  // 0x20000000) — getWheel inlines same walk with sub_5447D0.
  // veh=[node+0x4C], data=[child+0xC]. Load CM @ data+0x1EAC; if scene
  // [+0x84] flags&4 walk parent mats ([+0x1C]/[+0x20]) until
  // [veh+0x13BC]. Then += wheel[n] local @ *[veh+0x13E4]+n*0x2B4+0x78
  // (lea idx*173 dwords; ASM @ 0x43CFB9). ALWAYS Vector3 — never null
  // (fail → 0,0,0). NOT getWheel+WheelRef.getPos (race110 @ 0x00441B60:
  // channel-6 slot +0x1F44, ptr==0 → null; CarInfo uses whl.getPos
  // //chas.getWheelPos). Callees: UnboxArg, vm_get_int_field,
  // sub_419860, sub_48AEA0, Engine_malloc, JVM_getClass,
  // Instance_initialize, vm_set_float_field.
  // GAP: PE CM+xforms+phys +0x78; host slot 101+n pose (no +0x13BC /
  // +0x1EAC graph). Hard n>3 stand-in (PE indexes freely). !self /
  // miss → (0,0,0).
  if (!self || n < 0 || n > 3) return vec3_new(0, 0, 0);
  float px = 0, py = 0, pz = 0;
  if (part_slot_get_pose(self, 101 + n, &px, &py, &pz, nullptr, nullptr,
                         nullptr))
    return vec3_new(px, py, pz);
  return vec3_new(0, 0, 0);
}

void java_game_parts_bodypart_Chassis_forceUpdate(InvObject* self) {
  if (!self) return;
  // Chassis.load sets suspend_update=1; scripts clear via forceUpdate.
  tree_field_set_int(self, "suspend_update", 0);
  const int32_t n = tree_field_get_int(self, "force_update_count");
  tree_field_set_int(self, "force_update_count", n + 1);
  // Refresh rim/tyre visual pose from slots + current WheelRef steer.
  valocity_sync_wheel_visuals(self, 0.016f);
}

// PE @ 0x0043D080 size 0x1F2 (498) — Chassis.getWheelDamage(I)Ljava.lang.String;
// Twin setWheelDamage @ 0x0043D280 size 0x37C (sscanf/apply). Java save/load:
// while(wheels--) write(getWheelDamage(wheels)).
InvObject* java_game_parts_bodypart_Chassis_getWheelDamage(InvObject* self,
                                                          int32_t index) {
  // JVM_UnboxArg(this+I). dst[256]: [0]=byte_63C7B0 (0), memset rest.
  // index < 0 (test/jge @ 0x43D0CB) → null (not "").
  // JVM_vm_get_int_field(this, dword_62E008 Native.ptr)==0 → loc_43D25D "".
  // inner=*(handle+0xC); 0 → "". [inner+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0xA0000000 bytes 00 00 00 a0); test eax 80000000h → "".
  // vtbl+0xC(1.0f=0x3F800000). obj==0 → "". second=*(obj+0x44), edi=*(obj+0x4C).
  // sub_5447D0(0x20000000); sign → "". vtbl+0xC(1.0f). node=*(obj+0xC).
  // cmp index,[node+0x1F40]; jg @ 0x43D195 → null (contrast getWheel @ 0x00440C80:
  // jge id>=count). slot=*(edi+0x13E4)+index*0x2B4 (692=173*4 lea @ 0x43D19D).
  // ([slot+0x100]&0x20)==0 @ 0x43D1BC → "". Else Util_Sprintf(dst,
  // "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f" @ 0x60E500):
  // slot+0x78/+0x7C/+0x80, Ypr_fromMatrix(+0x90), slot+0xF4(f6), 1.0f stub
  // f7, 1.0f stub f8 (set sscanf defaults; get never reads f7/f8 slots),
  // slot+0xF8(f9). loc_43D25D: JVM_String_from_cstr(dst). Callees:
  // JVM_UnboxArg, JVM_vm_get_int_field, sub_5447D0, Ypr_fromMatrix,
  // Util_Sprintf, JVM_String_from_cstr. setWheelDamage uses sub_419860 (not
  // inline sub_5447D0). Do NOT rename sub_5447D0/sub_419860/sub_48AEA0/
  // sub_492030. GAP: PE miss → "" or null; host g_wheel_dmg round-trip (no
  // +0x13E4 wheel graph / +0x100 bit0x20 / pose serialize).
  if (index < 0) return nullptr;
  if (!self) return string_new("");
  if (index > 3) return nullptr;
  const int32_t wheel_limit = java_game_parts_bodypart_Chassis_getWheels(self);
  if (index > wheel_limit) return nullptr;
  const auto& slot = g_wheel_dmg[self][static_cast<size_t>(index)];
  if (slot.empty()) return string_new("");
  return string_new(slot.c_str());
}

void java_game_parts_bodypart_Chassis_setWheelDamage(InvObject* self,
                                                     int32_t index,
                                                     InvObject* data) {
  // PE @ 0x0043D280 (ILjava.lang.String;)V — NOT 0x43DF50 (mid setCooling).
  // Unbox this+index+cstr; dword_62E008; wheel stride 0x2B4; flag 0x20 @+0x100;
  // stores +0xF0/+0xF4/+0xF8; clear if strlen<=1. Host: string map stand-in.
  if (!self || index < 0 || index > 3) return;
  const char* s = data ? string_cstr(data) : "";
  g_wheel_dmg[self][static_cast<size_t>(index)] = s ? s : "";
}

void java_game_parts_bodypart_Chassis_setCooling(InvObject* self, float min,
                                                 float max, float spd) {
  // PE @ 0x0043DEB0 size 0x131 (IDA Chassis.setCooling).
  // Unbox this + FFF. Presets var_C/var_8/var_4 = 10/50/0.01
  // (0x41200000 / 0x42480000 / 0x3C23D70A) then dest1..3. Native.ptr
  // (dword_62E008)==0 → jz loc_43DFDC ret. NO Mighty. inner=*(handle+0xC);
  // 0 → ret. [inner+0x4C]!=1 → vtbl+0x14(0). sub_5447D0(0xA0000000
  // bytes 00 00 00 a0) test 80000000h sign → ret. vtbl+0xC(1.0f=
  // 0x3F800000). ecx=*(obj+0x4C); base=*(ecx+0x1FBC). Clamp vs
  // flt_5E73CC (0.0): min<0 → flt_5E7334 (10.0) else min → [base+0xA48];
  // max<0 → flt_5F0C3C (50.0) else max → [base+0xA4C]; spd<0 → 0.0
  // else spd → [base+0xA50]. Java: spd quadratic (Sala). GAP: PE miss →
  // silent ret; host TREE (no +0x1FBC cooling graph).
  if (!self) return;
  tree_field_set_float(self, "cooling_min", min);
  tree_field_set_float(self, "cooling_max", max);
  tree_field_set_float(self, "cooling_spd", spd);
}

// PE @ 0x00442210 size 0xFB (IDA Chassis.getSfxTable).
InvObject* java_game_parts_bodypart_Chassis_getSfxTable(InvObject* self,
                                                       int32_t id) {
  // Unbox this + I (var_4 preset 0). id < 0 → null. Native.ptr
  // (dword_62E008)==0 → null. NO Mighty. inner=*(handle+0xC); 0 → null.
  // [inner+0x4C]!=1 → vtbl+0x14(0). sub_5447D0(0xA0000000 bytes 00 00 00 a0)
  // test 80000000h sign → null. vtbl+0xC(1.0f=0x3F800000). ecx=*(obj+0x4C);
  // base=*(ecx+0x1FBC). Switch id: 0 → base+0xF8; 1 → base+0x340;
  // 2 → base+0x588; else null. slot==0 → null. JVM_getClass
  // "java.game.parts.SfxTable" + sub_404E20 → fresh host; vm_set_int_field
  // (Native.ptr, slot). Engine blocks: 0=engine, 1=?, 2=exhaust.
  // GAP: PE always new wrapper (same slot ptr); host TREE cache for identity
  // (smoke tab0==tab0b). No +0x1FBC audio blob.
  if (!self || id < 0 || id > 2) return nullptr;
  char key[32];
  std::snprintf(key, sizeof(key), "sfx_table_%d", id);
  InvObject* tab = tree_field_get_obj(self, key);
  if (!tab) {
    tab = tree_host_new("java.game.parts.SfxTable");
    tree_field_set_obj(self, key, tab);
    g_sfxtables[tab];  // ensure empty vector
  }
  return tab;
}

void java_game_parts_bodypart_Chassis_setSfxExhaustMinVol(InvObject* self,
                                                         float f) {
  // PE @ 0x004409D0 size 0x80 (IDA Chassis.setSfxExhaustMinVol).
  // Unbox this + F (var_4). Native.ptr (dword_62E008)==0 → jz loc_440A4D
  // ret. NO Mighty. inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 →
  // vtbl+0x14(0). sub_5447D0(0xA0000000 bytes 00 00 00 a0) test
  // 80000000h sign → ret. vtbl+0xC(1.0f=0x3F800000 bytes 00 00 80 3F).
  // mov [edx+7D0h], eax: edx=*(*(vtbl+0xC result)+0x4C)+0x1FBC);
  // *(edx+0x7D0)=unboxed F (exhaust min vol).
  // Twin setSteerWheelRadius @ 0x00440AF0 size 0x80: same walk, store +0xA14.
  // Engine blocks pass 0.6 / 0.9 (Baiern / Duhen). GAP: PE miss →
  // silent ret; host TREE "sfx_exhaust_min_vol" (no +0x1FBC audio graph).
  if (!self) return;
  tree_field_set_float(self, "sfx_exhaust_min_vol", f);
}

void java_game_parts_bodypart_Chassis_setSteerWheelRadius(InvObject* self, float f) {
  // PE @ 0x00440AF0
  // size 0x80 (IDA Chassis.setSteerWheelRadius). Unbox this + F (var_4).
  // Native.ptr (dword_62E008)==0 → jz loc_440B6D ret. NO Mighty.
  // inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0xA0000000) test 80000000h sign → ret.
  // vtbl+0xC(1.0f=0x3F800000). ecx=*(obj+0x4C); edx=*(ecx+0x1FBC);
  // *(edx+0xA14)=F (int_convert 2580). Twin setAckermann @ 0x00440B70
  // same walk, store +0xA1C. Contrast setSteerWheel (FF) @ 0x00440A50:
  // +0xA14=r AND +0xA18=z (Radius overwrites R only).
  // Host: +0xA14 → steer_wheel_r. GAP: PE miss → silent ret; TREE
  // (no +0x1FBC steer graph). sub_5447D0 / dword_62E008 not renamed.
  if (!self) return;
  tree_field_set_float(self, "steer_wheel_r", f);
}

void java_game_parts_bodypart_Chassis_setSteerWheel(InvObject* self, float r, float z) {
  // PE @ 0x00440A50 size 0x9A (IDA Chassis.setSteerWheel). Twin Radius
  // @ 0x00440AF0: same walk; here (FF) → [base+0xA14]=r, [base+0xA18]=z.
  if (!self) return;
  tree_field_set_float(self, "steer_wheel_r", r);
  tree_field_set_float(self, "steer_wheel_z", z);
}

void java_game_parts_bodypart_Chassis_setHornSFX(InvObject* self, InvObject* sfx,
                                                float pitch, int32_t index) {
  // PE @ 0x0043DC00: index [0,4); sfx null skips slot relink but pitch always
  // written @ base+0x874+index*4. Host TREE stand-in (no +0x1FBC blob).
  if (!self || index < 0 || index >= 4) return;
  char key[32];
  if (sfx) {
    std::snprintf(key, sizeof(key), "horn_sfx_%d", index);
    tree_field_set_obj(self, key, sfx);
  }
  std::snprintf(key, sizeof(key), "horn_pitch_%d", index);
  tree_field_set_float(self, key, pitch);
}

void java_game_parts_bodypart_Chassis_setNitroSFX(InvObject* self, InvObject* sfx,
                                                 float pitch) {
  // PE @ 0x0043DD70 size 0x13A. Unbox defaults: sfx=0, pitch=1.0f.
  // Native.ptr (dword_62E008)→inner=*(+0xC); [inner+0x4C]!=1 → vtbl+0x14(1.0f);
  // sub_5447D0(0x80000000,0,0) fail if eax&0x80000000; obj=vtbl+0xC(1.0f);
  // owner=*(obj+0x4C). EARLY-OUT if sfx==null — pitch unboxed but NEVER written
  // (contrast setHornSFX@0x43DC00: pitch always @ base+0x874+index*4).
  // Slot=*(owner+0x1FBC)+0x814 (16B ResourceRef); skip if [slot+0xC]==*(sfx+0xC);
  // else unlink/relink. Host TREE stand-in (no +0x1FBC blob).
  (void)pitch;
  if (!self || !sfx) return;
  tree_field_set_obj(self, "nitro_sfx", sfx);
}

// PE @ 0x0043DFF0 size 0x84 — IDA Chassis_getMileage.
// Unbox this (JVM_UnboxArg @ 0x0045D910). Native.ptr (dword_62E008 via
// JVM_vm_get_int_field @ 0x0042AB50)==0 → fld var_4 (0). NO Mighty.
// inner=*(handle+0xC); 0 → 0.0. [inner+0x4C]!=1 → vtbl+0x14(1.0f=
// 0x3F800000). sub_5447D0(0x80000000,0,0); EAX&0x80000000 → 0.0.
// obj=vtbl+0xC(1.0f); 0 → 0.0. fld float [*(obj+0x4C)+0x20FC]
// (int_convert 8444==0x20FC). Twin setMileage @ 0x0043E080 writes same
// slot; tick fadd in sub_454500. Single-level walk (contrast getMass
// @ 0x0043CAA0: 0xA0000000+0x20000000). Host: no Native.ptr / vtbl /
// sub_5447D0 — TREE "mileage" (setMileage twin). dword_62E008 /
// sub_5447D0 not renamed (shared / 310+ xrefs).

float java_game_parts_bodypart_Chassis_getMileage(InvObject* self) {
  // PE @ 0x0043DFF0 size 0x84. Phys → [*(obj+0x4C)+0x20FC]; host TREE.
  return self ? tree_field_get_float(self, "mileage") : 0.f;
}

void java_game_parts_bodypart_Chassis_setMileage(InvObject* self, float m) {
  // PE @ 0x0043E080 size 0x85 (IDA Chassis.setMileage).
  // Unbox this + F (var_4 preset 0 then dest1). Native.ptr
  // (dword_62E008)==0 → jz loc_43E102 ret. NO Mighty.
  // inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 → vtbl+0x14(1.0f=
  // 0x3F800000). sub_5447D0(0x80000000,0,0) test 80000000h sign → ret.
  // vtbl+0xC(1.0f). ecx=*(obj+0x4C); [ecx+0x20FC]=var_4 (mileage).
  // Twin getMileage @ 0x0043DFF0: same walk, fld same slot. Accumulators
  // PE: sub_454500 fadd [ebx+0x20FC]; init 0 in sub_44A250. Hex-rays
  // folded store=0 (lost var_4) — disasm mov edx,var_4 / mov [ecx+20FCh].
  // No clamp (contrast setCooling). GAP: PE miss → silent ret; host TREE
  // "mileage" (no +0x20FC phys / sub_5447D0 gate).
  if (!self) return;
  tree_field_set_float(self, "mileage", m);
}

void java_game_parts_bodypart_Chassis_setBuck(InvObject* self, int32_t partID,
                                              int32_t buckid, float freq,
                                              float prob, float rpmdep,
                                              float amp) {
  // PE @ 0x0043E110 (IIFFFF)I: buckid<=0 create node* ret; >0 update/delete
  // by pointer. Host void upsert stand-in — not 1:1.
  if (!self) return;
  auto& list = g_bucks[self];
  for (auto& e : list) {
    if (e.part_id == partID && e.buck_id == buckid) {
      e.freq = freq;
      e.prob = prob;
      e.rpmdep = rpmdep;
      e.amp = amp;
      return;
    }
  }
  BuckEntry e;
  e.part_id = partID;
  e.buck_id = buckid;
  e.freq = freq;
  e.prob = prob;
  e.rpmdep = rpmdep;
  e.amp = amp;
  list.push_back(e);
  tree_field_set_int(self, "buck_count", static_cast<int32_t>(list.size()));
}

int32_t java_game_parts_bodypart_Chassis_getWheels(InvObject* self) {
  if (!self) return 0;
  int32_t n = 0;
  for (int32_t i = 0; i < 4; ++i) {
    if (part_on_slot(self, 101 + i)) ++n;
  }
  if (n > 0) return n;
  // Chassis.java default `wheels = 4` when none installed yet.
  const int32_t field = tree_field_get_int(self, "wheels");
  return field > 0 ? field : 4;
}

// PE @ 0x00440C80 size 0x13b (IDA Chassis.getWheel).
InvObject* java_game_parts_bodypart_Chassis_getWheel(InvObject* self,
                                                    int32_t id) {
  // Unbox this + I (var_4 preset 0). id < 0 → null (jl loc_440DB4).
  // Native.ptr (dword_62E008)==0 → null. NO Mighty. inner=*(handle+0xC);
  // 0 → null. [inner+0x4C]!=1 → vtbl+0x14(0). sub_5447D0(0xA0000000
  // bytes 00 00 00 a0) test 80000000h sign → null. vtbl+0xC(1.0f=
  // 0x3F800000). second=*(obj+0x44), edi=*(obj+0x4C). same lock
  // sub_5447D0(0x20000000); vtbl+0xC(1.0f). Bounds: id >=
  // *[*(node+0xC)+0x1F40] → null (same count dword as getWheels @
  // 0x0043CD70 size 0xB9). wheel* = *[veh+0x13E4] + id*0x2B4 (lea
  // stride 173*4 — NOT deref of entry). base 0 → null. Else
  // JVM_getClass "java.game.parts.WheelRef" + sub_404E20 → fresh host;
  // vm_set_int_field(Native.ptr, wheel*). Always NEW wrapper.
  // GAP: PE miss → null; host Part on slot 101+id for TREE identity
  // (smoke gw0==w0). Empty slot → null (PE still wraps phys entry).
  // Hard id>3 stand-in for typical +0x1F40==4 (no phys count graph).
  if (!self || id < 0 || id > 3) return nullptr;
  return part_on_slot(self, 101 + id);
}

}  // namespace inv
