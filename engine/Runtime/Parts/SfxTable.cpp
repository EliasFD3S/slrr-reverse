// Split from natives_generated_world.cpp — SfxTable.cpp
// W34-14 Soft PE: deepen getItems/clear/addItem VA-backed (IDA 2026-09-26).
// W35-14 Soft PE: residual sfx+0x48 intrusive list + vol@+0x244 (IDA 2026-09-26).
// W36-14 Soft PE: full sample slot 0x24 floats + count@+0x240 (IDA 2026-10-01).
// W37-14 Soft PE: Chassis_SfxTable_play @ 0x0048F0E0 + addItem order (IDA 2026-10-01).
// W38 Soft PE: sfxtable_soft_play → nplay @ 0x00480D40 with Vector3 pos
// (PE Chassis float* a5; Soft nplay stand-in — IDA 2026-10-04).
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

// Cluster sfx_3d_cull @ 0x00480D40 — SfxRef.nplay 3D distance + volume cull
// in runtime/Audio/Sound.cpp (sfx_3d_listener_cull = Sfx_3DListenerCull @
// 0x00550560; inner[+0x54]*vol ≤ flt_5F0C20; listener Sfx_ListenerSetPose
// @ 0x005508F0 MainLoop-only / System.cpp OOS). Thresholds py_eval:
// flt_5F3AA0=62500.0, flt_5F0CFC=16.0, flt_5F0C20=0.01. Parent: remove
// the Resources.cpp nplay stub to avoid LNK2005 (batch D move-out).
// Soft PE play table-level cull (Chassis_SfxTable_play @ 0x48F108).
int32_t sfx_3d_listener_cull(float x, float y, float z, float radius);

// PE SfxTable native blob (Chassis_SfxTable_ctor @ 0x0045DC30, stride table
// 0x248 via Chassis.getSfxTable @ 0x00442210 → base+0xF8/0x340/0x588):
//   count @ +0x240 (576); vol @ +0x244 = 1.0f (0x3F800000 @ 0x45DC56);
//   sample slots ×16 stride 0x24: +0/4 list, +8=[sfx+0x50], +0xC=sfx,
//   +0x10 = float_1_0(0x005F08F0)/pitch, +0x14..+0x20 = pmin..vmax.
// Soft PE host: g_sfxtables[InvObject*] ≈ row mirror for Chassis Rebind;
// SoftSfxTableState ≈ full blob (count/vol/slots×0x24) linked at
// native_ptr_node(sfx)+0x48. Chassis forceUpdate Rebind still copies rows
// → hdr RH only (list splice on hdr embeds OOS — dual blob vs soft).
// Play: Chassis_SfxTable_play @ 0x0048F0E0 (sub_4518C0 ×4) — rpm*vol@+0x244,
// loop embeds, crossfade, Sfx_PlayWithListenerCull(slot RH). Soft → nplay.
constexpr int32_t kSfxTableSampleCap = 16;  // addItem cmp 10h / jge @ 0x442435
constexpr float kSfxTableFloat1 = 1.f;      // float_1_0 @ 0x005F08F0
constexpr float kSfxTableFltZero = 0.f;     // g_flt_zero / flt_5E73CC

// Soft PE sample slot stride 0x24 (Win32) ≡ PE slot @ handle+count*0x24.
// +0..+0xC ≡ ResHandle (clear @ 0x442392..0x4423B9 / addItem @ 0x4424A0..).
// +0x10..+0x20 written every addItem (incl. same-sfx skip-rebind @ 0x4424E1);
// clear leaves floats stale (PE never zeros +0x10..+0x20).
struct SoftSfxSampleSlot {
  SoftSfxSampleSlot* prev = nullptr;  // slot+0x00
  SoftSfxSampleSlot* next = nullptr;  // slot+0x04
  uint32_t key = 0;                   // slot+0x08 = [sfx+0x50]
  void* instance = nullptr;           // slot+0x0C = sfx node
  float inv_pitch = 0.f;              // slot+0x10 = float_1_0 / pitch
  float pmin = 0.f;                   // slot+0x14
  float pmax = 0.f;                   // slot+0x18
  float vmin = 0.f;                   // slot+0x1C
  float vmax = 0.f;                   // slot+0x20
};
static_assert(sizeof(SoftSfxSampleSlot) == 0x24, "PE sample slot stride 0x24");

