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
constexpr int32_t kAnimOpPlay = 0;
constexpr int32_t kAnimOpLoopPlay = 1;
constexpr int32_t kAnimOpPause = 3;
constexpr int32_t kAnimOpSeek = 4;
constexpr int32_t kAnimOpSetSpeed = 5;
constexpr int32_t kAnimOpSetFade = 6;

void anim_sync(InvObject* self, const AnimState& a) {
  if (!self) return;
  tree_field_set_float(self, "anim_speed", a.speed);
  tree_field_set_float(self, "anim_fade", a.fade);
  tree_field_set_float(self, "anim_pos", a.pos);
  tree_field_set_int(self, "anim_playing", a.playing ? 1 : 0);
  tree_field_set_int(self, "anim_loop", a.loop ? 1 : 0);
  tree_field_set_int(self, "anim_q", static_cast<int32_t>(a.queue.size()));
}

void anim_apply_op(AnimState& a, int32_t op, float arg) {
  switch (op) {
    case kAnimOpPlay:
      a.playing = true;
      a.loop = false;
      a.last_t = time_current();
      break;
    case kAnimOpLoopPlay:
      a.playing = true;
      a.loop = true;
      a.last_t = time_current();
      break;
    case kAnimOpPause:
      a.playing = false;
      break;
    case kAnimOpSeek:
      a.pos = arg;
      if (a.pos < 0.f) a.pos = 0.f;
      a.last_t = time_current();
      break;
    case kAnimOpSetSpeed:
      a.speed = arg;
      break;
    case kAnimOpSetFade:
      a.fade = arg;
      break;
    default:
      break;
  }
}

// Host applies immediately (stand-in for the unreversed render drain).
void anim_push(InvObject* self, int32_t op, float arg) {
  if (!self) return;
  auto& a = AN(self);
  a.queue.push_back(AnimOp{op, arg});
  anim_apply_op(a, op, arg);
  anim_sync(self, a);
}

}  // namespace

void java_util_resource_Animation_init(InvObject* self, InvObject* render,
                                       InvObject* type) {
  if (!self) return;
  auto& a = AN(self);
  a = AnimState{};
  a.last_t = time_current();
  // Java field RenderRef obj (aObj); PE init @ 0x0047EA10 links its
  // Native.ptr into ctor RenderRef — host stores ctor arg as "obj".
  tree_field_set_obj(self, "obj", render);
  tree_field_set_obj(self, "type", type);
  // PE init always JVM_vm_set_int_field(Native.ptr, v1); v1 non-zero only
  // when RenderRef inner walk succeeds. Host anim_clip≈ptr.
  tree_field_set_int(self, "anim_clip", (render && type) ? 1 : 0);
  anim_sync(self, a);
}

void java_util_resource_Animation_finalize(InvObject* self) {
  // PE @ 0x0047EBA0 size 0x5c (92). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // esi = JVM_vm_get_int_field(this, dword_62E008 @ 0x0042AB50). Field "obj"
  // (aObj @ 0x6130A0 / aJavaUtilResour_114 RenderRef) via
  // JVM_vm_get_instance_field @ 0x0042A690 → RenderRef Native.ptr; gate
  // [RenderRef.ptr+8]==0 (test ecx @ 0x47EBEA) OR esi==0 (test esi @
  // 0x47EBEE) → loc_47EBFA silent (NO Mighty). Else thiscall vtbl+0(esi,1)
  // @ 0x47EBF8 (release). Register Natives_RegisterAll @ 0x487f20 xref
  // 0x488f9e. ResourceRef.id @ 0x0047D290 also reads [handle+8] — same
  // dword. Host: ResourceRef_id(obj)==0 ≈ +8 gate; anim_clip==0 ≈ esi==0;
  // clear anim_clip + g_anims.erase ≈ vtbl+0(1).
  if (!self) return;
  InvObject* obj = tree_field_get_obj(self, "obj");
  if (!obj || java_util_resource_ResourceRef_id(obj) == 0) return;
  if (tree_field_get_int(self, "anim_clip") == 0) return;
  tree_field_set_int(self, "anim_clip", 0);
  g_anims.erase(self);
}

