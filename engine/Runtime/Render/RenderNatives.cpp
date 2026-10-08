// Split from natives_generated_world.cpp — RenderNatives.cpp
#include "natives.hpp"
#include "host_objects.hpp"
#include "Resources.h"
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

namespace inv {

// PE ResourceEngine_GCSweep @ 0x537ED0 — host body in Resources.cpp;
// not yet exported from Resources.h (flush @ 0x47C319 is the a2=1 caller).
extern int host_resource_engine_gcsweep(int a2);

namespace {

// Soft PE ≡ GfxEngine_skipBeginScene @ 0x0065AECC (was dword_65AECC).
// PresentFrame @ 0x4FCAB4: if 0 → device vt+24 BeginScene; then always
// writes 0 @ 0x4FCAC1. forceRendering@47C1F2 / flush@47C2F2 set 0 before
// each PresentFrame. sub_50E9E0 @ 0x50E9FB may set 1 (skip BeginScene).
// Host Present stand-in has no BeginScene gate — soft clear only.
int g_GfxEngine_skipBeginScene = 0;

// Soft PE Present branch (Engine_MainLoop @ 0x428CE8):
//   GfxEngine_AltPresentGate @ 0x4F9760 → AltPresent @ 0x4F9550
//   else GfxEngine_PresentFrame @ 0x4FCA30.
// PE forceRendering@47C1D0 / flush@47C2D0 always PresentFrame (no gate);
// host soft applies the MainLoop Present choice to their Present slot.
// Gate true: exclusive HWND FMV — soft AltPresent = video_fmv_present
// (EC pump) + Frontend.notify. Full AltPresent D3D TSS/Draw OOS.
void gfx_engine_present_frame_or_alt() {
  // PE @ 0x47C1F2 / 0x47C2F2: GfxEngine_skipBeginScene = 0 before Present.
  g_GfxEngine_skipBeginScene = 0;
  if (video_fmv_alt_present_gate() != 0) {
    video_fmv_present();
    frontend_gfx_engine_frame_notify();
  } else {
    render_d3d9_flush();
  }
}

// Soft PE Engine_ViewportCreate @ 0x538D60: type dword 18 (=0x12) on
// CreateNode desc; GfxEngine_ViewportBind @ 0x4FD020 gates *(node+0x4C)==0x12.
constexpr int32_t kViewportNodeType = 0x12;  // int_convert 18

void viewport_soft_seed_native(InvObject* self, int32_t pri, float x, float y,
                               float w, float h) {
  // Soft PE: Native.ptr + type-18 node (Engine_ViewportCreate / bind gate).
  // Full ResHandle_Link + params rect at vtbl+0xC OOS — TREE + D3D map.
  if (!self) return;
  if (!native_ptr_get(self)) native_ptr_ensure(self);
  if (void* node = native_ptr_node(self)) {
    *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x4C) =
        kViewportNodeType;
  }
  tree_field_set_int(self, "pri", pri);
  // PE params: +0xC=pri, +0x10=x, +0x14=y, +0x18=w, +0x1C=h (getWidth walk).
  tree_field_set_float(self, "vp_x", x);
  tree_field_set_float(self, "vp_y", y);
  tree_field_set_float(self, "vp_w", w);
  tree_field_set_float(self, "vp_h", h);
}

// Soft PE mesh_world_pose / setMatrix hierarchy (PATH-TO-WORLD):
//   RenderRef.setMatrix 4-arg @ 0x004810B0 → SetBoneMatrixParentLink
//   @ 0x0048BF50 (Ypr_toMatrix + Mat3x4_setBasis_posScaled10 *flt_5F37A0=10
//   → bone+0x54; bone+0xF0=1; identity → bone+0x3C|=2 else &=~2; HEAD
//   payload+0x178; payload+0xBC|=0x1800) → always LinkOrUnlinkBone
//   @ 0x0048BE10. 2-arg @ 0x00481240 = bindBone("bone00") + same body
//   with bone_ref=0. Host draw already World=Local*BoneLocal*ParentWorld
//   (render_d3d9 resolve_world); Resources owns the JNI body — soft TREE
//   publish of composed origin + parent/attach for readers here.
void render_ref_mesh_world_pose_soft_publish(InvObject* self) {
  if (!self) return;
  float wx = 0.f, wy = 0.f, wz = 0.f;
  render_d3d9_mesh_world_origin(self, &wx, &wy, &wz);
  tree_field_set_float(self, "world_px", wx);
  tree_field_set_float(self, "world_py", wy);
  tree_field_set_float(self, "world_pz", wz);
  tree_field_set_int(self, "mesh_world_posed", 1);
  auto* parent =
      reinterpret_cast<InvObject*>(render_d3d9_mesh_get_parent(self));
  tree_field_set_obj(self, "mesh_parent", parent);
  tree_field_set_int(self, "attach_bone",
                     render_d3d9_mesh_get_attach_bone(self));
}

}  // namespace

// Soft PE RenderRef_applyLight @ 0x48C9D0: BYTE * 0.00390625 (1/256) → unit.
// setLight@486AB0 defaults diff/spec=0xFFFFFF amb=0x404040; dir a5=null.
// PE getPayload tag 0x80000001 → Payload[+0x10]diff/[+0x20]spec/[+0x30]amb.
// Soft: TREE unit floats (host mid ≠ light layout; tag 0x80000001 OOS).
namespace {
constexpr float kLightByteToUnit = 1.f / 256.f;
float light_byte_to_unit(int32_t packed, int shift) {
  return static_cast<float>((packed >> shift) & 0xFF) * kLightByteToUnit;
}
}  // namespace

void render_ref_apply_light_soft(InvObject* self, int32_t diffuse,
                                 int32_t ambient, int32_t specular) {
  if (!self) return;
  if (!native_ptr_get(self)) native_ptr_ensure(self);
  // PE: node=*(handle+0xC); 0 → no-op (no Mighty).
  if (!native_ptr_node(self)) return;
  tree_field_set_int(self, "light_diffuse", diffuse);
  tree_field_set_int(self, "light_ambient", ambient);
  tree_field_set_int(self, "light_specular", specular);
  // PE BYTE2=R, BYTE1=G, BYTE0=B.
  tree_field_set_float(self, "light_diff_r", light_byte_to_unit(diffuse, 16));
  tree_field_set_float(self, "light_diff_g", light_byte_to_unit(diffuse, 8));
  tree_field_set_float(self, "light_diff_b", light_byte_to_unit(diffuse, 0));
  tree_field_set_float(self, "light_amb_r", light_byte_to_unit(ambient, 16));
  tree_field_set_float(self, "light_amb_g", light_byte_to_unit(ambient, 8));
  tree_field_set_float(self, "light_amb_b", light_byte_to_unit(ambient, 0));
  tree_field_set_float(self, "light_spec_r", light_byte_to_unit(specular, 16));
  tree_field_set_float(self, "light_spec_g", light_byte_to_unit(specular, 8));
  tree_field_set_float(self, "light_spec_b", light_byte_to_unit(specular, 0));
  tree_field_set_int(self, "light_applied", 1);
}

// Soft PE RenderRef_applyFlare @ 0x48CB40: getPayload → Rebind(+0xC4 glow RH)
// → store +0xD4 min / +0xD8 max / +0xE0 color / +0xE4 rays / +0x100 count.
// Soft: TREE stamp; ResHandle_ctorFromObj + Rebind dllist splice OOS.
void render_ref_apply_flare_soft(InvObject* self, InvObject* glowtexture,
                                 int32_t glowColor, float glowMinSize,
                                 float glowMaxSize, int32_t flareCount,
                                 int32_t rayCount) {
  if (!self) return;
  if (!native_ptr_get(self)) native_ptr_ensure(self);
  if (!native_ptr_node(self)) return;
  const int32_t tex_id =
      glowtexture ? java_util_resource_ResourceRef_id(glowtexture) : 0;
  tree_field_set_int(self, "flare_tex_id", tex_id);
  tree_field_set_int(self, "flare_color", glowColor);
  tree_field_set_float(self, "flare_min", glowMinSize);
  tree_field_set_float(self, "flare_max", glowMaxSize);
  tree_field_set_int(self, "flare_count", flareCount);
  tree_field_set_int(self, "flare_rays", rayCount);
  if (glowtexture) tree_field_set_obj(self, "flare_tex", glowtexture);
  tree_field_set_int(self, "flare_applied", 1);
}