struct SoftSfxTableState {
  std::array<SoftSfxSampleSlot, 16> embeds{};
  int32_t count = 0;  // PE +0x240
  float vol = 1.f;    // PE +0x244 ctor only; natives never touch
};

std::unordered_map<InvObject*, SoftSfxTableState> g_sfxtable_soft;

SoftSfxTableState& sfxtable_soft_ref(InvObject* self) {
  // First insert default-ctors count=0 vol=1.f (PE ctor @ 0x45DC50..56).
  return g_sfxtable_soft[self];
}

// Zero ResHandle words only (+0..+0xC); floats +0x10..+0x20 stay stale.
void sfxtable_slot_zero_rh(SoftSfxSampleSlot& emb) {
  emb.prev = nullptr;
  emb.next = nullptr;
  emb.key = 0;
  emb.instance = nullptr;
}

// PE clear unlink @ 0x442392..0x4423B0 / addItem unlink @ 0x442472..0x442491
// ≡ ResHandle_Unlink @ 0x429010 (this=inst+0x44, a2=slot).
void sfxtable_slot_unlink(SoftSfxSampleSlot& emb) {
  void* old = emb.instance;
  if (emb.prev)
    emb.prev->next = emb.next;
  else if (old)
    *reinterpret_cast<void**>(static_cast<char*>(old) + 0x48) = emb.next;
  if (emb.next) emb.next->prev = emb.prev;
  sfxtable_slot_zero_rh(emb);
}

// PE addItem link @ 0x4424A0..0x4424C4 ≡ ResHandle_Link @ 0x4290F0 head-insert
// at sfx+0x48; slot+0x08=[sfx+0x50]. Sentinel esi==-0x44 → slot+0xC=0
// (lea eax,[esi+44h]; jz @ 0x4424B9) — host never fabricates that node.
void sfxtable_slot_link(void* inst, SoftSfxSampleSlot& emb) {
  if (!inst) {
    sfxtable_slot_zero_rh(emb);
    return;
  }
  auto* head =
      reinterpret_cast<void**>(static_cast<char*>(inst) + 0x44);  // +0x48 HEAD
  auto* cur = reinterpret_cast<SoftSfxSampleSlot*>(head[1]);
  if (cur) cur->prev = &emb;
  emb.prev = nullptr;
  emb.next = cur;
  head[1] = &emb;
  emb.instance = inst;
  emb.key = *reinterpret_cast<uint32_t*>(static_cast<char*>(inst) + 0x50);
}

// PE addItem @ 0x44246C: if [slot+0xC]==esi skip rebind; else unlink/link/zero.
void sfxtable_slot_relink(SoftSfxSampleSlot& emb, void* inst) {
  if (emb.instance == inst) return;
  if (emb.instance) sfxtable_slot_unlink(emb);
  if (inst)
    sfxtable_slot_link(inst, emb);
  else
    sfxtable_slot_zero_rh(emb);
}

// PE [ResourceRef+0x0C] @ 0x442464 — host: native_ptr_node when Native.ptr live
// (no ensure — do not invent). Miss → nullptr ≈ esi==0 zero-slot path.
void* sfxtable_sfx_instance(InvObject* sfx) {
  if (!sfx || !native_ptr_get(sfx)) return nullptr;
  return native_ptr_node(sfx);
}

void sfxtable_soft_unlink_rows(InvObject* self, size_t n) {
  auto sit = g_sfxtable_soft.find(self);
  if (sit == g_sfxtable_soft.end()) return;
  SoftSfxTableState& st = sit->second;
  if (n > st.embeds.size()) n = st.embeds.size();
  for (size_t i = 0; i < n; ++i) {
    SoftSfxSampleSlot& emb = st.embeds[i];
    if (emb.instance)
      sfxtable_slot_unlink(emb);
    else
      emb.key = 0;  // PE clear @ 0x4423BD: only slot+0x08=0 when sfx==0
    // floats +0x10..+0x20 left stale (PE clear never writes them)
  }
}