void java_util_resource_Animation_setSpeed(InvObject* self, float speed) {
  // PE @ 0x0047EC00 size 0xbd (189). UnboxArg (F)V this+speed. esi =
  // JVM_vm_get_int_field(this, dword_62E008 @ 0x0042AB50). NO Animation
  // handle==0 test, NO Mighty. Gate: RenderRef field "obj" (aObj @
  // 0x6130A0 / aJavaUtilResour_114) Native.ptr +8 == 0 → jz loc_47ECB8
  // silent. Else fld/fstp speed RAW (no fmul/scale). edi=5; if
  // count[+0xA0]==cap[+0xA4]: cap+=0x20, realloc buf[+0x9C] via
  // sub_54F580 (high-xref, not renamed) size 8*cap. Write {5,speed}
  // stride 8. Host: ResourceRef_id(obj)==0 ≈ +8 gate (same as
  // ResourceRef.id @ 0x0047D290); anim_push apply-immediate stands in
  // for unreversed drain. NO esi==0 gate (unlike finalize @ 0x0047EBA0).
  if (!self) return;
  InvObject* obj = tree_field_get_obj(self, "obj");
  if (!obj || java_util_resource_ResourceRef_id(obj) == 0) return;
  anim_push(self, kAnimOpSetSpeed, speed);
}

void java_util_resource_Animation_setFade(InvObject* self, float fade) {
  // PE @ 0x0047ECC0 size 0xbd (189). UnboxArg (F)V this+fade. esi =
  // JVM_vm_get_int_field(this, dword_62E008 @ 0x0042AB50). NO Animation
  // handle==0 test, NO Mighty. Gate: RenderRef field "obj" (aObj @
  // 0x6130A0 / aJavaUtilResour_114) Native.ptr +8 == 0 → jz loc_47ED78
  // silent. Else fld/fstp fade RAW (no fmul/scale). edi=6; if
  // count[+0xA0]==cap[+0xA4]: cap+=0x20, realloc buf[+0x9C] via
  // sub_54F580 (high-xref, not renamed) size 8*cap. Write {6,fade}
  // stride 8. Same body as setSpeed @ 0x0047EC00 (opcode 5→6 only).
  // Host: ResourceRef_id(obj)==0 ≈ +8 gate (same as
  // ResourceRef.id @ 0x0047D290); anim_push apply-immediate stands in
  // for unreversed drain. NO esi==0 gate (unlike finalize @ 0x0047EBA0).
  if (!self) return;
  InvObject* obj = tree_field_get_obj(self, "obj");
  if (!obj || java_util_resource_ResourceRef_id(obj) == 0) return;
  anim_push(self, kAnimOpSetFade, fade);
}

void java_util_resource_Animation_play(InvObject* self) {
  anim_push(self, kAnimOpPlay, 0.f);
}

void java_util_resource_Animation_loopPlay(InvObject* self) {
  anim_push(self, kAnimOpLoopPlay, 0.f);
}

void java_util_resource_Animation_pause(InvObject* self) {
  // PE @ 0x0047EEE0 size 0xa9. UnboxArg ()V this. esi =
  // JVM_vm_get_int_field(this, dword_62E008). NO Animation handle==0
  // test, NO Mighty. Gate: RenderRef field "obj" (aObj /
  // aJavaUtilResour_114) Native.ptr +8 == 0 → jz loc_47EF87 silent.
  // Else xor ebx,ebx; edi=3; if count[+0xA0]==cap[+0xA4]: cap+=0x20,
  // realloc buf[+0x9C] via sub_54F580 (high-xref, not renamed) size
  // 8*cap. Write {3,0} stride 8. Host anim_push apply-immediate stands
  // in for unreversed drain; RenderRef+8 gate not mirrored (TREE this
  // may be collapsed).
  anim_push(self, kAnimOpPause, 0.f);
}