void java_render_Camera_create(InvObject* self, InvObject* parent, InvObject* vp, int32_t pri, float aov, float dmin, float dmax, float lodBias, float lodAmp, int32_t oc, int32_t pt) {
  // PE @ 0x004861E0 size 0x243 (int_convert 579). UnboxArg
  // (Ljava.util.resource.ResourceRef;Ljava.render.Viewport;IFFFFFII)V:
  // this, parent, vp, pri, aov, dmin, dmax, lodBias, lodAmp, oc, pt.
  // Camera.java ctor passes aov*0.5 (half-angle deg) into this native.
  // handle = JVM_vm_get_int_field(this, Native_ptr_field_id @ 0x62E008).
  // No Mighty ERROR. Default bone Vector3(0,0,3.0=0x40400000) +
  // Mat3x4_setIdentity @ 0x54E1D0. vpNode =
  // GfxEngine_findViewportByHandle @ 0x4FD220 (g_GfxEngine, vp).
  // FPU: fld[+0x74]/fld[+0x78] → fdiv w/h → fdivr 1.0 → a2=h/w.
  // ResourceEngine_type_rendertype_camera @ 0x539500 (&dword_636450,
  //   a2=h/w, aov, dmin, dmax, lodBias, lodAmp, oc):
  //   params+0xC = 1/a2 = w/h; +0x10=aov; +0x14/+0x18 = dmin/dmax*10;
  //   +0x30=oc; +0x34=lodBias; +0x38=lodAmp.
  // Then vtbl+0xC(cam,1.0f@0x3F800000); *(params+0x2C)=pt.
  // Temp list-stub on cam[+0x48/+0x50]; renderinst =
  // ResourceEngine_type_renderinst @ 0x539880 (parent, stub,
  // "s_renderref_camera", 0); relink handle onto renderinst list.
  // RenderRef_bindBone("bone00") @ 0x48BC40 + setBoneLocal @ 0x48C0F0.
  // GfxEngine_hookCameraViewport @ 0x4FCF10 → ViewportHookCamera
  // @ 0x516760; GfxEngine_registerCameraPri @ 0x4FD150 →
  // ViewportInsertCameraPri @ 0x516900 (pri, -1). Unlink stub.
  // Register @ Natives_RegisterAll 0x488CB6.
  // Soft: RenderRef_create + Native.ptr + TREE frustum (aspect/aov/
  // dmin*10/dmax*10/lod/oc/pt/pri) + bone00@(0,0,3) + D3D store +
  // type-18 vp gate for hook/register stand-in (pri list OOS).
  // OOS: ResourceEngine factory node, s_renderref_camera list link,
  // GfxEngine dllist hook/pri insert bodies.
  if (vp) {
    if (render_d3d9_viewport_get_width(vp) <= 0.f &&
        render_d3d9_viewport_get_height(vp) <= 0.f) {
      // Soft empty-vp seed before aspect (PE findViewportByHandle needs
      // an existing type-18 vp; create does not call Viewport.create).
      viewport_soft_seed_native(vp, 0, 0.f, 0.f, 1.f, 1.f);
      render_d3d9_viewport_create(vp, 0, 0.f, 0.f, 1.f, 1.f);
    }
  }
  // PE params+0xC = w/h after factory 1/(h/w). Soft from vp getters.
  float aspect = 1.f;
  if (vp) {
    const float vw = render_d3d9_viewport_get_width(vp);
    const float vh = render_d3d9_viewport_get_height(vp);
    if (vh > 1e-6f) aspect = vw / vh;
  }
  if (self) {
    java_util_resource_RenderRef_create(self, parent, nullptr, nullptr);
    // PE: Native.ptr already on ResourceRef; create relinks handle+0xC.
    // Host: allocate ResHandle box + camera/fog node chain (setFog dual hop).
    native_ptr_ensure(self);
    // Soft ≡ factory params write @ 0x5395DD.. + create pt @ 0x4862C5.
    tree_field_set_float(self, "cam_aspect", aspect);       // +0xC
    tree_field_set_float(self, "half_aov", aov);            // +0x10
    tree_field_set_float(self, "dmin", dmin);
    tree_field_set_float(self, "dmax", dmax);
    tree_field_set_float(self, "dmin_x10", dmin * 10.f);    // +0x14
    tree_field_set_float(self, "dmax_x10", dmax * 10.f);    // +0x18
    tree_field_set_float(self, "lod_bias", lodBias);        // +0x34
    tree_field_set_float(self, "lod_amp", lodAmp);          // +0x38
    tree_field_set_int(self, "oc", oc);                     // +0x30
    tree_field_set_int(self, "pt", pt);                     // +0x2C
    // PE RenderRef_bindBone("bone00") + setBoneLocal @ 0x48C0F0 (0,0,3) +
    // Mtx3x4_id — same matrix write path as SetBoneMatrixParentLink
    // @ 0x48BF50 (setMatrix @ 0x4810B0 / 2-arg @ 0x481240). RenderRef_create
    // already mesh_set_parent(parent). Soft deepen: TREE |=0x1800 + world
    // pose publish (World=Local*ParentWorld).
    const int32_t bone = render_d3d9_mesh_get_bone_id(self, "bone00");
    render_d3d9_mesh_set_bone_local(self, bone, 0.f, 0.f, 3.f, 0.f, 0.f, 0.f);
    tree_field_set_int(self, "applied_bone_id", bone);
    tree_field_set_int(self, "bone_link_flags",
                       tree_field_get_int(self, "bone_link_flags") | 0x1800);
    render_ref_mesh_world_pose_soft_publish(self);
  }
  render_d3d9_camera_create(self, parent, vp, pri, aov, dmin, dmax, lodBias,
                            lodAmp, oc, pt);
  // Soft ≡ hookCameraViewport @ 0x4FCF10 + registerCameraPri @ 0x4FD150
  // (type-18 gate; create does NOT set cam_active — that is orphan
  // activate @ 0x486470). Pri dllist InsertCameraPri @ 0x516900 OOS.
  if (self && vp && native_ptr_get(self) && native_ptr_get(vp)) {
    if (void* node = native_ptr_node(vp)) {
      if (*reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x4C) ==
          kViewportNodeType) {
        tree_field_set_obj(self, "cam_vp", vp);
        tree_field_set_int(self, "cam_pri", pri);
      }
    }
  }
}

void java_render_Camera_destroy(InvObject* self) {
  // PE @ 0x004864C0 size 0xae (174). UnboxArg ()V: this only.
  // handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → Mighty ERROR ("!" + "Mighty ERROR" via Engine_strcat_cap
  // + Engine_ErrorLogMsgBox on Engine_ErrorLogBuf @ 0x0062E018); buf[0]=0.
  // Else: node=*(handle+0xC); if node: thiscall ResHandle_getPayload @
  // 0x00419860 (node, 0xA0000000, 1.0f @ 0x3F800000, 0.f, 0.f) — same type
  // tag as setFog @ 0x00486570 (vtable+0x14 / sub_5447D0 / vtable+0xC). If
  // result && (*(result+0x80) & 0xFFFF0000)==0xFFFF0000: thiscall
  // sub_48A8D0(result+0x78). Always thiscall sub_48A8D0(handle) (size 0x1c:
  // *(this+8)&&*(this+0xC) → sub_427620). No D3D in this native. race125
  // Always thiscall sub_48A8D0(handle). Host: drop Native.ptr + D3D.
  native_ptr_clear(self);
  render_d3d9_camera_destroy(self);
}

void java_render_Camera_activate(InvObject* self, InvObject* vp, int32_t pri) {
  // PE @ 0x00486470 size 0x50 (int_convert 80). UnboxArg
  // (Ljava.render.Viewport;I)V: this, vp, pri. handle =
  // JVM_vm_get_int_field(this, dword_62E008). handle==0 → silent return
  // (no Mighty ERROR). Else GfxEngine_registerCameraPri @ 0x4FD150
  // (ecx=g_GfxEngine/off_6187B0, vp_handle, cam_handle, pri, -1).
  // registerCameraPri: *(vp+0xC) && *(node+0x4C)==0x12 →
  // GfxEngine_ViewportInsertCameraPri @ 0x516900. Contrast create @
  // 0x4861E0 which also calls GfxEngine_hookCameraViewport @ 0x4FCF10
  // → GfxEngine_ViewportHookCamera @ 0x516760 before register.
  // Orphan body — NOT in Natives_RegisterAll (stock table only
  // create@4861E0 / destroy@4864C0 / setFog@486570); Java Camera.java
  // still declares native activate. Soft: Native.ptr + type-18 vp gate +
  // TREE cam_pri/active + D3D (pri list OOS).
  if (!self) return;
  if (!native_ptr_get(self)) return;  // soft ≡ handle 0
  if (!vp || !native_ptr_get(vp)) return;
  if (void* node = native_ptr_node(vp)) {
    if (*reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x4C) !=
        kViewportNodeType)
      return;
  } else {
    return;
  }
  tree_field_set_int(self, "cam_pri", pri);
  tree_field_set_int(self, "cam_active", 1);
  render_d3d9_camera_activate(self, vp, pri);
}

