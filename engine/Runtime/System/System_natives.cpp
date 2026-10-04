#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"
#include "tree_interp.hpp"
#include "host_objects.hpp"
#include "jvm.hpp"
#include "Resources.h"
#include "System.h"
#include "System_internal.hpp"
#include "audio_win32.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace inv {

void engine_gii_control_register(int32_t gi_handle, InvObject* script) {
  eng_ctrl_register_gi(gi_handle, script);
}

void engine_gii_control_unregister(int32_t gi_handle) {
  eng_ctrl_unregister_gi(gi_handle);
}

// W18C — PE AsyncLoad_Submit @ 0x505960 (async path only; see System.h).
void* asyncload_submit(std::uintptr_t type, int32_t async, const char* path,
                       void* a4, void* a5, void* a6) {
  return asyncload_submit_impl(type, async, path, a4, a5, a6);
}

// Hand-written System natives (skipped by gen_native_stubs.py).

void java_lang_System_exit() {
  // PE @ 0x0047BE50 System.exit ()V size 0xb (11). Static; no UnboxArg; no
  // callees. Bytes: C7 05 20 C5 63 00 01 00 00 00 C3
  //   (mov dword ptr [Engine_quitRequested @ 0x63C520], 1; retn).
  // 1 xref: Natives_RegisterAll @ 0x00487F45. Flag only — no CRT exit /
  // TerminateProcess. Contrast exit(String) @ 0x0047BE60 size 0x15: UnboxArg
  // discard, does NOT set quit. Host: request_exit() → g_exit (MainLoop-polled
  // like Engine_quitRequested).
  request_exit();
}

void java_lang_System_exit_1(InvObject* s) {
  // PE @ 0x0047BE60 size 0x15 (21): static UnboxArg then ret — does NOT set
  // Engine_quitRequested (contrast exit()V @ 0x0047BE50). String discarded.
  // race123 no_pe→PE: body VA closed (Unbox discard only).
  // race124/125 pe_closed: sole callee JVM_UnboxArg @ 0x0045D910.
  (void)s;
}

// PE @ 0x0047BE80 System.log (Ljava.lang.String;)V size 0x47 (71).
// Static: JVM_UnboxArg dest0=nullptr. 1 xref: Natives_RegisterAll.
// After unbox, EAX = cstr (box+8):
//   EAX==0: LogStream_WriteCstr(g_ScriptLog, "<null>\n") @ 0x00612CDC
//   else:   WriteCstr(s) then WriteCstr("\n") @ 0x00612CD8
// g_ScriptLog @ 0x0062DEF8 opened as "Script log" (LogStream_ctor @ 0x0054BE80
// from JVM ctor 0x0040FB50). WriteCstr @ 0x0054BED0 size 0x26: thiscall
// lock/unlock sem at this+0x100 (INFINITE), xor eax,eax, retn 4 — cstr unused
// (stock write stub; same pattern as LogStream printf @ 0x0054BF00).
// Host stand-in: stderr. string_cstr(null) is "<null>" → same visible line.
void java_lang_System_log(InvObject* s) {
  // PE @ 0x0047BE80 size 0x47. Unbox→cstr; null→"<null>\n" else s+"\n"
  // via LogStream_WriteCstr @ 0x0054BED0 (stock write stub). Host: stderr.
  std::fprintf(stderr, "%s\n", string_cstr(s));
}

int32_t java_lang_System_buildNumber() {
  // PE @ 0x0047C0B0 System.buildNumber ()I size 0x6 (6). Static; no UnboxArg;
  // no callees. Bytes: B8 5E 02 00 00 C3 (mov eax,25Eh; retn).
  // 1 xref: Natives_RegisterAll. Always returns 0x25e (606).
  return 0x25e;
}

// PE @ 0x0047C0C0 System.info (I)Ljava.lang.String; size 0xf2 (242).
// Static; JVM_UnboxArg dest0=nullptr → int i. cmp i,5 / ja default.
// Util_Sprintf @ 0x00551220 ("%d") into 256-byte stack; thiscall
// JVM_String_from_cstr @ 0x004174A0 (ecx=JVM*). 1 xref: Natives_RegisterAll.
// PE index (list identities unknown — do not invent names):
//   0: count sentinel list [JVM+0x18] head+8 next+4 (skip first)
//   1: count [JVM+0x1C] same walk
//   2: count [JVM+0x10] same walk
//   3: dword [JVM+0x24]
//   4: dword [JVM+0x28]
//   5: JVM_liveObjectCount @ 0x0062F138 (ctor++ / dtor--; NOT buildNumber)
//   default: still String(uninit buf). Host returns "".
// Host stand-in (no stock JVM lists / liveObjectCount): load stats;
// case 5 → 0 (no mirrored counter).
InvObject* java_lang_System_info(int32_t i) {
  // PE @ 0x0047C0C0 size 0xf2. Unbox I; sprintf "%d"; JVM_String_from_cstr.
  // Cases 0..5 = JVM list counts / dwords / liveObjectCount (see header).
  // Host stand-in: load stats; case 5 → 0 (no mirrored counter).
  char buf[256];
  int32_t v = 0;
  switch (i) {
    case 0:  // PE: list [JVM+0x18]
      v = g_load_depth;
      break;
    case 1:  // PE: list [JVM+0x1C]
      v = g_load_peak;
      break;
    case 2:  // PE: list [JVM+0x10]
      v = g_load_opens;
      break;
    case 3:  // PE: dword [JVM+0x24]
      v = g_ld_priority;
      break;
    case 4:  // PE: dword [JVM+0x28]
      v = g_config_apply_count;
      break;
    case 5:  // PE: JVM_liveObjectCount @ 0x62F138 — not 0x25e
      v = 0;
      break;
    default:
      return string_new("");
  }
  std::snprintf(buf, sizeof(buf), "%d", v);
  return string_new(buf);
}

int32_t java_lang_System_gc() {
  // PE @ 0x0047C060 System.gc ()I size 0xb (11). Static; no UnboxArg.
  // eax=[esp+4] CallInfo*; ecx=[eax] (*CallInfo); jmp JVM_GC @ 0x417F80
  // (thiscall, size 0x140). 1 xref: Natives_RegisterAll.
  // JVM_GC: if *(JVM+0x20) work buf set → drain/collect (ret count or -1 busy);
  // else alloc/init GC state, JVM_gcActiveJvm @ 0x62E004=JVM*, ret -1.
  // Host: no JVM GC → 0.
  return 0;
}

