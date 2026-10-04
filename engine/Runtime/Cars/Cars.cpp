// Split from natives_generated_world.cpp — Cars.cpp
#include "natives.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "tree_interp.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"
#include "render_d3d9.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Parts/world_state.hpp"

namespace inv {

namespace {

// PE Navigator_MapProjectPos @ 0x00482C40 size 0xeb (235):
// Camera_projectVec3 @ 0x4FDB40 → Camera_projectWorld @ 0x513720 →
// four independent NDC clamps (not else-if) ±0.89999998 / ±0.85000002
// → Camera_unprojectVec3 @ 0x4FDB70 → Camera_unprojectNdc @ 0x513840.
// Caller (updateNavigator @ 0x48309B): ebx=vp Native.ptr;
// GfxEngine_findViewportByHandle @ 0x4FD220 (match list node+0x1C vs
// handle+8) then GfxEngine_projectCameraFromVp @ 0x4FDC80 →
// GfxEngine_vpProjectCam @ 0x516C20(vp, 0) → *(vp+0x3C) project cam
// (a2!=0 path unused here). Host activate(cam,vp) stand-in for that
// list walk + project cam (markers still run if activate no-ops).
void navigator_map_project_pos(InvObject* vp, InvObject* cam, float* px,
                               float* py, float* pz) {
  if (!px || !py || !pz) return;
  if (cam && vp) render_d3d9_camera_activate(cam, vp, 1);
  float ndc_x = 0.f, ndc_y = 0.f;
  // Soft PE deepen: PE always project→clamp→unproject (Camera_projectVec3
  // null-cam is no-op then unproject). Host: clamp+unproject only when
  // project succeeds; else leave pre-scaled world (TREE/smoke no D3D).
  if (!render_d3d9_project(*px, *py, *pz, &ndc_x, &ndc_y)) return;
  constexpr float kMaxX = 0.89999998f;
  constexpr float kMaxY = 0.85000002f;
  // PE: four separate ifs (x then y); same axis cannot hit both sides.
  if (ndc_x < -kMaxX) {
    const float s = -kMaxX / ndc_x;
    ndc_x *= s;
    ndc_y *= s;
  }
  if (ndc_x > kMaxX) {
    const float s = kMaxX / ndc_x;
    ndc_x *= s;
    ndc_y *= s;
  }
  if (ndc_y < -kMaxY) {
    const float s = -kMaxY / ndc_y;
    ndc_x *= s;
    ndc_y *= s;
  }
  if (ndc_y > kMaxY) {
    const float s = kMaxY / ndc_y;
    ndc_x *= s;
    ndc_y *= s;
  }
  float ox = *px, oy = *py, oz = *pz;
  if (vp && cam &&
      render_d3d9_viewport_unproject(vp, cam, ndc_x, ndc_y, &ox, &oy, &oz)) {
    *px = ox;
    *py = oy;
    *pz = oz;
  }
}

// PE RenderRef_bindBone("bone00") + RenderInst_SetLocalMatrix @ 0x48C270.
// Mat3x4_setBasis_posScaled10 @ 0x54F4C0 (thiscall ecx=dest 3x4, arg0=Ypr
// basis, arg1=pos): flt_5F37A0 @ 0x005F37A0 = 10.0f → dest+0x30/34/38 =
// pos*10. Used by mode1 cam + dynamarker/marker (NOT mode0, which writes
// xz*0.1 into GetLocalMatrix slots directly @ loc_483067).
void navigator_write_bone00(InvObject* rr, float px, float py, float pz,
                            float yaw, float pitch, float roll) {
  if (!rr) return;
  const int32_t bone = render_d3d9_mesh_get_bone_id(rr, "bone00");
  render_d3d9_mesh_set_bone_local(rr, bone, px, py, pz, yaw, pitch, roll);
}

void navigator_write_bone00_scaled10(InvObject* rr, float px, float py,
                                     float pz, float yaw, float pitch,
                                     float roll) {
  constexpr float kPosScale10 = 10.f;  // flt_5F37A0
  navigator_write_bone00(rr, px * kPosScale10, py * kPosScale10,
                         pz * kPosScale10, yaw, pitch, roll);
}

// PE @ 0x483212 / 0x4833A7 (after SetLocalMatrix): esi = symbol
// Native.ptr; node=*(esi+0xC); if node: dword[33..38]=0 then dword[32]=2
// (= +0x84..+0x98 then +0x80=2, int_convert). Same dirties as
// ParticleSystem init sub_48A490 @ 0x48A490 (zeros then +0x80=2 on
// renderinst node). Host: Native.ptr box via native_ptr_ensure /
// native_ptr_node (Camera/RenderRef path) — NOT TREE scene_poke*.
void navigator_scene_poke_symbol(InvObject* symbol) {
  if (!symbol) return;
  // PE: handle = ResourceRef Native.ptr; node = *(handle+0xC); null → skip.
  // Host: ensure ResHandle+node (same as Camera.setFog) then raw poke.
  if (!native_ptr_get(symbol)) native_ptr_ensure(symbol);
  void* node = native_ptr_node(symbol);
  if (!node) return;
  auto* base = reinterpret_cast<uint8_t*>(node);
  *reinterpret_cast<int32_t*>(base + 0x84) = 0;
  *reinterpret_cast<int32_t*>(base + 0x88) = 0;
  *reinterpret_cast<int32_t*>(base + 0x8C) = 0;
  *reinterpret_cast<int32_t*>(base + 0x90) = 0;
  *reinterpret_cast<int32_t*>(base + 0x94) = 0;
  *reinterpret_cast<int32_t*>(base + 0x98) = 0;
  *reinterpret_cast<int32_t*>(base + 0x80) = 2;
}

// PE @ 0x48314B: thiscall Engine_queryGameRefChannel(ecx=g_EngineState
// @ 0x00636338, mover Native.ptr, GII_DIR=4, dest=0) → EAX int degrees.
// GameType.GII_DIR=4 (sources/.../GameType.java). Query walks ResHandle
// → vtbl+0x3C getInfo / Engine_CallNamedMethod("getInfo") — physics
// returns heading as int°. Caller fild * flt_deg2rad @ 0x005F13BC
// (was flt_5F13BC) → Vec3_store yaw.
// Soft PE deepen: PE always consumes EAX (0 valid) — no ori fallback.
// Host: GameRef_getInfo(GII_DIR) ≡ channel when Native.ptr or nonzero;
// TREE-only movers (no handle) Soft-seed from getOri rad → int°.
constexpr int32_t kGiiDir = 4;  // GameType.GII_DIR

int32_t navigator_seed_query_gii_dir_degrees(InvObject* mover) {
  if (!mover) return 0;
  const int32_t via_info =
      java_util_resource_GameRef_getInfo(mover, kGiiDir, 0);
  // Soft PE deepen: PE dest=0 → EAX only (incl. 0). Trust channel when
  // handle exists OR getInfo returned nonzero. Soft ori seed ONLY when
  // both fail (TREE movers without Native.ptr).
  if (via_info != 0 || native_ptr_get(mover)) {
    tree_field_set_int(mover, "gii_dir", via_info);
    return via_info;
  }
  float yaw_rad = 0.f, pitch = 0.f, roll = 0.f;
  if (InvObject* ori = java_util_resource_GameRef_getOri(mover)) {
    ypr_get(ori, &yaw_rad, &pitch, &roll);
  } else {
    yaw_rad = tree_field_get_float(mover, "ori_y");
  }
  (void)pitch;
  (void)roll;
  // PE physics stores int°; host trunc toward 0 like MSVC (int)ftol.
  constexpr float kDeg2Rad = 0.017453292f;  // flt_deg2rad @ 0x005F13BC
  const int32_t deg = static_cast<int32_t>(yaw_rad / kDeg2Rad);
  tree_field_set_int(mover, "gii_dir", deg);
  return deg;
}

}  // namespace

int32_t java_game_GameLogic_kismajomCheck(InvObject* kismajomArray) {
  // PE @ 0x0047CB50 size 0xb1 (177). Sig ([Ljava.lang.String;)I static.
  // JVM_UnboxArg(CallInfo, dest0=nullptr) @ 0x0045D910 — static, array
  // stays in arg0. length = JVM_vm_get_int_field_by_name(arr, "length"
  // @ 0x00612D24) @ 0x0042A430. length==0 → -1 (or eax).
  // Walk i=length-1..0: elem = JVM_Array_getElement(arr,i) @ 0x0042B000;
  // cstr = JVM_vm_get_int_field(elem, dword_62E008) @ 0x0042AB50
  // (runtime Field*; BSS 0 at idle — String chars slot, not renamed here).
  // Inline strlen (repne scasb): empty → ecx=-1 → jl match (no ring probe).
  // Else movsx(encoded[c])-1 vs movsx(ring): walk ptr backward from
  // Input_cheatRingPtr @ 0x00612C68, wrap +16 if < Input_cheatRing
  // @ 0x00640924. Full match (ecx<0): * (ptr-1) = 0 (wrap); return i.
  // Exhaust → -1. No other side effects / no API beyond ring+JVM.
  // Host gaps: tree_vector_size/element_at + string_cstr stand in for
  // length/getElement/dword_62E008; match+zero via
  // input_cheat_try_match_encoded (g_cheat_ring in IO.cpp, not PE BSS).
  // null elem/cstr → miss (PE would deref); unsigned vs PE movsx on
  // high-bit bytes (cheats are ASCII+1). Empty "" matches like PE.
  const int32_t n = kismajomArray ? tree_vector_size(kismajomArray) : 0;
  if (n <= 0) return -1;
  for (int32_t i = n - 1; i >= 0; --i) {
    InvObject* enc = tree_vector_element_at(kismajomArray, i);
    const char* es = enc ? string_cstr(enc) : nullptr;
    if (input_cheat_try_match_encoded(es)) return i;
  }
  return -1;
}

// PE @ 0x00482D30 size 0x6c4 (1732) — Navigator.updateNavigator(GameRef,I)V.
// Soft PE deepen (Cars): MapProjectPos project/clamp/unproject VA chain
// Camera_projectWorld@513720 / Camera_unprojectNdc@513840; project_ok Soft
// Native.ptr cam+vp; GII_DIR Soft ori seed only when !handle.
// W13D close: body ends @ 0x4833E9 — cam bone00 + marker symbol bones
// only. PE never reads Navigator.route (Java RenderRef via plotRoute @
// 0x483960 elsewhere); never paints tiles/OSD. Host navigator_paint
// (GameRef.cpp) invents route_osd/marker_osd — parent gap, not Cars.
// Unbox this/car/mode @ JVM_UnboxArg 0x0045D910. cam=instance
// "java.render.Camera"; vp by ResRef name (aVp_Navigator @ 0x613514).
// Silent jz (no Mighty): !cam, cam handle+8==0, carHandle+8==0, !vp.
// PE does NOT write Java `mode`.
// Prologue: GetWorldPos/Ypr @ 0x48B280/300; bindBone("bone00") @ 0x48BC40;
// RenderInst_GetLocalMatrix @ 0x48C3D0 → local_mtx (+0x30/34/38 =
// bone_tx/ty/tz). offsetX/Z are world-cm deltas: world = car + prior
// (vm_get_float_field @ 0x42A560). mode0 clamp @ 0x482E3D: bounds
// left/top*100 + size*100*8 (flt_5F09C8/5F0DA0) inset bone_ty*5 /
// *1.3333334; on hit rewrite delta = bound - world + prior (vm_set @
// 0x42A040). loc_483067: bone_tx/tz = world*0.1 (flt_5F08F8); bone_ty
// + basis untouched. mode1 @ 0x482FD2: pitch=-1.5; Ypr_toMatrix;
// pos = m[2/6/10]*flt_Navigator_DEF_ZOOM + world*0.01;
// Mat3x4_setBasis_posScaled10 (ecx=local_mtx) @ 0x483060. mode>=2:
// SetLocalMatrix(seed) only @ 0x483089. SetLocalMatrix @ 0x483096
// always. Then findViewport + projectCamera → MapProjectPos markers.
// Scene poke unlocked (native_ptr_node).
// Host: cam via setMatrix_1 in Java units (PE setMatrix *10 only on
// matrix path — margin uses getPos_y*10; mode0 xz=world*0.01; mode1
// no extra *10). Markers still Mat3x4*10 via write_bone00_scaled10.
// Mode0 Ypr: changeMode defaults (no RenderRef.getOri). Paint lookat /
// tiles / Bind live in GameRef.cpp. Dynamarker yaw: PE GII_DIR
// channel-4 int° * flt_deg2rad (host seed+query below).
void java_game_Navigator_updateNavigator(InvObject* self, InvObject* car,
                                         int32_t mode) {
  // PE @ 0x00482D30 — Navigator.updateNavigator body size ~0x6c4.
  if (!self) return;
  // PE: cam = get_instance_field("cam"); handle=Native.ptr; handle[+8]==0 → jz.
  InvObject* cam = tree_field_get_obj(self, "cam");
  // PE: vp = get_ResourceRef_ptr_by_name(this, "vp"); !vp → jz.
  InvObject* vp = tree_field_get_obj(self, "vp");
  if (!cam || !vp) return;
  // PE: cam Native.ptr[+8]!=0 && car[+8]!=0 && vp. Host TREE often has
  // ResourceRef.id==0 without Native.ptr — do NOT gate on id (broke
  // valocity nav_upd smoke race123). Live object pointers only.
  if (!car) return;

  // PE GameRef_GetWorldPos(car) @ 0x48B280 — world cm.
  float cx = tree_field_get_float(car, "pos_x");
  float cz = tree_field_get_float(car, "pos_z");
  if (InvObject* wp = java_util_resource_GameRef_getPos(car)) {
    float wy = 0.f;
    vec3_get(wp, &cx, &wy, &cz);
    (void)wy;
  }
  tree_field_set_float(self, "follow_x", cx);
  tree_field_set_float(self, "follow_z", cz);

  constexpr float kWorld100 = 100.f;         // flt_5F09C8 @ 0x005F09C8
  constexpr float kScale01 = 0.01f;          // flt_5F0C20 @ 0x005F0C20
  // PE mode1 @ 0x48302x: world*=0.0099999998 (imm) before Mat3x4*10.
  constexpr float kScale01Mode1 = 0.0099999998f;
  constexpr float kScale10 = 0.100000001f;   // flt_5F08F8 / flt_5F08E8
  constexpr float kPosScale10 = 10.f;        // flt_5F37A0 @ 0x005F37A0
  constexpr float kDefZoom = 4.5f;           // flt_Navigator_DEF_ZOOM @ 0x005F13C0
  constexpr float kPitchCar = -1.5f;         // imm 0xBFC00000 @ 0x482FE1
  constexpr float kPitchMap = -1.57f;        // changeMode Ypr default
  constexpr float kYDyn = 0.030999999f;      // imm 0x3CFDF3B6 @ 0x483194
  constexpr float kYStatic = 0.0099999998f;  // imm 0x3C23D70A @ 0x483329
  constexpr float kGrid = 8.f;               // flt_5F0DA0 @ 0x005F0DA0
  constexpr float kMarginZ = 5.f;            // flt_5F0CDC @ 0x005F0CDC
  constexpr float kAspect = 1.3333334f;      // flt_5F0C78 @ 0x005F0C78
  constexpr float kDeg2Rad = 0.017453292f;   // flt_deg2rad @ 0x005F13BC
  // PE always GetWorldYpr(car) @ 0x48B300 (used mode1; overwritten pitch).
  // Host: GameRef_getOri → GameRefState oy/op/or_ (setMatrix rad).
  float car_yaw = 0.f, car_pitch = 0.f, car_roll = 0.f;
  if (InvObject* ori = java_util_resource_GameRef_getOri(car)) {
    ypr_get(ori, &car_yaw, &car_pitch, &car_roll);
  } else {
    car_yaw = tree_field_get_float(car, "ori_y");
  }
  (void)car_pitch;
  (void)car_roll;

  // PE RenderInst_GetLocalMatrix @ 0x482DED: seed bone_tx/ty/tz + basis.
  // Host: RenderRef_getPos = Java units (unscaled setMatrix ResState).
  // No RenderRef.getOri — mode0/mode>=2 use changeMode Ypr; mode1
  // overwrites from car yaw + pitch -1.5.
  float bone_x = 0.f;
  float bone_y = 0.f;
  float bone_z = 0.f;
  float bone_yaw = 0.f;
  float bone_pitch = kPitchMap;
  float bone_roll = 0.f;
  if (InvObject* bp = java_util_resource_RenderRef_getPos(cam)) {
    vec3_get(bp, &bone_x, &bone_y, &bone_z);
  }
  if (bone_y == 0.f) {
    bone_y = tree_field_get_float(self, "zoom");
    if (bone_y <= 0.f) bone_y = kDefZoom;
  }

  // PE @ 0x482DF2–0x482E39: world = car + prior offsetX/Z (world-cm deltas).
  float offset_x = tree_field_get_float(self, "offsetX");
  float offset_z = tree_field_get_float(self, "offsetZ");
  float wx = cx + offset_x;
  float wz = cz + offset_z;

  if (mode == 0) {
    const float left = tree_field_get_float(self, "left");
    const float top = tree_field_get_float(self, "top");
    const float size = tree_field_get_float(self, "size");
    // PE @ 0x482E43–0x482FC0: world-cm bounds from left/top/size*100,
    // inset bone_ty*5 / (*1.3333334). PE bone_ty is GetLocalMatrix
    // (post setMatrix *10). Host getPos is Java → * flt_5F37A0.
    // Rewrite deltas only on clamp hit. PE always runs (no size>0 gate).
    float z_lo = top * kWorld100;
    float x_lo = left * kWorld100;
    const float strip = size * kWorld100 * kGrid;
    float z_hi = z_lo + strip;
    float x_hi = x_lo + strip;
    const float pad_z = bone_y * kPosScale10 * kMarginZ;
    const float pad_x = pad_z * kAspect;
    z_lo += pad_z;
    z_hi -= pad_z;
    x_lo += pad_x;
    x_hi -= pad_x;
    if (wz < z_lo) {
      offset_z = z_lo - wz + offset_z;
      wz = z_lo;
      tree_field_set_float(self, "offsetZ", offset_z);
    } else if (wz > z_hi) {
      offset_z = z_hi - wz + offset_z;
      wz = z_hi;
      tree_field_set_float(self, "offsetZ", offset_z);
    }
    if (wx < x_lo) {
      offset_x = x_lo - wx + offset_x;
      wx = x_lo;
      tree_field_set_float(self, "offsetX", offset_x);
    } else if (wx > x_hi) {
      offset_x = x_hi - wx + offset_x;
      wx = x_hi;
      tree_field_set_float(self, "offsetX", offset_x);
    }
    // PE loc_483067: matrix xz = world*0.1 (no Mat3x4 *10); ty+Ypr kept.
    // Host setMatrix_1 is Java units (= PE matrix/10) → world*0.01.
    // Ypr stays changeMode default (gpsMode switches call changeMode).
    bone_x = wx * kScale01;
    bone_z = wz * kScale01;
  } else if (mode == 1) {
    // PE @ 0x482FD2: GetWorldYpr then pitch=-1.5; Ypr_toMatrix;
    // m[2/6/10]*DEF_ZOOM + world*0.01; Mat3x4_setBasis_posScaled10 @
    // 0x483060. Host setMatrix_1 does not *10 → pass pre-scale Java pos.
    // Does NOT rewrite offsetX/Z. Ypr_toMatrix m[2]=cp*sy, m[6]=-sp,
    // m[10]=cp*cy (verified @ 0x54ECD0).
    bone_yaw = car_yaw;
    bone_pitch = kPitchCar;
    bone_roll = 0.f;
    const float sy = std::sin(bone_yaw);
    const float cy = std::cos(bone_yaw);
    const float sp = std::sin(bone_pitch);
    const float cp = std::cos(bone_pitch);
    // Ypr_toMatrix @ 0x54ECD0: m[2]=cp*sy, m[6]=-sp, m[10]=cp*cy.
    const float nx = wx * kScale01Mode1;
    const float nz = wz * kScale01Mode1;
    bone_x = cp * sy * kDefZoom + nx;  // m[2]*4.5 + x*0.0099999998
    bone_y = -sp * kDefZoom;           // m[6]*4.5
    bone_z = cp * cy * kDefZoom + nz;  // m[10]*4.5 + z*0.0099999998
  }
  // else mode>=2 @ 0x482FD2 jnz→0x483089: keep GetLocalMatrix seed
  // (getPos + changeMode Ypr above); no offset rewrite.

  // PE @ 0x483096 SetLocalMatrix always (mode0/1 mutated buffer; mode>=2
  // seed). Host: RenderRef_setMatrix_1 → Java units; markers still use
  // navigator_write_bone00_scaled10 (Mat3x4*10 + SetLocalMatrix path).
  java_util_resource_RenderRef_setMatrix_1(
      cam, vec3_new(bone_x, bone_y, bone_z),
      ypr_new(bone_yaw, bone_pitch, bone_roll));

  // PE @ 0x48309B: findViewportByHandle + projectCameraFromVp →
  // GfxEngine_vpProjectCam(vp,0) → *(vp+0x3C) if list*(vp+0x2C)+4 alive.
  // Soft PE deepen: activate stand-in (no PE list walk). Gate markers on
  // active==cam (PE project cam nonzero). Soft: also allow when both
  // cam+vp have Native.ptr (TREE smoke without D3D vp list).
  render_d3d9_camera_activate(cam, vp, 1);
  const bool project_ok =
      (render_d3d9_camera_active() == cam) ||
      (native_ptr_get(cam) != nullptr && native_ptr_get(vp) != nullptr);

  // PE @ 0x48310B–0x48324E (W13D landed): dynamarker walk.
  // GetWorldPos(mover) + GII_DIR=4 Engine_queryGameRefChannel
  // (g_EngineState, mover, 4, 0) @ 0x48314B → int°; fild * flt_deg2rad
  // @ 0x48315C → Ypr_toMatrix → xz*0.1 y=0.031 → MapProjectPos @
  // 0x4831AA → *0.1 → Mat3x4_setBasis_posScaled10 @ 0x4831F0 →
  // bindBone bone00 @ 0x4831FC + SetLocalMatrix @ 0x48320D + scene
  // poke @ 0x483212 (node+0x84..+0x98=0, +0x80=2).
  // PE: only if findViewport+projectCamera nonzero (project_ok above).
  if (project_ok) {
  if (InvObject* dyns = tree_field_get_obj(self, "dynamarker")) {
    const int32_t nd = tree_vector_size(dyns);
    for (int32_t i = 0; i < nd; ++i) {
      InvObject* m = tree_vector_element_at(dyns, i);
      if (!m) continue;
      // DMarker.mover (PE JVM_getFieldResourceNative "mover"); "obj" alias.
      InvObject* mover = tree_field_get_obj(m, "mover");
      if (!mover) mover = tree_field_get_obj(m, "obj");
      if (!mover) continue;
      float mx = tree_field_get_float(mover, "pos_x");
      float my = tree_field_get_float(mover, "pos_y");
      float mz = tree_field_get_float(mover, "pos_z");
      if (InvObject* mp = java_util_resource_GameRef_getPos(mover)) {
        vec3_get(mp, &mx, &my, &mz);
      }
      // PE @ 0x48314B–0x48316A: query GII_DIR → fild * flt_deg2rad
      // → Vec3_store(yaw,0,0). Host seed+query "gii_dir" int° (same
      // unit contract); do NOT pass getOri rad straight through.
      const int32_t dir_deg = navigator_seed_query_gii_dir_degrees(mover);
      const float yaw = static_cast<float>(dir_deg) * kDeg2Rad;
      tree_field_set_float(m, "pos_x", mx);
      tree_field_set_float(m, "pos_z", mz);
      tree_field_set_float(m, "yaw", yaw);
      tree_field_set_int(m, "gii_dir", dir_deg);
      mx *= kScale10;
      my = kYDyn;
      mz *= kScale10;
      navigator_map_project_pos(vp, cam, &mx, &my, &mz);
      mx *= kScale10;
      my *= kScale10;
      mz *= kScale10;
      tree_field_set_float(m, "proj_x", mx);
      tree_field_set_float(m, "proj_y", my);
      tree_field_set_float(m, "proj_z", mz);
      InvObject* symbol = tree_field_get_obj(m, "symbol");
      if (symbol) {
        navigator_write_bone00_scaled10(symbol, mx, my, mz, yaw, 0.f, 0.f);
        navigator_scene_poke_symbol(symbol);
      }
    }
  }

  // PE @ 0x48329A–0x4833DC (W13D landed): static SMarker walk.
  // pos Vector3 x/y/z @ 0x4832BF–0x4832EF → yaw identity (Vec3_store
  // 0,0,0 + Ypr_toMatrix) → xz*0.1 y=0.01 → MapProjectPos @ 0x48333F
  // → *0.1 → Mat3x4_setBasis_posScaled10 @ 0x483385 → bindBone bone00
  // @ 0x483391 + SetLocalMatrix @ 0x4833A2 + scene poke @ 0x4833A7.
  // No PE route bone loop in this body (route RenderRef ≠ marker).
  if (InvObject* marks = tree_field_get_obj(self, "marker")) {
    const int32_t nm = tree_vector_size(marks);
    for (int32_t i = 0; i < nm; ++i) {
      InvObject* m = tree_vector_element_at(marks, i);
      if (!m) continue;
      InvObject* pos3 = tree_field_get_obj(m, "pos");
      float mx = 0.f, my = 0.f, mz = 0.f;
      if (pos3) {
        mx = tree_field_get_float(pos3, "x");
        my = tree_field_get_float(pos3, "y");
        mz = tree_field_get_float(pos3, "z");
      } else {
        mx = tree_field_get_float(m, "pos_x");
        my = tree_field_get_float(m, "pos_y");
        mz = tree_field_get_float(m, "pos_z");
      }
      mx *= kScale10;
      my = kYStatic;
      mz *= kScale10;
      navigator_map_project_pos(vp, cam, &mx, &my, &mz);
      mx *= kScale10;
      my *= kScale10;
      mz *= kScale10;
      tree_field_set_float(m, "proj_x", mx);
      tree_field_set_float(m, "proj_y", my);
      tree_field_set_float(m, "proj_z", mz);
      InvObject* symbol = tree_field_get_obj(m, "symbol");
      if (symbol) {
        navigator_write_bone00_scaled10(symbol, mx, my, mz, 0.f, 0.f, 0.f);
        navigator_scene_poke_symbol(symbol);
      }
    }
  }
  }  // project_ok

  tree_field_set_int(self, "update_count",
                     tree_field_get_int(self, "update_count") + 1);
  // PE returns @ 0x4833E9 — no paint. Host call below is GameRef-only
  // invent (tiles/lookat + route_osd/marker_osd shims). Parent: gate or
  // retire route/marker OSD in navigator_paint when tiles_absolu (map
  // rect already gated); no PE VA for those OSD paths.
  navigator_paint(self);
}

void java_game_Painter_doPaint(InvObject* self, InvObject* cursor, int32_t color,
                               int32_t brush, int32_t temp, float rot, float size,
                               int32_t flip) {
  // PE @ 0x00482960 Painter_doPaint size 0x293 (659). Sig (GameRef,IIIFFI)V.
  // JVM_UnboxArg dests (cdecl, I/F = box+8): this@var_120 unused — no
  // Mighty ERROR; cursor@var_13C; color@var_154; brush@var_150;
  // temp@var_140; rot@var_144; size@var_14C; flip@var_148.
  // Zero brush ResHandle@var_16C then thiscall sub_545FC0(&brushH, brush)
  // (30 xrefs — not renamed): stores RID at handle+8 (=var_164) via
  // sub_536820; sprintf second %d reads that slot (same as paintBrush.id()).
  // thiscall sub_426470(g_EngineState, cursor, GII_POS=2, &xyz[3]@var_118).
  // Cursor handle+8==0 → getInfo 0 → pick fails → no event. No CRT_rand
  // (contrast paintPart @ 0x004827D0). Engine_pickAt @ 0x0048D450(xyz,
  // &pick@var_17C, list@var_15C, scratch12, 0, scratch4, 0, hit@var_138,
  // nrm@var_12C, 0) — hit/nrm non-null (unlike paintPart nullptr outs).
  // getInfo(pick, GII_CATEGORY=7, dest0): EAX must be GIR_CAT_PART=9 or
  // GIR_CAT_VEHICLE=5 else thiscall sub_45FB80(list) + optional
  // ResHandle_Unlink(inst+0x44, &brushH|/&pick) → return 0.
  // Util_Sprintf(dst256, "bigdecal %d,%d %.3f,%.3f,%.3f %.3f,%.3f,%.3f "
  // "%.2f %.2f %d %d", color, brushH+8, hit, nrm, size, rot, temp, flip)
  // @ 0x00551220 (Invictus, not CRT). Engine_queueEvent(g_EngineState,
  // &pick, 0, EVENT_COMMAND=0x10, cmd, 0) trampoline @ 0x00426800 →
  // dispatch @ 0x004265C0. Epilogue always splices pick list + relinks
  // brushH/pick if owner!=0. Java: MODE_PAINTCOLOR / MODE_PAINTDECAL
  // (paintBrush.id(), temp 0|1); PE re-checks pick category.
  if (!self) return;
  PaintStroke s;
  s.cursor = cursor;
  s.color = color;
  s.brush = brush;
  s.temp = temp;
  s.rot = rot;
  s.size = size;
  s.flip = flip;
  g_painter[self].push_back(s);
  tree_field_set_int(self, "paint_count",
                     static_cast<int32_t>(g_painter[self].size()));
  tree_field_set_int(self, "paint_last_color", color);
  tree_field_set_int(self, "paint_last_brush", brush);
  tree_field_set_float(self, "paint_last_size", size);

  // PE dest is the picked handle (&var_17C), not Painter / not cursor.
  // Host has no Engine_pickAt @ 0x48D450 world ray — soft: queue on
  // cursor when id!=0; cat gate matches paintPart (PE getInfo GII_CATEGORY
  // @ 0x482A55 → must be GIR_CAT_PART=9 or GIR_CAT_VEHICLE=5). Hit =
  // Cursor_tick pick_+0x164 stand-in when pick_ok, else GII_POS-like
  // getPos; normals 0 (PE fills a8/a9 from pickAt). brush = raw unbox
  // (PE brushH+8 after ResHandle_Relink @ 0x545FC0). W35-11 pick_scale.
  if (!cursor || java_util_resource_ResourceRef_id(cursor) == 0) return;
  constexpr int32_t kGiiCategory = 7;
  constexpr int32_t kGirCatVehicle = 5;
  constexpr int32_t kGirCatPart = 9;
  const int32_t cat =
      java_util_resource_GameRef_getInfo(cursor, kGiiCategory, 0);
  if (cat != 0 && cat != kGirCatPart && cat != kGirCatVehicle) return;
  float hx = 0.f, hy = 0.f, hz = 0.f;
  if (tree_field_get_int(cursor, "pick_ok") != 0) {
    hx = tree_field_get_float(cursor, "pick_x");
    hy = tree_field_get_float(cursor, "pick_y");
    hz = tree_field_get_float(cursor, "pick_z");
  } else {
    vec3_get(java_util_resource_GameRef_getPos(cursor), &hx, &hy, &hz);
  }
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "bigdecal %d,%d %.3f,%.3f,%.3f %.3f,%.3f,%.3f %.2f %.2f %d %d",
                color, brush, hx, hy, hz, 0.f, 0.f, 0.f, size, rot, temp, flip);
  java_util_resource_GameRef_queueEvent(cursor, nullptr, 0x10, string_new(buf));
}