void java_render_Camera_deactivate(InvObject* self, InvObject* vp) {
  // PE @ 0x00486430 size 0x3f (int_convert 63). UnboxArg
  // (Ljava.render.Viewport;)V: this, vp. handle = Native.ptr dword_62E008.
  // handle==0 → silent return (no Mighty ERROR). Else
  // GfxEngine_unregisterCameraPri @ 0x4FD1A0 (ecx=g_GfxEngine, vp, handle):
  // *(vp+0xC) && *(node+0x4C)==0x12 → GfxEngine_ViewportRemoveCameraPri
  // @ 0x516A10 remove cam from vp list.
  // Inverse of activate@486470 / registerCameraPri@4FD150. Orphan body —
  // NOT in Natives_RegisterAll. Soft: Native.ptr + type-18 vp gate + TREE
  // + D3D (list unlink OOS).
  if (!self) return;
  if (!native_ptr_get(self)) return;
  if (!vp || !native_ptr_get(vp)) return;
  if (void* node = native_ptr_node(vp)) {
    if (*reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x4C) !=
        kViewportNodeType)
      return;
  } else {
    return;
  }
  tree_field_set_int(self, "cam_active", 0);
  render_d3d9_camera_deactivate(self, vp);
}

void java_render_GfxEngine_forceRendering() {
  // PE @ 0x0047C1D0 size 0x43 (67): static ()V, no UnboxArg, no this.
  // esi=7: ResourceEngine_PumpLoadQueue(off_618D48) @ 0x5378D0 +
  // ResourceEngine_PumpUnloadQueue @ 0x537B40;
  // GfxEngine_skipBeginScene@65AECC=0; GfxEngine_PresentFrame@4FCA30;
  // ++g_Engine_frameStamp@6200A4. Same pump as flush@47C2D0 minus tail
  // (NO GCSweep / DrainGpuCaches). Engine_MainLoop 1×/frame @ 0x428CC9.
  // Soft: PumpUnload already ++g_res_gc_frame (≡ frameStamp); Present slot
  // = AltPresentGate@4F9760 else PresentFrame (MainLoop soft, not PE).
  for (int i = 7; i != 0; --i) {
    (void)resource_engine_pump_load_queue();
    (void)resource_engine_pump_unload_queue();
    gfx_engine_present_frame_or_alt();
  }
}

void java_render_GfxEngine_setGlobalEnvmap(InvObject* texture) {
  // PE @ 0x0047C220 size 0xA6 (int_convert 166) static
  // (Ljava.util.resource.ResourceRef;)V. Only callee JVM_UnboxArg @
  // 0x0045D910: dest0=nullptr (no this), dest1=&arg0 in-place. arg0 :=
  // Native.ptr 16-byte box (not jobject). handle := *(box+0xC) engine
  // resource. Node at g_GfxEngine/off_6187B0+0x58 (dwords +0/+4/+8/+0xC
  // = +0x58/+0x5C/+0x60/+0x64). Same handle → no-op. Else: if current
  // (+0x64) nonzero unlink via resource+0x48/+0x50 then zero node; if
  // new nonzero relink (same inline as ResourceRef.set @ 0x0047CFC0);
  // handle 0 clears node. No D3D/SetTexture. Register @ 0x488C1B.
  // race125 deepen VERIFY: InvObject* stand-in for handle; g_envmap store
  // only. PE: UnboxArg dest0=nullptr → arg0 is Native.ptr box; compare
  // *(g_GfxEngine+0x64) vs *(box+0xC); unlink/relink @ +0x48/+0x50.
  render_d3d9_set_global_envmap(texture);
}

int32_t java_render_GfxEngine_numDisplayModes() {
  // PE @ 0x0047C440 size 0x5 (int_convert 5) static ()I. No UnboxArg /
  // this, no Mighty ERROR. Bytes: E9 AB DA 03 00 = jmp rel →
  // Gfx_GetNumDisplayModes @ 0x004B9EF0 size 0x6 (int_convert 6). That
  // getter: A1 F4 95 64 00 / C3 = mov eax, dword_6495F4; retn. Sole
  // caller of Gfx_GetNumDisplayModes; do not rename dword_6495F4. Count
  // filled by GfxDevice enum sub_4B86B0 @ 0x004B86B0 size 0x2AA: thiscall
  // on device *(g_GfxEngine+0x204); IDirect3D9 vt+24 GetAdapterModeCount,
  // vt+28 EnumAdapterModes; CheckDeviceFormat vt+36 for D3DFMT 16/24/32;
  // append 20-byte rows (stride 0x14) at dword_6466A4 (+0 w, +4 h, +8
  // depth, +0x10 adapter index); ++dword_6495F4; dedupe scan prior rows
  // on w/h/depth. Orchestrator GfxDevice_EnumDisplayModes sub_4B8970 @
  // 0x004B8970 (GfxDevice ctor sub_4B7D00 @ 0x004B7F2F after Direct3DCreate9).
  // Register Natives_RegisterAll @ 0x488B9F. race122 deepen: host
  // render_d3d9_num_display_modes → ensure_modes(); g_modes.size().
  return render_d3d9_num_display_modes();
}

int32_t java_render_GfxEngine_currDisplayMode() {
  // PE @ 0x0047C450 size 0x5 (int_convert 5) static ()I. No UnboxArg /
  // this, no Mighty ERROR. Bytes: E9 4B DA 03 00 = jmp rel →
  // Gfx_GetCurrDisplayMode @ 0x004B9EA0 size 0x4F (int_convert 79). Sole
  // xref of Gfx_GetCurrDisplayMode. If dword_6495F4<=0 → 0; else scan
  // 20-byte rows at dword_6466A4 (stride 0x14): w@+0 h@+4 depth@+8 vs
  // g_GfxDevice (dword_6495DC, set GfxDevice_ctor @ 0x004B7DFA) i16@+0x1BC
  // /+0x1BE/+0x1C0 (same triplet as changeVideoMode early-out; that path
  // loads device via *(g_GfxEngine+0x204)). Match → index; exhausted → 0.
  // Register Natives_RegisterAll @ 0x488BBE. race122 deepen: host scans
  // g_modes vs g_w/g_h/depth.
  return render_d3d9_curr_display_mode();
}

InvObject* java_render_GfxEngine_displayModeName(int32_t i) {
  char buf[64];
  // PE @ 0x0047C460 size 0x41 (65). Static UnboxArg (I); sub_4B9F00 formats
  // "%d %d %d" from dword_6466A4[20*i] (w/h/depth); OOB → empty string.
  // OptionsDialog.show token(0/1/2) then UI "W X H X D" — not in the native.
  if (i == 0)
    std::snprintf(buf, sizeof(buf), "800 600 32");
  else if (i == 1)
    std::snprintf(buf, sizeof(buf), "1024 768 32");
  else if (i == 2)
    std::snprintf(buf, sizeof(buf), "1280 720 32");
  else if (i == 3)
    std::snprintf(buf, sizeof(buf), "1920 1080 32");
  else if (i < 0)
    buf[0] = '\0';
  else
    std::snprintf(buf, sizeof(buf), "800 600 %d", 32 + i);
  return string_new(buf);
}

void java_render_GfxEngine_changeVideoMode(int32_t width, int32_t height, int32_t depth) {
  // PE @ 0x0047C3F0 size 0x44 (int_convert 68) static (III)V.
  // UnboxArg dest0=nullptr (no this); dest1=width, dest2=height,
  // dest3=&CallInfo arg in-place = depth. ecx = *(g_GfxEngine+0x204)
  // (device from Gfx_InitDisplay); thiscall GfxDevice_vtbl vt+0x14 →
  // GfxDevice_ChangeVideoMode @ 0x004B9810 size 0x223: early-out if
  // i16@+0x1BC/+0x1BE/+0x1C0 match or byte@+0x1C2==1; else CheckDeviceFormat
  // (IDirect3D9 vt+0x28) pick D3DFMT; store dims; GetClientRect if windowed.
  // No ResetDevice in this native. race123 filler: host resize/open via
  // render_d3d9; device = *(g_GfxEngine+0x204) vt+0x14.
  render_d3d9_change_video_mode(width, height, depth);
}

