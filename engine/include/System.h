#pragma once

#include <cstdint>

namespace inv {

struct InvObject;

// PE Engine_registerGameInstanceCallback @ 0x427370 mode 8 (GII_CONTROL):
// malloc(0x1C) node → CONTROL dllist tail-insert (eng+0x10/+0x18, ++eng+0xC4).
// Engine_SimulateFrame @ 0x428450 Soft PATH-TO-WORLD:
//   TickSim(accum) CONTROL walk → Phys substep (soft physWorld clock) →
//   TickTimers@0x427160 (EngTimerNode 0x30 vs *physWorld; Soft queueEvent
//   Rebind-only — DrainEvent dllist OOS) → Drain empty sentinels.
// SimulateFrame → Engine_TickSimObject @ 0x4291E0 (Rebind/PrepareLod +
// vtbl+0xC/0x14/0x2C control tick | CallNamedMethod("control") @ 0x425A90 →
// va @ 0x4255E0 THRD-RUNVMI malloc56+VMThread_init@0x41F340 / packArgs@0x425240
// tag3+79 → pushCallFrame@0x41F9F0 → Object_callMethod@0x408A30 →
// Thread_callMethod@0x4207C0 → VMThread_invokeMethod@0x41FBC0 →
// (Java ret0 → VMThread_run@0x420FF0 TREE; sync0 leave DONE for
// Jvm_PumpFrame@0x418D10 → Jvm_GcSlice@0x418C20 (host SKIP) →
// Jvm_RunThreadsBudgeted@0x416940 DONE→STOP dtor).
// CallFrame pool g_CallFramePool@0x62DDF0 / CallFramePool_alloc@0x408D90 /
// CallFrame_ctor@0x407B10. getResNameCstr@0x48ADB0 for THRD-RUNVMI name.
// EngineState+0xE4≡g_JVM@0x63641C gates PumpFrame. Residual: bytecode loop
// @0x4210D4 size 0x2979 (W14D family map in jvm.hpp vmthread_run). Optional
// script = GameType* when child+0x50 unset.
void engine_gii_control_register(int32_t gi_handle,
                                 InvObject* script = nullptr);

// PE Engine_unregisterGameInstanceCallback @ 0x4274E0 mode 8:
// walk eng[2], match node[5]==*(handle+8) → clear ResHandle slot (alive=0);
// SimulateFrame then vtbl-dtor / --count.
void engine_gii_control_unregister(int32_t gi_handle);

// W14B — PE Engine_LoadGameInit @ 0x53A8B2 GameTypeCtor hop (before THRD):
//   ctor = *(type_payload+0xC); if nonzero:
//     tmpRH linked to GI (+0x48/+0x50); eax = GameTypeCtor_create@429A00
//     (vtbl+0xC)(ctor, &tmpRH, params) — malloc(28)+link owner; params unused
//     on base GameTypeCtor_vtbl@5F09E0; store at GI mid+0x4C (not node+0x4C).
//   later @ 0x53AA26: GameTypeCtor_bindInstance vtbl+0x40 (base=nullsub).
// Host: side-map native blob (cannot overwrite HostLoadGameInitGi.type@+0x4C).
// Parent Resources wire (call when mid.leaf seeded — W14C):
//   void* nat = engine_load_game_init_run_gametype_ctor(
//       gi, /*type mid / HostMidPayload*/, params);
//   then attach_gametype (THRD-CREATE) as today.
void* engine_load_game_init_run_gametype_ctor(void* gi, void* type_payload,
                                              const char* params);
// Lookup native instance from ctor hop (nullptr if skipped / no leaf).
void* engine_load_game_init_native(void* gi);

// W12D — PE Engine_LoadGameInit GameType/THRD-CREATE slice @ 0x53A8FD.
// Stock (when type payload+0x10 Class* != 0): malloc(56)+VMThread_init
// (g_JVM, 10, 0, "THRD-CREATE") → Class_boxObject → *(GI+0x50) → pack
// AllocLocalRid InvObject → pushCallFrame → Native.ptr ResHandle(16) +
// Rebind(GI) → Object_callMethod_init @ 0x408A70 → <init>(I)V.
// Host: create script + THRD-CREATE invoke; return script (side-map by gi).
// Does NOT write InvObject* into GI+0x50 (PE 32-bit slot; host keeps
// AllocLocalRid for void_event_loadgameinit_rebind). Parent Resources wire:
//   after engine_load_game_init returns gi (rebind_key at +0x50 = RID):
//   (optional) engine_load_game_init_run_gametype_ctor(gi, type_mid, params);
//   InvObject* scr = engine_load_game_init_attach_gametype(
//       gi, /*FQN from type Class* / known*/, gi->rebind_key);
// Gaps: PE Class shell; bytecode loop VMThread_run @ 0x4210D4 size 0x2979
// (W14D: TREE covers ret/DONE only); specialized ctor vtbls beyond base;
// Resources must pass type mid + params (currently (void)params).
InvObject* engine_load_game_init_attach_gametype(void* gi,
                                                 const char* script_fqn,
                                                 int32_t local_rid);
// Lookup script attached for a LoadGameInit GI (nullptr if none).
InvObject* engine_load_game_init_script(void* gi);

// W18C/W19B/W20C/W21B/W23B/W24B/W25A/W26A — PE AsyncLoad_Submit @ 0x505960 +
// PumpOne @ 0x505DB0. type 0..5 (PE a1); async!=0 → EnqueuePathNorm@54D290
// → InFlight/Overflow; async==0 sync OOS. Type pumps + Type0_Process@506650
// (create → fail|0x80000004 / success|0x14). Type1@507110 / Type2@507840
// W26A create_dims+clamp (+Type2 FormatFromFourcc); mip upload OOS.
// Type3@501B90 INVO+fonts+.bon sync; ParseInvoChunks@502DE0 case4/5 →
// W26B vb/ib_create (W27A). Type4/5 Process /
// DrainWorkFail GetPackSlot@505EA0 OOS. FinishSlice no-op.
void* asyncload_submit(std::uintptr_t type, int32_t async, const char* path,
                       void* a4, void* a5, void* a6);

}  // namespace inv