void java_game_Painter_paintPart(InvObject* self, InvObject* cursor,
                                 int32_t color) {
  // PE @ 0x004827D0 Painter_paintPart size 0x189 (393). Sig (GameRef,I)V.
  // JVM_UnboxArg cdecl dests (I = box+8): this@var_50 unused — no Mighty
  // ERROR; cursor@var_70; color@var_74. Bytes @0x4827FA: lea ecx,&xyz;
  // push ecx; push 2; push cursor; thiscall sub_426470(g_EngineState,…).
  // CRT_rand() — EAX discarded (paintPart-only; doPaint @ 0x00482960 /
  // xPaint @ 0x00482C00 have none). Copy xyz→ray; Engine_pickAt @
  // 0x0048D450(ray,&pick@var_8C,list@var_7C,scratch12,0,scratch4,0,
  // hit=0,nrm=0,0) — hit/nrm nullptr (unlike doPaint bigdecal outs).
  // getInfo(pick,GII_CATEGORY=7,dest0): EAX must be GIR_CAT_PART=9 or
  // GIR_CAT_VEHICLE=5 else thiscall sub_45FB80(list) + optional
  // ResHandle_Unlink(owner+0x44,&pick) → return 0.
  // Util_Sprintf(dst64,"paint 0 0 %d 0 -1",color@var_74) @ 0x00551220
  // (Invictus; asm mov edx,[var_74] — not Hex-Rays v9[2]).
  // Engine_queueEvent(g_EngineState,&pick,0,EVENT_COMMAND=0x10,cmd,0)
  // trampoline @ 0x00426800. Epilogue always splices list + relink if
  // owner!=0. Contrast xPaint (literal bigdecal, no pick) / doPaint
  // (sprintf bigdecal). Java MODE_PAINTPART pre-filters cat then calls
  // with paintColor|0xFF000000; PE re-checks pick category.
  if (!self) return;
  PaintStroke s;
  s.cursor = cursor;
  s.color = color;
  s.part_fill = true;
  g_painter[self].push_back(s);
  tree_field_set_int(self, "paint_count",
                     static_cast<int32_t>(g_painter[self].size()));
  tree_field_set_int(self, "paint_last_color", color);
  tree_field_set_int(self, "paint_part_fills",
                     tree_field_get_int(self, "paint_part_fills") + 1);
  // Colorize cursor target part texture id if present.
  if (cursor) tree_field_set_int(cursor, "part_texture", color);

  // PE dest is the picked handle, not Painter / not cursor. Host has no
  // Engine_pickAt world pick (nor "paint" decode in GameRef.queueEvent).
  // Queue on cursor when id!=0; soft cat gate when host classifies
  // (0 = unknown → allow; PE only queues after pick cat∈{5,9}).
  if (!cursor || java_util_resource_ResourceRef_id(cursor) == 0) return;
  constexpr int32_t kGiiCategory = 7;
  constexpr int32_t kGirCatVehicle = 5;
  constexpr int32_t kGirCatPart = 9;
  const int32_t cat =
      java_util_resource_GameRef_getInfo(cursor, kGiiCategory, 0);
  if (cat != 0 && cat != kGirCatPart && cat != kGirCatVehicle) return;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "paint 0 0 %d 0 -1", color);
  java_util_resource_GameRef_queueEvent(cursor, nullptr, 0x10, string_new(buf));
}