int32_t java_lang_System_runFinalization() {
  // PE @ 0x0047C070 System.runFinalization ()I size 0x3 (3).
  // Static; no UnboxArg; no callees. Bytes: 33 C0 C3 (xor eax,eax; retn).
  // 1 xref: Natives_RegisterAll. Always returns 0 (no finalizer queue).
  return 0;
}
// PE @ 0x0047C080 System.compileAll (Ljava.lang.String;)I size 0x23 (35).
// Static; JVM_UnboxArg dest0=nullptr → String* path (cstr box+8).
// thiscall JVM_compileAllDir @ 0x00418EB0 (ecx=[CallInfo*]=JVM*,
// stdcall path). Callee: ensure trailing '\\', FindFirst("*"), recurse
// dirs (skip "."/".."), for each "*.java" → JVM_compileSource (dir,name)
// and ++count; FindClose; ret count. Contrast openLib @ 0x00487BE0
// (ResourceEngine_LoadPack, no JVM walk). 1 xref: Natives_RegisterAll.
// Host: jvm_active()->compile_all ≈ JVM_compileAllDir; loading_* stand-in
// for async load ring (PE compileAll does not touch isLoading flag).
int32_t java_lang_System_compileAll(InvObject* path) {
  // PE @ 0x0047C080 size 0x23. Unbox path → JVM_compileAllDir @ 0x00418EB0.
  Jvm* j = jvm_active();
  if (!j) return 0;
  const char* rel = string_cstr(path);
  loading_enter();
  const int32_t n = j->compile_all(rel);
  loading_leave();
  return n;
}

// Registry (not ticket dump): three distinct impl VAs — do NOT merge.
void java_lang_System_analyze(InvObject* o) {
  // PE @ 0x0047BED0 System.analyze (Ljava.lang.Object;)V size 0x1f (31).
  // Static UnboxArg dest0=0, dest1=&o. Then test o; nop if non-null; ret.
  // Sole callee JVM_UnboxArg @ 0x0045D910. No other side effects. Host: discard.
  (void)o;
}

void java_lang_System_analyze_1(float f) {
  // PE @ 0x0047BEF0 System.analyze (F)V size 0x15 (21). Static UnboxArg
  // dest0=0, dest1=&f; ret. Pure discard (no analyze work).
  // Sole callee JVM_UnboxArg @ 0x0045D910.
  (void)f;
}

void java_lang_System_analyze_2(int32_t i) {
  // PE @ 0x0047BF10 System.analyze (I)V size 0x15 (21). Static UnboxArg
  // dest0=0, dest1=&i; ret. Pure discard (sibling of float @ 0x0047BEF0).
  // Sole callee JVM_UnboxArg @ 0x0045D910.
  (void)i;
}

// PE @ 0x00487660 System.arraycopy (Ljava.lang.Object;ILjava.lang.Object;II)V
// size 0x1f8 (504). Static; JVM_UnboxArg dest0=nullptr → src, srcPos, dst,
// dstPos, length. Bounds: JVM_vm_get_int_field_by_name(arr,"length") @
// 0x0042A430; if srcPos+len > src.length OR dstPos+len > dst.length →
// "!"+"System.arraycopy: copy out of bounds" + Engine_ErrorLogMsgBox.
// elementData via Instance_getFieldAt @ 0x403820 (slot dword_62DFFC);
// null either →
// "!"+"System.arraycopy: empty array" + ErrorLogMsgBox.
// Same array object: malloc 4*len temp, copy *(elem+8) payloads, write,
// free (sub_54F560/54F5B0). Distinct arrays: direct *(src+8)→*(dst+8)
// loop (no temp). Host: tree_vector Object[] stand-in; always temp so
// overlapping regions match same-array PE path; no ErrorLogMsgBox.
void java_lang_System_arraycopy(InvObject* src, int32_t src_position,
                                InvObject* dst, int32_t dst_position,
                                int32_t length) {
  // PE @ 0x00487660 size 0x204. Bounds+empty → ErrorLogMsgBox; same-array
  // temp via Engine_malloc/free; else direct *(elem+8) copy. Host: TREE
  // vector always-temp (overlap-safe); no ErrorLogMsgBox.
  if (!src || !dst || length <= 0) return;
  if (src_position < 0 || dst_position < 0) return;
  const int32_t src_n = tree_vector_size(src);
  if (src_position > src_n || length > src_n - src_position) return;
  const int32_t need = dst_position + length;
  if (tree_vector_size(dst) < need) tree_vector_resize(dst, need);
  std::vector<InvObject*> tmp(static_cast<size_t>(length));
  for (int32_t i = 0; i < length; ++i)
    tmp[static_cast<size_t>(i)] =
        tree_vector_element_at(src, src_position + i);
  for (int32_t i = 0; i < length; ++i)
    tree_vector_set(dst, dst_position + i, tmp[static_cast<size_t>(i)]);
}

float java_lang_System_currentTime() {
  // PE @ 0x0047BFE0 System_currentTime ()F size 0xc (12). Static; no UnboxArg;
  // 1 callee. Asm: call System_currentTimeMs @ 0x005516C0; fmul flt_5F10A4 @
  // 0x005F10A4 (0.001f); retn. Callee: QPF→Frequency @ 0x76A280, QPC→now @
  // 0x76A290; (now−PerformanceCount @ 0x76A288)/Frequency*flt_5F0910 @
  // 0x005F0910 (1000.0) → ms; native ×0.001f → wall seconds. Epoch from
  // sub_551690 @ 0x00551690 (boot sub_551520 @ 0x00551520, start @ 0x005514C0).
  // 1 xref: Natives_RegisterAll @ 0x0048805C. Contrast simTime @ 0x0047BFD0.
  // Host: time_current() ≡ ms×0.001 via QPC epoch in time_init().
  return time_current();
}

