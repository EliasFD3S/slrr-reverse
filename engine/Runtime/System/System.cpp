#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"
#include "tree_interp.hpp"
#include "host_objects.hpp"
#include "jvm.hpp"

#include <cstdio>
#include <vector>

namespace inv {
namespace {
float g_measure = 1000.f;
float g_measure_div3600 = 1000.f / 3600.f;
float g_ten_div_measure = 10.f / 1000.f;
int32_t g_load_depth = 0;
int32_t g_load_peak = 0;
int32_t g_load_opens = 0;
int32_t g_ld_priority = 0;
float g_ld_work_scale = 0.1f;  // PE Engine_ldWorkScale @ 0x0060C8C4
int32_t g_ld_high = 0;         // PE System_ldHigh @ 0x00640920
int32_t g_config_apply_count = 0;
InvObject* g_config_host = nullptr;

// Engine globals written by getConfigOptions (PE Engine_ApplyConfigOptions @ 0x427BF0).
int32_t g_engine_headlight_rays = 0;       // dword_6187BC
int32_t g_engine_flares = 0;               // dword_6187C0
int32_t g_engine_shadow_size = 0;          // dword_6187B8
int32_t g_engine_shadows = 0;              // dword_6187B4
float g_engine_shadow_detail = 0.f;        // flt_65AED0
int32_t g_engine_texture_size = 0;         // dword_6188B4
float g_engine_object_detail = 0.f;        // flt_65C424
float g_engine_object_detail_dup = 0.f;    // flt_60C8C8 (stock reads object_detail twice)
float g_engine_object_detail_amp = 0.f;    // flt_6188C4
int32_t g_engine_texture_format = 0;       // dword_6188B8
float g_engine_video_gamma_inv = 1.f;      // flt_617728 = 1/video_gamma when set
float g_engine_particle_density = 0.f;     // flt_618948
int32_t g_engine_skidmark_max = 0;         // dword_618BF4
int32_t g_engine_texture_save_q = 0;       // dword_6188B0 = ftol(texture_save_quality*100)
float g_engine_external_damage = 0.f;      // flt_611358
float g_engine_internal_damage = 0.f;      // flt_61135C
float g_engine_deformation = 0.f;          // flt_611360
int32_t g_engine_mem_vertex_max = 0;       // dword_618D4C
int32_t g_engine_mem_vertex_min = 0;       // dword_618D50
int32_t g_engine_mem_texture_max = 0;      // dword_618D54
int32_t g_engine_mem_texture_min = 0;      // dword_618D58
int32_t g_engine_mem_instance_max = 0;     // dword_618D5C
int32_t g_engine_mem_instance_min = 0;     // dword_618D60
int32_t g_engine_mem_sound_max = 0;        // dword_618D64
int32_t g_engine_mem_sound_min = 0;        // off_618D68
int32_t g_engine_resource_loadrate = 0;    // dword_618D6C
int32_t g_engine_force_feedback = 0;       // dword_777448
float g_engine_ffb_strength = 0.f;         // flt_61AC40 = FFB_strength*10000
float g_engine_ffb_strength_emu = 0.f;     // flt_61AB14 = FFB_strength_emulated*1e-6
float g_engine_engine_inertia = 0.f;       // flt_60E028
float g_engine_wheel_gnd_feedback = 0.f;   // flt_60E020
float g_engine_wheel_brake_factor = 0.f;   // flt_60E024
int32_t g_engine_mouse_help = 0;           // dword_63C83C
float g_engine_steerhelp_turn = 0.f;       // flt_63C7A8
float g_engine_head_move_steer = 0.f;      // flt_60E014
float g_engine_head_move_vel = 0.f;        // flt_60E018
float g_engine_head_move_acc = 0.f;        // flt_60E01C

void loading_enter() {
  ++g_load_depth;
  if (g_load_depth > g_load_peak) g_load_peak = g_load_depth;
}

void loading_leave() {
  if (g_load_depth > 0) --g_load_depth;
}
}  // namespace

// Hand-written System natives (skipped by gen_native_stubs.py).

// PE @ 0x0047BE50 System.exit ()V size 0xb (11). Static; no UnboxArg; no
// callees. Bytes: C7 05 20 C5 63 00 01 00 00 00 C3
//   (mov dword ptr [Engine_quitRequested @ 0x63C520], 1; retn).
// 1 xref: Natives_RegisterAll @ 0x00487F45. Flag only — no CRT exit /
// TerminateProcess. Contrast exit(String) @ 0x0047BE60 size 0x15: UnboxArg
// discard, does NOT set quit. Host: request_exit() → g_exit (MainLoop-polled
// like Engine_quitRequested).
void java_lang_System_exit() { request_exit(); }  // PE @ 0x0047BE50

void java_lang_System_exit_1(InvObject* s) {
  // PE @ 0x0047BE60 size 0x15: static UnboxArg then ret — does NOT set
  // Engine_quitRequested (contrast exit()V @ 0x0047BE50). String discarded.
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
  std::fprintf(stderr, "%s\n", string_cstr(s));
}

// PE @ 0x0047C0B0 System.buildNumber ()I size 0x6 (6). Static; no UnboxArg;
// no callees. Bytes: B8 5E 02 00 00 C3 (mov eax,25Eh; retn).
// 1 xref: Natives_RegisterAll. Always returns 0x25e (606).
int32_t java_lang_System_buildNumber() { return 0x25e; }  // PE @ 0x0047C0B0

// PE @ 0x0047C0C0 System_info (I)Ljava.lang.String; size 0xf2.
// JVM_UnboxArg dest0=nullptr (static); cmp eax,5 / ja default; sprintf
// "%d" (sub_551220 -> Util_Vsprintf) into 256-byte stack, then thiscall
// JVM_String_from_cstr @ 0x004174A0. 1 xref: Natives_RegisterAll.
// PE index (offsets only — list identities unknown, do not invent):
//   0: count sentinel list [JVM+0x18] head+8 next+4 (skip first)
//   1: count [JVM+0x1C] same walk
//   2: count [JVM+0x10] same walk
//   3: dword [JVM+0x24]
//   4: dword [JVM+0x28]
//   5: dword_62F138 (ctor++ / dtor-- live objects; not buildNumber 0x25e)
//   default: still String(uninit buf). Host returns "".
// Host stand-in (no stock JVM lists): load stats + buildNumber.
InvObject* java_lang_System_info(int32_t i) {
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
    case 5:  // PE: dword_62F138, not 0x25e
      v = java_lang_System_buildNumber();
      break;
    default:
      return string_new("");
  }
  std::snprintf(buf, sizeof(buf), "%d", v);
  return string_new(buf);
}