void java_game_Painter_xPaint(InvObject* self, InvObject* part) {
  // PE @ 0x00482C00 Painter_xPaint size 0x34 (52). Sig (GameRef)V.
  // Single BB. Callees: JVM_UnboxArg @ 0x0045D910, Engine_queueEvent
  // trampoline @ 0x00426800 → Engine_queueEvent_dispatch @ 0x004265C0.
  // Prologue push ecx = var_4. UnboxArg cdecl dests: dest0=&var_4 (this,
  // unused — no Mighty ERROR), dest1=&arg_0 (part ResHandle* overwrites
  // CallInfo). No getInfo / Engine_pickAt / Util_Sprintf / CRT_rand.
  // Contrast paintPart @ 0x004827D0 (pick + "paint 0 0 %d 0 -1", queue
  // &pick) and doPaint @ 0x00482960 (pick + sprintf bigdecal).
  // Stack: push part (bare DWORD), 0, EVENT_COMMAND=0x10, aBigdecal000000
  // @ 0x006134D8 "bigdecal 0,0 0,0,0 0,0,0 0 0 1 0", 0; mov ecx,
  // g_EngineState @ 0x00636338; call trampoline (retn 14h). Dispatch
  // treats arg0 as ResHandle* ([ebx+0xC]) — no null check. Epilogue
  // pop ecx; retn — no pick-list splice (unlike paintPart).
  // Java MODE_PAINTDECAL: flush texture via xPaint(player.car).
  // Host: xpaint_* bookkeeping; queue always (PE ignores this).
  if (self) {
    tree_field_set_obj(self, "xpaint_part", part);
    tree_field_set_int(self, "xpaint_count",
                       tree_field_get_int(self, "xpaint_count") + 1);
  }
  java_util_resource_GameRef_queueEvent(
      part, nullptr, 0x10,
      string_new("bigdecal 0,0 0,0,0 0,0,0 0 0 1 0"));
}