void java_util_resource_Animation_seek(InvObject* self, float position) {
  // PE @ 0x0047EF90 size 0xbd. UnboxArg (F)V this+pos. esi =
  // JVM_vm_get_int_field(this, dword_62E008). NO Animation handle==0
  // test, NO Mighty. Gate: RenderRef (aJavaUtilResour_114) Native.ptr
  // +8 == 0 → jz loc_47F048 silent. Else fld/fstp pos RAW (no fmul).
  // edi=4; if count[+0xA0]==cap[+0xA4]: cap+=0x20, realloc buf[+0x9C]
  // via sub_54F580 (high-xref, not renamed) size 8*cap. Write {4,pos}
  // stride 8. Host anim_push apply-immediate stands in for unreversed
  // drain; RenderRef+8 gate not mirrored (TREE this may be collapsed).
  anim_push(self, kAnimOpSeek, position);
}

float java_util_resource_Animation_getPos(InvObject* self) {
  // PE @ 0x0047F050 size 0x5f. UnboxArg ()F this. esi =
  // JVM_vm_get_int_field(this, dword_62E008). NO Animation handle==0
  // test, NO Mighty. Gate: RenderRef field "obj" (aObj /
  // aJavaUtilResour_114) Native.ptr +8 == 0 → fld flt_5E73CC (0.0) ret
  // (loc_47F0A7). Else thiscall Animation_normalizedPos(esi) @ 0x53FE70
  // size 0x71: *(handle+28) / clip duration (sub_532E50; sub_5447D0
  // high-xref, not renamed). Contrast seek @ 0x0047EF90 / play @
  // 0x0047ED80: those enqueue {4,pos}/{0,0} stride 8 via sub_54F580
  // (not renamed); getPos is READ-only — no queue write. Host: a.pos
  // (+ anim_advance) stands in for normalizedPos; RenderRef+8 gate not
  // mirrored (TREE this may be collapsed).
  if (!self) return 0.f;
  auto& a = AN(self);
  anim_advance(a);
  anim_sync(self, a);
  return a.pos;
}

// Phase 2.84 setWater / addWaterLimit: natives_gameref.cpp

void java_util_resource_GroundRef_setFog(InvObject* self, int32_t color, float near, float far) {
  // PE @ 0x00486A20 size 0x88. UnboxArg (IFF)V. handle==0 silent (NO Mighty;
  // contrast Camera_setFog @ 0x00486570). Alloc 16: byte0=1, +4/+8=near/far
  // * flt_5E7334 (10.0), +0xC=color (no mask). thiscall sub_426470
  // (ecx=dword_636338, handle, type 0x4A, pkt). Bind not mirrored.
  if (!self) return;
  const float n = near * 10.f;
  const float f = far * 10.f;
  tree_field_set_int(self, "fog_on", 1);
  tree_field_set_int(self, "fog_color", color);
  tree_field_set_float(self, "fog_near", n);
  tree_field_set_float(self, "fog_far", f);
  render_d3d9_set_fog(color & 0x00ffffff, n, f);
}

// VA 0x00486570 — Camera.setFog(IFF)V; host shares D3D fog path with GroundRef.

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
  // JVM_vm_get_int_field(this, dword_62E008 @ 0x0042AB50). handle==0 →
  // Mighty ERROR ("!" @ 0x61319C + "Mighty ERROR" @ 0x6131A0) + 0.0
  // (flt_5E73CC). inner=[handle+0xC]; inner==0 OR [inner+0x4C]!=1
  // INSTANCE_GAME AND !=3 INSTANCE_RENDER → Wrong ResourceType + 0.0.
  // [handle+8]==0 → 0.0. Else return *(float*)(inner+0x6C) LOD detail bias.
  // dword_62E008 / Engine_ErrorLogBuf not renamed. Host: ResourceRef_type/
  // id gates; tree "detail" ≈ inner+0x6C; Mighty/WrongResourceType not
  // mirrored.
  return resref_get_detail_impl(self);
}