void java_render_GfxEngine_printScreen(InvObject* filename) {
  // PE @ 0x0047C3C0 size 0x25 (int_convert 37) static (Ljava.lang.String;)V.
  // UnboxArg dest0=nullptr (no this), dest1 in-place → char* path. thiscall
  // Engine_GfxPrintScreen @ 0x004FDF60 size 0x13 ecx=g_GfxEngine/off_6187B0.
  // device=*(engine+0x204) via Gfx_InitDisplay/sub_4FE940/sub_4B7D00
  // (off_5F1550); call vt+0x58 = GfxDevice_PrintScreen @ 0x004BC3A0
  // size 0x15b. fopen wb; dword_6495E4 vt+0x20 GetDisplayMode(0);
  // lock vt+0x50; TGA type-2 (dword 0x20000) + u16 w/h + bpp 0x18 +
  // descriptor 0x20; BGRA→BGR rows; unlock vt+0x54; fclose.
  // Not portable 1:1 (D3D lock+TGA). race125 deepen VERIFY:
  // Engine_GfxPrintScreen @ 0x4FDF60 → GfxDevice_PrintScreen vt+0x58;
  // marker file stand-in.
  const char* path = filename ? string_cstr(filename) : nullptr;
  render_d3d9_print_screen(path);
}

void java_render_GfxEngine_flush() {
  // PE @ 0x0047C2D0 size 0x55 (int_convert 85) static ()V. No UnboxArg /
  // this. Same esi=7 pump as forceRendering @ 0x0047C1D0:
  //   PumpLoad@5378D0 + PumpUnload@537B40; GfxEngine_skipBeginScene=0;
  //   GfxEngine_PresentFrame@4FCA30; ++g_Engine_frameStamp@6200A4.
  // Unique tail after the loop (NOT in forceRendering):
  //   ResourceEngine_GCSweep(a2=1) @ 0x537ED0 — force gates
  //   eng+0x106ED8..EE4 then clear @ 0x53833C;
  //   GfxDevice_DrainGpuCaches @ 0x4B7B90 → device+0x33C Texture /
  //   +0x320 VB / +0x324 IB drains (IDA: DrainTextureCache@4F8240,
  //   DrainVertexBufferSlots@4ECDB0, DrainIndexBufferSlots@4EBDF0).
  // Soft deepen (IDA-backed): skipBeginScene clear in present helper;
  // PE flush always PresentFrame (no AltPresentGate@4F9760 — that gate is
  // MainLoop@428CE8 only); host soft still routes Present through the
  // MainLoop choice. DrainGpuCaches host freelists soft (W35-02).
  // PumpUnload already ++g_res_gc_frame (≡ frameStamp).
  for (int i = 7; i != 0; --i) {
    (void)resource_engine_pump_load_queue();
    (void)resource_engine_pump_unload_queue();
    gfx_engine_present_frame_or_alt();
  }
  // PE @ 0x47C319: ResourceEngine_GCSweep(1). Host impl in Resources.cpp
  // (PumpUnload calls a2=0; flush forces a2=1).
  (void)host_resource_engine_gcsweep(1);
  // PE @ 0x47C323: GfxDevice_DrainGpuCaches — W35-02 soft freelists.
  render_d3d9_drain_gpu_caches();
}

int32_t java_render_GfxEngine_openVideo(InvObject* filename, int32_t non_exclusive,
                                        int32_t loop) {
  // PE @ 0x0047C330 size 0x47 (int_convert 71). Unbox filename, non_exclusive I,
  // loop I. Path null → -1. Else FMV_DirectShow_Open @ 0x004F8DD0: success 0.
  // non_exclusive==0 RenderFile+HWND exclusive; !=0 TextureRenderer.
  // loop → dword_64A38C, EC_COMPLETE seek. On success Open writes
  // dword_64A394=1 @ 0x004F92EE (playing flag). W35-05: non_exclusive
  // stored → FMV_nonExclusive@61852C feeds AltPresentGate@4F9760.
  const char* path = filename ? string_cstr(filename) : nullptr;
  return video_fmv_open(path, non_exclusive, loop);
}

void java_render_GfxEngine_closeVideo() {
  // PE @ 0x0047C380 size 0x5 (int_convert 5) static ()V. No UnboxArg /
  // this. jmp FMV_DirectShow_Close @ 0x004F9320: if byte_64A390==0 ret;
  // else dword_64A394=0 (playing flag) then tear down DirectShow graph
  // (filters/media). Contrast openVideo @ 0x0047C330 = Unbox+Open body;
  // isPlayingVideo @ 0x0047C390 = jmp flag read only. race125 filler
  // VERIFY: thin jmp → FMV_DirectShow_Close @ 0x4F9320; host
  // video_fmv_close clears g_playing + graph.
  video_fmv_close();
}

int32_t java_render_GfxEngine_isPlayingVideo() {
  // PE @ 0x0047C390 size 0x5 (int_convert 5) static ()I. No UnboxArg /
  // this. jmp sub_4F9750 @ 0x004F9750 size 0x6: mov eax, dword_64A394;
  // retn (raw playing flag). Open writes 1 @ 0x004F92EE; Close writes 0
  // @ 0x004F9337 when byte_64A390!=0. Contrast: openVideo = Unbox+Open
  // body; closeVideo = jmp Close; this = jmp flag read only. race125
  // filler VERIFY: jmp → sub_4F9750 mov eax,dword_64A394; host g_playing.
  return video_fmv_is_playing();
}

namespace {
// PE dword_612C64 .data ; u32 init 0xFFFFFFFF (int_convert 4294967295).
int32_t g_text_default_color = static_cast<int32_t>(0xFFFFFFFFu);
}

void java_render_Text_create(InvObject* self, InvObject* parent, InvObject* charset, float x, float z) {
  // PE @ 0x00487050 size 0x7d (int_convert 125). UnboxArg (ResourceRef,ResourceRef,FF)V:
  // dest0=this dest1=parent dest2=charset dest3=x dest4=z. renderinst =
  // JVM_vm_get_ResourceRef_ptr_by_name(this,"renderinst") @ 0x0042A010.
  // Engine pos=(x,z,0) — Java (x,z) → engine (x,y,0). Text_createRenderInst
  // @ 0x0048C4B0 (ecx=renderinst): parent, unk_640934 default label, pos,
  // charset, dword_612C64 DEF_COLOR, flags=2, scale=1.0f (0x3F800000).
  // No Mighty ERROR. Soft deepen mesh_world_pose: Text_createRenderInst
  // @ 0x48C4B0 wires parent + bone00 local via setBoneLocal @ 0x48C0F0
  // (same SetBoneMatrixParentLink matrix path as setMatrix @ 0x4810B0).
  // Host: mesh parent + bone00 local + world TREE publish; D3D text stand-in.
  if (charset && !render_d3d9_font_ready(charset)) {
    const int32_t rid = java_util_resource_ResourceRef_id(charset);
    if (!render_d3d9_font_load_from_rid(charset, rid))
      render_d3d9_font_load(charset, "simple20");
  }
  render_d3d9_text_create(self, charset, x, z);
  if (self) {
    tree_field_set_int(self, "_text_kind", 1);  // PE type "r_text"
    tree_field_set_float(self, "_text_px", x);
    tree_field_set_float(self, "_text_py", z);
    tree_field_set_float(self, "_text_pz", 0.f);
    tree_field_set_int(self, "_text_posed", 1);
    tree_field_set_int(self, "_text_color", g_text_default_color);
    render_d3d9_text_set_color(self, static_cast<uint32_t>(g_text_default_color));
    if (parent) {
      render_d3d9_mesh_set_parent(self, parent);
      const int32_t bone00 = render_d3d9_mesh_get_bone_id(parent, "bone00");
      render_d3d9_mesh_set_attach_bone(self, bone00);
    }
    const int32_t bone = render_d3d9_mesh_get_bone_id(self, "bone00");
    render_d3d9_mesh_set_bone_local(self, bone, x, z, 0.f, 0.f, 0.f, 0.f);
    tree_field_set_int(self, "applied_bone_id", bone);
    tree_field_set_int(self, "bone_link_flags",
                       tree_field_get_int(self, "bone_link_flags") | 0x1800);
    render_ref_mesh_world_pose_soft_publish(self);
  }
}  // PE @ 0x00487050