InvObject* java_io_MouseCursor_getPos(InvObject* self) {
  // PE @ 0x00487970 size 0xeb (235). Sig ()Ljava.lang.Vector3;.
  // UnboxArg this. JVM_getFieldResourceNative(this, "cursor").
  // Handle==0: CRT_strcat_n_thunk(Engine_ErrorLogBuf, "!", 256) +
  // "Mighty Mouse ERROR 2" + Engine_ErrorLogMsgBox + clear scratch + null.
  // Else thiscall Engine_queryGameRefChannel(g_EngineState @ 0x00636338,
  //   handle, GII_POS=2, &xyz). Return ignored. RESTYPE_GAME=8 →
  //   vtbl+0x3C Cursor_getInfo @ 0x004625F0 (ecx=inst, 0, 2, dest):
  //   dest!=0: dest[0]=inst[41]=+0xA4, dest[1]=inst[42]=+0xA8,
  //   dest[2]=inst[43] then overwrite dest[2]=inst[92]=+0x170.
  //   return 1. Cursor_tick / EVENT_COMMAND "move" write NDC at +0xA4/+0xA8.
  // Engine_malloc(0x1C=28) Vector3, JVM_getClass, JVM_Object_vtbl,
  // JVM_Instance_initialize(class, 0), set float x,y only — z left at
  // ctor 0 (channel fills z but JNI never sets "z").
  // Contrast getPickedPos @ 0x00487870: query 59, sets x,y,z; ERROR
  // without " 2". race125 deepen VERIFY: GII_POS=2 + Cursor_getInfo
  // +0xA4/+0xA8; JNI x,y only (z ctor 0); !cursor → null (PE Mighty
  // "Mighty Mouse ERROR 2"). Host cursor_x/y stand-in; z always 0.
  // No ResourceRef_id gate.
  InvObject* inner = self ? tree_field_get_obj(self, "cursor") : nullptr;
  if (!inner) return nullptr;
  float cx = 0.f, cy = 0.f, cz = 0.f;
  if (tree_field_get_int(inner, "cursor_set") != 0) {
    cx = tree_field_get_float(inner, "cursor_x");
    cy = tree_field_get_float(inner, "cursor_y");
  } else {
    vec3_get(java_util_resource_GameRef_getPos(inner), &cx, &cy, &cz);
    (void)cz;
  }
  return vec3_new(cx, cy, 0.f);
}