float java_lang_System_simTime() {
  // PE @ 0x0047BFD0 System.simTime ()F size 0x8 (8). Static; no UnboxArg;
  // no callees. Asm: mov eax, Engine_simTime (ptr @ 0x617650); fld dword [eax];
  // retn — pure load of *Engine_simTime. IDB ptr value 6556100 → float @
  // 0x6409C4 (int_convert). 1 data xref: Natives_RegisterAll @ 0x0048807B.
  // Contrast currentTime @ 0x0047BFE0 (wall QPC). Does NOT write/advance the
  // clock (MainLoop zeros *[Engine_simTime] @ 0x00428A6A; tick path writes
  // elsewhere). Does NOT touch day clock (dword_640988) or night flag
  // (dword_6178D0) — syncGameTime only. Host: time_sim() → g_sim (host
  // *Engine_simTime).
  return time_sim();
}

// PE @ 0x0047BF80 System.timeWarp (F)F size 0x45 (69). Static;
// JVM_UnboxArg dest0=nullptr, dest1=&m. 2 callees: JVM_UnboxArg @ 0x0045D910,
// Engine_setTimeWarp @ 0x004283F0 (thiscall ecx=g_EngineState @ 0x636338).
// 1 xref: Natives_RegisterAll @ 0x0048809A (push impl / "(F)F" / "timeWarp").
// Asm: fld m; fcomp flt_5E73CC @ 0x005E73CC (0.0f); mov edx, Engine_timeWarp
// (dword @ 0x636448 = g_EngineState+0x110, the scale itself — not a ptr);
// test ah,1 (C0=m<0) → skip; else push m; call Engine_setTimeWarp; fld prev.
// Engine_setTimeWarp size 0x56: if m>=0 write this+0x110; wall =
// System_currentTimeMs @ 0x005516C0 * flt_5F09CC @ 0x005F09CC (0.001f);
// test ah,41h (C3|C0: m==0 — outer already required m>=0) skip fdiv;
// else this+0x108 = wall - (*Engine_simTime)/m (continuity). m==0: wall only.
// Does NOT write *Engine_simTime (ptr @ 0x617650 → float @ 0x6409C4) nor
// dword_640988 / dword_6178D0. Engine_InitState @ 0x0042798F seeds +0x110 =
// 0x3F800000 (1.0f). Java: timeWarp(-1)=query (Track pause/speed); 0=pause.
// Host: time_warp — ret prev g_warp; apply iff m>=0 (init 1.f like InitState).
float java_lang_System_timeWarp(float m) {
  // PE @ 0x0047BF80 size 0x45. Unbox F; ret prior Engine_timeWarp @ 0x636448;
  // if m>=0 → Engine_setTimeWarp @ 0x004283F0 (ecx=g_EngineState).
  return time_warp(m);
}

// PE @ 0x0047BFF0 System.syncGameTime (F)V size 0x61 (97). Static;
// JVM_UnboxArg dest0=nullptr; fld t; call Engine_ftol @ 0x005D6750;
// cdq; idiv 0x15180 (86400); mov Engine_secOfDay @ 0x640988, edx;
// Engine_isNight @ 0x6178D0 := 1 iff edx>=0x11940 (72000=20h) || edx<0xE10
// (3600=1h) || (edx>=0x4650 (18000=5h) && edx<0x6270 (25200=7h)); else 0.
// Ret eax=days discarded (sig V). 1 xref: Natives_RegisterAll.
// Contrast: simTime = read *Engine_simTime; timeWarp = scale Engine_timeWarp;
// syncGameTime = calendar TOD + night (GameLogic.setTime/spendTime).
void java_lang_System_syncGameTime(float t) {
  // PE @ 0x0047BFF0 size 0x61. Engine_ftol % 86400 → Engine_secOfDay;
  // Engine_isNight thresholds as above.
  time_sync_game(t);
  const int32_t tod = time_game_day_seconds();
  g_engine_is_night =
      (tod >= 72000 || tod < 3600 || (tod >= 18000 && tod < 25200)) ? 1 : 0;
}

void java_lang_System_setMeasure(float m) {
  // PE @ 0x0047C4B0 size 0x48. Static UnboxArg (F); default stack 1000.0.
  // Stores: System_measure=m; measure_div3600=m/3600; ten_div_measure=10/m.
  // Java: 1000=km, 1600=mile.
  g_measure = m;
  g_measure_div3600 = m / 3600.f;
  g_ten_div_measure = (m != 0.f) ? (10.f / m) : 0.f;
}

