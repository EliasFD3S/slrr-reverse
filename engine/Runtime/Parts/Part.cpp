// Split from natives_generated_world.cpp — Part.cpp
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

// ---- W35-09 soft phys ResHandle_PrepareLod / getPayload (OOS W34-09) ----
// PE dual hop (partOnSlot/getLogo/disableSlot/install_OK family):
//   Native.ptr → *(h+0xC); type!=1 → vtbl+0x14(1.0f);
//   ResHandle_PrepareLod@5447D0(tag) ≥0 → vtbl+0xC(1.0f) mid;
//   nest=*(mid+0x44); block=*(mid+0x4C); second tag 0x20000000.
// PE getPayload@419860 = type check + PrepareLod + vtbl+0xC.
// Host: native_ptr_node + soft lod-ready (+0x64 bit0 / early +0x5C==0)
//   + res_handle_get_payload(A0) → &HostMidPayload. Tag 0x20000000 uses
//   same getEmbeddedMid stand-in after PrepareLod soft (host API A0-only).
// OOS: RelinkLodSlot / TouchResNode / vtbl* / phys flags+0x78 lists /
//   leaf+0x1120 logo / Veh_ensureSceneBound mass / sub_473B10 probe.
constexpr int32_t kPartResTagA0 = static_cast<int32_t>(0xA0000000u);
constexpr int32_t kPartResTag20 = static_cast<int32_t>(0x20000000u);

// Soft PrepareLod@5447D0 — PE ≥0 iff (node+0x64)&1; early set when +0x5C==0
// (≡ host_lgi_prepare_lod / sub_537240 early). Scale table / Relink OOS.
static bool part_soft_prepare_lod_ok(void* node, int32_t /*tag*/) {
  if (!node) return false;
  auto* p = reinterpret_cast<uint8_t*>(node);
  uint32_t* lod_st = reinterpret_cast<uint32_t*>(p + 0x64);
  if ((*lod_st & 1u) == 0u) {
    if (*reinterpret_cast<int32_t*>(p + 0x5C) == 0) {
      *lod_st |= 1u;
      *reinterpret_cast<float*>(p + 0x68) = 1.0f;
    }
  }
  return (*lod_st & 1u) != 0u;
}

// Soft getPayload@419860 — PrepareLod gate then host mid (&node mid).
static void* part_soft_get_payload(void* node, int32_t tag) {
  if (!node || !part_soft_prepare_lod_ok(node, tag)) return nullptr;
  // Host res_handle_get_payload accepts A0 only; PE post-PrepareLod
  // getEmbeddedMid is tag-agnostic — map 0x20000000 → A0 stand-in.
  (void)tag;
  return res_handle_get_payload(node, kPartResTagA0);
}

// Hop1 A0 → mid. Dual PE also requires nest@+0x44 + tag20.
// Soft: nest unwired (host default null) → mid only (TREE callers).
// Nest live but PrepareLod/getPayload fail → nullptr (PE fail).
static void* part_soft_phys_mid(InvObject* self, bool /*require_dual*/) {
  void* node = native_ptr_node(self);
  if (!node) return nullptr;
  void* mid = part_soft_get_payload(node, kPartResTagA0);
  if (!mid) return nullptr;
  void* nest =
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mid) + 0x44);
  if (!nest) return mid;
  if (!part_soft_get_payload(nest, kPartResTag20)) return nullptr;
  return mid;
}

// PE @ 0x00469340 size 0xD5 — IDA Part_getWear. Unbox this. Native.ptr
// (dword_62E008). Handle 0 / inner=*(handle+0xC)==0 / PrepareLod sign /
// vtbl+0xC==0 / nest=*(mid+0x44)==0 → 1.0 (no Mighty). Dual walk:
// [inner+0x4C]!=INSTANCE_GAME=1 → vtbl+0x14(1.0f); PrepareLod(0xA0000000);
// mid=vtbl+0xC; nest=*(mid+0x44); block=*(mid+0x4C); second PrepareLod
// (0x20000000) + vtbl+0xC. Success: 1.0 - *(float*)(block+0xB8)
// (int_convert 184) — PE stores *consumed* wear; Java unit = remaining
// 0..1 (1=new). Twin getTear @ 0x004695D0 (+0xB4, NOT inverted).
// Soft W35-09: Native.ptr live → dual PrepareLod/getPayload (fail→1.0);
// block+0xB8 OOS (host mid.block=attach≠part blob) → TREE "wear"
// (setWear writes Java-facing remaining directly).

float java_game_parts_Part_getWear(InvObject* self) {
  // PE @ 0x00469340 size 0xD5 (213). Unbox this; dual PrepareLod; fail→1.0;
  // success 1.0−*(block+0xB8). Host TREE wear until phys block live.
  if (!self) return 1.f;
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return 1.f;
  // Pristine default until first setWear (PE: no Native.ptr / no phys block).
  if (tree_field_get_int(self, "wear_set") == 0) return 1.f;
  return tree_field_get_float(self, "wear");
}

float java_game_parts_Part_setWear(InvObject* self, float value) {
  // PE @ 0x00469420 size 0xD3. Same phys walk as getWear; success →
  // *(payload+0x4C)+0xB8 = 1.0−value; fail walk → no-op store; always
  // returns value (no clamp). Host TREE "wear" = Java-facing value.
  if (!self) return value;
  tree_field_set_int(self, "wear_set", 1);
  tree_field_set_float(self, "wear", value);
  return value;
}

// PE @ 0x00469500 size 0xcf — IDA Part_setMaxWear. Handle 0 / failed
// phys walk → silent ret (no Mighty ERROR; contrast getWear/getTear
// fail → 1.0). No clamp. PE stores 1.0/value at *(inner+0x4C)+0xBC
// (flt_5F08F0 bytes 00 00 80 3F). Host TREE "max_wear" is the
// Java-facing lifetime budget (kmToMaxWear = km*1000; writes it
// directly). sub_5447D0 310 xrefs — not renamed.

void java_game_parts_Part_setMaxWear(InvObject* self, float value) {
  // PE @ 0x00469500 size 0xcf. Unbox this+F. Handle 0 / inner 0 /
  // sub_5447D0 sign / vtbl+0xC==0 → silent ret. NO Mighty (unlike
  // getWear @ 0x00469340 / getTear @ 0x004695D0 fail → 1.0).
  // Success: fstp 1.0/value at *(inner+0x4C)+0xBC. No clamp.
  // Host: Java kmToMaxWear budget in TREE max_wear (not 1/x).
  if (!self) return;
  tree_field_set_float(self, "max_wear", value);
}

// PE @ 0x004695D0 size 0xCF — IDA Part_getTear. Unbox this. Native.ptr
// (dword_62E008). Handle 0 / inner=*(handle+0xC)==0 / sub_5447D0 sign /
// vtbl+0xC==0 → 1.0 (no Mighty ERROR). Dual walk like getTexture:
// [inner+0x4C]!=INSTANCE_GAME=1 → vtbl+0x14(1.0f); sub_5447D0(0xA0000000);
// payload=vtbl+0xC; second=*(payload+0x44); block=*(payload+0x4C);
// second walk sub_5447D0(0x20000000). Success: float [block+0xB4]
// (int_convert 180) remaining 0..1 (NOT 1.0−x; wear +0xB8 inverted).
// Host TREE "tear" (setTear / tear_set); fail stand-in → 1.0.