InvObject* java_io_MouseCursor_getPickedPos(InvObject* self) {
  // PE @ 0x00487870 size 0xff (255). Sig ()Ljava.lang.Vector3;.
  // UnboxArg this. JVM_getFieldResourceNative(this, "cursor").
  // Handle==0: CRT_strcat_n_thunk(Engine_ErrorLogBuf, "!", 256) +
  // "Mighty Mouse ERROR" + Engine_ErrorLogPrintf + clear scratch + null.
  // Else thiscall Engine_queryGameRefChannel(g_EngineState @ 0x00636338,
  //   handle, 59=0x3B, &xyz). Return ignored. RESTYPE_GAME=8 →
  //   vtbl+0x3C Cursor_getInfo @ 0x004625F0 (ecx=inst, 0, 59, dest):
  //   dest!=0: dest[0..2] = inst[+0x164/+0x168/+0x16C] (dword[89,90,91]).
  //   eax = inst[+0x144] (dword[81] pick handle). dest==0: skip copy.
  //   Cursor_ctor @ 0x0045FDF0 zeros [89,90,91]. Cursor_tick @ 0x00460140
  //   writes them from Engine_pickAt @ 0x0048D450 when (inst+0x50)&1.
  // Engine_malloc(0x1C=28) Vector3, JVM_getClass, JVM_Object_vtbl,
  // JVM_Instance_initialize(class, 0), set float x,y,z (all three).
  // Contrast getPos @ 0x00487970: query GII_POS=2, Cursor_getInfo @
  // 0x004625F0 copies [41,42]=+0xA4/+0xA8 then z=[92]=+0x170; JNI
  // sets x,y only. Query 59 unnamed in GameType.java (gap 57..70).
  // Case 44 is GII_PICKEDINSTANCE (commented). Unique GII push 3Bh.
  // Host: GameRef_getInfo int ABI — not this path. W35-11: tick soft
  // scratch lerp → pick_x/y/z (+0x164) + pick_scale (+0x170); mirror
  // pick_ok. Always alloc Vector3 (PE returns +0x164 even when miss).
  // No Mighty log.
  InvObject* inner = self ? tree_field_get_obj(self, "cursor") : nullptr;
  if (!inner) return nullptr;
  const float wx = tree_field_get_float(inner, "pick_x");
  const float wy = tree_field_get_float(inner, "pick_y");
  const float wz = tree_field_get_float(inner, "pick_z");
  const int32_t pok = tree_field_get_int(inner, "pick_ok");
  if (self) {
    tree_field_set_float(self, "pick_x", wx);
    tree_field_set_float(self, "pick_y", wy);
    tree_field_set_float(self, "pick_z", wz);
    tree_field_set_int(self, "pick_ok", pok);
    tree_field_set_float(self, "pick_scale",
                         tree_field_get_float(inner, "pick_scale"));
  }
  return vec3_new(wx, wy, wz);
}