InvObject* system_config_host() {
  if (!g_config_host) {
    g_config_host = tree_host_new("java.util.Config");
    // Stock Config defaults used by OptionsDialog.show (read path).
    tree_field_set_int(g_config_host, "flares", 1);
    tree_field_set_int(g_config_host, "headlight_rays", 1);
    tree_field_set_int(g_config_host, "video_windowed", 0);
    tree_field_set_int(g_config_host, "video_x", 800);
    tree_field_set_int(g_config_host, "video_y", 600);
    tree_field_set_int(g_config_host, "video_depth", 32);
    tree_field_set_float(g_config_host, "video_gamma", 1.f);
    tree_field_set_int(g_config_host, "texture_size", 2);
    tree_field_set_int(g_config_host, "texture_format", 3);
    tree_field_set_float(g_config_host, "texture_save_quality", 1.f);
    tree_field_set_int(g_config_host, "shadow_size", 256);
    tree_field_set_float(g_config_host, "shadow_detail", 0.25f);
    tree_field_set_int(g_config_host, "shadows", 2);
    tree_field_set_float(g_config_host, "object_detail", 0.0345f);
    tree_field_set_float(g_config_host, "object_detail_amp", 13.f);
    tree_field_set_float(g_config_host, "particle_density", 0.6f);
    tree_field_set_float(g_config_host, "camera_ext_viewrange", 200.f);
    tree_field_set_float(g_config_host, "camera_int_viewrange", 200.f);
    tree_field_set_float(g_config_host, "trafficDensity", 1.f);
    tree_field_set_float(g_config_host, "pedestrianDensity", 0.f);
    tree_field_set_float(g_config_host, "mouseSensitivity", 1.f);
    tree_field_set_int(g_config_host, "SysCursor", 1);  // Config.java default
    tree_field_set_float(g_config_host, "FFB_strength", 1.f);
    tree_field_set_float(g_config_host, "FFB_strength_emulated", 0.05f);
    tree_field_set_int(g_config_host, "metricSystem", 1);
    tree_field_set_int(g_config_host, "gpsMode", 0);
    tree_field_set_int(g_config_host, "player_transmission", 1);
    tree_field_set_float(g_config_host, "player_steeringhelp", 0.666f);
    tree_field_set_float(g_config_host, "player_abs", 0.f);
    tree_field_set_float(g_config_host, "player_asr", 0.f);
    tree_field_set_float(g_config_host, "deformation", 0.6f);
    tree_field_set_float(g_config_host, "external_damage", 0.25f);
    tree_field_set_float(g_config_host, "internal_damage", 1.f);
    tree_field_set_int(g_config_host, "skidmark_max", 4096);
    tree_field_set_int(g_config_host, "resource_loadrate", 128);
    tree_field_set_int(g_config_host, "ForceFeedBack", 1);
    tree_field_set_int(g_config_host, "mem_vertex_max", 20 * 1024 * 1024);
    tree_field_set_int(g_config_host, "mem_vertex_min", 16 * 1024 * 1024);
    tree_field_set_int(g_config_host, "mem_texture_max", 48 * 1024 * 1024);
    tree_field_set_int(g_config_host, "mem_texture_min", 40 * 1024 * 1024);
    tree_field_set_int(g_config_host, "mem_instance_max", 4500);
    tree_field_set_int(g_config_host, "mem_instance_min", 4000);
    tree_field_set_int(g_config_host, "mem_sound_max", 16 * 1024 * 1024);
    tree_field_set_int(g_config_host, "mem_sound_min", 12 * 1024 * 1024);
    tree_field_set_int(g_config_host, "MouseHelp", 0);
    tree_field_set_float(g_config_host, "engine_inertia_factor", 6.f);
    tree_field_set_float(g_config_host, "wheel_gndfeedback_factor", 7.f);
    tree_field_set_float(g_config_host, "wheel_brake_factor", 6.f);
    tree_field_set_float(g_config_host, "steerhelp_turn", 1.f);
    tree_field_set_float(g_config_host, "head_move_steer", 1.f);
    tree_field_set_float(g_config_host, "head_move_vel", 1.f);
    tree_field_set_float(g_config_host, "head_move_acc", 1.f);
    tree_field_set_int(g_config_host, "Sound_Mix_HW", 2);
    tree_field_set_int(g_config_host, "Sound_3D_HW", 2);
    tree_field_set_obj(g_config_host, "version", string_new("v2.1.9r5"));
  }
  return g_config_host;
}

// PE Engine_ApplyConfigOptions @ 0x00427BF0 (size 0x680). Reads Config static
// fields via GameInit config slot (+0x50) / Config_GetInt / Config_GetFloat /
// Config_GetOrDefault; writes engine globals; tail GfxEngine_ApplyShadowSize
// @ 0x004FC590 (shadow_size @ dword_6187B8). Host mirror: system_config_host.
static void engine_apply_config_options(InvObject* cfg) {
  g_engine_headlight_rays = tree_field_get_int(cfg, "headlight_rays");
  g_engine_flares = tree_field_get_int(cfg, "flares");
  g_engine_shadow_size = tree_field_get_int(cfg, "shadow_size");
  g_engine_shadows = tree_field_get_int(cfg, "shadows");
  g_engine_shadow_detail = tree_field_get_float(cfg, "shadow_detail");
  g_engine_texture_size = tree_field_get_int(cfg, "texture_size");
  const float object_detail = tree_field_get_float(cfg, "object_detail");
  g_engine_object_detail = object_detail;
  g_engine_object_detail_dup = object_detail;  // PE stores object_detail twice
  g_engine_object_detail_amp = tree_field_get_float(cfg, "object_detail_amp");
  g_engine_texture_format = tree_field_get_int(cfg, "texture_format");
  const float gamma = tree_field_get_float(cfg, "video_gamma");
  g_engine_video_gamma_inv =
      (gamma > 0.f) ? (1.f / gamma) : g_engine_video_gamma_inv;
  g_engine_particle_density = tree_field_get_float(cfg, "particle_density");
  g_engine_skidmark_max = tree_field_get_int(cfg, "skidmark_max");
  g_engine_texture_save_q = static_cast<int32_t>(
      tree_field_get_float(cfg, "texture_save_quality") * 100.f);
  g_engine_external_damage = tree_field_get_float(cfg, "external_damage");
  g_engine_internal_damage = tree_field_get_float(cfg, "internal_damage");
  g_engine_deformation = tree_field_get_float(cfg, "deformation");
  g_engine_mem_vertex_max = tree_field_get_int(cfg, "mem_vertex_max");
  g_engine_mem_vertex_min = tree_field_get_int(cfg, "mem_vertex_min");
  g_engine_mem_texture_max = tree_field_get_int(cfg, "mem_texture_max");
  g_engine_mem_texture_min = tree_field_get_int(cfg, "mem_texture_min");
  g_engine_mem_instance_max = tree_field_get_int(cfg, "mem_instance_max");
  g_engine_mem_instance_min = tree_field_get_int(cfg, "mem_instance_min");
  // Config_GetOrDefault(mem_sound_max): stock Config.java defines the field.
  g_engine_mem_sound_max = tree_field_get_int(cfg, "mem_sound_max");
  g_engine_mem_sound_min = tree_field_get_int(cfg, "mem_sound_min");
  g_engine_resource_loadrate = tree_field_get_int(cfg, "resource_loadrate");
  g_engine_force_feedback = tree_field_get_int(cfg, "ForceFeedBack");
  g_engine_ffb_strength =
      tree_field_get_float(cfg, "FFB_strength") * 10000.f;
  g_engine_ffb_strength_emu =
      tree_field_get_float(cfg, "FFB_strength_emulated") * 0.000001f;
  g_engine_engine_inertia = tree_field_get_float(cfg, "engine_inertia_factor");
  g_engine_wheel_gnd_feedback =
      tree_field_get_float(cfg, "wheel_gndfeedback_factor");
  g_engine_wheel_brake_factor = tree_field_get_float(cfg, "wheel_brake_factor");
  g_engine_mouse_help = tree_field_get_int(cfg, "MouseHelp");
  g_engine_steerhelp_turn = tree_field_get_float(cfg, "steerhelp_turn");
  g_engine_head_move_steer = tree_field_get_float(cfg, "head_move_steer");
  g_engine_head_move_vel = tree_field_get_float(cfg, "head_move_vel");
  g_engine_head_move_acc = tree_field_get_float(cfg, "head_move_acc");
  render_d3d9_set_flares_enabled(g_engine_flares != 0);
  // GfxEngine_ApplyShadowSize @ 0x004FC590: recreate shadow maps from
  // g_engine_shadow_size — no host GfxEngine shadow allocator yet.
  (void)g_engine_shadow_size;
}