float java_util_resource_RenderRef_getDetail(InvObject* self) {
  // PE @ 0x00480440 size 0xbb (187) — java.util.resource.RenderRef.getDetail()F;
  // same entry as GameRef.getDetail()F (Natives_RegisterAll @ 0x487f20 data
  // xrefs 0x4897bb / 0x489c17). Callees: JVM_UnboxArg @ 0x0045D910,
  // JVM_vm_get_int_field @ 0x0042AB50 (dword_62E008 Native.ptr),
  // CRT_strcat_n_thunk @ 0x00551140, Engine_ErrorLogPrintf @ 0x005513B0.
  // Unbox this. handle==0 → Mighty ERROR ("!" @ 0x61319C + "Mighty ERROR"
  // @ 0x6131A0) + flt_5E73CC (0.0). inner=[handle+0xC] (int_convert 0xC=12);
  // inner==0 OR [inner+0x4C]!=1 AND !=3 → Wrong ResourceType ("!" @
  // 0x613184 + string @ 0x613188) + 0.0. [handle+8]==0 → 0.0 silent.
  // Else fld [inner+0x6C] ret (LOD detail bias). Host: ResourceRef_type/id
  // gates; tree "detail" ≈ inner+0x6C; Mighty/WrongResourceType not mirrored.
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
  // PE @ 0x0047F120 size 0x2f. Unbox this. handle =
  // JVM_vm_get_int_field(this, dword_62E008). handle==0 → ret (NO Mighty).
  // Else JMP sub_48A610(handle) size 0x51 (11 xrefs, not renamed):
  // inner=*(handle+0xC); 0 → ret. *(inner+0x4C)==1 skip vtbl+0x14(1.0f).
  // sub_5447D0(inner, 0xA0000001, 0, 0); sign bit → ret. Else vtbl+0xC(1.0f);
  // obj=*(eax+0x18); OR 0x40000000 at [obj+0x6C]. No alias walk.
  // vs init @ 0x0047F0B0: handle/parent/type==0 silent ret (also NO Mighty),
  // else sub_48A490 binds renderinst (zeros +0x84..+0x98, [+0x80]=2, OR
  // 0x8000). stop does not unbind/zero those slots.
  // Host: missing ParticleState ≈ handle==0; ps_stopped + actions.clear
  // stand in for the stop bit (vtbl/sub_5447D0 not mirrored).
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  auto& st = it->second;
  st.stopped = true;
  st.actions.clear();
  tree_field_set_int(self, "ps_stopped", 1);
  tree_field_set_int(self, "ps_actions", 0);
}

void java_util_resource_ParticleSystem_setFreq(InvObject* self, float freq) {
  // PE @ 0x0047F1F0 size 0x7e. Unbox this+freq. handle =
  // JVM_vm_get_int_field(this, dword_62E008). handle==0 or
  // inner=*(handle+0xC)==0 → silent ret (NO Mighty). *(inner+0x4C)==1
  // skip vtbl+0x14(0x3F800000). sub_5447D0(inner, 0xA0000001, 0, 0);
  // TEST EAX,0x80000000 → ret. Else vtbl+0xC(0x3F800000);
  // obj=*(eax+0x18); if obj: fstp [obj+0xC]=freq (system-level).
  // Same inner-walk as modePermanent@0x0047F150 / stop helper
  // sub_48A610. Contrast modePermanent: toggles 0x20000000 at
  // [obj+0x6C] (permanent 0/1). Contrast setCounter@0x0047FA70:
  // sub_419860 + type-13 action list (pos/r payload), NOT [obj+0xC];
  // setSource writes per-action freq at action+0x54.
  // Host: missing ParticleState ≈ handle==0; st.freq stands in for
  // [obj+0xC] (vtbl/sub_5447D0 not mirrored).
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  it->second.freq = freq;
  tree_field_set_float(self, "ps_freq", freq);
}

