// Split from natives_generated_world.cpp — AnimationParticle.cpp
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

#include "../Parts/world_state.hpp"

namespace inv {
namespace {

// PE opcodes on the Animation native queue (stride 8, cap grows by 32).
// Enqueue: JNI write via Engine_realloc @ 0x54F580 (buf+0x9C / count+0xA0 /
// cap+0xA4). Drain: Animation_drainQueue @ 0x53FEF0 (from sub_541FE0 tick).
constexpr int32_t kAnimOpPlay = 0;
constexpr int32_t kAnimOpLoopPlay = 1;
constexpr int32_t kAnimOpStop = 2;  // drain-only; JNI does not enqueue
constexpr int32_t kAnimOpPause = 3;
constexpr int32_t kAnimOpSeek = 4;
constexpr int32_t kAnimOpSetSpeed = 5;
constexpr int32_t kAnimOpSetFade = 6;

// Animation_drainSeek @ 0x540120 / flt_5F37D8 — keyframe quantum.
constexpr float kAnimKeyStep = 0.05f;

void anim_sync(InvObject* self, const AnimState& a) {
  if (!self) return;
  tree_field_set_float(self, "anim_speed", a.speed);
  tree_field_set_float(self, "anim_fade", a.fade);
  tree_field_set_float(self, "anim_pos", a.pos);
  tree_field_set_int(self, "anim_playing", a.playing ? 1 : 0);
  tree_field_set_int(self, "anim_loop", a.loop ? 1 : 0);
  tree_field_set_int(self, "anim_q", static_cast<int32_t>(a.queue.size()));
}

// Soft PE ≈ Animation_drainQueue @ 0x53FEF0 size 0x14d: switch on queued
// {op,arg} stride 8. PE +0x94 (this[37]): 0 idle / 1 play / 2 pause / 3
// loop — Soft tree "anim_pe_state". Op6 (setFade) is default-skip in PE
// drain — Soft still mirrors fade for TREE readers. Seek →
// Animation_drainSeek @ 0x540120: AnimClip_lastKeyTime @ 0x532E50 stand-in
// a.duration; pos = Engine_ftol(key/0.05)*frac(arg)*0.05 (flt_5F37D8).
void anim_drain_op(InvObject* self, AnimState& a, int32_t op, float arg) {
  int32_t st = self ? tree_field_get_int(self, "anim_pe_state") : 0;
  switch (op) {
    case kAnimOpPlay:
      // PE: state0 → pos(+0x1C)=0,state1; state2 → state1; 1/3 no-op.
      if (st == 0) a.pos = 0.f;
      if (st == 0 || st == 2) st = 1;
      a.last_t = time_current();
      break;
    case kAnimOpLoopPlay:
      // PE: state0 → +0x98(this[38])=arg, pos=0, state3; state2 → state3.
      if (st == 0) {
        a.pos = 0.f;
        if (self) tree_field_set_float(self, "anim_loop_arg", arg);
      }
      if (st == 0 || st == 2) st = 3;
      a.last_t = time_current();
      break;
    case kAnimOpStop:
      // PE case 2: state==1 || (state-2)<=1 → {1,2,3} → 0.
      if (st == 1 || st == 2 || st == 3) st = 0;
      break;
    case kAnimOpPause:
      // PE: state==0 || state-1==0 || state-1==2 → {0,1,3} → 2.
      if (st == 0 || st == 1 || st == 3) st = 2;
      break;
    case kAnimOpSeek: {
      // Animation_drainSeek @ 0x540120 + AnimClip_lastKeyTime @ 0x532E50.
      // Engine_ftol @ 0x5D6750 = (__int64)trunc toward 0 — Soft std::trunc.
      float key = a.duration;
      if (key < 0.01f) key = 1.f;
      const float frac = arg - std::trunc(arg);
      const float steps = std::trunc(key / kAnimKeyStep);
      a.pos = steps * frac * kAnimKeyStep;
      if (a.pos < 0.f) a.pos = 0.f;
      // PE state0 seek also forces pause (state=2); 1/2/3 keep state.
      if (st == 0) st = 2;
      a.last_t = time_current();
      break;
    }
    case kAnimOpSetSpeed:
      // PE: *[+0x28]=*[+0x24]=arg; *[+0x20]=0. Soft: speed + TREE mirrors.
      a.speed = arg;
      if (self) {
        tree_field_set_float(self, "anim_speed", arg);       // ≈ +0x24
        tree_field_set_float(self, "anim_speed_cur", arg);   // ≈ +0x28
        tree_field_set_float(self, "anim_speed_ramp", 0.f);  // ≈ +0x20
      }
      break;
    case kAnimOpSetFade:
      // PE drain default/case6: continue (discard). Soft TREE mirror.
      a.fade = arg;
      break;
    default:
      break;
  }
  a.playing = (st == 1 || st == 3);
  a.loop = (st == 3);
  if (self) tree_field_set_int(self, "anim_pe_state", st);
}

// Soft gate ≈ PE RenderRef Native.ptr+8==0 silent. Missing "obj" (TREE
// smoke / collapsed ctor) → allow — Soft stand-in when field unwired.
bool anim_render_dead(InvObject* self) {
  if (!self) return true;
  InvObject* obj = tree_field_get_obj(self, "obj");
  if (!obj) return false;
  return java_util_resource_ResourceRef_id(obj) == 0;
}

// Soft: enqueue (Engine_realloc write) then drain one op (tick stand-in).
// Host keeps queue history for anim_q; PE zeros count[+0xA0] after drain.
void anim_push(InvObject* self, int32_t op, float arg) {
  if (!self) return;
  auto& a = AN(self);
  a.queue.push_back(AnimOp{op, arg});
  anim_drain_op(self, a, op, arg);
  anim_sync(self, a);
}

}  // namespace

void java_util_resource_Animation_init(InvObject* self, InvObject* render,
                                       InvObject* type) {
  // PE @ 0x0047EA10 size 0x186 (390). UnboxArg sig
  // (Ljava.util.resource.RenderRef;Ljava.util.resource.ResourceRef;) —
  // this + render + type; two extra stack outs fed to clipSet* (uninit /
  // often 0 — not in JNI sig). Walk render inner=[+0xC]: vtbl+0x14 if
  // +0x4C!=1, ResHandle_PrepareLod @ 0x5447D0 (0x80000000), vtbl+0xC →
  // mesh; sub_541400 gate → Animation_createLinkedHandle @ 0x541600
  // (malloc 0xA8 + Animation_handleCtor @ 0x540440); then
  // Animation_clipSetSpeed @ 0x541810 / Animation_clipSetFade @ 0x541860 /
  // Animation_clipSetPos @ 0x5417F0. Always JVM_vm_set_int_field(this,
  // Native_ptr_field_id @ 0x62E008, clip) — 0 on fail. Link field "obj"
  // RenderRef Native.ptr dllist (+0x48/+0x50) to render inner. Soft:
  // g_anims + anim_clip≈clip; tree "obj"/"type"; speed_cur=1 Soft stand-in
  // (PE ctor zeros +0x20/+0x24/+0x28 then clipSetSpeed overwrites).
  if (!self) return;
  auto& a = AN(self);
  a = AnimState{};
  a.last_t = time_current();
  tree_field_set_obj(self, "obj", render);
  tree_field_set_obj(self, "type", type);
  tree_field_set_int(self, "anim_clip", (render && type) ? 1 : 0);
  tree_field_set_int(self, "anim_pe_state", 0);  // Soft ≈ PE +0x94 idle
  tree_field_set_float(self, "anim_loop_arg", 0.f);     // Soft ≈ +0x98
  tree_field_set_float(self, "anim_speed_ramp", 0.f);   // Soft ≈ +0x20
  tree_field_set_float(self, "anim_speed_cur", 1.f);    // Soft ≈ +0x28
  anim_sync(self, a);
}

void java_util_resource_Animation_finalize(InvObject* self) {
  // PE @ 0x0047EBA0 size 0x5c (92). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // esi = JVM_vm_get_int_field(this, Native_ptr_field_id @ 0x62E008 /
  // JVM_vm_get_int_field @ 0x0042AB50) — Animation handle. Field "obj"
  // (off_6130A0 / aJavaUtilResour_114 "java.util.resource.RenderRef") via
  // JVM_vm_get_instance_field @ 0x0042A690 → RenderRef Native.ptr.
  // Gate order (disasm): [RenderRef.ptr+8]==0 (test ecx @ 0x47EBEA jz
  // loc_47EBFA) OR esi==0 (test esi @ 0x47EBEE jz) → silent (NO Mighty).
  // Else thiscall (**esi)(esi,1) vtbl+0 @ 0x47EBF8 (release). Contrast
  // getPos @ 0x0047F050: ONLY +8 gate, NO esi==0 test. ResourceRef.id @
  // 0x0047D290 reads same [handle+8]. Soft: null obj → silent (PE would
  // deref Native.ptr; TREE collapsed ctor); ResourceRef_id(obj)==0 ≈ +8;
  // anim_clip==0 ≈ esi==0; clear anim_clip/pe_state + g_anims.erase ≈
  // vtbl+0(1).
  if (!self) return;
  InvObject* obj = tree_field_get_obj(self, "obj");
  if (!obj || java_util_resource_ResourceRef_id(obj) == 0) return;
  if (tree_field_get_int(self, "anim_clip") == 0) return;
  tree_field_set_int(self, "anim_clip", 0);
  tree_field_set_int(self, "anim_pe_state", 0);
  g_anims.erase(self);
}

void java_util_resource_Animation_setSpeed(InvObject* self, float speed) {
  // PE @ 0x0047EC00 size 0xbd (189). UnboxArg (F)V this+speed. esi =
  // JVM_vm_get_int_field(this, dword_62E008 @ 0x0042AB50). NO Animation
  // handle==0 test, NO Mighty. Gate: RenderRef field "obj" (aObj @
  // 0x6130A0 / aJavaUtilResour_114) Native.ptr +8 == 0 → jz loc_47ECB8
  // silent. Else fld/fstp speed RAW (no fmul/scale). edi=5; if
  // count[+0xA0]==cap[+0xA4]: cap+=0x20, realloc buf[+0x9C] via
  // Engine_realloc @ 0x54F580 size 8*cap. Write {5,speed} stride 8.
  // Drain: Animation_drainQueue @ 0x53FEF0 case5 → +0x28/+0x24/+0x20
  // (same layout as Animation_clipSetSpeed @ 0x541810 when rampDt==0).
  // Soft: anim_render_dead (null obj allow); anim_push enqueue+drain.
  // NO esi==0 gate (unlike finalize @ 0x0047EBA0).
  if (anim_render_dead(self)) return;
  anim_push(self, kAnimOpSetSpeed, speed);
}

void java_util_resource_Animation_setFade(InvObject* self, float fade) {
  // PE @ 0x0047ECC0 size 0xbd (189). Twin of setSpeed @ 0x0047EC00
  // (opcode 5→6). Engine_realloc write {6,fade}. Drain @ 0x53FEF0
  // default/case6: continue (discard) — Soft TREE still mirrors fade.
  if (anim_render_dead(self)) return;
  anim_push(self, kAnimOpSetFade, fade);
}

void java_util_resource_Animation_play(InvObject* self) {
  // PE @ 0x0047ED80 size 0xa6 (166). UnboxArg ()V. RenderRef+8 gate.
  // Engine_realloc write {0,0}. Drain @ 0x53FEF0 case0 play state.
  // Soft: anim_render_dead; anim_push enqueue+drain.
  if (anim_render_dead(self)) return;
  anim_push(self, kAnimOpPlay, 0.f);
}

void java_util_resource_Animation_loopPlay(InvObject* self) {
  // PE @ 0x0047EE30 size 0xa9 (169). Twin of play (opcode 0→1).
  // Engine_realloc write {1,0}. Drain case1 loop state.
  if (anim_render_dead(self)) return;
  anim_push(self, kAnimOpLoopPlay, 0.f);
}

void java_util_resource_Animation_pause(InvObject* self) {
  // PE @ 0x0047EEE0 size 0xa9 (169). Engine_realloc write {3,0}.
  // Drain @ 0x53FEF0 case3 → pause state (+0x94=2). Soft drain map.
  if (anim_render_dead(self)) return;
  anim_push(self, kAnimOpPause, 0.f);
}

void java_util_resource_Animation_seek(InvObject* self, float position) {
  // PE @ 0x0047EF90 size 0xbd (189). Engine_realloc write {4,pos} RAW.
  // Drain: Animation_drainSeek @ 0x540120 — pos(+0x1C) =
  // Engine_ftol(AnimClip_lastKeyTime/0.05)*frac(arg)*0.05 where frac =
  // arg−Engine_ftol(arg) (0x5D6750 trunc toward 0; Soft std::trunc).
  // Soft: a.duration ≈ lastKeyTime until init wires clip.
  if (anim_render_dead(self)) return;
  anim_push(self, kAnimOpSeek, position);
}

float java_util_resource_Animation_getPos(InvObject* self) {
  // PE @ 0x0047F050 size 0x5f (95). Unbox this. esi = Animation
  // Native.ptr (JVM_vm_get_int_field / Native_ptr_field_id @ 0x62E008).
  // Field "obj" RenderRef (off_6130A0 / aJavaUtilResour_114). Gate ONLY
  // [RenderRef.ptr+8]==0 (test ecx @ 0x47F09A jz loc_47F0A7) → fld
  // flt_5E73CC (0.0). NO esi==0 test (contrast finalize @ 0x47EBEE).
  // Else thiscall Animation_normalizedPos @ 0x53FE70 (ecx=esi):
  //   mesh=*(handle+0x18); null → 0.0
  //   [mesh+0x4C]!=1 → vtbl+0x14(1.0f=0x3F800000)
  //   ResHandle_PrepareLod(mesh, 0x80000000, 0, 10.0f=0x41200000) @
  //   0x5447D0; sign bit → 0; else vtbl+0xC(1.0f); clip=*(eax+0xC)
  //   clip==0 → 0.0; else *(float*)(handle+0x1C) /
  //   AnimClip_lastKeyTime(clip) @ 0x532E50 (keys[count-1] float).
  // READ-only — no queue write, no drain, no tick. Soft: anim_render_dead
  // (null-obj allow); missing g_anims ≈ mesh/clip null → 0; a.pos /
  // a.duration stand-in for +0x1C / lastKeyTime (LOD/vtbl OOS). NO
  // anim_advance (host fiction retired).
  if (anim_render_dead(self)) return 0.f;
  auto it = g_anims.find(self);
  if (it == g_anims.end()) return 0.f;
  const AnimState& a = it->second;
  float dur = a.duration;
  if (dur < 0.01f) dur = 1.f;  // Soft when clip unwired (PE lastKeyTime may be 0)
  const float norm = a.pos / dur;
  tree_field_set_float(self, "anim_pos", norm);
  return norm;
}

// Phase 2.84 setWater / addWaterLimit: natives_gameref.cpp

void java_util_resource_GroundRef_setFog(InvObject* self, int32_t color, float near, float far) {
  // PE @ 0x00486A20 size 0x88. UnboxArg (IFF)V. handle==0 silent (NO Mighty;
  // contrast Camera.setFog @ 0x00486570 Mighty + dual PrepareLod hop).
  // Engine_malloc @ 0x54F560 size 16: byte0=1, +4/+8=near/far * 10.0
  // (flt_5E7334), +0xC=color (no mask). Engine_queryGameRefChannel @
  // 0x426470(handle, type 0x4A=74, pkt). Soft: TREE fog_* + D3D stand-in
  // (channel bind / pkt lifetime OOS). Camera.setFog Soft lives in
  // RenderNatives.cpp — distinct PE path, shared host D3D helper only.
  if (!self) return;
  const float n = near * 10.f;
  const float f = far * 10.f;
  tree_field_set_int(self, "fog_on", 1);
  tree_field_set_int(self, "fog_color", color);
  tree_field_set_float(self, "fog_near", n);
  tree_field_set_float(self, "fog_far", f);
  render_d3d9_set_fog(color & 0x00ffffff, n, f);
}

namespace {

// PE @ 0x00480440 — shared GameRef/RenderRef.getDetail()F body.
float resref_get_detail_impl(InvObject* self) {
  if (!self) return 0.f;
  // [inner+0x4C] INSTANCE_GAME=1 | INSTANCE_RENDER=3 (int_convert 0x4C=76).
  const int32_t rtype = java_util_resource_ResourceRef_type(self);
  if (rtype != 1 && rtype != 3) return 0.f;
  // [handle+8] resource id (int_convert 0x8=8).
  if (java_util_resource_ResourceRef_id(self) == 0) return 0.f;
  // *(float*)(inner+0x6C) LOD detail bias (int_convert 0x6C=108).
  return tree_field_get_float(self, "detail");
}

}  // namespace

float java_util_resource_GameRef_getDetail(InvObject* self) {
  // PE @ 0x00480440 size 0xbb — shared with RenderRef.getDetail()F.
  // Unbox this (JVM_UnboxArg @ 0x0045D910). handle =
  // JVM_vm_get_int_field(this, Native_ptr_field_id @ 0x62E008). handle==0 →
  // Engine_strcat_cap @ 0x551140 ("!" @ 0x61319C + "Mighty ERROR" @
  // 0x6131A0) + Engine_ErrorLogMsgBox @ 0x5513B0 → 0.0. inner=[handle+0xC];
  // inner==0 OR [inner+0x4C]!=1 INSTANCE_GAME AND !=3 INSTANCE_RENDER →
  // Wrong ResourceType + 0.0. [handle+8]==0 → 0.0. Else *(float*)(inner+0x6C)
  // LOD detail bias. Soft: ResourceRef_type/id gates; tree "detail" ≈
  // inner+0x6C; Mighty/WrongResourceType not mirrored.
  return resref_get_detail_impl(self);
}

float java_util_resource_RenderRef_getDetail(InvObject* self) {
  // PE @ 0x00480440 size 0xbb (187) — same entry as GameRef.getDetail()F
  // (Natives_RegisterAll data xrefs). Callees: JVM_UnboxArg @ 0x0045D910,
  // JVM_vm_get_int_field @ 0x0042AB50 (Native_ptr_field_id),
  // Engine_strcat_cap @ 0x00551140, Engine_ErrorLogMsgBox @ 0x005513B0.
  // Gates/returns identical to GameRef path above. Soft: shared
  // resref_get_detail_impl.
  return resref_get_detail_impl(self);
}

// Phase 2.84 water / halt / pedDistance + traffic behaviour: natives_gameref.cpp
// getNearestCross / getStartDirection / getRouteLength / alignToRoad:
//   natives_resources.cpp (Phase 2.22–2.23)
// setPedestrianDensityN / add|remPedestrianType: natives_gameref.cpp

void java_util_resource_ParticleSystem_init(InvObject* self, InvObject* parent,
                                            InvObject* type, InvObject* alias) {
  // PE @ 0x0047F0B0 size 0x63. UnboxArg (LLLjava.lang.String;)V via
  // JVM_UnboxArg @ 0x0045D910: this, parent (ResourceRef), type (RenderRef),
  // alias (String). handle = JVM_vm_get_int_field(this, dword_62E008).
  // Gate: handle!=0 && parent!=0 && type!=0 (alias unchecked). Else silent
  // ret (NO Mighty). thiscall sub_48A490(handle, parent, type, cb=0, a5=0,
  // alias, 0.0f) — 5 xrefs, do NOT rename. Helper: inner=*(type+0xC);
  // sub_419860(inner, 0xA0000001, …); ResourceEngine_type_renderinst(parent,
  // type, alias, 0) → link handle+0xC; sub_540FF0 → blob at wrapper+0x18;
  // OR 0x8000 at +0xBC. cb=0 → no deferred callback. Contrast stop@
  // 0x0047F120: JMP sub_48A610 OR 0x40000000 at [obj+0x6C] (no unbind).
  // modePermanent@0x0047F150: same inner-walk inline, toggle 0x20000000 at
  // [obj+0x6C]. init does not touch stop/permanent bits. Helpers sub_48A490
  // / sub_419860 / sub_540FF0 / dword_62E008 NOT renamed.
  // Host: g_particles[self] fresh bind stands in for sub_48A490;
  // resref_set_parent for parent link; Native.ptr gate (prior newNative from
  // ctor super()) not mirrored when TREE collapses ctor; vtbl/sub_5447D0 not
  // mirrored.
  if (!self || !parent || !type) return;
  auto& st = PS(self);
  st = ParticleState{};
  st.parent = parent;
  st.type = type;
  st.sys_alias = alias_key(alias);
  st.stopped = false;
  resref_set_parent(self, parent);
  tree_field_set_obj(self, "ps_parent", parent);
  tree_field_set_obj(self, "ps_type", type);
  tree_field_set_obj(self, "ps_alias", alias);
  tree_field_set_int(self, "ps_stopped", 0);
}

void java_util_resource_ParticleSystem_stop(InvObject* self) {
  // PE @ 0x0047F120 size 0x2f (47). Unbox this. handle =
  // JVM_vm_get_int_field(this, dword_62E008). handle==0 → ret (NO Mighty).
  // Else JMP sub_48A610(handle) size 0x51 (11 xrefs, not renamed):
  // inner=*(handle+0xC); 0 → ret. *(inner+0x4C)==1 skip vtbl+0x14(1.0f).
  // ResHandle_PrepareLod(inner, 0xA0000001, 0, 0) @ 0x5447D0; sign bit →
  // ret. Else vtbl+0xC(1.0f); obj=*(eax+0x18); OR 0x40000000 at
  // [obj+0x6C]. No alias walk — action list untouched.
  // vs init @ 0x0047F0B0: handle/parent/type==0 silent ret (also NO Mighty),
  // else sub_48A490 binds renderinst (zeros +0x84..+0x98, [+0x80]=2, OR
  // 0x8000). stop does not unbind/zero those slots or erase actions.
  // Host: missing ParticleState ≈ handle==0; st.stopped stands in for
  // OR 0x40000000 (vtbl/ResHandle_PrepareLod not mirrored). Actions kept
  // (PE does not clear — smoke nact3==0 is host fiction; Soft keeps PE).
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  auto& st = it->second;
  st.stopped = true;
  tree_field_set_int(self, "ps_stopped", 1);
}

void java_util_resource_ParticleSystem_setFreq(InvObject* self, float freq) {
  // PE @ 0x0047F1F0 size 0x7e. Unbox this+freq. handle =
  // JVM_vm_get_int_field(this, dword_62E008). handle==0 or
  // inner=*(handle+0xC)==0 → silent ret (NO Mighty). *(inner+0x4C)==1
  // skip vtbl+0x14(0x3F800000). ResHandle_PrepareLod(inner,
  // 0xA0000001, 0, 0) @ 0x5447D0; TEST EAX,0x80000000 → ret. Else
  // vtbl+0xC(0x3F800000); obj=*(eax+0x18); if obj: fstp [obj+0xC]=freq
  // (system-level). Same inner-walk as modePermanent@0x0047F150 / stop
  // helper sub_48A610. Contrast modePermanent: toggles 0x20000000 at
  // [obj+0x6C] (permanent 0/1). Contrast setCounter@0x0047FA70:
  // ResHandle_getPayload + type-13 action list (pos/r payload), NOT
  // [obj+0xC]; setSource writes per-action freq at action+0x54.
  // Host: missing ParticleState ≈ handle==0; st.freq stands in for
  // [obj+0xC] (vtbl/ResHandle_PrepareLod not mirrored).
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  it->second.freq = freq;
  tree_field_set_float(self, "ps_freq", freq);
}

void java_util_resource_ParticleSystem_setDirectSource(
    InvObject* self, InvObject* alias, InvObject* pos, float rmin, float rmax,
    InvObject* vel, float vmin, float vmax, float num, InvObject* bone) {
  // PE @ 0x0047F270 size 0x3e8 (1000). Unbox this+alias+pos+rmin/rmax+vel+
  // vmin/vmax+num+bone. handle=JVM_vm_get_int_field(this, dword_62E008);
  // null / handle[+0xC] / ResHandle_getPayload(0xA0000001,1.0,0,0) /
  // slot=result[+0x18] → silent (NO Mighty). NO stop-bit (0x40000000)
  // gate. Read Vector3 floats via unk_6130C8..DC; bone≠0 →
  // RenderRef_bindBone @ 0x0048BC40. Walk list *(slot+0x24)
  // next=Engine_DllistNext @ 0x40CFC0; match Util_stricmp @ 0x5D7190
  // (node+0x14, alias). Hit + type[+0x10]==11 → replace spheres at
  // +0x58/+0x5C (free old), store num at +0x54, bone at +0x60. Miss /
  // empty list → malloc 0x64 (100), type=11, vtbl off_5F1128, insert
  // GameRef_physDcList_insertTail @ 0x45FAF0. Contrast setSource@
  // 0x0047F660: type==10, malloc 0x68, vtbl off_5F138C, insert
  // sub_489F70, Java `freq`→+0x54. Soft PE: missing ParticleState ≈
  // handle==0; kind≠Direct on existing alias ≈ type≠11 (no overwrite);
  // rate=num ≈ +0x54 ONLY — PE does NOT touch type-13 getCounter +0x58
  // (smoke ping==75 / counter+=n retired). Create zeros counter; update
  // preserves it. ResHandle_getPayload / list insert OOS. PE swaps
  // rmin/rmax (and vmin/vmax) when hi<lo — Soft mirrors. bone alias_key
  // stand-in for RenderRef_bindBone @ 0x48BC40 (+0x60).
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  auto& st = it->second;
  const std::string key = alias_key(alias);
  if (key.empty()) return;
  ParticleAction& a = st.actions[key];
  if (a.kind != ParticleAction::None && a.kind != ParticleAction::Direct)
    return;
  const bool created = (a.kind == ParticleAction::None);
  a.kind = ParticleAction::Direct;
  float rlo = rmin, rhi = rmax;
  if (rhi < rlo) {
    const float t = rlo;
    rlo = rhi;
    rhi = t;
  }
  float vlo = vmin, vhi = vmax;
  if (vhi < vlo) {
    const float t = vlo;
    vlo = vhi;
    vhi = t;
  }
  if (pos) vec3_get(pos, &a.px, &a.py, &a.pz);
  a.rmin = rlo;
  a.rmax = rhi;
  if (vel) vec3_get(vel, &a.vx, &a.vy, &a.vz);
  a.vmin = vlo;
  a.vmax = vhi;
  a.rate = num;
  a.bone = alias_key(bone);
  if (created) a.counter = 0;
  tree_field_set_int(self, "ps_actions",
                     static_cast<int32_t>(st.actions.size()));
}  // PE @ 0x0047F270

void java_util_resource_ParticleSystem_setSource(
    InvObject* self, InvObject* alias, InvObject* pos, float rmin, float rmax,
    InvObject* vel, float vmin, float vmax, float freq, InvObject* bone) {
  // PE @ 0x0047F660 size 0x401 (1025). Unbox this+alias+pos+rmin/rmax+vel+
  // vmin/vmax+freq+bone. handle=JVM_vm_get_int_field(this, dword_62E008);
  // null / handle[+0xC] / ResHandle_getPayload(0xA0000001,1.0,0,0) /
  // slot=result[+0x18] → silent (NO Mighty). NO stop-bit (0x40000000)
  // gate. Read Vector3 floats via unk_6130E0..F4; bone≠0 →
  // RenderRef_bindBone. Walk list *(slot+0x24) next=Engine_DllistNext @
  // 0x40CFC0; match Util_stricmp @ 0x5D7190 (node+0x14, alias). Hit +
  // type[+0x10]==10 → replace spheres at +0x58/+0x5C (free old), store
  // freq at +0x54, bone at +0x60; NO counter bump. Miss / empty list →
  // malloc 0x68, type=10, vtbl off_5F138C, +0x64=0, insert sub_489F70.
  // Contrast setDirectSource@0x0047F270: type==11, malloc 0x64, vtbl
  // off_5F1128, insert GameRef_physDcList_insertTail @ 0x45FAF0; Java
  // `num`→+0x54. Soft PE: missing ParticleState ≈ handle==0; kind≠Source
  // on existing alias ≈ type≠10 (no overwrite); rate=freq ≈ +0x54, no
  // counter++; create zeros counter (PE create +0x64=0 stand-in). PE
  // swaps rmin/rmax (and vmin/vmax) when hi<lo — Soft mirrors.
  // ResHandle_getPayload / list insert OOS; bone alias_key stand-in for
  // bindBone.
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  auto& st = it->second;
  const std::string key = alias_key(alias);
  if (key.empty()) return;
  ParticleAction& a = st.actions[key];
  if (a.kind != ParticleAction::None && a.kind != ParticleAction::Source)
    return;
  const bool created = (a.kind == ParticleAction::None);
  a.kind = ParticleAction::Source;
  float rlo = rmin, rhi = rmax;
  if (rhi < rlo) {
    const float t = rlo;
    rlo = rhi;
    rhi = t;
  }
  float vlo = vmin, vhi = vmax;
  if (vhi < vlo) {
    const float t = vlo;
    vlo = vhi;
    vhi = t;
  }
  if (pos) vec3_get(pos, &a.px, &a.py, &a.pz);
  a.rmin = rlo;
  a.rmax = rhi;
  if (vel) vec3_get(vel, &a.vx, &a.vy, &a.vz);
  a.vmin = vlo;
  a.vmax = vhi;
  a.rate = freq;
  a.bone = alias_key(bone);
  if (created) a.counter = 0;
  tree_field_set_int(self, "ps_actions",
                     static_cast<int32_t>(st.actions.size()));
}

void java_util_resource_ParticleSystem_setCounter(InvObject* self,
                                                  InvObject* alias,
                                                  InvObject* pos, float r) {
  // PE @ 0x0047FA70 size 0x259 (601). Unbox this+alias+pos+r. handle=
  // JVM_vm_get_int_field(this, dword_62E008); null / inner+0xC /
  // ResHandle_getPayload(0xA0000001,1.0,0,0) @ 0x419860 / slot=result[+0x18]
  // → silent (NO Mighty). NO stop-bit gate. Read Vector3 via
  // unk_6130F8..00 (float fields). Walk list *(slot+0x18)+0x1C; empty →
  // Engine_SimObjectListEmpty @ 0x429390; next=Engine_DllistNext @
  // 0x40CFC0; match Util_stricmp @ 0x5D7190 (node+0x14, alias).
  // Hit + type[+0x10]==13 → malloc 0x20 payload (pos@+4..+C; r>=0 →
  // +0x10=r +0x14=0 else +0x14=r +0x10=0; sq@+0x18/+0x1C), free old
  // [node+0x60], store new; NO touch +0x54/+0x58/+0x5C. Hit type≠13 →
  // silent. Miss/empty → malloc 0x64 type=13 vtbl off_5F13A0, strncpy
  // alias@+0x14(64), payload@+0x60, zero +0xC/+0x54/+0x58/+0x5C, insert
  // DLL. Contrast setSource@0x0047F660: type==10, list+0x24, freq@+0x54.
  // Contrast getCounter@0x0047FCD0: consume +0x58 only (not payload).
  // Soft PE: missing ParticleState ≈ handle==0; kind≠Counter on existing
  // alias ≈ type≠13 (no overwrite); rate=r kept; TREE ps_ctr_rpos /
  // ps_ctr_rneg mirror payload +0x10/+0x14 sign split; create zeros
  // counter (+0x58), update preserves it.
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  auto& st = it->second;
  const std::string key = alias_key(alias);
  if (key.empty()) return;
  ParticleAction& a = st.actions[key];
  if (a.kind != ParticleAction::None && a.kind != ParticleAction::Counter)
    return;
  const bool created = (a.kind == ParticleAction::None);
  a.kind = ParticleAction::Counter;
  if (pos) vec3_get(pos, &a.px, &a.py, &a.pz);
  a.rate = r;
  if (created) a.counter = 0;
  // Soft PE payload sign split (PE malloc 0x20 @ +0x10/+0x14).
  if (r >= 0.f) {
    tree_field_set_float(self, "ps_ctr_rpos", r);
    tree_field_set_float(self, "ps_ctr_rneg", 0.f);
  } else {
    tree_field_set_float(self, "ps_ctr_rpos", 0.f);
    tree_field_set_float(self, "ps_ctr_rneg", r);
  }
  tree_field_set_int(self, "ps_actions",
                     static_cast<int32_t>(st.actions.size()));
}

int32_t java_util_resource_ParticleSystem_getCounter(InvObject* self,
                                                     InvObject* alias) {
  // PE @ 0x0047FCD0 size 0xd3 (211). Unbox this+alias. handle =
  // JVM_vm_get_int_field(this, dword_62E008); handle==0 / inner+0xC==0 →
  // return 0 (NO Mighty). *(inner+0x4C)!=1 → vtbl+0x14(1.0f);
  // ResHandle_PrepareLod(inner, 0xA0000001, 0, 0) @ 0x5447D0; sign bit →
  // 0. Else vtbl+0xC(1.0f); slot=*(eax+0x18); walk list *(slot+0x24)
  // next=+4; match Util_stricmp @ 0x5D7190 (node+0x14, alias). Hit +
  // type[+0x10]==13 → v=*(node+0x58), store 0 at +0x58 (destructive),
  // return v. Hit type≠13 or miss → 0. Does NOT read setCounter payload
  // +0x60. Soft PE: PE getCounter never reads type-11 Direct (+0x54
  // rate only) — kind!=Counter → 0 (smoke ping==75 fiction retired).
  // Missing ParticleState ≈ handle==0; consume a.counter ≈ +0x58.
  // ResHandle_PrepareLod / list walk OOS.
  if (!self) return 0;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return 0;
  const std::string key = alias_key(alias);
  if (key.empty()) return 0;
  auto jt = it->second.actions.find(key);
  if (jt == it->second.actions.end()) return 0;
  if (jt->second.kind != ParticleAction::Counter) return 0;
  const int32_t v = jt->second.counter;
  jt->second.counter = 0;
  return v;
}

void java_util_resource_ParticleSystem_delAction(InvObject* self,
                                                 InvObject* alias) {
  // PE @ 0x0047FDB0 size 0xbc (188). Unbox this+alias; handle =
  // JVM_vm_get_int_field(this, dword_62E008); null / inner+0xC → silent
  // (NO Mighty). Gate *(inner+0x4C)!=1 → vtbl+0x14(1.0f);
  // ResHandle_PrepareLod(0xA0000001,0,0) @ 0x5447D0 bit31 → silent;
  // slot=vtbl+0xC(1.0f); walk list at *(*(slot+0x18)+0x24) next=+0x4;
  // match alias Util_stricmp @ 0x5D7190 (node+0x14); hit → (**node)(node,1)
  // destroy ANY type — NO type[+0x10]==13 gate. Contrast getCounter@
  // 0x47FCD0: same walk but require type==13 then consume +0x58. Contrast
  // setCounter@0x47FA70: type-13 create/update only. Host: erase by
  // alias_key (empty alias → erase "" no-op if absent).
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  it->second.actions.erase(alias_key(alias));
  tree_field_set_int(self, "ps_actions",
                     static_cast<int32_t>(it->second.actions.size()));
}

void java_util_resource_ParticleSystem_modePermanent(InvObject* self,
                                                     int32_t permanent) {
  // PE @ 0x0047F150 size 0x94 (148). UnboxArg (I)V this+permanent.
  // handle = JVM_vm_get_int_field(this, dword_62E008). handle==0 or
  // inner=*(handle+0xC)==0 → silent ret (NO Mighty). *(inner+0x4C)==1
  // skip vtbl+0x14(1.0f). ResHandle_PrepareLod(inner, 0xA0000001, 0, 0)
  // @ 0x5447D0; TEST EAX,0x80000000 → ret. Else vtbl+0xC(1.0f);
  // obj=*(eax+0x18); if obj: permanent==1 → OR 0x20000000 at [obj+0x6C];
  // permanent==0 → AND ~0x20000000; other ints no-op. Same inner-walk as
  // setFreq @ 0x0047F1F0 / stop helper sub_48A610. Host: missing
  // ParticleState ≈ handle==0; st.permanent stands in for bit
  // (vtbl/ResHandle_PrepareLod not mirrored). Only 0/1 toggle (PE
  // dec/jnz otherwise).
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  if (permanent != 0 && permanent != 1) return;
  it->second.permanent = permanent != 0;
  tree_field_set_int(self, "ps_permanent", permanent != 0 ? 1 : 0);
}

}  // namespace inv