// PE @ 0x0047C500 System.getConfigOptions ()V size 0xa (10). Static; no
// UnboxArg. Asm: mov ecx, g_EngineState; jmp Engine_ApplyConfigOptions @
// 0x00427BF0. 1 xref: Natives_RegisterAll. Host: engine_apply_config_options
// on system_config_host() mirror + render flares gate (dword_6187C0).
void java_lang_System_getConfigOptions() {  // PE @ 0x0047C500
  InvObject* cfg = system_config_host();
  engine_apply_config_options(cfg);
  ++g_config_apply_count;
  tree_field_set_int(cfg, "apply_count", g_config_apply_count);
  tree_field_set_int(cfg, "apply_skipped", 0);
  tree_field_set_int(cfg, "applied_video_x",
                     tree_field_get_int(cfg, "video_x"));
  tree_field_set_int(cfg, "applied_video_y",
                     tree_field_get_int(cfg, "video_y"));
}

// OOS Soft-only — java.lang.System.netHost / netJoin / netLeave:
// Declared in System.java + host natives_stubs, ABSENT from stock
// Natives_RegisterAll / native_registry.json. IDA find/string = 0 hits.
// Excluded from pe/table (SOFT_ONLY_JNI). Stubs keep mod callers safe.
int32_t java_lang_System_netHost() { return 0; }
int32_t java_lang_System_netJoin() { return 0; }
int32_t java_lang_System_netLeave() { return 0; }

// PE @ 0x00487BE0 System.openLib (Ljava.lang.String;)I size 0xc5 (197).
// Static; JVM_UnboxArg dest0=nullptr → cstr (box+8). 1 xref: Natives_RegisterAll.
// Flow (disasm — Hex-Rays mis-types LoadPack arg as CallInfo*):
//   packIdx = ResourceEngine_LoadPack(g_ResourceEngine @ 0x618D48, path) @ 0x538380
//   (find slot stride 0x70 @ +0xFFE94 by name sub_543FE0, else init sub_543EB0).
//   resId = packIdx<<16; slot = sub_5383F0(thiscall GetPackSlot).
//   if slot && *(slot+0x50) >= 2 (sub_544000 loadState) → ret packIdx.
//   else stack binder + sub_545FC0(binder, (pack<<16)|1) force-resolve localId=1.
//   binder.owner==0 → ret -1; else unlink intrusive list → ret packIdx.
// loadState +0x50: 0=none, 1=opening, 2=loaded (sub_544170).
// Contrast compileAll @ 0x0047C080: JVM path only, no LoadPack.
// Host: rpak_open ≈ LoadPack+sub_544170; early-out if same resolved path
// already open (parsed_entries || is_registry); registry skips local:1 probe;
// indexed packs require rpak_find_entry((pack<<16)|1) like PE resolve probe.
int32_t java_lang_System_openLib(InvObject* libName) {  // PE @ 0x00487BE0
  loading_enter();
  ++g_load_opens;

  const char* rel = string_cstr(libName);
  const std::string resolved = rpak_resolve_path(rel);
  if (resolved.empty()) {
    loading_leave();
    return -1;
  }

  // PE early-out: GetPackSlot(pack<<16) && loadState>=2 (already loaded).
  const std::string base =
      resolved.substr(resolved.find_last_of('/') + 1);
  if (const RpakPack* open = rpak_find_by_name(base.c_str())) {
    if (open->path == resolved &&
        (open->parsed_entries || open->is_registry)) {
      loading_leave();
      return open->pack_id;
    }
  }

  const int32_t pack_id = rpak_open(rel);
  if (pack_id == 0) {
    loading_leave();
    return -1;
  }

  const RpakPack* pack = rpak_get(pack_id);
  if (!pack) {
    loading_leave();
    return -1;
  }

  // PE sub_545FC0 resolve probe on (pack<<16)|1; registry has no file index.
  if (!pack->is_registry &&
      !rpak_find_entry(rpak_make_id(pack_id, 1))) {
    loading_leave();
    return -1;
  }

  loading_leave();
  return pack_id;
}

// PE @ 0x0047C3A0 System.isLoadingReset ()V size 0xb (11). Static; no
// UnboxArg. Asm: mov ecx, g_ResourceEngine (@ 0x618D48); jmp 0x5378B0.
// Tail @ 0x5378B0: eax=dword_618D6C; rep stosd 0x20 dwords at dword_765E8C
// with eax; dword_765F24 = eax<<5 (32*dword_618D6C). Seeds ResourceEngine
// load-ring for PumpLoadQueue — does NOT clear [RE+0x106ED4].
// 1 xref: Natives_RegisterAll. Contrast isLoading (pure dword load of flag).
// Host stand-in: zero depth/peak (LoadingScreen.track arms a fresh watch).
void java_lang_System_isLoadingReset() {
  // PE @ 0x0047C3A0 size 0xb. jmp ResourceEngine ring seed @ 0x5378B0
  // (memset32 dword_765E8C ×0x20; dword_765F24=32*dword_618D6C). Host:
  // zero depth/peak (LoadingScreen.track arms a fresh watch).
  g_load_depth = 0;
  g_load_peak = 0;
}

// PE @ 0x0047C3B0 System.isLoading ()I size 0xc (12). Static; no UnboxArg;
// no callees. Bytes: A1 48 8D 61 00  8B 80 D4 6E 10 00  C3
//   mov eax, g_ResourceEngine; mov eax, [eax+0x106ED4]; retn
// Returns dword flag @ ResourceEngine+0x106ED4 (0 or 1). Sole writers:
// ResourceEngine_PumpLoadQueue @ 0x005378D0 — after ring update, sets flag
// iff (unsigned)dword_765F24 * 0.03125 (flt_5F3564) > 4.0 (flt_5F0CC8);
// else 0. 1 xref: Natives_RegisterAll.
// Contrast isLoadingReset (ring seed only) / setLdPriority (time-scale
// globals, not this flag). Host: no RE object — depth refcount from
// openLib/compileAll; booleanize like PE 0/1 (LoadingScreen: !isLoading).
int32_t java_lang_System_isLoading() {  // PE @ 0x0047C3B0
  return g_load_depth > 0 ? 1 : 0;
}