void java_util_resource_ParticleSystem_setDirectSource(
    InvObject* self, InvObject* alias, InvObject* pos, float rmin, float rmax,
    InvObject* vel, float vmin, float vmax, float num, InvObject* bone) {
  if (!self) return;
  auto& st = PS(self);
  if (st.stopped) return;
  const std::string key = alias_key(alias);
  if (key.empty()) return;
  ParticleAction& a = st.actions[key];
  a.kind = ParticleAction::Direct;
  if (pos) vec3_get(pos, &a.px, &a.py, &a.pz);
  a.rmin = rmin;
  a.rmax = rmax;
  if (vel) vec3_get(vel, &a.vx, &a.vy, &a.vz);
  a.vmin = vmin;
  a.vmax = vmax;
  a.rate = num;
  a.bone = alias_key(bone);
  const int32_t n = static_cast<int32_t>(num > 0.f ? num + 0.5f : 0.f);
  a.counter += n;
  tree_field_set_int(self, "ps_actions",
                     static_cast<int32_t>(st.actions.size()));
}

void java_util_resource_ParticleSystem_setSource(
    InvObject* self, InvObject* alias, InvObject* pos, float rmin, float rmax,
    InvObject* vel, float vmin, float vmax, float freq, InvObject* bone) {
  // PE @ 0x0047F660 size 0x401. Unbox this+alias+pos+rmin/rmax+vel+vmin/vmax
  // +freq+bone. handle=JVM_vm_get_int_field(this, dword_62E008); null /
  // handle[+0xC] / sub_419860(0xA0000001,1.0,0,0) / slot=result[+0x18] →
  // silent (NO Mighty). Read Vector3 floats via unk_6130E0..F4; bone≠0 →
  // RenderRef_bindBone. Walk list *(slot+0x24) next=sub_40CFC0; match
  // sub_5D7190(node+0x14, alias). Hit + type[+0x10]==10 → replace spheres
  // at +0x58/+0x5C (free old), store freq at +0x54, bone at +0x60; NO
  // counter bump. Miss / empty list → malloc 0x68, type=10, vtbl
  // off_5F138C, +0x64=0, insert sub_489F70. Helpers sub_419860 /
  // sub_429390 / sub_5D7190 / sub_40CFC0 / sub_45F5A0 / sub_489F70 NOT
  // renamed.
  // Contrast setDirectSource@0x0047F270 size 0x3e8: same unbox/walk shape
  // but type==11, malloc 0x64, vtbl off_5F1128, insert sub_45FAF0, Vector3
  // fields unk_6130C8..DC; Java `num`→+0x54 (host counter+=n stand-in).
  // Host: missing ParticleState ≈ handle==0; kind≠Source on existing
  // alias ≈ type≠10 (no overwrite); rate=freq, no counter++.
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  auto& st = it->second;
  if (st.stopped) return;
  const std::string key = alias_key(alias);
  if (key.empty()) return;
  ParticleAction& a = st.actions[key];
  if (a.kind != ParticleAction::None && a.kind != ParticleAction::Source)
    return;
  a.kind = ParticleAction::Source;
  if (pos) vec3_get(pos, &a.px, &a.py, &a.pz);
  a.rmin = rmin;
  a.rmax = rmax;
  if (vel) vec3_get(vel, &a.vx, &a.vy, &a.vz);
  a.vmin = vmin;
  a.vmax = vmax;
  a.rate = freq;
  a.bone = alias_key(bone);
  tree_field_set_int(self, "ps_actions",
                     static_cast<int32_t>(st.actions.size()));
}