void java_io_MouseCursor_tickSysCursor() {
  // PE Cursor_tick @ 0x00460140 (IDA Cursor_tick). SysCursor NDC →
  // +0xA4/+0xA8 then EVENT_CURSOR edges. When (inst+0x50)&1:
  // Engine_pickAt @ 0x48D450 (renamed) via g_CursorPickScratch
  // (off_610A54): near@+0, far@+0xC, t@+0x48 (=Engine_pickAt_t),
  // pickH@+0x74, list@+0x6C. Post @ 0x460363–0x4603F0 ALWAYS:
  //   pick = (near-far)*t + far → +0x164/+0x168/+0x16C; +0x170 = t.
  // Miss: pickAt zeros t early, leaves near/far → pick=stale far, t=0.
  // LDRAG @ 0x460FD3–0x46140C: press latches +0xD4/+0xE0/+0xF4/+0xEC/
  // +0x10C; BEGIN=9 / END=10 fan-out press Group; LDROP=11 if pickAt.
  // Host soft: OSD gadget = resolved hit (t=1); miss keeps stale
  // pick_* + t=0. No Camera_unproject / Engine_rayPickScene @ 0x49E3D0.
  // HOVER SfxRef.play / Present must not re-enter. Do not break
  // dispatchCursor / dispatchCursorTo call sites.
  static bool in_tick = false;
  if (in_tick) return;
  in_tick = true;
  struct Clear {
    bool* f;
    ~Clear() { *f = false; }
  } clear{&in_tick};

  InvObject* mc = java_io_Input_cursor();
  InvObject* inner = mc ? tree_field_get_obj(mc, "cursor") : nullptr;
  if (!inner) return;

  InvObject* cfg = system_config_host();
  const int32_t sys = cfg ? tree_field_get_int(cfg, "SysCursor") : 1;
  float x = 0.f, y = 0.f;
  const bool have_ndc = input_syscursor_ndc(&x, &y);
  if (sys != 0 && have_ndc) {
    if (x > 1.f) x = 1.f;
    if (x < -1.f) x = -1.f;
    if (y > 1.f) y = 1.f;
    if (y < -1.f) y = -1.f;
    java_util_resource_GameRef_setPos(inner, vec3_new(x, y, 0.f));
    tree_field_set_float(inner, "cursor_x", x);
    tree_field_set_float(inner, "cursor_y", y);
    tree_field_set_int(inner, "cursor_set", 1);
    tree_field_set_float(mc, "cursor_x", x);
    tree_field_set_float(mc, "cursor_y", y);
    tree_field_set_int(mc, "cursor_set", 1);
  } else {
    float z = 0.f;
    vec3_get(java_util_resource_GameRef_getPos(inner), &x, &y, &z);
    (void)z;
  }

  // PE 0x0046024A: dx²+dy² vs flt_5F0ED4 (~1e-6) → moved (var_24C).
  static float prev_ndc_x = 0.f, prev_ndc_y = 0.f;
  static bool have_prev_ndc = false;
  int32_t moved = 0;
  if (have_prev_ndc) {
    const float dx = x - prev_ndc_x;
    const float dy = y - prev_ndc_y;
    if (dx * dx + dy * dy > 1e-6f) moved = 1;
  }
  prev_ndc_x = x;
  prev_ndc_y = y;
  have_prev_ndc = true;

  const int32_t cursor_id = java_util_resource_ResourceRef_id(inner);
  InvObject* phy = nullptr;
  InvObject* group = nullptr;
  float px = 0.f, py = 0.f, pz = 0.f;
  const bool picked = physics_pick_osd_gadget(x, y, &phy, &group, &px, &py, &pz);
  // W35-11 soft g_CursorPickScratch lerp (residual W34-11).
  // PE @ 0x460363: pick=(near-far)*t+far; @ 0x4603E7: +0x170=t.
  // Soft hit: OSD pos ≡ already-lerped; t stand-in 1.f (no ray t).
  // Soft miss: keep prior pick_* (stale far); t=0; pick_ok=0.
  float pick_t = 0.f;
  if (picked) {
    pick_t = 1.f;
    tree_field_set_float(inner, "pick_x", px);
    tree_field_set_float(inner, "pick_y", py);
    tree_field_set_float(inner, "pick_z", pz);
    tree_field_set_float(inner, "pick_scale", pick_t);
    tree_field_set_int(inner, "pick_ok", 1);
    tree_field_set_float(mc, "pick_x", px);
    tree_field_set_float(mc, "pick_y", py);
    tree_field_set_float(mc, "pick_z", pz);
    tree_field_set_float(mc, "pick_scale", pick_t);
    tree_field_set_int(mc, "pick_ok", 1);
  } else {
    tree_field_set_float(inner, "pick_scale", 0.f);
    tree_field_set_int(inner, "pick_ok", 0);
    tree_field_set_float(mc, "pick_x", tree_field_get_float(inner, "pick_x"));
    tree_field_set_float(mc, "pick_y", tree_field_get_float(inner, "pick_y"));
    tree_field_set_float(mc, "pick_z", tree_field_get_float(inner, "pick_z"));
    tree_field_set_float(mc, "pick_scale", 0.f);
    tree_field_set_int(mc, "pick_ok", 0);
  }
  const int32_t phy_id =
      (picked && phy) ? java_util_resource_ResourceRef_id(phy) : 0;

  // EC_LEAVE=16 / EC_HOVER=15: "%d %d %d %d %d" @ 0x0046043F / 0x00460509.
  // token3 = phy id (+0x14C / +0x50). token4 = moved. Dest +0x13C then cursor.
  // Group.handleEvent only hilites HOVER when moved != 0.
  static InvObject* hover_phy = nullptr;
  static InvObject* hover_group = nullptr;
  static int32_t hover_phy_id = 0;
  static InvObject* hover_state = nullptr;
  InvObject* cur_state = game_logic_actual_state();
  if (cur_state != hover_state) {
    hover_phy = nullptr;
    hover_group = nullptr;
    hover_phy_id = 0;
    hover_state = cur_state;
  }
  auto osd_click_lock = [](InvObject* grp) -> bool {
    InvObject* osd = grp ? tree_field_get_obj(grp, "osd") : nullptr;
    return osd && tree_field_get_int(osd, "clickLock") != 0;
  };
  auto fire_hl = [&](int32_t code, InvObject* dest, InvObject* obj, int32_t id) {
    if (!dest || osd_click_lock(dest)) return;
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%d %d %d %d %d", code, cursor_id, id, id,
                  moved);
    java_lang_GameType_dispatchCursorTo(dest, obj, string_new(buf));
  };
  if (phy != hover_phy) {
    if (hover_group && hover_phy_id) fire_hl(16, hover_group, hover_phy, hover_phy_id);
    // Java Group.handleEvent EC_HOVER: only hilite if token4 (moved) != 0.
    if (moved && picked && group && phy_id) fire_hl(15, group, phy, phy_id);
    hover_phy = picked ? phy : nullptr;
    hover_group = picked ? group : nullptr;
    hover_phy_id = phy_id;
  }

  // LDOWN(1) on MK_LBUTTON edge; LUP(2)+LCLICK(5) on release.
  // LDRAGBEGIN(9)/END(10)/LDROP(11) on L hold. R* on MK_RBUTTON.
  // Soft PE deepen Cursor_tick @ 0x460140 LDRAG: press latches = +0xF4
  // (handle)/+0xEC (Group)/+0x10C (phy)/+0xE0 (drag). BEGIN/END fan-out
  // to press Group, not current hover. LDROP needs press + pickAt ok.
  static uint32_t prev_mk = 0;
  static InvObject* ldown_phy = nullptr;
  static InvObject* ldown_group = nullptr;
  static int32_t ldown_phy_id = 0;
  static int32_t ldown_handle_id = 0;  // Soft +0xF4 (OSD: same as phy id)
  static float ldown_px = 0.f, ldown_py = 0.f, ldown_pz = 0.f;
  static float ldown_x = 0.f, ldown_y = 0.f;
  static bool ldrag = false;
  static float rdown_x = 0.f, rdown_y = 0.f;
  static bool rdrag = false;
  static InvObject* rdrag_state = nullptr;
  const uint32_t mk = input_syscursor_buttons();
  constexpr uint32_t kMkLbutton = 1u;
  constexpr uint32_t kMkRbutton = 2u;
  const bool ldown = (mk & kMkLbutton) != 0;
  const bool lwas = (prev_mk & kMkLbutton) != 0;
  const bool rdown = (mk & kMkRbutton) != 0;
  const bool rwas = (prev_mk & kMkRbutton) != 0;
  prev_mk = mk;

  auto fire_long = [&](int32_t code) {
    char buf[160];
    // PE 0x00461239 / 0x004619F0: Mechanic/Garage addHandler dest=cursor
    std::snprintf(buf, sizeof(buf),
                  "%d %d %d %.3f %.3f %.3f %.3f %.3f %.3f", code, cursor_id, 0,
                  0.f, 0.f, 0.f, x, y, 0.f);
    java_lang_GameType_dispatchCursor(inner, string_new(buf));
  };
  auto fire_r4 = [&](int32_t code) {
    char buf[80];
    // PE 0x004615F3 / 0x00461779: "%d %d %d %d" → cursor (esi)
    std::snprintf(buf, sizeof(buf), "%d %d %d %d", code, cursor_id, 0, 0);
    java_lang_GameType_dispatchCursor(inner, string_new(buf));
  };
  auto fire_r3 = [&](int32_t code, int32_t tok2 = 0) {
    char buf[80];
    // PE 0x0046105C / 0x004612BB / 0x004616D5 / 0x00461A36: "%d %d %d"
    std::snprintf(buf, sizeof(buf), "%d %d %d", code, cursor_id, tok2);
    java_lang_GameType_dispatchCursor(inner, string_new(buf));
  };
  // PE LDRAGBEGIN @ 0x46105C / LDRAGEND @ 0x4612BB: always cursor, then
  // +0xEC only when press handle +0xF4 != 0 (latched at LDOWN — not hover).
  auto fire_r3_press = [&](int32_t code, int32_t tok2) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%d %d %d", code, cursor_id, tok2);
    InvObject* p = string_new(buf);
    java_lang_GameType_dispatchCursor(inner, p);
    if (ldown_handle_id && ldown_group)
      java_lang_GameType_dispatchCursorTo(ldown_group, ldown_phy, p);
  };

  // Don't leave Garage look-axes mapped after CAS (Valocity / MainMenu).
  if (cur_state != rdrag_state) {
    if (rdrag) fire_r3(13);
    if (ldrag) fire_r3_press(10, ldown_phy_id);
    rdrag = false;
    ldrag = false;
    ldown_phy = nullptr;
    ldown_group = nullptr;
    ldown_phy_id = 0;
    ldown_handle_id = 0;
    rdrag_state = cur_state;
  }

  if (rdown && !rwas) {
    rdown_x = x;
    rdown_y = y;
    rdrag = false;
    fire_r4(3);
  } else if (rdown && rwas && !rdrag) {
    // PE 0x0046165A: hypot(press+0xBC − pos+0xA4) vs [this+0x20].
    // Cursor factory ctor @ 0x00429926 zeros +0x20 → any pixel move starts drag.
    const float dx = x - rdown_x;
    const float dy = y - rdown_y;
    if (dx * dx + dy * dy > 1e-6f) {
      rdrag = true;
      fire_r3(12);
    }
  } else if (!rdown && rwas) {
    fire_r4(4);
    if (rdrag) {
      // PE 0x00461A36: RDRAGEND=13; skips RCLICK.
      fire_r3(13);
      rdrag = false;
    }
    // PE 0x004617DD: RCLICK only if +0x104 pick && !rdrag. No 3D pick yet.
  }

  if (ldown && !lwas) {
    ldown_phy = picked ? phy : nullptr;
    ldown_group = picked ? group : nullptr;
    ldown_phy_id = phy_id;
    // Soft +0xF4: PE ResHandle id after Rebind; OSD soft ≡ phy id.
    ldown_handle_id = picked ? phy_id : 0;
    ldown_px = px;
    ldown_py = py;
    ldown_pz = pz;
    ldown_x = x;
    ldown_y = y;
    ldrag = false;
    if (picked && group) {
      char buf[160];
      // PE 0x00460F73: "%d %d %d %d %.3f %.3f %.3f"
      // token2=+0xF4 handle, token3=+0x10C phy; Soft both = phy_id.
      // Group.handleEvent EC_LDOWN: physicsId = token(3)
      std::snprintf(buf, sizeof(buf), "%d %d %d %d %.3f %.3f %.3f", 1, cursor_id,
                    phy_id, phy_id, px, py, pz);
      java_lang_GameType_dispatchCursorTo(group, phy, string_new(buf));
    }
    fire_long(1);
    return;
  }

  if (ldown && lwas) {
    // PE loc_460FD3: hold; skip if +0xE0; hypot³(press+0xB0 − pos+0xA4)
    // vs [Cursor+0x20]² (fcompp; test ah,41h). Factory zeros +0x20 →
    // any nonzero move. Soft SysCursor z=0 → 2D NDC 1e-6.
    if (!ldrag) {
      const float dx = x - ldown_x;
      const float dy = y - ldown_y;
      if (dx * dx + dy * dy > 1e-6f) {
        ldrag = true;
        // PE 0x0046105C EC_LDRAGBEGIN=9; tok2=+0x10C phy id.
        fire_r3_press(9, ldown_phy_id);
      }
    }
    return;
  }

  if (!ldown && !lwas) return;

  // LUP: PE 0x004610F0 "%d %d %d %d" token2=+0xF4 token3=+0x10C phy id
  if (ldown_handle_id && ldown_group) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%d %d %d %d", 2, cursor_id, ldown_handle_id,
                  ldown_phy_id);
    java_lang_GameType_dispatchCursorTo(ldown_group, ldown_phy, string_new(buf));
  }
  fire_long(2);

  if (ldrag) {
    // PE loc_4612A0: skip LCLICK; LDRAGEND=10 → cursor then +0xEC if +0xF4.
    // LDROP=11 only if +0xF4 && Engine_pickAt EAX (Soft: picked).
    fire_r3_press(10, ldown_phy_id);
    if (ldown_handle_id && picked && group) {
      char sbuf[80];
      // PE 0x00461358 → pickH(+0x74): "%d %d %d %d %d"
      // 11 cursor hover_phy(+0x6C+0x50) press_handle(+0xF4) press_phy.
      std::snprintf(sbuf, sizeof(sbuf), "%d %d %d %d %d", 11, cursor_id, phy_id,
                    ldown_handle_id, ldown_phy_id);
      java_lang_GameType_dispatchCursorTo(group, phy, string_new(sbuf));
      char lbuf[160];
      // PE 0x004613DC → cursor: "%d %d %d %d %.3f %.3f %.3f %d"
      // tok2=scratch+0x7C pickH id, tok3=+0xF4, xyz=+0x164, last=+0x10C.
      std::snprintf(lbuf, sizeof(lbuf), "%d %d %d %d %.3f %.3f %.3f %d", 11,
                    cursor_id, phy_id, ldown_handle_id, px, py, pz,
                    ldown_phy_id);
      java_lang_GameType_dispatchCursor(inner, string_new(lbuf));
    }
    ldrag = false;
    ldown_phy = nullptr;
    ldown_group = nullptr;
    ldown_phy_id = 0;
    ldown_handle_id = 0;
    return;
  }

  // LCLICK: PE gates on press +0xF4 (not current hover). Dest +0xEC.
  // token(2) = +0x10C phy id. Group.handleEvent EC_LCLICK.
  if (ldown_handle_id && ldown_group && ldown_phy_id) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d %d %d %.3f %.3f %.3f", 5, cursor_id,
                  ldown_phy_id, ldown_px, ldown_py, ldown_pz);
    java_lang_GameType_dispatchCursorTo(ldown_group, ldown_phy, string_new(buf));
  }
  fire_long(5);
  ldown_phy = nullptr;
  ldown_group = nullptr;
  ldown_phy_id = 0;
  ldown_handle_id = 0;
}