float java_game_parts_Part_getTear(InvObject* self) {
  // PE @ 0x004695D0 size 0xCF. Phys → [*(payload+0x4C)+0xB4]; fail → 1.0.
  if (!self) return 1.f;
  if (tree_field_get_int(self, "tear_set") == 0) return 1.f;
  return tree_field_get_float(self, "tear");
}

float java_game_parts_Part_setTear(InvObject* self, float value) {
  // PE @ 0x004696A0 size 0xcd. Same phys walk as setWear @ 0x00469420;
  // success → *(payload+0x4C)+0xB4 = value (NOT 1.0−value; wear is
  // +0xB8 inverted). Fail walk → no-op store; always returns value
  // (no clamp). Host TREE "tear" = Java-facing value. sub_5447D0 /
  // dword_62E008 not renamed.
  if (!self) return value;
  tree_field_set_int(self, "tear_set", 1);
  tree_field_set_float(self, "tear", value);
  return value;
}

// PE @ 0x00469AB0 size 0xc7 — IDA java_game_parts_Part_getTexture.
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner=*(handle+0xC)==0
// / sub_5447D0 sign / vtbl+0xC==0 → return 0 (xor ebp,ebp). NO Mighty
// ERROR (unlike ResourceRef.id @ 0x0047D290). [inner+0x4C]!=INSTANCE_GAME=1
// → vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → 0. payload=vtbl+0xC(1.0f). second=*(payload+0x44);
// block=*(payload+0x4C). second 0 → 0. Same INSTANCE_GAME check on
// second. sub_5447D0(second, 0x20000000, 0.0, 0.0) sign / vtbl+0xC==0
// → 0. Success: dword [block+0x84] (int_convert 132). Twin of getMesh @
// 0x00469CC0 (+0x94 mesh). Same payload+0x4C block as getCar @
// 0x004690A0 / getWear @ 0x00469340. Java ()I → texture resource ID
// (Part.save). No Native.ptr / vtable / sub_5447D0 on host. Host TREE
// "part_texture" (setTexture @ 0x00469B80 writes it, always returns 1).
// sub_5447D0 / dword_62E008 not renamed.

int32_t java_game_parts_Part_getTexture(InvObject* self) {
  // PE @ 0x00469AB0 size 0xc7 (199). Phys walk → [*(payload+0x4C)+0x84].
  // Host stand-in: TREE part_texture until native phys objects exist.
  // race123 deepen: dual hop — second=*(payload+0x44) gate
  // sub_5447D0(0x20000000); success dword [block+0x84] (132).
  // race124 deepen: size 0xc7; NO Mighty; second hop 0x20000000.
  // race125 deepen: twin table getMesh+0x94 / getRenderType+0xA4;
  // sub_5447D0 (310+ xrefs) still unnamed; TREE part_texture unchanged.
  return self ? tree_field_get_int(self, "part_texture") : 0;
}

// PE @ 0x00469B80 size 0x13f — IDA Part_setTexture. Unbox this+I.
// Native.ptr. Dual walk (vtbl+0x14 / PrepareLod 0xA0000000 then
// 0x20000000 / vtbl+0xC). leaf=*(hop2+0xC); block=*(mid+0x4C).
// ResHandle_Bind(&slot, ID, type=7, 0) @ 0x00546070. If [block+0x84]!=0
// → Part_rebindTextureRes@467FF0(leaf, block, &slot). ALWAYS eax=1
// (even walk fail / Bind skip). NO Mighty. Twin setMesh type 5 / +0x94 /
// Part_rebindMeshRes@467D30; setRenderType type 14 / +0xA4 /
// Part_rebindRenderTypeRes@4682A0.
// Soft W35-09: Native.ptr live + dual fail → return 1 (no store, PE);
// Bind/rebind OOS → TREE part_texture; return 1 (not prev).
int32_t java_game_parts_Part_setTexture(InvObject* self, int32_t ID) {
  // PE @ 0x00469B80 size 0x13f (319). Unbox this+I; dual walk; Bind type=7;
  // ALWAYS eax=1. Host TREE part_texture (Bind/rebind OOS).
  if (!self) return 1;  // PE always 1 at epilogue
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return 1;
  tree_field_set_int(self, "part_texture", ID);
  return 1;
}

// PE @ 0x00469CC0 size 0xc7 — IDA java_game_parts_Part_getMesh.
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner=*(handle+0xC)==0
// / sub_5447D0 sign / vtbl+0xC==0 → return 0 (xor ebp,ebp). NO Mighty
// ERROR (unlike ResourceRef.id @ 0x0047D290). [inner+0x4C]!=INSTANCE_GAME=1
// → vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → 0. payload=vtbl+0xC(1.0f). mesh_obj=*(payload+0x44);
// block=*(payload+0x4C). mesh_obj 0 → 0. Same INSTANCE_GAME check on
// mesh_obj. sub_5447D0(mesh_obj, 0x20000000, 0.0, 0.0) sign / vtbl+0xC==0
// → 0. Success: dword [block+0x94] (int_convert 148). Same payload+0x4C
// pointer as getCar @ 0x004690A0 / getWear @ 0x00469340. Java ()I →
// ResourceRef(mshID) (Part.save). No Native.ptr / vtable / sub_5447D0 on
// host. Host TREE "part_mesh" (setMesh @ 0x00469D90 writes it, always 1).
// sub_5447D0 / dword_62E008 not renamed.

int32_t java_game_parts_Part_getMesh(InvObject* self) {
  // PE @ 0x00469CC0 size 0xc7 (199). Phys walk → [*(payload+0x4C)+0x94].
  // Host stand-in: TREE part_mesh until native phys/mesh objects exist.
  // race123 deepen: twin getTexture — second=*(payload+0x44);
  // sub_5447D0(0x20000000); success dword [block+0x94] (148).
  // race124 deepen: mesh @ +0x94 vs texture +0x84 / renderType +0xA4.
  // race125 deepen: same dual-hop family; TREE part_mesh unchanged.
  return self ? tree_field_get_int(self, "part_mesh") : 0;
}

// PE @ 0x00469D90 size 0x13f — IDA Part_setMesh. Unbox this+I. Dual Res
// walk as setTexture @ 0x00469B80. ResHandle_Bind(&slot, ID, type=5, 0).
// If [block+0x94]!=0 → Part_rebindMeshRes@467D30. ALWAYS eax=1 (even
// walk fail). NO Mighty.
// Soft W35-09: Native.ptr live + dual fail → return 1 (no store);
// Bind/rebind OOS → TREE part_mesh; return 1 (not prev).
int32_t java_game_parts_Part_setMesh(InvObject* self, int32_t ID) {
  // PE @ 0x00469D90 size 0x13f (319). Unbox this+I; dual walk; Bind type=5;
  // ALWAYS eax=1. Host TREE part_mesh (Bind/rebind OOS).
  if (!self) return 1;
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return 1;
  tree_field_set_int(self, "part_mesh", ID);
  return 1;
}