void java_util_resource_ParticleSystem_setCounter(InvObject* self,
                                                  InvObject* alias,
                                                  InvObject* pos, float r) {
  // PE @ 0x0047FA70 size 0x259. Unbox this+alias+pos+r. handle=
  // JVM_vm_get_int_field(this, dword_62E008); null / inner+0xC /
  // sub_419860(0xA0000001,1.0,0,0) / slot=result[+0x18] → silent (NO
  // Mighty). Read Vector3 via Vector3_field_x/y/z. Walk list
  // *(slot+0x18)+0x1C next=sub_40CFC0; match sub_5D7190(node+0x14,alias).
  // Hit + type[+0x10]==13 → malloc 0x20 payload (pos@+4..+C; r>=0→+0x10
  // else +0x14; sq@+0x18/+0x1C), free old [node+0x60], store new; NO
  // touch +0x54/+0x58/+0x5C. Hit type≠13 → silent. Miss/empty → malloc
  // 0x64 type=13 vtbl off_5F13A0, strncpy alias@+0x14(64), payload@+0x60,
  // zero +0xC/+0x54/+0x58/+0x5C, insert DLL. Helpers sub_419860 /
  // sub_429390 / sub_5D7190 / sub_40CFC0 / sub_429130 NOT renamed.
  // Contrast setSource@0x0047F660: type==10, list+0x24, freq@+0x54.
  // Contrast getCounter@0x0047FCD0: consume +0x58 only (not payload).
  // Host: missing ParticleState ≈ handle==0; kind≠Counter on existing
  // alias ≈ type≠13 (no overwrite); rate=r ≈ radius payload; create
  // zeros counter (+0x58), update preserves it.
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  auto& st = it->second;
  if (st.stopped) return;
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
  tree_field_set_int(self, "ps_actions",
                     static_cast<int32_t>(st.actions.size()));
}

int32_t java_util_resource_ParticleSystem_getCounter(InvObject* self,
                                                     InvObject* alias) {
  // PE @ 0x0047FCD0 size ~0x??. Unbox this+alias; type-13 action match;
  // return *(node+0x58) then clear +0x58 (destructive). Fail → 0. Does NOT
  // read setCounter payload +0x60.
  if (!self) return 0;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return 0;
  const std::string key = alias_key(alias);
  auto jt = it->second.actions.find(key);
  if (jt == it->second.actions.end()) return 0;
  const int32_t v = jt->second.counter;
  jt->second.counter = 0;
  return v;
}

void java_util_resource_ParticleSystem_delAction(InvObject* self,
                                                 InvObject* alias) {
  // PE @ 0x0047FDB0 size 0xbc. Unbox this+alias; handle =
  // JVM_vm_get_int_field(this, dword_62E008); null / inner+0xC → silent
  // (NO Mighty). Gate *(inner+0x4C)!=1 → vtbl+0x14(1.0f); sub_5447D0
  // (0xA0000001,0,0) bit31 → silent; slot=vtbl+0xC(1.0f); walk list at
  // *(*(slot+0x18)+0x24) next=+0x4; match alias sub_5D7190(node+0x14);
  // hit → (**node)(node,1) destroy ANY type — NO type[+0x10]==13 gate.
  // Contrast getCounter@0x47FCD0: same walk but require type==13 then
  // consume +0x58. Contrast setCounter@0x47FA70: type-13 create/update
  // only (sub_419860). Helpers sub_5447D0/sub_5D7190 NOT renamed.
  if (!self) return;
  auto it = g_particles.find(self);
  if (it == g_particles.end()) return;
  it->second.actions.erase(alias_key(alias));
  tree_field_set_int(self, "ps_actions",
                     static_cast<int32_t>(it->second.actions.size()));
}

void java_util_resource_ParticleSystem_modePermanent(InvObject* self,
                                                     int32_t permanent) {
  if (!self) return;
  PS(self).permanent = permanent != 0;
  tree_field_set_int(self, "ps_permanent", permanent != 0 ? 1 : 0);
}

}  // namespace inv