// PE @ 0x0047BF30 System.setLdPriority (I)V size 0x46 (70). Static;
// JVM_UnboxArg dest0=nullptr → int pri. Does NOT store the int into JVM.
//   pri==0 (LD_NORM): Engine_ldWorkScale@0x60C8C4=0.1 (0x3DCCCCCD),
//                     System_ldHigh@0x640920=0
//   else (LD_HIGH):   Engine_ldWorkScale=-1.0 (0xBF800000), System_ldHigh=1
// Engine_MainLoop @ 0x00428960 size 0x651 (1617) — mainloop_1to1 PARTIAL.
// Pre: ResHandle_getPayload Config; LogStream_WriteCstr @ 0x54BED0 on miss;
//   ResHandle_Bind(init, 8, 0) @ 0x546070; Engine_LoadGameInit("GameInit")
//   @ 0x53A5E0; ResHandle_Link @ 0x4290F0; Engine_GetTimeMs @ 0x5516C0.
// while !Engine_quitRequested @ 0x63C520:
//   Input_tick(Engine_frameDt) @ 0x54DDA0 → Engine_SimulateFrame @ 0x428450
//   → Jvm_PumpFrame(10.0) @ 0x418D10 if EngineState+0xE4
//   → Engine_DispatchAnimateEvents(dt) @ 0x426FD0 → Engine_DrainEventQueues
//   @ 0x4277F0 → Engine_CameraMatrix_GetColumn (1|2) @ 0x54E270 on
//   Engine_CameraMatrix @ 0x636498 → Sfx_ListenerSetPose @ 0x5508F0 →
//   Sfx_UpdateVoices @ 0x550980 → ResourceEngine_Pump* +
//   AltPresentGate @ 0x4F9760 ? AltPresent @ 0x4F9550 : PresentFrame @ 0x4FCA30;
//   Class_findByName("java.render.Frontend") @ 0x411600 +
//   Class_getFieldByName("render") @ 0x404820 → Object_MonitorNotifyAll;
//   AsyncLoad_* gated by Engine_ldWorkScale (scale<=0 → unlimited +
//   Engine_SleepMs(10.0) @ 0x551730 → Win32_Sleep);
//   Engine_PollWindowQuit @ 0x5522A0; Engine_MainLoop_EndFrame @ 0x554DC0.
// Soft frame order hosted in system_mainloop_sfx_listener_update (Input→
//   Simulate→Pump→Animate/Drain empty→Sfx→RE→AltPresent→Async→Poll→End).
// Host advance race120: Sfx_ListenerSetPose xyz + Sfx_UpdateVoices stub.
// Host advance race121: Engine_PollWindowQuit stand-in —
//   render_d3d9_pump(0) + request_exit if WM_QUIT (same PeekMessageA
//   filter msg==18 as PE @ 0x5522A0). Frontend.notify already on Present
//   flush (render_d3d9.cpp → frontend_gfx_engine_frame_notify).
// race122: next after PollWindowQuit is Engine_MainLoop_EndFrame @
//   0x00554DC0 — walks FileAsync_Ring (+71) when *slot==4 →
//   FileAsync_Malloc then Engine_ReleaseSemaphore(FileAsync_WakeSem).
// race123: Engine_SleepMs(10.0) @ 0x00551730 when Engine_ldWorkScale<=0
//   (LD_HIGH=-1.0) hostable — Win32 Sleep; AsyncLoad_* still OOS.
// race124: EndFrame FileAsync_Malloc @ 0x0054F6E0 was misnamed
//   CompletePending/CloseHandle — IDA: CRT heap alloc (size>0).
// race126: Present via render_d3d9_flush. SimulateFrame @ 0x00428450 OOS.
// race127: EndFrame FileAsync host stand-in — ring layout PE-clear
//   (64×71 dwords @ 0x76AA30..0x76F130); drain state4→Malloc→state1 +
//   WakeSem no-op. No worker/enqueue/disk IO. SimulateFrame still OOS.
// W16C: FileAsync_EnqueuePath@5553B0 / EnqueueRead@555470 hosted (ring
//   fill + Active + WakeSem no-op).
// W17C: WorkerThread@554EA0 map+IDA rename; host pump (prio/RR) after
//   EndFrame. W20C: FilePool disk hop (CreateFile/Seek/Read/Close).
//   W21B: PackFile Open/Read oneshot before FilePool @ 0x555096
//   (Lookup@5542F0; Base unset → miss).
// W35-01 Soft PE: HandleCache 8×66 @ 0x76F138 (carte W34-20) keep-open +
//   LRU/Stamp + FilePool[64]@777558 pool-full -2 requeue Ring state=1;
//   sync ReadEntire still oneshot Close. FlushHandlesForPath@555550 /
//   LockSem/WakeSem real / Worker shutdown cache drain OOS.
// W18C: AsyncLoad queues Ready/Overflow/InFlight + Submit@505960 async
//   stub (EnqueuePathNorm→OnFileCb); HasWork drains Overflow + sets
//   PumpCursor. W19B: PumpOne@505DB0 stub. W23B: type0..5 pump shells
//   (012@507E50 / 3@501A70 / 4@503A00 / 5@53BAB0) + Fail/Work lists.
// W24B: Type0_Process@506650 create+fail/success flag paths; W31A wires
//   vt+0xF0 CreateTextureFromMem@4F8A40 (a5=0, fmt=[21]) via
//   render_d3d9_texture_create_from_mem; GetLevel0WH/SumMipBppWeight soft.
//   Type3_Process@501B90 W25A (INVO magic+fonts+.bon sync
//   FilePool_ReadEntire@54C300; ParseInvoVle3@502070 / ParseInvoChunks@502DE0);
  //   W27A ParseInvoChunks case4/5 → W26B mesh_vb/ib_create; W28A case4
  //   PackVertex@4FF710 walk + AABB/radius; W30A case1-3 soft
  //   MatObj@505100/AddTex@4FE0F0/SetMode@4FE130/MatVec@505120;
  //   W32A case0 soft CreateVB wrap; W33A CreateMatFromStages@4BEFC0 type0
  //   + GetUvTexInfo@4C68E0 slots; W34-01 UV xform@503788 + soft
  //   PostLoop@4FFFE0 usage (D3D create / CreateMat non-type0 OOS); W26A