// PE @ 0x00469ED0 size 0xc7 — IDA java_game_parts_Part_getRenderType.
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner=*(handle+0xC)==0
// / sub_5447D0 sign / vtbl+0xC==0 → return 0 (xor ebp,ebp). NO Mighty
// ERROR (unlike ResourceRef.id @ 0x0047D290). [inner+0x4C]!=INSTANCE_GAME=1
// → vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → 0. payload=vtbl+0xC(1.0f). second=*(payload+0x44);
// block=*(payload+0x4C). second 0 → 0. Same INSTANCE_GAME check on
// second. sub_5447D0(second, 0x20000000, 0.0, 0.0) sign / vtbl+0xC==0
// → 0. Success: dword [block+0xA4] (int_convert 164). Twin of getTexture @
// 0x00469AB0 (+0x84 texture) / getMesh @ 0x00469CC0 (+0x94 mesh). Same
// payload+0x4C block as getCar @ 0x004690A0 / getWear @ 0x00469340. Java
// ()I → render-type resource ID (tyre LOD scripts setRenderType). No
// Native.ptr / vtable / sub_5447D0 on host. Host TREE "part_render_type"
// (setRenderType @ 0x00469FA0 writes it, always 1). sub_5447D0 /
// dword_62E008 not renamed.

int32_t java_game_parts_Part_getRenderType(InvObject* self) {
  // PE @ 0x00469ED0 size 0xc7 (199). Phys walk → [*(payload+0x4C)+0xA4].
  // Host stand-in: TREE part_render_type until native phys objects exist.
  // race123 deepen: twin getTexture/getMesh — second=*(payload+0x44);
  // sub_5447D0(0x20000000); success dword [block+0xA4] (164).
  // race124 deepen: +0xA4 (164); TREE part_render_type.
  // race125 deepen: same dual-hop; no phys host — TREE stand-in.
  return self ? tree_field_get_int(self, "part_render_type") : 0;
}

// PE @ 0x00469FA0 size 0x13f — IDA Part_setRenderType. Unbox this+I.
// Dual Res walk as setTexture. ResHandle_Bind(&slot, ID, type=14, 0).
// If [block+0xA4]!=0 → Part_rebindRenderTypeRes@4682A0. ALWAYS eax=1
// (even walk fail). NO Mighty.
// Soft W35-09: Native.ptr live + dual fail → return 1 (no store);
// Bind/rebind OOS → TREE part_render_type; return 1 (not prev).
int32_t java_game_parts_Part_setRenderType(InvObject* self, int32_t ID) {
  // PE @ 0x00469FA0 size 0x13f (319). Unbox this+I; dual walk; Bind type=14;
  // ALWAYS eax=1. Host TREE part_render_type (Bind/rebind OOS).
  if (!self) return 1;
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return 1;
  tree_field_set_int(self, "part_render_type", ID);
  return 1;
}

// ---- Part drag / chassis aero fields (PE + Java Part.java) ----
// Java Part fields (scripts; most lack PE string xrefs — only drag_center /
// C_drag / drag_act appear as PE C strings):
//   drag_occluded     — how much this occludes parent (mesh leaf+0x11E4)
//   drag_own          — own Cd if unoccluded          (mesh leaf+0x11E0)
//   drag_own_center   — Vector3 position if unoccluded
//   C_drag            — actual Cd after child occlusion (Chassis hdr+0x908)
//   drag_center       — Vector3 actual position       (Chassis hdr+0x8FC..904)
// BodyPart.drag_reduction is pure Java (Chassis.updatevariables subtracts
// it from fully_stripped_drag → C_drag) — no Part native; not hosted here.
//
// PE Part_computeDragAct @ 0x0046A4A0 size 0xF9 (renamed; ORPHAN — not in
// Natives_Register_PartDyno; 0 code/data xrefs). JNI-shaped: Unbox this;
// Native.ptr; dual ResHandle_PrepareLod 0xA0000000 then 0x20000000 like
// getWear; fail → 0.0. Success: Part_accumulateMeshDrag(*(hop2+0xC),
// block=*(mid+0x4C), mode=0) @ 0x0046A250; JVM_vm_set_float_field(this,
// "drag_act", *(float*)(block+0xF0)); return that float.
//
// PE Part_accumulateMeshDrag @ 0x0046A250 size 0x245 (renamed). thiscall
// (leaf, block, mode). int_convert: leaf+4576=0x11E0 drag_own, leaf+4580=
// 0x11E4 drag_occluded, leaf+4432=0x1150 slot count, block+120=0x78 slots
// (step 26 dwords=0x68), block+240=0xF0 store. Slot dword layout (PE):
//   [2]=+0x08 occupied, [3]=+0x0C ResHandle, [5]=+0x14 flags
//   (bit0=parent link kept in v24, bit1=child → dual Res hop).
// Formula:
//   v22 = *(leaf+0x11E0); v21 = 0; v24 = parent-slot or null;
//   for each slot (count leaf+0x1150) where [2]!=0:
//     flags&1 → remember parent slot (v24); else flags&2 + dual hop ok:
//       if mode==0: recurse Part_accumulateMeshDrag(child_leaf, child_block, 0);
//       // mode!=0: NO child recurse — read existing child_block+0xF0
//       v22 *= (1.0 - *(child_leaf+0x11E4));
//       v21 += *(child_block+0xF0);
//   *(block+0xF0) = v21 + v22;
//   if v24: dual hop parent handle → recurse mode=1 (always 1, not mode);
//   return *(this block+0xF0) (parent recurse does not replace retval).
// Callers: Part_computeDragAct@46A4A0 mode=0; Part_installMeshSlot@474170 /
// Part_removeMeshSlot@4723F0 mode=1 @4742BE / @47259A.
// Soft: scalar formula + TREE part_slots / part_on_slot as child stand-in
// (flags&2); part_parent as flags&1 parent-slot bubble. Mesh bit flags /
// ResHandle hops / leaf Cd tables OOS — not invented. drag_occluded /
// drag_own / drag_act are Java TREE mirrors of those leaf/block floats.

// Ensure Java-facing Vector3 bags Chassis_forceUpdate_apply reads
// (drag_center → hdr+0x8FC..+0x904). Floats written by scripts /
// part_soft_compute_drag_act (get returns 0 if absent).
static void part_soft_ensure_drag_fields(InvObject* self) {
  if (!self) return;
  if (!tree_field_get_obj(self, "drag_center"))
    tree_field_set_obj(self, "drag_center", vec3_new(0.f, 0.f, 0.f));
  if (!tree_field_get_obj(self, "drag_own_center"))
    tree_field_set_obj(self, "drag_own_center", vec3_new(0.f, 0.f, 0.f));
  // drag_occluded / drag_own / drag_act: get_float → 0 if absent (Java
  // Part.java defaults 0.0). accumulate always writes drag_act.
}