// PE @ 0x0047C060 System.gc ()I size 0xb (11). Static; no UnboxArg.
// eax=[esp+4] CallInfo*; ecx=[eax] (*CallInfo); jmp JVM_GC @ 0x417F80
// (thiscall, size 0x140). 1 xref: Natives_RegisterAll.
// JVM_GC: if *(JVM+0x20) work buf set → drain/collect (ret count or -1 busy);
// else alloc/init GC state, dword_62E004=JVM*, ret -1. Host: no JVM GC → 0.
int32_t java_lang_System_gc() { return 0; }

// PE @ 0x0047C070 System.runFinalization ()I size 0x3 (3).
// Static; no UnboxArg; no callees. Bytes: 33 C0 C3 (xor eax,eax; retn).
// 1 xref: Natives_RegisterAll. Always returns 0 (no finalizer queue).
int32_t java_lang_System_runFinalization() { return 0; }
int32_t java_lang_System_compileAll(InvObject* path) {
  Jvm* j = jvm_active();
  if (!j) return 0;
  const char* rel = string_cstr(path);
  loading_enter();
  const int32_t n = j->compile_all(rel);
  loading_leave();
  return n;
}

void java_lang_System_analyze(InvObject* o) { (void)o; }
void java_lang_System_analyze_1(float f) { (void)f; }
void java_lang_System_analyze_2(int32_t i) { (void)i; }