//   Type1@507110 / Type2@507840 W29A create+W28B Begin/Poll/Finish+
//   fail/success (mip upload loops OOS); Type4/5 Process + DrainWorkFail
//   GetPackSlot@505EA0 OOS.
// race128: SimulateFrame Physics_Step @ 0x4A5190 via physics_integrate(step)
//   inside PE substep loop (MarkStepReset→Step→TickTimers→Drain). Soft
//   maxStep=0.05 (*(physWorld+8) init OOS); solver body lists OOS.
// race129/W8B: SimulateFrame TickSimObject — GII_CONTROL dllist +
//   Rebind/PrepareLod @ 0x4291E0 + vtbl+0xC/0x14/0x2C control tick +
//   CallNamedMethod("control") @ 0x425A90 → jvm invoke control(F)V.
//   TickSim now gated on simDtAccum>1e-4 and receives ACCUM (PE @0x4284EE).
// W9D: CallNamedMethod_va RH gate (+0x44 PrepareLod/vtbl+0xC) + packArgs
//   tag3/sentinel79 VA docs; Object_callMethod→Thread_callMethod slice.
// W10D: VMThread 56B blob (vmthread_init) + pack_arg_float + pushCallFrame
//   operand mirror → jvm invoke control(F)V + requestStop.
// W11C: g_CallFramePool / CallFrame_ctor 64B + FrameList 28B; pushCallFrame
//   → curr_frame+0x28; invokeMethod slice (Native.ptr arg unbox ACC_NATIVE
//   → jvm invoke).
// W12C: invokeMethod Java path queues pending (ret 0); CallNamedMethod_va
//   → VMThread_run(0) TREE stand-in; sync==0 leaves DONE'd green-thread;
//   MainLoop jvm_pump_frame(10) ≡ Jvm_PumpFrame→RunThreadsBudgeted drain.
// W13C: sleep +0x30 deadline; DONE(0x40)→STOP dtor; prio-scale slice;
//   EngineState+0xE4≡g_JVM gate; getResNameCstr@0x48ADB0 name.
//   Residual gap: bytecode opcode loop VMThread_run @ 0x4210D4..0x4238ED
//   (size 0x2979 / 10617B) — cooperative yield &0x78 / budget mid-method.
//   TickTimers Soft PE empty EngState+0x94 dllist + pollTimers stand-in PER
//   phys substep (PE @0x4285B7); fallback Drain when stepped<=0 (@0x4285EF);
//   Drain Soft PE empty +0x5C/+0x78/+0xB0 do-while (producers OOS); cam≡0.
// W14E: GII_CONTROL Rebind@register + Unlink@unregister + dtor@0x45F8B0
//   walk fidelity; TickSim via a2=node+12; no bytecode VM.
// W14F Soft deepen SimulateFrame/TickSim (System.cpp only, no VM invent):
//   frameDt × time_warp(-1) ≡ *[EngineState+0x110]; PrepareLod gate =
//   test eax,80000000h; Physics_Step fail step<1e-4; simFrameCounter wrap;
//   !child → PE epilogue (host CallNamed only if owner vtbl missing).
// W15B: Jvm_PumpFrame @ 0x418D10 = ++Jvm+0x40 → Jvm_GcSlice @ 0x418C20
//   (size 0xE8) → EMA +0x24 → Jvm_RunThreadsBudgeted. GcSlice map:
//   +0x40 gen (lo-byte==0 → recalc); +0x3C budget clamp [4,64];
//   +0x44 finalizeQ; +0x30 grey list; +0x2C splice target.
//   Drain ≤+0x3C via Object_FinalizeFree @ 0x408560 (finalize→
//   VMThread_run → bytecode residual). Host: gen bump + RunThreads only;
//   GcSlice body SKIP (no PE dllists; FinalizeFree pulls opcode loop).
// Open: Type4/5 Process; Type1/2 mip upload (BeginMipDecode/StepMipUpload /
//   BeginAsyncMipUpload); Type3 ParseInvoVle3 pack/AABB + .bon table +
//   async BonOnFileCb (W27A chunks case4/5 VB/IB; W28A PackVertex+AABB;
//   W32A/W33A/W34-01 case0 CreateVB+type0 CreateMat/GetUvTexInfo+UV
//   xform@503788+PostLoop usage; PostLoop D3D / CreateMat non-type0 OOS);
//   Type0
//   SumMipBppWeight exact FOURCC table;
//   DrainWorkFail GetPackSlot; File_FlushHandlesForPath@555550;
//   FilePool_LockSem; Worker shutdown HandleCache drain; PackFile_SetRoot
//   call sites; Engine_DrainEventQueues Soft PE empty sentinels (dispatch
//   body OOS); PumpFrame GC (Jvm_GcSlice SKIP).
void java_lang_System_setLdPriority(int32_t pri) {
  // PE @ 0x0047BF30 size 0x46. Unbox I discarded as store; pri==0 →
  // Engine_ldWorkScale=0.1 + System_ldHigh=0; else scale=-1.0 + ldHigh=1.
  // race123 no_pe→PE: VA closed inside body.
  if (pri == 0) {
    g_ld_work_scale = 0.1f;
    g_ld_high = 0;
  } else {
    g_ld_work_scale = -1.0f;
    g_ld_high = 1;
  }
  g_ld_priority = pri;  // host test mirror (PE does not keep the int)
}