bool text_create_rtext2_inst_soft(InvObject* self, InvObject* parent,
                                  InvObject* charset, float x, float z,
                                  int32_t color, int32_t align, float scale) {
  // Soft PE Text_createRText2Inst @ 0x0048C670:
  //   ResourceEngine_type_renderinst(..., "r_text2", 0)
  //   charset handle+8==0 → null; Engine_malloc(buf) owned @ params+0xC0
  //   flags |=1 |0x8000; color@+0xCC align@+0xC4 scale@+0xC8
  //   bone00 setBoneLocal + PrepareLod walk.
  // Soft: D3D text stand-in + TREE kind=2; factory/PrepareLod/mid OOS.
  if (!self || !charset) return false;
  if (charset && !render_d3d9_font_ready(charset)) {
    const int32_t rid = java_util_resource_ResourceRef_id(charset);
    if (!render_d3d9_font_load_from_rid(charset, rid))
      render_d3d9_font_load(charset, "simple20");
  }
  if (!render_d3d9_font_ready(charset)) return false;
  (void)scale;
  render_d3d9_text_create(self, charset, x, z);
  tree_field_set_int(self, "_text_kind", 2);  // PE type "r_text2"
  tree_field_set_obj(self, "_text_type", string_new("r_text2"));
  tree_field_set_float(self, "_text_px", x);
  tree_field_set_float(self, "_text_py", z);
  tree_field_set_float(self, "_text_pz", 0.f);
  tree_field_set_int(self, "_text_posed", 1);
  tree_field_set_int(self, "_text_owned_buf", 1);  // PE malloc path
  tree_field_set_int(self, "_text_color", color);
  tree_field_set_int(self, "_text_align", align);
  render_d3d9_text_set_color(self, static_cast<uint32_t>(color));
  render_d3d9_text_set_align(self, align);
  if (parent) {
    render_d3d9_mesh_set_parent(self, parent);
    const int32_t bone00 = render_d3d9_mesh_get_bone_id(parent, "bone00");
    render_d3d9_mesh_set_attach_bone(self, bone00);
  }
  const int32_t bone = render_d3d9_mesh_get_bone_id(self, "bone00");
  render_d3d9_mesh_set_bone_local(self, bone, x, z, 0.f, 0.f, 0.f, 0.f);
  tree_field_set_int(self, "applied_bone_id", bone);
  tree_field_set_int(self, "bone_link_flags",
                     tree_field_get_int(self, "bone_link_flags") | 0x1800);
  render_ref_mesh_world_pose_soft_publish(self);
  return true;
}

void java_render_Text_setDefaultColor(int32_t color) {
  // PE @ 0x00487020 size 0x1f (int_convert 31). STATIC (I)V.
  // UnboxArg dest0=0 skip this, dest1=color I. dword_612C64 = edx
  // full DWORD (no AND / 0x00FFFFFF mask). Sibling get @ 0x00487040
  // size 0x6: mov eax, dword_612C64; retn. race125 filler VERIFY.
  g_text_default_color = color;
}

int32_t java_render_Text_getDefaultColor() {
  // PE @ 0x00487040 size 0x6 (int_convert 6). STATIC ()I.
  // No UnboxArg / this, no Mighty ERROR. Bytes: A1 64 2C 61 00 C3 =
  // mov eax, Text_defaultColor @ dword_612C64; retn. .data init
  // 0xFFFFFFFF (Java Text.DEF_COLOR). Pair with setDefaultColor @
  // 0x00487020 (same global, full DWORD, no 0x00FFFFFF mask). Register
  // race123 filler: g_text_default_color. race125 VERIFY dword_612C64.
  return g_text_default_color;
}

void java_render_Text_changeColor(InvObject* self, int32_t color) {
  // PE @ 0x00487150 size 0x7c (124). UnboxArg (I)V: this + color I.
  // renderinst = JVM_vm_get_ResourceRef_ptr_by_name(this, "renderinst")
  // @ 0x0042A010 → node = *[eax+0xC]. node==0 → ret (no Mighty).
  // If [node+0x4C]!=1: vtable+0x14(node, 1.0f). sub_5447D0(node,
  // 0x80000001, 0.0, 10.0); sign bit set → null; else vtable+0xC(node,
  // 1.0f). Success: *[result+0xCC]=color (offset 204 int_convert). No
  // separate update call. race125 deepen VERIFY: TREE _text_color stand-in
  // for +0xCC; D3D color store + update stand-in for live draw (PE writes
  // params only).
  if (self) tree_field_set_int(self, "_text_color", color);
  render_d3d9_text_set_color(self, static_cast<uint32_t>(color));
  render_d3d9_text_update(self);
}  // PE @ 0x00487150

void java_render_Text_changeAlign(InvObject* self, int32_t align) {
  // PE @ 0x004871D0 size 0x7c (int_convert 124). UnboxArg (I)V: dest0=this
  // (arg_0), dest1=align (var_4). renderinst =
  // JVM_vm_get_ResourceRef_ptr_by_name(this,"renderinst") @ 0x0042A010 →
  // node=*[eax+0xC]. node==0 → ret (no Mighty ERROR). Same load walk as
  // changeColor @ 0x00487150: [node+0x4C]!=1 → vtable+0x14(node, 1.0f /
  // 0x3F800000); sub_5447D0(node, 0x80000001, 0.0, 10.0 / 0x41200000);
  // sign bit → null else vtable+0xC(node, 1.0f). Success:
  // *[result+0xC4]=align (offset 196 int_convert). race125 deepen VERIFY:
  // tree _text_align stand-in for +0xC4; D3D align store + update (PE
  // writes engine text params only).
  if (!self) return;
  tree_field_set_int(self, "_text_align", align);
  render_d3d9_text_set_align(self, align);
  render_d3d9_text_update(self);
}  // PE @ 0x004871D0

void java_render_Text_setPos(InvObject* self, float x, float z) {
  // PE @ 0x004870D0 size 0x77 (int_convert 119). UnboxArg (FF)V: dest0=this
  // dest1=x dest2=z. renderinst = JVM_vm_get_ResourceRef_ptr_by_name(this,
  // "renderinst"). Mat3x4_setIdentity @ 0x0054E1D0 on local 3x4. Engine pos=
  // (x,z,0) — same Java→engine map as create @ 0x00487050. RenderRef_bindBone
  // ("bone00") @ 0x0048BC40 then RenderRef_setBoneLocal @ 0x0048C0F0
  // (bone, mode=0, pos, identity). Same SetBoneMatrixParentLink matrix
  // write as setMatrix @ 0x4810B0 (bone_ref=0). No Mighty ERROR / no
  // Text.update call. Soft deepen: bone00 local (Java x,z → engine x,y,0)
  // + world-pose TREE publish; D3D set_pos/update stand-in for live draw.
  if (self) {
    tree_field_set_float(self, "_text_px", x);
    tree_field_set_float(self, "_text_py", z);
    tree_field_set_float(self, "_text_pz", 0.f);
    tree_field_set_int(self, "_text_posed", 1);
    const int32_t bone = render_d3d9_mesh_get_bone_id(self, "bone00");
    render_d3d9_mesh_set_bone_local(self, bone, x, z, 0.f, 0.f, 0.f, 0.f);
    tree_field_set_int(self, "applied_bone_id", bone);
    tree_field_set_int(self, "bone_link_flags",
                       tree_field_get_int(self, "bone_link_flags") | 0x1800);
    render_ref_mesh_world_pose_soft_publish(self);
  }
  render_d3d9_text_set_pos(self, x, z);
  render_d3d9_text_update(self);
}  // PE @ 0x004870D0

InvObject* java_render_Text_getPos(InvObject* self) {
  // PE @ 0x00487250 size 0xf5 (int_convert 245). Unbox this. renderinst +
  // RenderRef_bindBone("bone00") + sub_48C3D0 (7 xrefs, not renamed):
  // *(handle+8)==0 or bone 0 → nullptr. No Mighty ERROR. Not Vector3(0,0,0).
  // Else alloc Vector3 0x1C (28). qmemcpy 0x40 (64) from bone+0x54 (84);
  // xyz = mtx._41/_42/_43 * flt_5F08E8 (bits 0x3dcccccd, 1036831949).
  // setPos/create write Java (x,z,0) as engine (x,y,z). Host stash is already
  // Java units (no *10 in render_d3d9) — do not fmul 0.1 here.
  // !self / never create: handle+8==0 analogue → nullptr.
  if (!self) return nullptr;
  if (!tree_field_get_int(self, "_text_posed")) return nullptr;
  return vec3_new(tree_field_get_float(self, "_text_px"),
                  tree_field_get_float(self, "_text_py"),
                  tree_field_get_float(self, "_text_pz"));
}

void java_render_Text_update(InvObject* self) {
  // PE @ 0x00487350 size 0xb1 (int_convert 177). UnboxArg ()V: this only.
  // renderinst = JVM_vm_get_ResourceRef_ptr_by_name(this,"renderinst").
  // JVM_vm_get_instance_field(this,"text","java.lang.String") @ 0x0042A690;
  // null → cstr=0; else JVM_vm_get_int_field(str, dword_62E008)=Native.ptr.
  // node = *[renderinst+0xC]; node==0 → ret. Same load walk as changeColor
  // @ 0x00487150: [node+0x4C]!=1 → vtable+0x14(node,1.0f); sub_5447D0(node,
  // 0x80000001, 0.0, 10.0f / 0x41200000); sign → null else vtable+0xC(1.0f).
  // Success: *[result+0xC0]=cstr; cstr==0 → *[result+0xC0]=&unk_640938
  // (empty fallback). No Mighty ERROR. Host: tree "text" → D3D string + update.
  if (!self) return;
  if (InvObject* s = tree_field_get_obj(self, "text"))
    render_d3d9_text_set_string(self, string_cstr(s));
  else
    render_d3d9_text_set_string(self, "");
  render_d3d9_text_update(self);
}  // PE @ 0x00487350