// --- java.game.Vehicle (registry Cars; was GameRef.cpp) ---
// PATH-TO-WORLD Soft (Bot.createCar / enterCar — sources/.../Bot.java):
//   createCar(map, c): world=map; deleteCar; car=c; enterCar(car).
//   enterCar: setTransmission(SEMIAUTO=5) → GameRef.create BOTBRAIN
//   (sl:0x6E) + renderinstance / controllable / AI_suspend queueEvents.
// Vehicle spawn (Vehicle.java): create(parent,type,"0,0,0,0,0,0") @
// 0x0047D7B0 OR set(chassis) @ ResourceRef.set(L) 0x0047CFC0 — PE
// Native.ptr after set/create = chassis phys. Host Soft: TREE may keep
// PhysicsRef on chassis InvObject while Vehicle has Native.ptr type-only
// (createCar host leaf ResourceRef_set(id)) or shared handle without
// shape on self.

// Soft query key for Vehicle natives — phys path ≡ gameref_live_phys_key
// (chassis PhysicsRef first). TREE / horn / crime keys stay prefer-self
// when Vehicle Native.ptr (sethorn / zone clear stickiness).
static InvObject* vehicle_soft_phys_key(InvObject* self) {
  if (!self) return nullptr;
  InvObject* ch = tree_field_get_obj(self, "chassis");
  if (ch && physics_shape(ch) != 0) return ch;
  if (physics_shape(self) != 0) return self;
  return nullptr;
}