// PE Engine_MainLoop @ 0x00428960 callees (frame-order Soft PATH-TO-WORLD):
//   Input_tick@54DDA0 → SimulateFrame@428450 → Jvm_PumpFrame@418D10 →
//   DispatchAnimateEvents@426FD0 → DrainEventQueues@4277F0 →
//   CameraMatrix_GetColumn@54E270 → Sfx_ListenerSetPose@5508F0 →
//   Sfx_UpdateVoices@550980 → RE_PumpLoad@5378D0 → RE_PumpUnload@537B40 →
//   AltPresentGate@4F9760 ? AltPresent@4F9550 : PresentFrame@4FCA30 →
//   Frontend MonitorNotifyAll → AsyncLoad_HasWork/PumpOne/FinishSlice →
//   (SleepMs@551730 iff ldWorkScale<=0) → PollWindowQuit@5522A0 →
//   MainLoop_EndFrame@554DC0.
// Sfx pose v29[12]: hasCameraMatrix@63C550>0 → GetColumn(2/1)+tx/ty/tz;
// else translation 0. Host: lookat eye stand-in; orient+DS OOS.
// W16C..W35 Soft AsyncLoad/FileAsync/Simulate residual unchanged.
void system_mainloop_sfx_listener_update() {
  // Soft ≡ Engine_frameDt @ 0x63C534 — PE stores PerfTimerEnd(7) @ 0x428ED0
  // at frame tail; next Input_tick consumes prior-frame dt.
  static float s_engine_frame_dt = 0.f;
#ifdef _WIN32
  static unsigned s_frame_ms = 0;
#endif

  // PE @ 0x00428AAD Input_tick(Engine_frameDt) — pollDevices + tickAxes.
  // Residual: host boot may also call input_live_poll (adjacency double-tick).
  input_tick(s_engine_frame_dt);

  // PE @ 0x00428AB7 Engine_SimulateFrame (TickSim+Phys+Timers+Drain+cam).
  engine_simulate_frame();

  // PE @ 0x00428ABC..0x00428ACB: EngineState+0xE4 == g_JVM@0x63641C →
  // Jvm_PumpFrame(10.0). PumpFrame: ++Jvm+0x40 → GcSlice(SKIP) →
  // RunThreadsBudgeted. Host gate: jvm_active().
  if (jvm_active()) jvm_pump_frame(10.f);

  // PE @ 0x00428AD5..0x00428B02: wall GetTimeMs dt vs flt_63C530, clamp
  // Engine_dtClamp@60C8CC (=0.1f) → Engine_DispatchAnimateEvents(dt).
  // Soft: animate dllist empty (producers OOS); no CallNamedMethod("animate").
  // PE @ 0x00428B12 Engine_DrainEventQueues — Soft empty +0x5C/+0x78/+0xB0
  // sentinels (same Soft as SimulateFrame substep Drain; MainLoop call site
  // only; no export — order marker, body ≡ empty pass).

  // PE @ 0x00428B17..0x00428CBE: cam → Sfx_ListenerSetPose → UpdateVoices.
  float x = 0.f, y = 0.f, z = 0.f;
  void* cam = render_d3d9_camera_active();
  float ax = 0.f, ay = 0.f, az = 0.f;
  if (cam &&
      render_d3d9_camera_get_lookat(cam, &x, &y, &z, &ax, &ay, &az)) {
    // PE Engine_hasCameraMatrix > 0 path — eye ≡ CameraMatrix_t*
  } else {
    // PE Engine_hasCameraMatrix <= 0: v34/v35/v12 stay 0
    x = y = z = 0.f;
  }
  sfx_listener_set_pos(x, y, z);  // PE @ 0x00428CB6 → 0x005508F0
  sfx_update_voices();            // PE @ 0x00428CBE → 0x00550980

  // PE @ 0x00428CC9..0x00428CD4: PumpLoadQueue then PumpUnloadQueue.
  (void)resource_engine_pump_load_queue();
  (void)resource_engine_pump_unload_queue();

  // PE @ 0x00428CE8: AltPresentGate ≠0 → AltPresent else PresentFrame.
  // Soft AltPresent ≡ video_fmv_present + Frontend.notify (D3D TSS OOS).
  // PresentFrame stand-in: render_d3d9_flush (Present + Frontend.notify).
  if (video_fmv_alt_present_gate() != 0) {
    video_fmv_present();
    frontend_gfx_engine_frame_notify();
  } else {
    render_d3d9_flush();
  }

  // PE @ 0x00428E12..0x00428EC1 AsyncLoad dual path on Engine_ldWorkScale:
  //   scale<=0 (LD_HIGH=-1.0): HasWork → while PumpOne; FinishSlice; Sleep(10)
  //   else (LD_NORM=0.1): budgeted PumpOne while PerfTimer < (scale+1)*EMA
  //     clamp 0.1; FinishSlice; no Sleep.
  // Soft: LD_HIGH uncapped (host 4096 storm cap); LD_NORM capped 64 (budget
  // timer OOS). ++g_asyncload_frame ≡ PE ++dword_6200A4 between PumpOne.
  if (g_ld_work_scale <= 0.f) {
    if (asyncload_has_work()) {
      int pumps = 0;
      while (asyncload_pump_one() && ++pumps < 4096) {
        ++g_asyncload_frame;
      }
    }
    asyncload_finish_slice();
#ifdef _WIN32
    Sleep(10);  // PE Engine_SleepMs(10.0) @ 0x00551730
#endif
  } else {
    if (asyncload_has_work()) {
      int pumps = 0;
      while (asyncload_pump_one() && ++pumps < 64) {
        ++g_asyncload_frame;
      }
    }
    asyncload_finish_slice();
  }

  // PE @ 0x00428F5F Engine_PollWindowQuit @ 0x005522A0 → quit flag.
  render_d3d9_pump(0);
  if (render_d3d9_quit_requested()) request_exit();

  // PE @ 0x00428F6B Engine_MainLoop_EndFrame @ 0x00554DC0.
  engine_mainloop_endframe();

  // Soft Engine_frameDt ← wall ms (PE PerfTimerEnd(7) @ 0x428ECB..0x428ED0).
#ifdef _WIN32
  {
    const unsigned now = GetTickCount();
    if (s_frame_ms != 0) {
      s_engine_frame_dt = static_cast<float>(now - s_frame_ms) * 0.001f;
    }
    s_frame_ms = now;
  }
#endif
}

// Test/host access to Config mirror used by getConfigOptions.
InvObject* system_config_host_for_test() { return system_config_host(); }

int32_t system_loading_peak_for_test() { return g_load_peak; }
int32_t system_loading_opens_for_test() { return g_load_opens; }
int32_t system_ld_priority_for_test() { return g_ld_priority; }


}  // namespace inv