float java_render_Text_getWidthPixels(InvObject* str, InvObject* font) {
  // PE @ 0x00487410 size 0x44. Static UnboxArg. cstr==0 → 0.0 (no Mighty).
  // Inner Engine_TextMeasureWidth @ 0x004FDE50 unique; scale 1.0f flags 0.
  // font==0 PE crash [eax+8]; host-safe 0. PE ready-fail → 0.0 (no simple20).
  if (!str) return 0.f;
  if (font && !render_d3d9_font_ready(font)) {
    const int32_t rid = java_util_resource_ResourceRef_id(font);
    if (!render_d3d9_font_load_from_rid(font, rid))
      render_d3d9_font_load(font, "simple20");
  }
  return render_d3d9_font_measure_px(font, string_cstr(str));
}

void java_render_Viewport_create(InvObject* self, int32_t pri, float x, float y, float w, float h) {
  // PE @ 0x004814E0 size 0x11c (284). UnboxArg (IFFFF)V: this + pri I +
  // x,y,w,h F. handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → Mighty ERROR ("!" + "Mighty ERROR") on Engine_ErrorLogBuf
  // @ 0x0062E018; buf[0]=0. Else Engine_ViewportCreate @ 0x00538D60
  // (arg0=0, pri,x,y,w,h) — alloc type-18 "_viewport", write rect/pri on
  // params, link list. If current [handle+0xC] != new: unlink old, then
  // ResHandle_Link @ 0x4290F0 + [handle+8]=[new+0x50]. Contrast destroy @
  // 0x00481600 (GfxEngine_ViewportUnbind @ 0x4FD050 + ResHandle_UnlinkList).
  // Soft: Native.ptr + node+0x4C=0x12 + TREE rect (Link/list OOS).
  if (!self) return;  // PE Mighty on handle 0 — host no Native.ptr yet
  viewport_soft_seed_native(self, pri, x, y, w, h);
  render_d3d9_viewport_create(self, pri, x, y, w, h);
}  // PE @ 0x004814E0

void java_render_Viewport_destroy(InvObject* self) {
  // PE @ 0x00481600 size 0x80 (128). UnboxArg ()V: this only.
  // handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → Mighty ERROR ("!" + "Mighty ERROR" via Engine_strcat_cap
  // + Engine_ErrorLogMsgBox on Engine_ErrorLogBuf @ 0x0062E018); buf[0]=0.
  // Else: thiscall GfxEngine_ViewportUnbind @ 0x4FD050 ecx=g_GfxEngine/
  // off_6187B0, arg=handle — same callee as Viewport.deactivate @ 0x004816C0
  // (unique xrefs: destroy@0x481631 + deactivate@0x4816ee; type
  // *(node+0x4C)==0x12 list unbind) — then always thiscall
  // ResHandle_UnlinkList(handle) (was sub_48A8D0). Soft: D3D erase +
  // Native.ptr clear (Mighty / list unbind OOS).
  if (!self) return;
  render_d3d9_viewport_destroy(self);
  native_ptr_clear(self);
}

void java_render_Viewport_activate(InvObject* self, int32_t renderflags) {
  // PE @ 0x00481680 size 0x3e. UnboxArg (I)V: this + renderflags I (DWORD at
  // box+8). handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → silent return (no Mighty ERROR). Else
  // GfxEngine_ViewportBind(handle, -1, -1) @ 0x4FD020 (stdcall retn 0Ch);
  // ecx=off_6187B0 dead. Bind: *(handle+0xC) && *(node+0x4C)==0x12 →
  // GfxEngine_ViewportBindList @ 0x4FD280. Unboxed renderflags NEVER
  // consumed in this 0x3e. Java CLEARDEPTH=0x1 / CLEARTARGET=0x2 (host
  // kViewportClear*). Soft: TREE flags + D3D pending_clear on flush (PE
  // bind ignores flags). Catalog render viewport_activate DONE soft.
  if (!self) return;
  if (!native_ptr_get(self)) return;  // soft ≡ handle 0
  if (void* node = native_ptr_node(self)) {
    if (*reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x4C) !=
        kViewportNodeType)
      return;
  }
  const int32_t flags = renderflags & kViewportClearMask;
  tree_field_set_int(self, "vp_renderflags", flags);
  render_d3d9_viewport_activate(self, flags);
}

void java_render_Viewport_deactivate(InvObject* self) {
  // PE @ 0x004816C0 size 0x34 (int_convert 52). UnboxArg ()V: this only
  // (no flags). Native.ptr dword_62E008; 0 → silent return (no Mighty
  // ERROR). Else thiscall GfxEngine_ViewportUnbind @ 0x4FD050 ecx=off_6187B0
  // gfx, arg=handle. Inverse of activate's GfxEngine_ViewportBind @ 0x4FD020.
  // Unbind itself gates *(handle+0xC) && *(node+0x4C)==0x12 (type 18) before
  // list unlink — native wrapper only checks handle!=0. Soft deepen: mirror
  // that type-18 gate + clear TREE flags (!Native.ptr = handle 0).
  if (!self) return;
  if (!native_ptr_get(self)) return;
  if (void* node = native_ptr_node(self)) {
    if (*reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x4C) !=
        kViewportNodeType)
      return;
  }
  tree_field_set_int(self, "vp_renderflags", 0);
  render_d3d9_viewport_deactivate(self);
}

float java_render_Viewport_getAspect(InvObject* self) {
  // PE @ 0x004817B0 size 0x37. UnboxArg ()F: this. handle =
  // JVM_vm_get_int_field(this, dword_62E008). FLD flt_5F08F0 (1.0 @
  // 0x005F08F0). handle==0 → RET with 1.0 (no Mighty ERROR). Else FSTP that
  // 1.0 and JMP Viewport_getAspect_inner @ 0x0048CDA0 size 0x82 (ecx=handle).
  // Inner: *(handle+0xC); 0 → 1.0. type *(node+0x4C)==1 skip vtable+0x14(1.0).
  // sub_5447D0(node, 0xA0000001, 0, 10.0); sign → 1.0. Else vtable+0xC(1.0)
  // rect. sub_4FD580 (ecx=off_6187B0) writes two display ints (vtable+0x64 on
  // engine+0x204). FLD [rect+0x18] FIMUL both ints, FDIVP → display_w/display_h
  // (width field cancels; +0x1C height unused). Fail → 1.0.
  if (!self || !native_ptr_get(self)) return 1.f;
  return render_d3d9_viewport_get_aspect(self);
}

float java_render_Viewport_getWidth(InvObject* self) {
  // PE @ 0x004817F0 size 0x7a (int_convert 122). UnboxArg ()F: this.
  // Native.ptr dword_62E008; handle==0 / inner=*(handle+0xC)==0 /
  // vtable+0xC==0 → FLD [ESP+8] (unboxed this bits; no Mighty ERROR).
  // Else: type *(node+0x4C)==1 skip vtable+0x14(1.0f); sub_5447D0(node,
  // 0x80000001, 0.0, 10.0); success FLD [rect+0x18] (offset 24). Units:
  // Java create/resize (FFFF) stored as-is (no video_x scale) →
  // normalized [0,1]. Soft fail → 0 (boot getters; PE this-bits OOS).
  if (!self || !native_ptr_get(self)) return 0.f;
  return render_d3d9_viewport_get_width(self);
}

float java_render_Viewport_getHeight(InvObject* self) {
  // PE @ 0x00481870 size 0x7a (int_convert 122). Same handle/inner/
  // sub_5447D0 walk as getWidth @ 0x004817F0. Success: FLD [rect+0x1C]
  // (offset 28). Fail: FLD [ESP+8], no Mighty ERROR. Soft fail → 0.
  if (!self || !native_ptr_get(self)) return 0.f;
  return render_d3d9_viewport_get_height(self);
}

float java_render_Viewport_getTop(InvObject* self) {
  // PE @ 0x004818F0 size 0x7a (int_convert 122). Same walk as getWidth.
  // Success: FLD [rect+0x14] (offset 20). Soft fail → 0.
  if (!self || !native_ptr_get(self)) return 0.f;
  return render_d3d9_viewport_get_top(self);
}