// Soft Part_accumulateMeshDrag @ 0x0046A250 (no mesh tables).
// mode==0: DFS recurse children first (computeDragAct path).
// mode!=0: read existing child.drag_act (install/remove path).
// Then drag_act = Σ child.drag_act + drag_own * Π(1 − child.drag_occluded);
// then bubble part_parent with mode=1 (PE flags&1 parent-slot recurse).
// Returns this node's drag_act (PE returns this block+0xF0).
static float part_soft_accumulate_mesh_drag(InvObject* self, int mode,
                                           int depth) {
  if (!self) return 0.f;
  part_soft_ensure_drag_fields(self);
  float v22 = tree_field_get_float(self, "drag_own");  // leaf+0x11E0
  float v21 = 0.f;
  if (depth < 32) {
    const int32_t n = part_slot_count(self);
    for (int32_t i = 0; i < n; ++i) {
      const int32_t sid = part_slot_id_at(self, i);
      if (sid <= 0) continue;
      InvObject* child = part_on_slot(self, sid);
      if (!child || child == self) continue;
      // PE: only slots with flags&2 + dual Res hop; soft: occupied
      // part_on_slot stand-in (flags&1 parent is part_parent, not a child).
      float child_act;
      if (mode == 0) {
        // PE @ 0x46A343: recurse child mode=0 before multiply/add.
        child_act = part_soft_accumulate_mesh_drag(child, /*mode=*/0,
                                                   depth + 1);
      } else {
        // PE mode!=0: no child recurse; use existing child_block+0xF0.
        part_soft_ensure_drag_fields(child);
        child_act = tree_field_get_float(child, "drag_act");
      }
      const float occl = tree_field_get_float(child, "drag_occluded");  // +0x11E4
      v22 *= (1.f - occl);
      v21 += child_act;  // child block+0xF0
    }
  }
  const float act = v21 + v22;  // store block+0xF0
  tree_field_set_float(self, "drag_act", act);
  // PE @ 0x46A3A5..0x46A442: if flags&1 parent slot kept → dual hop +
  // Part_accumulateMeshDrag(parent_leaf, parent_block, mode=1). Soft:
  // part_parent stand-in (install graph). depth guard avoids cycles.
  if (depth < 32) {
    InvObject* parent = tree_field_get_obj(self, "part_parent");
    if (parent && parent != self)
      part_soft_accumulate_mesh_drag(parent, /*mode=*/1, depth + 1);
  }
  return act;  // PE: return *(this block+0xF0), not parent's
}

// Soft Part_computeDragAct @ 0x0046A4A0. Dual-hop fail with Native.ptr → 0.0.
// Else soft accumulate mode=0 (child DFS + parent bubble).
static float part_soft_compute_drag_act(InvObject* self) {
  if (!self) return 0.f;
  part_soft_ensure_drag_fields(self);
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true)) {
    tree_field_set_float(self, "drag_act", 0.f);
    return 0.f;
  }
  return part_soft_accumulate_mesh_drag(self, /*mode=*/0, /*depth=*/0);
}

// PE Part_installMeshSlot@474170 / Part_removeMeshSlot@4723F0 call
// accumulate mode=1 on the live mesh; parent-slot recurse is inside
// accumulate (soft: part_parent bubble mode=1).
static void part_soft_refresh_drag_install_remove(InvObject* node) {
  if (!node) return;
  part_soft_accumulate_mesh_drag(node, /*mode=*/1, /*depth=*/0);
}