void java_game_parts_SfxTable_finalize(InvObject* self) {
  // Soft GC stand-in — no SfxTable.finalize in stock RegisterNative.
  // Soft related: Animation.finalize PE @ 0x0047EBA0 / String.finalize
  // PE @ 0x00486190; Object_FinalizeFree @ 0x00408560.
  if (!self) return;
  auto it = g_sfxtables.find(self);
  auto sit = g_sfxtable_soft.find(self);
  const size_t n =
      (sit != g_sfxtable_soft.end())
          ? static_cast<size_t>(sit->second.count)
          : ((it == g_sfxtables.end()) ? 0 : it->second.size());
  sfxtable_soft_unlink_rows(self, n);
  g_sfxtable_soft.erase(self);
  g_sfxtables.erase(self);
}

int32_t java_game_parts_SfxTable_getItems(InvObject* self) {
  // PE @ 0x00442310 size 0x31 (49). IDA java_game_parts_SfxTable_getItems.
  // Callees: JVM_UnboxArg @ 0x0045D910 (sig ()I dest0),
  // JVM_vm_get_int_field (dword_62E008 / Native.ptr) @ 0x0042AB50.
  // Xref: Natives_Register_Partial data @ 0x00442A52. Handle==0 →
  // xor eax,eax ret 0 (NO Mighty). Else mov eax,[eax+0x240] (576)
  // item count; retn. clear @ 0x00442350 zeros +0x240; addItem @
  // 0x004423E0 incs same dword (jge 16 only there). NO cmp 16 here.
  // Soft PE W36-14: SoftSfxTableState.count ≈ [+0x240]; map-miss soft +
  // vector miss ≈ handle==0 → 0; else vector.size() mirror before soft.
  if (!self) return 0;
  auto sit = g_sfxtable_soft.find(self);
  if (sit != g_sfxtable_soft.end()) return sit->second.count;
  auto it = g_sfxtables.find(self);
  if (it == g_sfxtables.end()) return 0;
  return static_cast<int32_t>(it->second.size());
}

void java_game_parts_SfxTable_clear(InvObject* self) {
  // PE @ 0x00442350 size 0x88 (136). IDA java_game_parts_SfxTable_clear.
  // Callees: JVM_UnboxArg @ 0x0045D910 (sig ()V dest0),
  // JVM_vm_get_int_field (dword_62E008 / Native.ptr) @ 0x0042AB50.
  // Xref: Natives_Register_Partial data @ 0x00442A71. Handle==0 → jz ret
  // (NO Mighty). Loop edi=0..[handle+0x240]-1 (count at +576, slot cursor
  // starts handle+4 = slot0+0x04, stride +0x24): esi=[ecx+8] (sfx @
  // slot+0x0C). esi!=0 → unlink sfx+0x48 intrusive list via
  // [ecx-4]/[ecx]/[ecx+4], then zero [ecx-4],[ecx],[ecx+4],[ecx+8];
  // else only [ecx+4]=0 (slot+0x08). Floats +0x10..+0x20 left stale until
  // next addItem overwrites. End: [handle+0x240]=0. NO cmp 16.
  // Soft PE W36-14: unlink SoftSfxSampleSlot×n RH words; floats stale;
  // soft.count=0; vector.clear(); vol@+0x244 untouched.
  if (!self) return;
  auto it = g_sfxtables.find(self);
  if (it == g_sfxtables.end()) return;
  auto sit = g_sfxtable_soft.find(self);
  // PE loop bound = [handle+0x240]; soft.count primary, else vector mirror.
  const size_t n =
      (sit != g_sfxtable_soft.end())
          ? static_cast<size_t>(sit->second.count)
          : it->second.size();
  sfxtable_soft_unlink_rows(self, n);
  if (sit != g_sfxtable_soft.end()) sit->second.count = 0;
  it->second.clear();
}