static InvObject* vehicle_soft_tree_key(InvObject* self) {
  if (!self) return nullptr;
  InvObject* ch = tree_field_get_obj(self, "chassis");
  // Soft PE deepen PATH-TO-WORLD: PE single handle post-set/create.
  // Prefer self when Native.ptr or live shape; Soft chassis only when
  // Vehicle has neither (pre-set / TREE createCar smoke without set).
  if (!native_ptr_get(self) && physics_shape(self) == 0 && ch) return ch;
  return self;
}

float java_game_Vehicle_getSpeedSquare(InvObject* self) {
  // PE @ 0x00480500 size 0xa9 (169). Sig ()F.
  // JVM_UnboxArg this. Native.ptr (dword_62E008)==0 → Engine_strcat_cap
  // "!"+"Mighty ERROR"+Engine_ErrorLogMsgBox + return 0.0 (flt_5E73CC).
  // Else Engine_queryGameRefChannel(handle, GII_VEL=3, &xyz[3]) @
  // 0x00426470 (ecx=g_EngineState) — same channel as GameRef.getVel @
  // 0x0047DCE0. Return vx²+vy²+vz² (asm order y²+z²+x² ≡ same).
  // Soft PE deepen PATH-TO-WORLD (createCar/enterCar world spawn): PE
  // one handle (Vehicle Native.ptr after set/create = chassis). Soft
  // live PhysicsRef prefers chassis first (≡ gameref_live_phys_key) so
  // City/Bot spawn with chassis PhysicsRef + Vehicle Native.ptr type
  // leaf still reads live |v|². Soft chassis GameRefState only when
  // Vehicle has neither Native.ptr nor shape (pre-set / TREE smoke).
  // Shape present → physics_speed_square (incl. 0) — do not fall through
  // to a stale GameRef vx cache. Soft: no Mighty log; GameRef_getVel =
  // public GII_VEL=3 stand-in. Do not rename Engine_queryGameRefChannel
  // (164 xrefs).
  if (!self) return 0.f;
  if (InvObject* phys = vehicle_soft_phys_key(self))
    return physics_speed_square(phys);
  InvObject* key = vehicle_soft_tree_key(self);
  // Soft: when Vehicle has Native.ptr, query self so GameRef_getVel Softs
  // chassis phys via gameref_live_phys_key; Soft chassis key only when
  // Vehicle has no handle (TREE smoke setState on chassis).
  InvObject* vel =
      java_util_resource_GameRef_getVel(native_ptr_get(self) ? self : key);
  if (!vel) return 0.f;
  float vx = 0.f, vy = 0.f, vz = 0.f;
  vec3_get(vel, &vx, &vy, &vz);
  return vx * vx + vy * vy + vz * vz;
}

int32_t java_game_Vehicle_getHorn(InvObject* self) {
  // PE @ 0x0043DB60 size 0x93 (147). Sig ()I. IDA java_game_Vehicle_getHorn.
  // Unbox this. Native.ptr==0 → 0 (edi). NO Mighty.
  // inner=*(handle+0xC)==0 → 0. [inner+0x4C]!=1 → vtbl+0x14(1.0f).
  // ResHandle_PrepareLod(inner, 0x80000000, 0, 0); test eax,80000000h → 0.
  // vtbl+0xC(1.0f)==0 → 0; obj=*(eax+0x4C).
  // Asm @ 0x43DBCE..0x43DBE9: idx=[obj+0x1DCC]; hdr=[obj+0x1FBC];
  // setnz dword [hdr + idx*16 + 0x83C] (int_convert 7628/8124/2108).
  // Same row as Chassis.setHornSFX @ 0x0043DC00: slot =
  // hdr+0x834+idx*0x10 (16B ResourceRef); +8 ≡ hdr+0x83C+idx*16.
  // Writer: GameRef_voidEvent_parse "sethorn" @ 0x45A589 stores v%4 at
  // +0x1DCC (and ecx,80000003h). Host TREE "horn" + GameRefState.horn.
  // City.getHorn / Bot.pressHorn (post-enterCar): sethorn 0|1 then getHorn.
  // Soft PE deepen PATH-TO-WORLD: no +0x1FBC blob / PrepareLod hop OOS.
  // Prefer TREE horn_sfx_{idx} setnz(id|Native.ptr) ≡ PE mid; empty
  // Relink → 0 (do NOT fall through to setnz(idx)). Soft setnz(idx)
  // only when no SFX row so sethorn smoke works without prior setHornSFX.
  if (!self) return 0;
  InvObject* ch = tree_field_get_obj(self, "chassis");
  // Soft PE deepen PATH-TO-WORLD (createCar/enterCar): PE single handle
  // = Vehicle Native.ptr post-set (=chassis). Prefer self when Native.ptr
  // / live shape; Soft chassis only when Vehicle has neither (pre-set /
  // TREE smoke). Do NOT merge self horn==0 with chassis nonzero —
  // sethorn 0 must stick on the live Vehicle key (Bot.pressHorn).
  InvObject* key = vehicle_soft_tree_key(self);
  int32_t slot = tree_field_get_int(key, "horn");
  // PE signed-mod 4 on store; keep index in [0,4) for SFX row.
  slot %= 4;
  if (slot < 0) slot += 4;

  char sfx_key[32];
  std::snprintf(sfx_key, sizeof(sfx_key), "horn_sfx_%d", slot);
  // setHornSFX writes Chassis; Vehicle.set may leave SFX on chassis.
  InvObject* sfx = tree_field_get_obj(key, sfx_key);
  if (!sfx && key != self) sfx = tree_field_get_obj(self, sfx_key);
  if (!sfx && key == self && ch) sfx = tree_field_get_obj(ch, sfx_key);
  if (sfx) {
    // Soft PE deepen: PE setnz(*(hdr+0x83C+16*idx)) — empty Relink → 0
    // even when idx!=0. Host: id!=0 or Native.ptr ≡ nonzero mid.
    if (java_util_resource_ResourceRef_id(sfx) != 0) return 1;
    if (native_ptr_get(sfx)) return 1;
    return 0;
  }
  // Soft: no +0x1FBC SFX row — setnz(idx) for sethorn smoke.
  return slot ? 1 : 0;
}

float java_game_Vehicle_hasCrime(InvObject* self) {
  // PE @ 0x00440BF0 size 0x81 (129). Sig ()F.
  // Unbox this. Native.ptr==0 → -1.0 (stack 0xBF800000). inner==0 → -1.
  // [inner+0x4C]!=1 → vtbl+0x14(0). ResHandle_PrepareLod(0xA0000000);
  // sign-bit fail → -1. vtbl+0xC(1.0f); obj=*(eax+0x4C);
  // return *(float*)(obj+0x2104) (8452) — zone speed limit m/s (raw,
  // incl. -1.0 clear). Writer: GameRef_voidEvent_parse EVENT type
  // 0x400000 @ 0x45C170 — enter: [msg+0x2C] → [obj+0x2104] (@ 0x45C225);
  // leave/clear → dword -1.0f 0xBF800000 (@ 0x45C299). Also gates AI
  // throttle in sub_44E690 when (flags&0x1000) && limit>0. NOT
  // EVENT_COMMAND. City: maxSpeed=hasCrime()*1.1; if (maxSpeed>=0)
  // overspeed. Soft PE deepen PATH-TO-WORLD: zone EVENT OOS — TREE
  // crime_speed = +0x2104 stand-in. Soft key = self when Native.ptr/
  // shape else chassis (vehicle_soft_tree_key; createCar pre-set). PE
  // fail path (empty+id0+!Native.ptr) → -1; unset/0 → -1; stored>0 →
  // limit; stored<0 (PE clear) → -1. PrepareLod hop OOS.
  if (!self) return -1.f;
  // Soft PE deepen PATH-TO-WORLD (createCar/enterCar): PE single handle
  // (Vehicle Native.ptr post-set = chassis). Prefer self when Native.ptr
  // / live shape; Soft chassis only when Vehicle has neither. Do not
  // fall through self stored<=0 to chassis when Vehicle is the live key
  // (PE zone clear = -1 on that phys only).
  InvObject* key = vehicle_soft_tree_key(self);
  const float stored = tree_field_get_float(key, "crime_speed");
  const bool key_empty = java_util_resource_GameRef_isEmpty(key) != 0;
  // Soft PE deepen: PE Native.ptr==0 / inner==0 → -1. Host empty+id0
  // and no handle when Soft field also unset → -1.
  if (stored <= 0.f && key_empty &&
      java_util_resource_ResourceRef_id(key) == 0 && !native_ptr_get(key))
    return -1.f;
  if (stored > 0.f) return stored;
  // Soft: PE clear writes -1.0f; TREE unset=0 and Soft clear<0 → -1.
  return -1.f;
}

// GameType event/timer/createNativeInstance: natives_gametype.cpp

}  // namespace inv