InvObject* java_game_parts_Part_install_OK(InvObject* self, InvObject* dest, int32_t slot, InvObject* part, int32_t slot2, InvObject* pos) {
  // PE @ 0x0046A680 size 0x1F3 — IDA java_game_parts_Part_install_OK.
  // Unbox this+dest+slot+part+slot2+pos (Vector3). dest==0 → null.
  // Native.ptr (dword_62E008). Dual ResHandle_getPayload @ 0x00419860
  // (0xA0000000 then 0x20000000). ResHandle_Rebind(*(dest+0xC)).
  // If pos: JVM_vm_get_float_field x/y/z (asc_611500/"x"..); else null
  // vec ptr. sub_473B10(block, &outA, &outB, &xyz|0) → probe ok flag.
  // Success: Engine_malloc(0x1C)+sub_410890("[I")+sub_407DA0 len2;
  // sub_42B060 store [0]=parentSlot [1]=childSlot. Fail → null.
  // W35-09 soft: Native.ptr live → dual getPayload gate (fail→null);
  // Rebind / sub_473B10 probe still OOS. Host TREE part_install / CFG.
  (void)pos;
  if (self && native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return nullptr;
  InvObject* parent = dest ? dest : self;
  if (!parent) return nullptr;

  int32_t ps = slot;
  int32_t cs = slot2 > 0 ? slot2 : 1;
  InvObject* child = part;

  // Phase 2.140: slot==0 → CFG auto-match for `self` onto `dest`.
  // Explicit slots keep Valocity/probe semantics (install `part` child).
  if (slot <= 0) {
    if (!self || !dest) return nullptr;
    if (!part_find_cfg_install(dest, self, &ps, &cs)) return nullptr;
    child = self;
    parent = dest;
    if (part) {
      java_util_resource_ResourceRef_set_1(part, dest);
      tree_field_set_obj(part, "script_instance", dest);
    }
  } else if (!child) {
    return nullptr;
  }

  if (!part_install(parent, ps, child, cs)) return nullptr;
  // PE Part_installMeshSlot@474170 @4742BE: accumulate(leaf, block, mode=1)
  // after link flags|2; parent bubble is inside accumulate (flags&1 →
  // mode=1). Soft: mode=0 on child first so TREE subtree drag_act exist
  // (cold install); then mode=1 on parent matches install entry. Child
  // mode=0 already bubbles part_parent; parent mode=1 re-reads children.
  if (child && child != parent)
    part_soft_accumulate_mesh_drag(child, /*mode=*/0, /*depth=*/0);
  part_soft_refresh_drag_install_remove(parent);
  // Stock returns int[2] = {parentSlot, childSlot}; host: length-2 vector.
  InvObject* out = tree_vector_new();
  InvObject* a = gameref_new();
  InvObject* b = gameref_new();
  tree_field_set_int(a, "value", ps);
  tree_field_set_int(b, "value", cs);
  tree_vector_add(out, a);
  tree_vector_add(out, b);
  return out;
}

// PE @ 0x0046A5A0 size 0xD9 — IDA java_game.parts.Part.flap(I)I.
// Unbox this + mode (I) via JVM_UnboxArg @ 0x0045D910. Native.ptr
// (dword_62E008) via JVM_vm_get_int_field @ 0x0042AB50. Handle 0 /
// inner=*(handle+0xC)==0 / [inner+0x4C]!=INSTANCE_GAME=1 → vtbl+0x14(1.0f
// 0x3F800000) / sub_5447D0(inner, 0xA0000000, 0, 0) sign / vtbl+0xC==0 →
// return -1 (or ebp,ebp=0xFFFFFFFF). NO Mighty ERROR. Two-level walk like
// getCar @ 0x004690A0 / getMesh @ 0x00469CC0: payload=vtbl+0xC(1.0f);
// second=*(payload+0x44); block=*(payload+0x4C); same INSTANCE_GAME +
// sub_5447D0(second, 0x20000000) / vtbl+0xC on second. Success:
// Part_flapApply(obj, block, mode) @ 0x00474B00 (size 0x29C, renamed).
// PE encode @ 0x474CA3 from phys [+0x88]:
//   enc = ((flags&1)==0) + 1;  // bit0 set→1, clear→2
//   if (flags&2) enc |= 4; if (flags&4) enc |= 8;
// mode 0 (Java getFlap): return encoding (miss loop → 0).
// mode 1 (Java toggleFlap): returns PRE-change encoding; mutates bits.
// modes 2–4: jump table @ 0x474D9C. Java only 0/1.
//
// TRADEOFF (race118–120 batch D — intentional, do NOT flip to PE 1/2):
// PE Part_flapApply @ 0x00474B00 encode from phys [+0x88]:
//   enc = ((flags&1)==0) + 1;  // bit0 set→1, clear→2
//   if (flags&2) enc |= 4; if (flags&4) enc |= 8;
// mode 0 (getFlap): return encoding (miss loop → 0).
// mode 1 (toggleFlap): returns PRE-change encoding; mutates bits.
// Smoke Phase 2.80 (main.cpp) requires Java-visible 0/1:
//   flap(0)→0, flap(1)→1, flap(0)→1, flap(1)→0.
// PE clear bit0 returns 2 (not 0); PE mode1 returns OLD enc → flap1==0
// fails smoke. Host keeps TREE flap_state as Java 0/1 bit0 stand-in and
// returns NEW state after toggle so smoke + part_flap_toggle stay green.
// race120: reconfirmed PE 1/2 vs smoke 0/1 — keep smoke-safe; no Part_flapApply.
// GAP: PE 1/2+|4+|8; mode1 anim (sub_43F280); miss→0 after walk ok;
// Native.ptr / Part_flapApply walk not hosted. Skip fillers isSlotDisabled.

int32_t java_game_parts_Part_flap(InvObject* self, int32_t mode) {
  // PE @ 0x0046A5A0 size 0xD9. Fail walk → eax=-1; Part_flapApply miss → 0.
  if (!self) return -1;
  int32_t st = tree_field_get_int(self, "flap_state");
  if (mode == 1) {
    // Host: return NEW 0/1 (smoke). PE mode1 returns OLD enc 1/2.
    st ^= 1;
    tree_field_set_int(self, "flap_state", st);
  }
  return st;  // intentional 0/1 vs PE ((~bit0)+1)|flags — see TRADEOFF
}

// PE @ 0x004690A0 size 0x7c — IDA java_game_parts_Part_getCar.
// Registered Natives_Register_PartDyno @ 0x0046B530 xref 0x0046B7FF.
// Callees: JVM_UnboxArg @ 0x0045D910, JVM_vm_get_int_field @ 0x0042AB50,
// sub_5447D0 @ 0x005447D0. Unbox this. Native.ptr (dword_62E008). Handle 0
// / inner=*(handle+0xC)==0 / sub_5447D0 sign / vtbl+0xC==0 → return 0
// (xor edi,edi). NO Mighty ERROR (unlike ResourceRef.id @ 0x0047D290).
// [inner+0x4C]!=INSTANCE_GAME=1 → vtbl+0x14(1.0f 0x3F800000).
// sub_5447D0(inner, 0xA0000000, 0.0, 0.0) test 0x80000000 → 0. Success:
// dword [*(vtbl+0xC(1.0f)+0x4C)+0xC8] (int_convert 200). Same payload+0x4C
// pointer as getWear @ 0x00469340 v5 (wear float +0xB8). Java ()I → new
// GameRef(carID) (Part.addPart). No Native.ptr / vtable / sub_5447D0 on
// host. Host TREE: part_car_root (part_parent walk) then ResourceRef.id on
// install-graph root. dword_62E008 / sub_5447D0 not renamed.

int32_t java_game_parts_Part_getCar(InvObject* self) {
  // PE @ 0x004690A0 size 0x7c. Phys walk → [*(payload+0x4C)+0xC8].
  if (!self) return 0;
  InvObject* root = part_car_root(self);
  return root ? java_util_resource_ResourceRef_id(root) : 0;
}

// PE @ 0x004691F0 size 0x96 — IDA java_game_parts_Part_getWheelID.
// Unbox this via JVM_UnboxArg @ 0x0045D910. Native.ptr (dword_62E008)
// via JVM_vm_get_int_field @ 0x0042AB50. Handle 0 / inner=*(handle+0xC)==0
// → return -1 (edi=0xFFFFFFFF). NO Mighty ERROR. [inner+0x4C]!=INSTANCE_GAME=1
// → vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → -1. payload=vtbl+0xC(1.0f); block=*(payload+0x4C).
// slot=[block+0xD0] (int_convert 208). slot>100 (0x64) && slot<=400 (0x190):
// return (slot-101)%10 (idiv 10, edx). Else -1. Single-level phys walk
// (not two-level getCar @ 0x004690A0). Java ()I → Chassis.getWheel(id).
// 1 xref data: Natives_Register_PartDyno @ 0x46B81E. No Native.ptr / vtable /
// sub_5447D0 on host. Host part_wheel_id: part_parent walk to hop under
// car root; chassis slots 101..104 → 0..3; tyre inherits via rim.
// dword_62E008 / sub_5447D0 not renamed.

int32_t java_game_parts_Part_getWheelID(InvObject* self) {
  // PE @ 0x004691F0 size 0x96. Phys → [*(payload+0x4C)+0xD0]; (slot-101)%10.
  if (!self) return -1;
  return part_wheel_id(self);
}

void java_game_parts_Part_disableSlot(InvObject* self, int32_t slotID,
                                      int32_t status) {
  // PE @ 0x00469770 size 0x1FD — IDA java_game_parts_Part_disableSlot
  // (renamed from Part_disableSlot). Unbox this+slotID+status.
  // Native.ptr (dword_62E008). Dual ResHandle_getPayload @ 0x00419860
  // (0xA0000000 then 0x20000000). List *[obj+0xC]+0x1154 (4436) via
  // Engine_SimObjectListEmpty / head+8; flags base=[edi+0x78] step 0x68.
  // Hit [node+0x48]==slotID: flags+0x14 &= ~0x50 (0xFFFFFFAF); then
  // status dec-switch (asm @ 0x469861): status==1 → |0x10; status==2 →
  // |0x40 then fallthrough |0x10 (=0x50); status==0 leave clear.
  // Mate-side (flags+0x8 child handle → ResHandle_PrepareLod walk):
  // same &=~0x50 + status OR on mate flags+0x14 (asm @ 0x469945).
  // Fail / miss → silent ret 0. NO Mighty.
  // W35-09 soft: Native.ptr live + dual getPayload fail → silent (PE).
  // Phys flags+0x78 / mate PrepareLod still OOS. Host TREE otherwise.
  if (self && native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return;
  part_disable_slot(self, slotID, status);
  if (!self || slotID == 0) return;
  InvObject* slots = tree_field_get_obj(self, "part_slots");
  if (!slots) return;
  const int32_t n = tree_vector_size(slots);
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(slots, i);
    if (!s || tree_field_get_int(s, "slot_id") != slotID) continue;
    tree_field_set_int(s, "disabled_status", status);
    return;
  }
}

// PE @ 0x00468AB0 size 0x193 (IDA java_game_parts_Part_getSlotDamage).
// dst[256]: [0]=byte_63C860 (0), memset rest. Unbox this+slotIndex
// (var_114 preset 0). Dual walk (vtbl+0x14(1.0f) / sub_5447D0
// 0xA0000000 then 0x20000000). slotIndex<0 or >=[*(leaf+0xC)+0x1150]
// → empty String. flags=*[edi+0x78]+slotIndex*0x68 (lea*8).
// ([flags+0x14]&0x20)==0 → "". Else Ypr_fromMatrix(flags+0x38) +
// Util_Sprintf "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f" (pos +0x2C..+0x34 + ypr).
// ALWAYS JVM_String_from_cstr(dst) — never null. Twin setSlotDamage @
// 0x00468C50. Host g_slot_dmg CSV; "" on miss (no phys flags).
InvObject* java_game_parts_Part_getSlotDamage(InvObject* self,
                                              int32_t slotIndex) {
  // PE @ 0x00468AB0 size 0x193. Dual phys walk; flags&0x20 → sprintf 6f CSV;
  // ALWAYS String_from_cstr (empty on miss). Host g_slot_dmg CSV.
  if (!self || slotIndex < 0) return string_new("");
  if (slotIndex >= part_slot_count(self)) return string_new("");
  auto it = g_slot_dmg.find(self);
  if (it == g_slot_dmg.end()) return string_new("");
  auto jt = it->second.find(slotIndex);
  if (jt == it->second.end() || jt->second.empty()) return string_new("");
  return string_new(jt->second.c_str());
}

// PE @ 0x00468C50 size 0x193 — IDA java_game_parts_Part_setSlotDamage.
// Unbox this + slotIndex (I) + data (Ljava/lang/String;) via JVM_UnboxArg
// @ 0x0045D910. Native.ptr (dword_62E008) via JVM_vm_get_int_field @
// 0x0042AB50. Same two-level walk as getSlotDamage @ 0x00468AB0 /
// getSlots @ 0x004684A0: handle→inner+0xC, INSTANCE_GAME / vtbl+0x14(1.0f)
// / sub_5447D0(inner, 0xA0000000) sign / vtbl+0xC; second=*(payload+0x44);
// edi=*(payload+0x4C); sub_5447D0(second, 0x20000000) / vtbl+0xC;
// obj=*(payload+0xC). slotIndex>=0 && slotIndex<[obj+0x1150] (4432) else
// silent ret. flags=*(edi+0x78)+slotIndex*0x68 (104). String len>1:
// Util_Sscanf "%f,%f,%f,%f,%f,%f" → pos [flags+0x2C..+0x34] (44..52),
// Ypr_toMatrix @ +0x38 (56), [flags+0x14]|=0x20. len<=1: [flags+0x14]&=~0x20.
// Fail walk → silent ret (no Mighty ERROR). Java (ILjava/lang/String;)V.
// Host g_slot_dmg[slotIndex] = formatted 6-float CSV (getSlotDamage twin);
// erase when clear. No Native.ptr / vtable / sub_5447D0 on host.
// dword_62E008 / sub_5447D0 not renamed.

void java_game_parts_Part_setSlotDamage(InvObject* self, int32_t slotIndex,
                                        InvObject* data) {
  // PE @ 0x00468C50 size 0x193. Phys flags walk; len>1 → 6-float blob.
  if (!self || slotIndex < 0) return;
  if (slotIndex >= part_slot_count(self)) return;

  const char* s = data ? string_cstr(data) : "";
  const size_t len = s ? std::strlen(s) : 0;
  if (len <= 1) {
    auto it = g_slot_dmg.find(self);
    if (it != g_slot_dmg.end()) it->second.erase(slotIndex);
    return;
  }

  float px = 0.f, py = 0.f, pz = 0.f, oy = 0.f, op = 0.f, or_ = 0.f;
  std::sscanf(s, "%f,%f,%f,%f,%f,%f", &px, &py, &pz, &oy, &op, &or_);
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", px, py, pz,
                oy, op, or_);
  g_slot_dmg[self][slotIndex] = buf;
}