void java_lang_System_arraycopy(InvObject* src, int32_t src_position,
                                InvObject* dst, int32_t dst_position,
                                int32_t length) {
  // Object[] / Vector elementData hosts share tree_vector storage.
  if (!src || !dst || length <= 0) return;
  if (src_position < 0 || dst_position < 0) return;
  const int32_t src_n = tree_vector_size(src);
  if (src_position > src_n || length > src_n - src_position) return;
  const int32_t need = dst_position + length;
  if (tree_vector_size(dst) < need) tree_vector_resize(dst, need);
  // Temp buffer so overlapping regions match Java memmove semantics.
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
float java_lang_System_timeWarp(float m) { return time_warp(m); }  // PE @ 0x0047BF80

// PE @ 0x0047BFF0 System.syncGameTime (F)V size 0x61 (97). Static;
// JVM_UnboxArg dest0=nullptr; fld t; call sub_5D6750 (__ftol ST0→eax);
// cdq; idiv 0x15180 (86400); mov dword_640988, edx (sec-of-day);
// dword_6178D0 := 1 iff edx>=0x11940 (72000=20h) || edx<0xE10 (3600=1h)
// || (edx>=0x4650 (18000=5h) && edx<0x6270 (25200=7h)); else 0.
// Ret eax=days discarded (sig V). 1 xref: Natives_RegisterAll.
// Contrast: simTime = read *Engine_simTime; timeWarp = scale Engine_timeWarp;
// syncGameTime = calendar TOD only (GameLogic.setTime/spendTime). Host:
// time_sync_game stores sec-of-day (g_game_tod); night flag not mirrored here.
void java_lang_System_syncGameTime(float t) { time_sync_game(t); }  // PE @ 0x0047BFF0

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

// Leftover unused natives (declared in System.java, never called; empty in stock).
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
// Body @ 0x5378B0: eax=dword_618D6C; rep stosd 0x20 dwords at dword_765E8C
// with eax; dword_765F24 = eax<<5 (32*dword_618D6C). Seeds ResourceEngine
// load-ring for PumpLoadQueue — does NOT clear [RE+0x106ED4].
// 1 xref: Natives_RegisterAll. Contrast isLoading (pure dword load of flag).
// Host stand-in: zero depth/peak (LoadingScreen.track arms a fresh watch).
void java_lang_System_isLoadingReset() {
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
// JVM_UnboxArg dest0=nullptr → int pri. Does NOT store the int.
//   pri==0 (LD_NORM): Engine_ldWorkScale=0.1 (0x3DCCCCCD), System_ldHigh=0
//   else (LD_HIGH):   Engine_ldWorkScale=-1.0 (0xBF800000), System_ldHigh=1
// MainLoop: scale<=0 → unlimited async drain. Orthogonal to isLoading flag.
// Host: mirror the two PE globals; keep g_ld_priority for info(3)/smoke.
void java_lang_System_setLdPriority(int32_t pri) {
  if (pri == 0) {
    g_ld_work_scale = 0.1f;
    g_ld_high = 0;
  } else {
    g_ld_work_scale = -1.0f;
    g_ld_high = 1;
  }
  g_ld_priority = pri;
}

// Test/host access to Config mirror used by getConfigOptions.
InvObject* system_config_host_for_test() { return system_config_host(); }

int32_t system_loading_peak_for_test() { return g_load_peak; }
int32_t system_loading_opens_for_test() { return g_load_opens; }
int32_t system_ld_priority_for_test() { return g_ld_priority; }

}  // namespace inv