float java_render_Viewport_getLeft(InvObject* self) {
  // PE @ 0x00481970 size 0x7a (int_convert 122). Same walk as getWidth.
  // Success: FLD [rect+0x10] (offset 16). Soft fail → 0.
  if (!self || !native_ptr_get(self)) return 0.f;
  return render_d3d9_viewport_get_left(self);
}

void java_render_Viewport_resize(InvObject* self, float x, float y, float w, float h) {
  // PE @ 0x00481700 size 0xa6. UnboxArg (FFFF)V: this + x,y,w,h (DWORD at
  // box+8 each). handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 / inner=*(handle+0xC)==0 / rect==0 → silent ret (no Mighty
  // ERROR). Else same walk as getWidth @ 0x004817F0: type *(node+0x4C)==1
  // skip vtable+0x14(1.0); sub_5447D0(node, 0x80000001, 0, 10.0); sign →
  // skip. Else vtable+0xC(1.0) rect. FLD unboxed x,y,w,h then FSTP
  // [rect+0x10/14/18/1C] as-is (no video_* scale). Units: Java [0,1]
  // (Viewport.java; Navigator.changeSize 0.02/0.78/0.2/0.18). Soft: TREE
  // rect + D3D ViewportState.
  if (!self) return;
  if (!native_ptr_get(self) || !native_ptr_node(self)) return;
  tree_field_set_float(self, "vp_x", x);
  tree_field_set_float(self, "vp_y", y);
  tree_field_set_float(self, "vp_w", w);
  tree_field_set_float(self, "vp_h", h);
  render_d3d9_viewport_resize(self, x, y, w, h);
}

InvObject* java_render_Viewport_unproject(InvObject* self, InvObject* v,
                                          int32_t cameraId) {
  // PE @ 0x00487A60 size 0x176. Unbox this, Vector3, cameraId I.
  // Walk viewport cams; match [cam+0x630]==cameraId (0 is a real id, not
  // "active"). Miss → nullptr. sub_513840 uses xyz then ×0.1 / ×-0.1 / ×0.1.
  // Host: cameraId 0 / miss still falls back to active + vec3(0,0,0) — smoke
  // boot unproject is calibrated on that. Do not retarget without the probe.
  float vx = 0, vy = 0, vz = 0;
  if (v) vec3_get(v, &vx, &vy, &vz);
  void* cam = nullptr;
  if (cameraId != 0) {
    if (InvObject* o = resref_find_by_id(cameraId)) cam = o;
  }
  if (!cam) cam = render_d3d9_camera_active();
  float ox = 0, oy = 0, oz = 0;
  if (!render_d3d9_viewport_unproject(self, cam, vx, vy, &ox, &oy, &oz))
    return vec3_new(0, 0, 0);
  return vec3_new(ox, oy, oz);
}

// Sound / SfxRef audio: natives_resources.cpp + audio_win32.cpp (Phase 2.21)

// VA 0x0047EA10 — bind Animation to RenderRef + clip ResourceRef.

void java_render_Camera_setFog(InvObject* self, int32_t color, float near,
                               float far) {
  // PE @ 0x00486570 size 0x142 (322). UnboxArg (IFF)V @ 0x0045D910:
  // this, color I, near F, far F (DWORD at box+8).
  // handle = JVM_vm_get_int_field(this, dword_62E008) @ 0x0042AB50.
  // handle==0 → "!Mighty ERROR" via Engine_strcat_cap + Engine_ErrorLogMsgBox.
  // Dual TREE hop (inlined ResHandle_getPayload @ 0x00419860; NOT GroundRef
  // packet 0x4A):
  //   node0 = *(handle+0xC); if [node0+0x4C]!=1 →
  //     ResourceEngine_BumpPriority @ vt+0x14 (0x00544780)(1.0f);
  //   ResHandle_PrepareLod @ 0x5447D0 (node0, 0xA0000000, 0, 0);
  //   mid = ResourceEngine_GetParamExt @ vt+0xC (0x0053FCB0)(1.0f);
  //   node1 = *(mid+0x84); same BumpPriority/PrepareLod/GetParamExt;
  //   fog = GetParamExt(node1). Writes (flt_5E7334=10.0 @ 0x005E7334):
  //     +0x24=1.0f, +0x28=color (raw DWORD), +0x1C=near*10, +0x20=far*10.
  // No D3D in this native. GroundRef.setFog @ 0x00486A20 = packet 0x4A.
  // Same tag 0xA0000000 as RenderRef_LinkOrUnlinkBone @ 0x0048BE10 a4≠0
  // parent_inner hop (→ ResPayload_createBoneParentHook @ 0x0053F7E0:
  // malloc 0x1C, ResHandle_Link(node+0x44), |=0x4000, insert payload+0x78).
  // Host bone path: render_d3d9_mesh_set_parent (RenderRef.setMatrix 4-arg).
  // Host: native_ptr_ensure in Camera.create; dual hop via
  // res_handle_get_payload(tag 0xA0000000) → mid+0x84 → fog params.
  // W35-05 residual: dual hop already soft; BumpPriority/PrepareLod
  // live inside PE getPayload — host getPayload is layout stand-in OOS.
  if (!self) return;
  // PE: handle = Native.ptr; 0 → Mighty. Host: ensure then gate on node.
  if (!native_ptr_get(self)) native_ptr_ensure(self);
  void* node0 = native_ptr_node(self);
  if (!node0) return;
  constexpr int32_t kTag = static_cast<int32_t>(0xA0000000u);
  void* mid = res_handle_get_payload(node0, kTag);
  if (!mid) return;
  // PE: node1 = *(mid+0x84)
  void* node1 = *reinterpret_cast<void**>(reinterpret_cast<char*>(mid) + 0x84);
  if (!node1) return;
  void* fog = res_handle_get_payload(node1, kTag);
  if (!fog) return;
  const float n = near * 10.f;
  const float f = far * 10.f;
  *reinterpret_cast<float*>(reinterpret_cast<char*>(fog) + 0x24) = 1.f;
  *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(fog) + 0x28) = color;
  *reinterpret_cast<float*>(reinterpret_cast<char*>(fog) + 0x1C) = n;
  *reinterpret_cast<float*>(reinterpret_cast<char*>(fog) + 0x20) = f;
  // Mirror TREE for host readers + D3D stand-in (not in PE native).
  tree_field_set_int(self, "fog_on", 1);
  tree_field_set_float(self, "fog_enable", 1.f);
  tree_field_set_int(self, "fog_color", color);
  tree_field_set_float(self, "fog_near", n);
  tree_field_set_float(self, "fog_far", f);
  render_d3d9_set_fog(color & 0x00ffffff, n, f);
}

// VA 0x00480440 — GameRef/RenderRef.getDetail()F (LOD bias on resource).

// ---------------------------------------------------------------------------
// PE helpers for queueEvent physical cluster (GameRef_voidEvent_parse @
// 0x00458C00, size 0x38fe). Rebind owner @ 0x45AFC9 (add_light); rem_light
// unbind via GameRef_LightNode_dtor @ 0x45F8B0. LoadGameInit Rebind @
// 0x45C3C9 wired (void_event_loadgameinit_rebind); Engine_LoadGameInit
// @ 0x53A5E0 host stand-in = engine_load_game_init (W11B: PoolAlloc 348B +
// TREE splice parent+0x38; GI +0x48/+0x50 Rebind). Chassis "render" Rebind
// @ 0x458D88 stays OOS.
// ---------------------------------------------------------------------------

namespace {

// PE node+0x54 payload flags dword (HostResNode pad covers +0x50..+0xCB).
int32_t* host_node_payload_flags(void* node) {
  if (!node) return nullptr;
  return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x54);
}

}  // namespace

int32_t void_event_res_handle_rebind_owner(void* rh /*light_node+0xC*/,
                                           InvObject* type, int32_t seed_key) {
  // PE add_light @ 0x45AF8E..0x45AFC9: malloc 0x2C -> Engine_SimCallbackNode_ctor
  // @ 0x429130 -> zero ResHandle at node+0xC -> ResHandle_Rebind(node+0xC,
  // *(type_handle+0xC)). rh+8 <- *(owner+0x50) (= rem_light match @ +0x14).
  if (!rh || !type) return 0;
  if (!native_ptr_get(type)) native_ptr_ensure(type);
  void* owner = native_ptr_node(type);
  if (!owner) return 0;
  if (seed_key != 0) {
    auto* p50 =
        reinterpret_cast<int32_t*>(reinterpret_cast<char*>(owner) + 0x50);
    if (*p50 == 0) *p50 = seed_key;
  }
  res_handle_rebind(rh, owner);
  return *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(rh) + 8);
}