void java_game_parts_Part_setSlotPos(InvObject* self, int32_t slotID, InvObject* pos, InvObject* ypr) {
  // PE @ 0x00468DF0 size 0x2A9 — setSlotPos(ILjava.lang.Vector3;Ljava.lang.Ypr;)V.
  // Unbox this+slotID+pos+ypr. Dual sub_419860 walk; flags at [edi+0x78]
  // via list walk (sub_429390/sub_40CFC0). Writes pos +0x2C..+0x34, clears
  // bit0x20 then sets if pos; Ypr_toMatrix+Mat3x4_mulLeft on +0x38 if ypr;
  // may sub_473230/sub_470E20 rebuild. Always ret 1 (eax discarded VOID).
  // Host TREE: part_set_slot_pos + WheelRef sync slots 101..104.
  // GAPS: sub_419860 / phys flags / Mat3x4_mulLeft not mirrored.
  part_set_slot_pos(self, slotID, pos, ypr);
  // Phase 2.65: chassis wheel slots keep WheelRef state in sync.
  if (slotID >= 101 && slotID <= 104) {
    InvObject* rim = part_on_slot(self, slotID);
    if (!rim) return;
    auto& w = WR(rim);
    if (pos) {
      vec3_get(pos, &w.px, &w.py, &w.pz);
      w.has_pos = true;
    }
    if (ypr) {
      ypr_get(ypr, &w.oy, &w.op, &w.or_);
      w.has_ypr = true;
    }
  }
}

// PE @ 0x004684A0 size 0xc4 — IDA java_game_parts_Part_getSlots.
// Unbox this (JVM_UnboxArg @ 0x0045D910). Native.ptr via
// JVM_vm_get_int_field @ 0x0042AB50 (dword_62E008). ebx=0 fail sentinel
// (xor ebx,ebx; mov eax,ebx). Handle 0 / inner=*(handle+0xC)==0 → 0.
// NO Mighty ERROR. [inner+0x4C]!=INSTANCE_GAME=1 → vtbl+0x14(1.0f
// 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0) test 0x80000000
// → 0. payload=vtbl+0xC(1.0f)==0 → 0. second=*(payload+0x44)==0 → 0.
// Same INSTANCE_GAME + sub_5447D0(second, 0x20000000, 0.0, 0.0) /
// vtbl+0xC. Success: dword [*(*(vtbl+0xC(1.0f))+0xC)+0x1150]
// (int_convert 4432). Two-level walk like getMesh @ 0x00469CC0.
// Xref: Natives_Register_PartDyno @ 0x0046B536. Java ()I → max slot
// index+1 (Part.java). Host TREE: part_slot_count (part_slots size).
// No Native.ptr / vtable / sub_5447D0 on host. dword_62E008 /
// sub_5447D0 not renamed (race109 TREE gate, 100+ xrefs).