void java_game_parts_SfxTable_addItem(InvObject* self, InvObject* sfx,
                                     float pitch, float pmin, float pmax,
                                     float vmin, float vmax) {
  // PE @ 0x004423E0 size 0x120 (288). Unbox this+ResourceRef+FFFFF
  // (sig (Ljava.util.resource.ResourceRef;FFFFF)V dest0..6) via
  // JVM_UnboxArg @ 0x0045D910. Native.ptr = JVM_vm_get_int_field
  // (dword_62E008) @ 0x0042AB50. Handle==0 → jz ret (NO Mighty).
  // Count=[handle+0x240]; cmp 10h / jge no-op. Else slot=handle+count*0x24
  // (lea edx+edx*8,*4), then inc count into +0x240 FIRST (@ 0x442456..5e).
  // esi=[ResourceRef+0x0C]; if slot+0x0C!=esi: unlink/link ≡ ResHandle_Rebind
  // @ 0x429060 (inline; sentinel esi==-0x44 → slot+0xC=0). Same-sfx → skip
  // rebind. Then FPU @ 0x4424E1: float_1_0 fdiv pitch → +0x10; +0x14..+0x20
  // = pmin,pmax,vmin,vmax (always, even same-sfx). Cap 16 here only.
  // pitch==0 → Inf (PE fdiv). Soft PE W37-14: soft.count inc before floats
  // (PE order); SoftSfxSampleSlot full 0x24; vol@+0x244 ctor-only; rows
  // mirror for Chassis hdr Rebind + soft play nplay (list splice OOS).
  if (!self) return;
  auto it = g_sfxtables.find(self);
  if (it == g_sfxtables.end()) return;  // ≈ handle==0
  auto& rows = it->second;
  SoftSfxTableState& st = sfxtable_soft_ref(self);
  if (st.count >= kSfxTableSampleCap) return;  // cmp 10h / jge @ 0x442435
  const size_t idx = static_cast<size_t>(st.count);
  SoftSfxSampleSlot& slot = st.embeds[idx];
  ++st.count;  // PE @ 0x44245e before RH / floats
  void* inst = sfxtable_sfx_instance(sfx);
  sfxtable_slot_relink(slot, inst);  // ≈ ResHandle_Rebind inline
  // PE @ 0x4424E1..0x4424F8 — always, even when same-sfx skipped rebind.
  slot.inv_pitch = kSfxTableFloat1 / pitch;  // float_1_0 / pitch
  slot.pmin = pmin;
  slot.pmax = pmax;
  slot.vmin = vmin;
  slot.vmax = vmax;
  SfxItem item;
  item.sfx = sfx;
  item.pitch = slot.inv_pitch;
  item.pmin = pmin;
  item.pmax = pmax;
  item.vmin = vmin;
  item.vmax = vmax;
  rows.push_back(item);
}

