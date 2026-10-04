#pragma once

// W7B — voidEvent ResHandle_Rebind owner path (PE @ 0x00429060).
// Declared here so GameRef can call without touching host_objects.hpp yet.

#include "natives.hpp"

#include <cstdint>

namespace inv {

// PE ResHandle_Rebind @ 0x00429060:
// this = ResHandle blob (bone+0x44 / light_node+0xC); a2 = owner HostResNode*|0.
// Unlink old owner+0x48; if a2 head-insert a2+0x48; rh+8=*(a2+0x50); rh+0xC=a2.
void res_handle_rebind(void* rh, void* owner_node);

// voidEvent add_light @ 0x45AFC9: Rebind(node+0xC, *(type_handle+0xC)).
// Ensures type Native.ptr; optional seed_key → type_node+0x50 when zero
// (rem_light matches node+0x14 == that key). Returns *(rh+8) after Rebind.
int32_t void_event_res_handle_rebind_owner(void* rh /*light_node+0xC*/,
                                           InvObject* type, int32_t seed_key);

// rem_light dtor @ 0x45F8B0 (GameRef_LightNode_dtor): unlink rh from owner list
// (same as Rebind(rh, 0) owner slice — no second Rebind call in PE).
void void_event_res_handle_unbind_owner(void* rh /*light_node+0xC*/);

// W8D — PE GameRef_voidEvent_parse @ 0x45C3C9 (EVENT type 0x80 case 2):
// ResHandle_Rebind(stack_rh_ch18, Engine_LoadGameInit(...)).
// Owner = LoadGameInit GI return (node +0x48/+0x50), NOT InvObject type.
// Parent GameRef wire: after LoadGameInit stand-in returns GI, call this
// before voiding "controllable %d" / "AI_GoToTrafficSlow" on that RH.
void void_event_loadgameinit_rebind(void* rh /*stack ResHandle ch18*/,
                                    void* loadgameinit_gi /*owner|0*/);

// W9A/W11B/W12B/W13B/W14C/W15C — PE Engine_LoadGameInit @ 0x0053A5E0 size 0x51f.
// cdecl (wtroot_rh, type_rh, params_cstr, alias_cstr) → GI*|0.
// voidEvent @ 0x45C3BD: (wtroot, g_voidEvent_LoadGameInit_typeRH@63C680,
//   "1,0.25", "bot") then ResHandle_Rebind @ 0x45C3C9.
// Host: AllocLocalRid +0x50, InstanceNode_PoolAlloc 348B, CreateNodeByDesc
// TREE splice (parent+0x38 / sentinel+0x30), flags 33|0x200, alias, LOD.
// W12B: wtroot+0xC==0 → ResolveParent(wt_key,type=1) @ 0x537000.
// W13B: eng+0x11C SimObject root (FindByRid); Class*→FQN via
// Class_getNameCstr@404EA0 (payload+0x10) → attach_gametype.
// W14C: seed mid+0x10 via resource_engine_type_gametype @ 0x53A1A0 /
//   mov [eax+10h],ecx @ 0x53A27D (createNativeInstance @ 0x481AD6).
// W15C: parent LOD @ 0x53A706 — PrepareLod@5447D0 gate (+ sub_537240 early
//   when +0x5C==0) + ResNode_setLodReadyBit@544B90; float copies ≡
//   ResNode_vtbl+0x18/+0x14 (setLodCopy70/6C, scale@+0xBC=1).
// W16A: ResNode_relinkLodSlot@537790 — tryRelinkLod@53F090 (vtbl+0x10) +
//   setLodCopy6C behind maybeRelinkLod@425150.
// W17A: eng+0xE0C freelist pop + slot+0x18→node+0x48 + OR 0x4000000 +
//   active push eng+0xE00/0xDF8; seed cold freelist.
// W18B: ResourceEngine_PumpUnloadQueue@537B40 mark+recycle → freelist push
//   @ 0x537C51 (MainLoop/forceRendering/flush callers still unwired here).
// W28C: PumpUnload SimObjectList@429350 shell + GCSweep@537ED0 soft
//   (counters 765F10/14/18); Bind LookupById@536820 extracted.
// W29C: GCSweep type-buckets (stride 0x90, touch@+0x150, typeGate@618DC8).
// W30C: PumpUnload mark via ResNode_wantUnloadFrame@53EFB0 (vtbl+0x2C,
//   stamp +0x78); drain vtbl+0x30 null@545220 (base never pushes SimList).
// W31C: mid_gameTypeUnloadUpdate@53E620 PrepareLod soft (mid+0x44 nested).
// W32C: GCSweep force eng+0x106ED8..EE4 + pre-scan budget Hi@618D4C..
// W33C: GCSweep touch setLodCopy6C(0); aged-scan evict +0x198/+0x1C0;
//   post-evict Lo@618D50.. + caps@618F80...
// W34: ResNode_tryUnload@53EC20 soft (gate/children/lod-ready tail).
// W35-03: TouchResNode@537D10 soft (tryUnload/PrepareLod/PumpLoad).
// Gaps: CreateNodeByDesc cases≠1; RemapLocalId TOC@+0x5C File_SlotRead;
//   Bind Resolve miss; EnsureIndex RPAK TOC/+0x5C table;
//   LoadPack@538380; void producers; GameType ctor vtbl+0xC
//   @ 0x53A8B2; GT Destructor vtbl+0x10; GT Update vtbl+0x14/+0x18.
// W36 Soft: RemapFromPath@538440 + cloneHdr lazy Bind (sl/particles RHs).
void* engine_load_game_init(void* wtroot_rh, void* type_rh,
                            const char* params, const char* alias);

// PE ResourceEngine_RemapFromPath @ 0x538440 — PathEq/InitSlot/EnsureIndex/
// RemapLocalId. Soft: rpak_open cold + pack slot stand-in.
uint32_t resource_engine_remap_from_path(const char* path, uint32_t local);

// PE Chassis_forceUpdate_cloneHdr @ 0x43EDD7.. — lazy Remap+Bind into BSS
// g_voidEvent_LoadGameInit_typeRH / g_RH_particles_rpk_A/B.
void resource_engine_clone_hdr_lazy_binds();
void* resource_engine_void_event_type_rh();
uint32_t resource_engine_void_event_type_rh_id();
uint32_t resource_engine_particles_rh_a_id();
uint32_t resource_engine_particles_rh_b_id();

// W18A/W19C/W20A/W21D/W22D — PE Engine_CreateGameInstanceNative @ 0x0053A2A0
// size 0x332. cdecl (parent_rh, type_rh, script, params, alias) → GI*|0.
// create_native @ 0x47D975: (parent|wtroot, type, script=0, params, alias).
// createNativeInstance @ 0x481B52: (parent, &typeHandle, script=this, …).
// Shared with LoadGameInit: AllocLocalRid + InstanceNode TREE + LOD + |0x200.
// Diff: gate type_rh+8!=0 (not parent+8); GameTypeCtor only — NO THRD-CREATE.
// W19C: ResHandle_Rebind(GI+0xD8 mid+0x38, *(type_rh+0xC)) @ 0x53A435.
// W20A: mid+0x50=script(a3) @ 0x53A515.
// W21D: ResNode_unionChildAabb @ 0x4988A0 (call @ 0x53A546 / LoadGameInit
//   @ 0x53AA3A) — AABB union type==2 kids → GI+0x80/+0x84/+0x90.
// W22D: ResNode_drainSpatial @ 0x498B10 (while @ 0x53A54C / 0x53AA40) —
//   early aabb==2 + parent-climb stop; reparent/sibling OOS.
// Gaps: drain reparent/sibling; Type53_ensureHandleSlot@4B3EE0.
// create_native JNI @ 0x47D975 wired (parent).
void* engine_create_game_instance_native(void* parent_rh, void* type_rh,
                                         void* script, const char* params,
                                         const char* alias);

// PE ResHandle_maybeRelinkLod @ 0x00425150 — gate HostResNode+0x54 & 0x100
// then RelinkLodSlot(0.1, 0, 1.0). host_res_node = *(handle+0xC).
void res_handle_maybe_relink_lod_node(void* host_res_node);

// PE ResourceEngine_PumpUnloadQueue @ 0x537B40 — mark+recycle freelist
// eng+0xE0C; W28C SimList ctor/empty + GCSweep(0) soft; W29C type-bucket
// touch/age + typeGate scan; W30C wantUnloadFrame@53EFB0 stamp gate;
// W31C mid@53E620 PrepareLod soft; W32C GCSweep force+budget Hi;
// W33C touch setLodCopy6C(0)+evict move/Lo/caps; W34 tryUnload@53EC20 soft;
//   W35-03 TouchResNode@537D10 soft.
// Returns 0. Still OOS: GT Update/Destructor vtbl; dllist teardown.
int resource_engine_pump_unload_queue();

// PE ResourceEngine_PumpLoadQueue @ 0x5378D0 size 0x270 — walk eng+0xDB8
// load dllist; complete ResNode vtbl+0x28; recycle eng+0xDD0/0xDD4;
// LoadRing → eng+0x106ED4 isLoading.
// W19A: empty + ring/isLoading. W20B: LABEL_28 recycle. W21A: vtbl+0x28
// LoadLod LABEL_68 slice + flag clear + buf==0 recycle. W22A: cold parse
// slice. Returns 0.
int resource_engine_pump_load_queue();

// PE ResNode_GameType_parseLodBuf @ 0x53DEE0 — W22A prologue + W23A body
// (PHYS/POLY mid) + W24A EXTP Prefetch@544370 + expand AABB half +
// W25C GetPackSlot@5383F0 / EnsureIndex@544170 + W26C children →
// ParseChildRecord@544010 → CreateNodeUnder@536900 (type1 TREE) +
// W27C LABEL_19 text/RSD ReadLine + gametype Bind(mid+0x38,type8) + params +
// W28C Bind→LookupById@536820.
// (mid=node+0xD8, node, buf, size, scale) → float.
// CreateNodeByDesc≠1 / Remap TOC@+0x5C / Bind Resolve miss OOS.
float res_node_gametype_parse_lod_buf(void* mid, void* node, void* buf,
                                      int size, float scale);

// PE ResNode_GameType_vtbl28_LoadLod @ 0x53E860 — W21A LABEL_68 + W22A cold
// parseLodBuf wire (+ W23A body). (node, buf, scale) → float. Ctor/Rebind OOS.
float res_node_gametype_vtbl28_load_lod(void* node, void* buf, float scale);

// PE ResourceEngine_type_gametype @ 0x53A1A0 — RESTYPE_GAME=8 create.
// (parent_rh, Class*|0, name_cstr) → HostResNode*|0. Seeds mid+0x10
// Class* (or FQN cstr / class_box_object). createNativeInstance sole PE
// caller @ 0x481AD6; host also native_ptr_ensure when ResState.type==8.
void* resource_engine_type_gametype(void* parent_rh, void* clazz,
                                    const char* name);

}  // namespace inv