int32_t java_game_parts_Part_getSlots(InvObject* self) {
  // PE @ 0x004684A0 size 0xc4. Phys → [*(*(obj+0xC)+0x1150)]; fail ebx=0.
  if (!self) return 0;
  return part_slot_count(self);
}

// PE @ 0x00468570 size 0x164 — IDA java_game_parts_Part_getSlotID.
// Unbox this + slotIndex (I). Native.ptr (dword_62E008). Same two-level
// phys walk as getSlots @ 0x004684A0: handle→inner+0xC, INSTANCE_GAME /
// vtbl+0x14(1.0f) / sub_5447D0(inner, 0xA0000000) sign / vtbl+0xC;
// second=*(payload+0x44); edi=*(payload+0x4C); sub_5447D0(second,
// 0x20000000) / vtbl+0xC; obj=*(payload+0xC). Fail → ebp=0. NO Mighty
// ERROR. After walk (asm jl @ 0x46864B; Hex-Rays drops this branch):
//   slotIndex>=0: list=obj+0x1154 (4436); head=sub_429390?0:[list+8];
//     walk idx times via [node+4] (null if [node+4]==0 || *+4==0);
//     return [node+0x48] (72).
//   slotIndex<0 (Java -1): head from *[obj+0x115C] (4444) via [list+4]
//     gate; flags=[edi+0x78]+0x14 step 0x68 over count=[obj+0x1150];
//     stop when flag&1; return [node+0x48] (mate on this part).
// Java (I)I: index→slot ID; -1→parent mate slot ID (Part.java). Host TREE:
// >=0 part_slot_id_at (OOB→0); <0 mate via part_parent +
// part_slot_id_on_slot(parent, part_parent_slot). No Native.ptr / vtable /
// sub_5447D0 on host. dword_62E008 / sub_5447D0 / sub_429390 not renamed.

int32_t java_game_parts_Part_getSlotID(InvObject* self, int32_t slotIndex) {
  // PE @ 0x00468570 size 0x164. Phys → [node+0x48]; fail ebp=0.
  if (!self) return 0;
  if (slotIndex < 0) {
    // PE loc_468688: *[obj+0x115C] / flag&1 → mate slot ID on self.
    InvObject* parent = tree_field_get_obj(self, "part_parent");
    const int32_t ps = tree_field_get_int(self, "part_parent_slot");
    if (!parent || ps == 0) return 0;
    return part_slot_id_on_slot(parent, ps);
  }
  // PE loc_46864D: +0x1154 / walk [node+4] slotIndex → [node+0x48].
  const int32_t id = part_slot_id_at(self, slotIndex);
  return id < 0 ? 0 : id;
}

// PE @ 0x004686E0 size 0x10B — IDA java_game_parts_Part_getSlotIndex.
// Unbox this + slotID (I) into arg0 / var_4 (JVM_UnboxArg). Native.ptr
// (dword_62E008). Same two-level walk as getSlotID @ 0x00468570 /
// getSlots @ 0x004684A0: handle→inner+0xC, INSTANCE_GAME /
// vtbl+0x14(1.0f) / sub_5447D0(inner, 0xA0000000) sign / vtbl+0xC;
// second=*(payload+0x44); sub_5447D0(second, 0x20000000) / vtbl+0xC;
// obj=*(payload+0xC). List: ecx=*[obj+0x115C] (4444); add eax,1154h
// dead (overwritten); head=([list+4]!=0)?list:0 — same node chain as
// getSlotID idx≥0 head v7[2]=*(obj+0x1154+8), gate differs (no
// sub_429390). Loop @ 0x4687C1: cmp [node+0x48], var_4/slotID → jz
// return edx; else node=[node+4] (null if node==0 || [node+4]==0),
// ++edx; miss ebx=0. Hex-Rays falsely showed while([node+0x48]!=0).
// Index 0 and fail both return 0 (PE). No Mighty ERROR. Inverse of
// getSlotID(idx≥0) by ID. Host TREE: scan part_slot_id_at until match.
// No Native.ptr / vtable / sub_5447D0 on host. dword_62E008 /
// sub_5447D0 not renamed.

int32_t java_game_parts_Part_getSlotIndex(InvObject* self, int32_t slotID) {
  // PE @ 0x004686E0 size 0x10B. Phys: [node+0x48]==slotID → index.
  if (!self) return 0;
  const int32_t n = part_slot_count(self);
  for (int32_t i = 0; i < n; ++i) {
    if (part_slot_id_at(self, i) == slotID) return i;
  }
  return 0;
}

// PE @ 0x00469970 size 0x13C — IDA Part_isSlotDisabled. Unbox this+slotID.
// Native.ptr. Dual walk as getSlotIndex @ 0x004686E0. List head
// *[obj+0x115C] gated by [list+4] (int_convert 4444); flags base=
// [edi+0x78] step 0x68. Hit [node+0x48]==slotID: flags+0x14 test 0x50
// → 2; else (flags>>4)&1 (= bit 0x10 → 1). Miss / fail → ebx=0.
// Twin disableSlot @ 0x00469770: status1→0x10, status2→0x50, 0 clear.
// Host TREE disabled_status (set by disableSlot) → PE 2/1/0; legacy
// disabled bit alone → 2.

int32_t java_game_parts_Part_isSlotDisabled(InvObject* self, int32_t slotID) {
  // PE @ 0x00469970 size 0x13C. ebx=0 fail; &0x50→2; else (>>4)&1.
  if (!self || slotID == 0) return 0;
  InvObject* slots = tree_field_get_obj(self, "part_slots");
  if (!slots) return 0;
  const int32_t n = tree_vector_size(slots);
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(slots, i);
    if (!s || tree_field_get_int(s, "slot_id") != slotID) continue;
    const int32_t st = tree_field_get_int(s, "disabled_status");
    if (st == 2) return 2;
    if (st == 1) return 1;
    if (st == 0 && tree_field_get_int(s, "disabled") != 0) return 2;
    return 0;
  }
  return 0;
}

// PE @ 0x0046A0E0 size 0xc4 — IDA Part_getLogo.
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner 0 /
// ResHandle_PrepareLod sign / vtbl+0xC==0 → 0. Dual hop: first
// 0xA0000000, second=*(payload+0x44) with 0x20000000. Success:
// dword [*(*(leaf+0xC)+0x1120)] (int_convert 4384). NO Mighty.
// Twin family getTexture +0x84 / getMesh +0x94. Java Part.getLogo()
// scripts often wrap manufacturer; PE reads phys logo rid.
// W35-09 soft: Native.ptr live → dual PrepareLod/getPayload (fail→0);
// leaf+0x1120 rid OOS → TREE manufacturer||logo.
int32_t java_game_parts_Part_getLogo(InvObject* self) {
  // PE @ 0x0046A0E0 size 0xc4 (196). Unbox this; dual PrepareLod; fail→0;
  // success dword [*(*(leaf+0xC)+0x1120)]. Host TREE manufacturer||logo.
  if (!self) return 0;
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return 0;
  const int32_t m = tree_field_get_int(self, "manufacturer");
  if (m) return m;
  return tree_field_get_int(self, "logo");
}