// Soft PE Chassis_SfxTable_play @ 0x0048F0E0 size 0x177 (375). IDA rename
// from sub_48F0E0. thiscall this=SfxTable blob. Args: rpm, vol_scale,
// flags, pos_xyz|0, instance, radius, a8, a9 (retn 0x20). Xrefs×4 from
// sub_4518C0 @ 0x451A3B/0x451CD8/0x451D93/0x451F66 (tables +0xF8/+0x340/
// +0x588). rpm_eff = rpm * [this+0x244] (table vol, ctor 1.0f). If pos!=0:
// Sfx_3DListenerCull(pos,radius)<0 → ret. Loop i=0..count-1 (@+0x240) or
// until slot+0x0C==0: if rpm_eff in [pmin,pmax] (@+0x14/+0x18):
//   t=(rpm_eff-pmin)/(pmax-pmin); vol_lerp=(1-t)*vmin+t*vmax;
//   w=1; if i>0 && rpm_eff<=prev.pmax && (prev.pmax-pmin)>0:
//     w=(rpm_eff-pmin)/(prev.pmax-pmin); if i<count-1 && rpm_eff>=next.pmin
//     && (pmax-next.pmin)>0: w=1-(rpm_eff-next.pmin)/(pmax-next.pmin);
//   if w>0: Sfx_PlayWithListenerCull(this=slot RH, pitch=rpm_eff*inv_pitch,
//     vol=w*vol_lerp*vol_scale, flags, pos=a5, instance, radius, a8, a9).
// Soft: soft embeds + rows[i].sfx → SfxRef.nplay @ 0x00480D40 (PE plays
// ResHandle embed directly — Soft no DS RH). nplay then:
//   vol*=Sound_volumeEffects @ 0x612C58; pos null→flags|=4; else stack xyz
//   → Sfx_PlayWithListenerCull @ 0x0048CFB0 (2nd cull). Soft a8/a9=0 always
//   (nplay pushes 0,0 — PE Chassis can pass non-null; Chassis wire OOS).
void sfxtable_soft_play(InvObject* self, float rpm, float vol_scale,
                        int32_t flags, const float* pos_xyz, int32_t instance,
                        float radius) {
  if (!self) return;
  auto sit = g_sfxtable_soft.find(self);
  if (sit == g_sfxtable_soft.end()) return;  // ≈ this==0
  SoftSfxTableState& st = sit->second;
  // PE @ 0x48F0FB: rpm_eff = rpm * vol@+0x244
  const float rpm_eff = rpm * st.vol;
  // PE @ 0x48F0F9..0x48F112: pos!=0 → Sfx_3DListenerCull; <0 ret
  if (pos_xyz != nullptr) {
    if (sfx_3d_listener_cull(pos_xyz[0], pos_xyz[1], pos_xyz[2], radius) < 0)
      return;
  }
  // Soft bridge: PE float* a5 → nplay Vector3 (JVM_vm_get_float_field x/y/z
  // @ 0x480D9A..0x480DC1). Reuse one host Vector3; null a5 → nplay nullptr
  // → PlayWithListenerCull flags|=4 @ 0x48D0AA.
  InvObject* pos_obj = nullptr;
  if (pos_xyz != nullptr) {
    static InvObject* s_nplay_pos = nullptr;
    if (!s_nplay_pos) s_nplay_pos = vec3_new(0.f, 0.f, 0.f);
    vec3_set(s_nplay_pos, pos_xyz[0], pos_xyz[1], pos_xyz[2]);
    tree_field_set_float(s_nplay_pos, "x", pos_xyz[0]);
    tree_field_set_float(s_nplay_pos, "y", pos_xyz[1]);
    tree_field_set_float(s_nplay_pos, "z", pos_xyz[2]);
    pos_obj = s_nplay_pos;
  }
  auto rit = g_sfxtables.find(self);
  const std::vector<SfxItem>* rows =
      (rit != g_sfxtables.end()) ? &rit->second : nullptr;
  const int32_t n = st.count;
  for (int32_t i = 0; i < n && i < kSfxTableSampleCap; ++i) {
    SoftSfxSampleSlot& slot = st.embeds[static_cast<size_t>(i)];
    // PE @ 0x48F132: break if slot+0x0C==0 (instance)
    if (!slot.instance) break;
    // PE @ 0x48F13D..0x48F158: rpm_eff in [pmin, pmax]
    if (rpm_eff < slot.pmin || rpm_eff > slot.pmax) continue;
    // PE @ 0x48F16F..0x48F174: fdivr (pmax-pmin) — zero span → Inf.
    const float t = (rpm_eff - slot.pmin) / (slot.pmax - slot.pmin);
    const float vol_lerp =
        (kSfxTableFloat1 - t) * slot.vmin + t * slot.vmax;
    float w = kSfxTableFloat1;
    // PE @ 0x48F18C..0x48F1B5: i>0 && rpm_eff <= prev.pmax → fade-in
    if (i > 0 && rpm_eff <= st.embeds[static_cast<size_t>(i - 1)].pmax) {
      const float prev_pmax = st.embeds[static_cast<size_t>(i - 1)].pmax;
      const float din = prev_pmax - slot.pmin;
      if (din > kSfxTableFltZero) w = (rpm_eff - slot.pmin) / din;
    }
    // PE @ 0x48F1BA..0x48F1F6: i<count-1 && rpm_eff >= next.pmin → fade-out
    if (i < n - 1 &&
        rpm_eff >= st.embeds[static_cast<size_t>(i + 1)].pmin) {
      const float next_pmin = st.embeds[static_cast<size_t>(i + 1)].pmin;
      const float dout = slot.pmax - next_pmin;
      if (dout > kSfxTableFltZero)
        w = kSfxTableFloat1 - (rpm_eff - next_pmin) / dout;
    }
    // PE @ 0x48F1FB: w <= 0 → skip (fcomp g_flt_zero / test ah,41h)
    if (!(w > kSfxTableFltZero)) continue;
    const float play_vol = w * vol_lerp * vol_scale;
    const float play_pitch = rpm_eff * slot.inv_pitch;
    // Soft: InvObject from row mirror → nplay @ 0x480D40 (PE: PlayWithListenerCull
    // on slot RH @ 0x48F23A — Soft no DS RH).
    InvObject* sfx_obj = nullptr;
    if (rows && static_cast<size_t>(i) < rows->size())
      sfx_obj = (*rows)[static_cast<size_t>(i)].sfx;
    if (!sfx_obj) continue;
    (void)java_util_resource_SfxRef_nplay(sfx_obj, pos_obj, radius, play_pitch,
                                          play_vol, flags, instance);
  }
}

}  // namespace inv
