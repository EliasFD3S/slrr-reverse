// W9C/W10B/W14A/W15A/W16B/W17B/W18D/W19D/W23D/W34-07/W35-07 — Chassis cam /
// slot tables + forceUpdate hdr.
// Soft PE aero apply (Chassis_physWheelTick @ 0x456a6b..0x456d2d):
//   chassis_phys_aero_soft_apply — TREE side-band only (no Δv; Resources Cd).
// Live phys+0xDC list: include/GameRef.h (gameref_phys_dc_*).
// W15A: cloneHdr Sfx sample Rebind @ 0x43E6B5/70E/791 lives in Chassis.cpp
// (g_sfxtables → hdr+0xF8/+0x340/+0x588 slots). Layout owned by Chassis.cpp.
// W16B: findPartNodeBySlotId @ 0x4707D0 stand-in + wheel RH splice @ 0x448b65
// (wheel+0x1D8 embeds); soft PrepareLod hop gate @ 0x4485DC.
// W17B: phys+0x78×0x68 side-map = Part_buildPhysSlotTable @ 0x46EAE0
// (store @ 0x46f2e9); +0x115C = SimObjectList(+0x1154).head — host part_slots.
// W18D: Part_allocPhysBlob @ 0x46E930 (size 0x1a3) malloc(284=0x11C) + ctor
// before buildPhysSlotTable — smaller than Chassis_allocCamBlob @ 0x44A250
// (0x16c2). Shared prefix with camBlob_baseCtor @ 0x45E910.
// W19D: Chassis_camBlob_baseCtor @ 0x45E910 (size 0xc7) — call @ 0x44a28a
// after malloc 0x212C.
// W23D: Chassis_camEntryCtor @ 0x45E7D0 ×4 @ 0x44a295 (blob+0x11C stride
// 0x4A8).
// W34-07: post-entry mid @ 0x44a2bb..0x44a2ff — dword0 +0x13BC/+0x13C0,
// RH_ctor_zero +0x13C4/+0x13D4, arrayCtor +0x13E8 (slots), arrayCtor
// +0x14EC ×16 stride 0x8C via Chassis_camAuxRow_zero4 @ 0x45E9E0
// (row+0x30..+0x3C).
// W35-07: Engine_CircList_ctor @ 0x428fd0 ×2 @ allocCamBlob 0x44a304/0x44a359
// (blob+0x1DDC / +0x1FA0, size 0x1C empty).
// W36-07: post-CircList RH pads + listCtor_205C..20B0 ×4 + RH +0x20D8 @
// 0x44a30f..0x44a3f4; then Part_buildPhysSlotTable @ 0x44a410 (W17B soft).
// Rest OOS: Veh_ensureSceneBound / wheel primitives / LOD / Phys_wake*.
#pragma once

#include <cstdint>

namespace inv {

struct InvObject;

// Soft-ensure host side-band (zeros embeds, map=-1; cam_count from TREE).
void chassis_cam_tables_ensure(InvObject* chassis);

// Soft PE Chassis_physWheelTick aero @ 0x456a6b..0x456d2d (not forceUpdate).
// Reads hdr+0x8FC..+0x908 / force_update_drag_*; writes TREE side-band
// (aero_force_* / aero_impulse_* / aero_phys_20f4 / aero_v_* / aero_r_w_* /
// aero_torque_* / aero_hat_*). Live Δv ONLY in
// Resources::physics_apply_chassis_aero (integrate) — never physics_set_velocity
// here. Soft: Ypr rotateLocalVec + PE half/quarter clamp (I-term OOS=0).
// Full Phys_accumForceAtLocalPoint body write / inertia tensor / phys parent
// walk (+0x58)&4 still OOS.
void chassis_phys_aero_soft_apply(InvObject* chassis, float dt);

// PE Chassis_allocCamBlob @ 0x44a788: fill +0x13E8 slots × hdr+0xA54
// via Chassis_camSlotFillFromHdr @ 0x48B940 (def @ hdr+0xA58 stride 0x1C).
// Host: PE-backed count/def float slice; camera ResourceEngine create OOS.
// W18D Part_allocPhysBlob@46E930 preamble is Chassis.cpp-internal
// (chassis_phys_blob_ensure) before phys78_rebuild.
// W19D camBlob_baseCtor@45E910 soft-ensure is Chassis.cpp-internal
// (chassis_cam_blob_base_ensure) before cam tables.
// W23D camEntryCtor@45E7D0×4 soft-ensure is Chassis.cpp-internal
// (chassis_cam_entries_ensure) after baseCtor, before cam tables.
// W34-07 cam mid/aux @44a2bb..44a2ff soft-ensure is Chassis.cpp-internal
// (chassis_cam_mid_ctor_ensure) after entries, before cam_count TREE.
// W35-07 CircList×2 @44a304/44a359 (+0x1DDC/+0x1FA0) soft-ensure is
// Chassis.cpp-internal (chassis_cam_circlist_ensure) after mid.
// W36-07 RH pads + listCtor×4 +0x20D8 soft-ensure is Chassis.cpp-internal
// (chassis_cam_listctor_ensure) after CircList, before cam_count TREE.
// Veh_ensureSceneBound / primitives / LOD still OOS.
void chassis_cam_slot_fill_from_hdr(InvObject* chassis);

int32_t chassis_cam_count(InvObject* chassis);     // PE +0x177C
int32_t chassis_cam_active(InvObject* chassis);    // PE +0x1780
void chassis_cam_set_active(InvObject* chassis, int32_t idx);

// PE +0x17A8: map[cam_idx*36 + cam_num] → slot_idx (<0 skip Rebind).
int32_t chassis_cam_slot_map_get(InvObject* chassis, int32_t cam_idx,
                                 int32_t cam_num);
void chassis_cam_slot_map_set(InvObject* chassis, int32_t cam_idx,
                              int32_t cam_num, int32_t slot_idx);

// PE +0x13F4 + slot_idx*16: embed[+0xC] instance/node for ResHandle_Rebind.
void* chassis_cam_slot_node(InvObject* chassis, int32_t slot_idx);
void chassis_cam_slot_set_node(InvObject* chassis, int32_t slot_idx,
                               void* node);

// PE Chassis_camApplyBoundSlot @ 0x449680 (this=chassis, a2=veh, a3=cam_idx).
// Callers: GameRef_voidEvent_parse activate @ 0x4591c7 / 0x4592a2, osd @
// 0x459537 (post Bind); also Chassis_camClearSlotThenApply @ 0x449eb6,
// Chassis_camCopyRebind @ 0x44a23f.
// Host soft: bones/gauge label gates + TREE; look/move/zoom queueEvent,
// Text_createRText2Inst / RenderRef_* / channel21 OOS.
// Parent wire (GameRef only — do not invent here): after osd/activate Bind,
//   chassis_cam_apply_bound_slot(chassis, cam_idx, osd_live);
void chassis_cam_apply_bound_slot(InvObject* chassis, int32_t cam_idx,
                                  int32_t osd_live);

}  // namespace inv