float java_game_parts_Part_getMass(InvObject* self) {
  // PE @ 0x00469290 size 0xAC — IDA java_game_parts_Part_getMass.
  // Unbox this. Native.ptr. Single hop only (ResHandle_PrepareLod
  // 0xA0000000; NO second 0x20000000). Handle 0 / inner 0 / sign /
  // vtbl+0xC==0 / Veh_ensureSceneBound(*(payload+0x4C)+0x1C)==0 /
  // inv_mass<=0 → 0.0. Success: 1.0 / *(float*)( *[*(body+0x84)+0x5C]
  // +0x14 ) (int_convert body+0x84=132, +0x5C=92, inv+0x14=20).
  // Veh_ensureSceneBound @ 0x0048AEA0. NO Mighty.
  // W35-09 soft: Native.ptr live → single PrepareLod/getPayload
  // (fail→0.0); body mass OOS → TREE mass>0.
  if (!self) return 0.f;
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/false))
    return 0.f;
  const float m = tree_field_get_float(self, "mass");
  return m > 0.f ? m : 0.f;
}

// PE @ 0x0046A1B0 size 0x9d — IDA java_game_parts_Part_setSfxLoopParams.
// Unbox this+F+F (var_8/var_4). Native.ptr==0 / inner 0 /
// ResHandle_PrepareLod(0xA0000000) sign / vtbl+0xC==0 → return 0.
// Single hop only (NO 0x20000000). Success: block=*(obj+0x4C);
// *[block+0x114]=a, *[block+0x118]=b; return 1. NO Mighty.
// W35-09 soft: Native.ptr live → single getPayload (fail→0); phys
// block+0x114 store OOS (attach≠part blob) → TREE sfx_loop_a/b.
int32_t java_game_parts_Part_setSfxLoopParams(InvObject* self, float a,
                                              float b) {
  // PE @ 0x0046A1B0 size 0x9d. Fail walk → 0; success store + return 1.
  if (!self) return 0;
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/false))
    return 0;
  tree_field_set_float(self, "sfx_loop_a", a);
  tree_field_set_float(self, "sfx_loop_b", b);
  return 1;
}

// PE @ 0x004687F0 size 0x19F — IDA java_game_parts_Part_partOnSlot.
// Unbox this+slotID. Native.ptr (dword_62E008). Dual ResHandle_PrepareLod
// walk (0xA0000000 then 0x20000000) like getSlotIndex @ 0x004686E0.
// Fail walk → 0. NO Mighty. slotID==-1 → dword [payload+0x50] (self
// Part*; same +0x50 as getCarRef hop2). Else: list=*(obj+0xC)+0x1154
// (4436); head via Engine_SimObjectListEmpty / [list+8]; flags base=
// [block+0x78] step 0x68; match [node+0x48]==slotID; require
// flags+0x8!=0 + child handle flags+0xC; third PrepareLod 0xA0000000
// on child → return *[child_payload+0x50]. Miss → 0.
// W35-09 soft: Native.ptr live → dual hop (fail→null); third child
// PrepareLod / list+0x1154 OOS → TREE part_on_slot.
InvObject* java_game_parts_Part_partOnSlot(InvObject* self, int32_t slotID) {
  // PE @ 0x004687F0 size 0x19F. Fail → null; -1 → self Part*; else child.
  if (!self) return nullptr;
  if (native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return nullptr;
  if (slotID == -1) return self;
  return part_on_slot(self, slotID);
}

// PE @ 0x00468990 size 0x11F — IDA java_game_parts_Part_slotIDOnSlot.
// Unbox this+slotID. Dual ResHandle_PrepareLod (0xA0000000 /
// 0x20000000). List head *[obj+0x115C] (4444) gated by [list+4];
// flags base=[edi+0x78] step 0x68. Match [node+0x48]==slotID →
// dword [flags+0x10] (mate/child slot ID). Miss / fail → 0. NO Mighty.
// W35-09 soft: Native.ptr live → dual hop (fail→0); flags+0x10 OOS →
// TREE part_slot_id_on_slot.
int32_t java_game_parts_Part_slotIDOnSlot(InvObject* self, int32_t slotID) {
  // PE @ 0x00468990 size 0x11F. Phys → [flags+0x10]; fail 0.
  if (self && native_ptr_get(self) && !part_soft_phys_mid(self, /*dual=*/true))
    return 0;
  return part_slot_id_on_slot(self, slotID);
}

// PE @ 0x00469120 size 0xc9 — IDA Part_getCarRef (renamed).
// Unbox this. Native.ptr (dword_62E008). Handle 0 / inner=*(handle+0xC)==0
// / sub_5447D0 sign / vtbl+0xC==0 → return 0 (xor eax). NO Mighty ERROR
// (unlike ResourceRef.id @ 0x0047D290). [inner+0x4C]!=INSTANCE_GAME=1 →
// vtbl+0x14(1.0f 0x3F800000). sub_5447D0(inner, 0xA0000000, 0.0, 0.0)
// test 0x80000000 → 0. payload=vtbl+0xC(1.0f). Same payload+0x4C block as
// getCar @ 0x004690A0 (+0xC8 carID) / getWear @ 0x00469340: chassis
// native = dword [*(payload+0x4C)+0xCC] (int_convert 204). Null → 0.
// Second walk on that object (INSTANCE_GAME / sub_5447D0 / vtbl+0xC);
// success → dword [payload2+0x50] (int_convert 80) = Java Part* (same
// +0x50 as partOnSlot -1 @ 0x004687F0). Fail → 0. Java ()Ljava.game.
// parts.Part; → chassis/root Part (Part.getWheel).
// race121 deepen: confirmed two-level walk +0x4C→+0xCC chassis handle →
// second +0x50 Part*; host TREE part_car_root remains install-graph
// stand-in (no Native.ptr / sub_5447D0). dword_62E008 / sub_5447D0 not
// renamed (310+ xrefs).
// race122: IDA reconfirm same offsets; still no phys host — deepen docs only.

InvObject* java_game_parts_Part_getCarRef(InvObject* self) {
  // PE @ 0x00469120 size 0xc9 (201). Phys: [*(payload+0x4C)+0xCC] → [+0x50].
  // Host stand-in: install-graph root via part_parent until native phys.
  // race123/124 deepen: chassis @ block+0xCC; hop1 → [payload2+0x50];
  // hop0 flag 0xA0000000; TREE part_car_root.
  // race125 deepen: dual walk reconfirm; no phys host — part_car_root.
  return part_car_root(self);
}

}  // namespace inv