void void_event_res_handle_unbind_owner(void* rh /*light_node+0xC*/) {
  // PE GameRef_LightNode_dtor @ 0x45F8B0: this+3 (=node+0xC) unlink from
  // owner+0x48 — same owner-slice as ResHandle_Rebind(rh, 0).
  res_handle_rebind(rh, nullptr);
}

void void_event_loadgameinit_rebind(void* rh /*stack ResHandle ch18*/,
                                    void* loadgameinit_gi /*owner|0*/) {
  // PE @ 0x45C3C9 inside GameRef_voidEvent_parse (EVENT type 0x80 @
  // 0x45C15A → jpt case 2 @ 0x45C323):
  //   ResHandle_ctor_zero(&stack_rh_ch18);
  //   sub_429860(self, 0x18, &stack_rh_ch18);  // skip if ch18 already bound
  //   gi = Engine_LoadGameInit(wtroot_rh, g_voidEvent_LoadGameInit_typeRH,
  //                            "1,0.25", "bot");  // aBot @ 0x610948
  //   ResHandle_Rebind(&stack_rh_ch18, gi);       // PE @ 0x45C3C9
  // W11B/W12B/W13B factory (Resources): AllocLocalRid@536ED0 →
  // InstanceNode_PoolAlloc@538720 (348B) → CreateNodeByDesc TREE@536DFF
  // (parent+0x38) / ResolveParent@537000 when wtroot+0xC==0;
  // eng+0x11C SimObject root; Class*→FQN@404EA0 → attach_gametype.
  // W14C: type_rh mid+0x10 seeded by resource_engine_type_gametype
  // @ 0x53A1A0 / 0x53A27D (createNativeInstance) — null Class* = PE skip.
  // Gaps: GameType ctor vtbl+0xC@53A8B2; PrepareLod vtbl.
  // Then sprintf "controllable %d" + sub_45F780(EVENT_COMMAND) on that RH,
  // plus "AI_GoToTrafficSlow" + Engine_addTimer(0x80000080).
  // Gates (PE): self+0x1FC0==0, +0x1FCC!=0, +0x1FD0!=0 && *[+0x4C]==8
  // (GII_CONTROL), clear self+0x70 bit 0x40000.
  // Owner shape = GI node (+0x48 list / +0x50 key) — same ResHandle_Rebind
  // @ 0x429060 as add_light, but owner is LoadGameInit return not type+0xC.
  //
  // Parent GameRef.cpp wire (this agent must NOT edit GameRef):
  //   Real site = EVENT type 0x80 case-2 body. When wiring:
  //     gi = engine_load_game_init(wtroot_rh, type_rh, "1,0.25", "bot");
  //     void_event_loadgameinit_rebind(&stack_rh_ch18, gi);
  res_handle_rebind(rh, loadgameinit_gi);
}

int32_t res_handle_or_payload_flags(InvObject* self, int32_t flags) {
  // PE ResHandle_orPayloadFlags @ 0x0048D1F0 size 0x1a.
  // thiscall ecx=Native.ptr handle; node=*(handle+0xC)=*(this+3).
  // node==0 → return 0. Else *(node+0x54) |= flags; return new value.
  // voidEvent add_light @ 0x45B05C: push 0x400000; call (ecx=type handle).
  // rem_light @ 0x45B132 also or's 0x400000 when flags&0x40 clear path.
  // Contrast RenderRef.setLight @ 0x486AB0 → RenderRef_applyLight @ 0x48C9D0
  // (getPayload tag 0x80000001; RGB * 1/256) — separate from this bit.
  if (!self) return 0;
  if (!native_ptr_get(self)) native_ptr_ensure(self);
  void* node = native_ptr_node(self);
  int32_t* pf = host_node_payload_flags(node);
  if (!pf) return 0;
  const int32_t before = *pf;
  *pf |= flags;
  tree_field_set_int(self, "payload_flags", *pf);
  // Soft PE: first arm of add_light 0x400000 → TREE unit defaults matching
  // setLight Unbox defaults (0xFFFFFF / 0x404040 / 0xFFFFFF). PE does NOT
  // write RGB here — host-only for voidEvent light readers.
  if ((~before & *pf & 0x400000) != 0)
    render_ref_apply_light_soft(self, 0x00FFFFFF, 0x00404040, 0x00FFFFFF);
  return *pf;
}

int32_t res_handle_and_not_payload_flags(InvObject* self, int32_t flags) {
  // PE ResHandle_andNotPayloadFlags @ 0x0048D210 size 0x1c.
  // Same walk; *(node+0x54) = ~flags & *pf; return new or 0.
  // voidEvent rem_light @ 0x45B10D/0x45B11B: push 0x400000 when flags&0x40.
  // Soft deepen: when 0x400000 clears, drop TREE light unit floats that
  // or_payload_flags seeded on the set arm (PE andNot does not touch RGB).
  if (!self) return 0;
  void* node = native_ptr_node(self);
  int32_t* pf = host_node_payload_flags(node);
  if (!pf) return 0;
  const int32_t before = *pf;
  *pf = (~flags) & *pf;
  tree_field_set_int(self, "payload_flags", *pf);
  if ((before & ~*pf & 0x400000) != 0) {
    tree_field_set_int(self, "light_diffuse", 0);
    tree_field_set_int(self, "light_ambient", 0);
    tree_field_set_int(self, "light_specular", 0);
    tree_field_set_float(self, "light_diff_r", 0.f);
    tree_field_set_float(self, "light_diff_g", 0.f);
    tree_field_set_float(self, "light_diff_b", 0.f);
    tree_field_set_float(self, "light_amb_r", 0.f);
    tree_field_set_float(self, "light_amb_g", 0.f);
    tree_field_set_float(self, "light_amb_b", 0.f);
    tree_field_set_float(self, "light_spec_r", 0.f);
    tree_field_set_float(self, "light_spec_g", 0.f);
    tree_field_set_float(self, "light_spec_b", 0.f);
  }
  return *pf;
}

int32_t res_handle_get_payload_flags(InvObject* self) {
  // Host read of PE *(node+0x54). No PE getter of this exact name; mirrors
  // GameRef_getFlags walk (inner+0x54) for ResourceRef/type handles.
  if (!self) return 0;
  void* node = native_ptr_node(self);
  int32_t* pf = host_node_payload_flags(node);
  return pf ? *pf : 0;
}

int32_t render_ref_apply_bone_id(InvObject* self, int32_t bone_id) {
  // PE RenderRef_applyBoneId @ 0x0048BD30 size 0xd8.
  // thiscall ecx=handle; arg=bone id from RenderRef_bindBone @ 0x48BC40.
  // Temp ResHandle list splice; vtbl+0x14 bump if type!=1; sub_5447D0
  // (0x80000000); vtbl+0xC payload; RenderInst_findBoneById @ 0x541310
  // (was sub_541310): walk bone list match +0x34; hit → payload+0xBC
  // |=0x1800 + bone vtbl(1). Fail / null handle → 0 after unlink restore.
  // voidEvent add_linked @ 0x45B55C/0x45B570 + rem_linked @ 0x45B7A9/0x45B7BD:
  // bindBone("bone00"/"bone01") then applyBoneId(eax).
  // Soft deepen mesh_world_pose: attach_bone feeds resolve_world
  // (Local*BoneLocal*ParentWorld) — same hierarchy consumers as setMatrix
  // @ 0x4810B0 → SetBoneMatrixParentLink @ 0x48BF50 → LinkOrUnlinkBone.
  // Host: attach_bone stand-in (setMatrix parent-link already |=0x1800 in
  // Resources); TREE applied_bone_id + world_px/py/pz publish. No invent
  // of bone* BST / vtbl. Return bone_id on ok, 0 on miss (PE null→0).
  if (!self) return 0;
  if (bone_id < 0) return 0;
  if (!native_ptr_get(self)) native_ptr_ensure(self);
  if (!native_ptr_node(self)) return 0;
  // PE findBoneById miss still returns a node ptr; host has no bone BST —
  // accept any non-negative id that bindBone/getBoneId produced (incl. 0
  // for bone00/root stand-in in render_d3d9_mesh_get_bone_id).
  render_d3d9_mesh_set_attach_bone(self, bone_id);
  tree_field_set_int(self, "applied_bone_id", bone_id);
  // PE payload+0xBC |= 0x1800 on hit — host TREE mirror (ResState.bone_link
  // stays in Resources setMatrix path).
  tree_field_set_int(self, "bone_link_flags",
                     tree_field_get_int(self, "bone_link_flags") | 0x1800);
  render_ref_mesh_world_pose_soft_publish(self);
  return bone_id;
}

}  // namespace inv
